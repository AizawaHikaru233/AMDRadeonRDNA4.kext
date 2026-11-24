//
//  flip.cpp
//  AMDRadeonRDNA4
//
//  DCN page-flip discovery and latch helpers. The boot-time W5 test adds
//  trails around its first hardware operations; runtime Present/Restore uses
//  the same helpers without writing bring-up breadcrumbs.
//

#include "flip.hpp"
#include "klog.hpp"

#include "compute.hpp"
#include "pipe.hpp"
#include "sdma.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#define FLOG(fmt, ...) rdna4Log("AMDRadeonRDNA4: flip: " fmt "\n", ## __VA_ARGS__)

namespace Flip {

namespace {

constexpr uint32_t kBad = 0xffffffffu;

// DCN 4.1.0 offset header: register names and BASE_IDX segments are kept
// beside every literal so emulator and hardware accesses cannot drift.
// regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, BASE_IDX 2.
constexpr uint32_t kHubpAddressLo = 0x060a;
// regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, BASE_IDX 2.
constexpr uint32_t kHubpAddressHi = 0x060b;
// regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, BASE_IDX 2.
constexpr uint32_t kHubpEarliestLo = 0x061c;
// regHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, BASE_IDX 2.
constexpr uint32_t kHubpEarliestHi = 0x061d;
// regHUBPREQ0_DCSURF_FLIP_CONTROL, BASE_IDX 2.
constexpr uint32_t kHubpFlipControl = 0x0613;
constexpr uint32_t kHubpFlipPending = 1u << 8;
constexpr uint32_t kHubpFlipType = 1u << 1;
constexpr uint32_t kHubpStereoMode = 3u << 12;
constexpr uint32_t kHubpStereoIn = 1u << 16;
// regHUBPREQ0_DCSURF_SURFACE_CONFIG, BASE_IDX 2: ROTATION_ANGLE [9:8] (0, 1, 2, 3 = 0, 90, 180, 270 degrees the plane reads its
// surface turned), H_MIRROR_EN [10].
constexpr uint32_t kHubpSurfaceConfig = 0x05e5;
constexpr uint32_t kRotationMask = 0x300;
// regOTG0_OTG_STATUS_POSITION and regOTG0_OTG_MASTER_UPDATE_LOCK, BASE_IDX 2.
constexpr uint32_t kOtgStatusPosition = 0x1b4a;
constexpr uint32_t kOtgMasterUpdateLock = 0x1b89;
// regMPCC0_MPCC_TOP_SEL and regMPCC0_MPCC_OPP_ID, BASE_IDX 3.
constexpr uint32_t kMpccTopSel = 0x0000;
constexpr uint32_t kMpccOppId = 0x0002;
constexpr uint32_t kMaxAddressBytes = 0xffffffffu;

// A HUBP's rotation: the angle the OS asked for and the scanout size the address a turned read starts at is worked out from.
struct ScanoutRotation {
	uint32_t degrees { 0 };
	uint32_t variant { 0 };           // 2: the last pixel, 3: the last row's first (PanelRot's mode, passed in)
	uint32_t pitch { 0 };             // bytes a row
	uint32_t width { 0 }, height { 0 };
};
ScanoutRotation gRotation[Pipe::kMaxOtg];

uint32_t angleBits(uint32_t degrees) {
	return degrees == 90 ? 1u : degrees == 180 ? 2u : degrees == 270 ? 3u : 0u;
}

} // namespace

// (privately) the bytes a turned read's start address is past the surface's first pixel, for the mode in force.
uint64_t addressOffset(uint8_t hubp) {
	if (hubp >= Pipe::kMaxOtg)
		return 0;
	const ScanoutRotation &r = gRotation[hubp];

	if (r.degrees != 180)
		return 0;

	const uint64_t lastRow = r.height ? static_cast<uint64_t>(r.pitch) * (r.height - 1) : 0;

	switch (r.variant) {
	case 2: return lastRow + (r.width ? static_cast<uint64_t>(r.width - 1) * 4 : 0);
	case 3: return lastRow;
	default: return 0;
	}
}

uint32_t rotation(uint8_t hubp) {
	return hubp < Pipe::kMaxOtg ? gRotation[hubp].degrees : 0;
}

uint32_t surfaceConfig(RDNA4Compute &compute, uint8_t hubp) {
	if (hubp >= Pipe::kMaxOtg)
		return kBad;

	return compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpSurfaceConfig + static_cast<uint32_t>(hubp) * Pipe::Reg::kHubpStride });
}

bool setRotation(RDNA4Compute &compute, uint8_t hubp, uint32_t degrees, uint32_t variant, uint32_t pitch, uint32_t width,
                 uint32_t height) {
	if (hubp >= Pipe::kMaxOtg)
		return false;

	const uint32_t base = static_cast<uint32_t>(hubp) * Pipe::Reg::kHubpStride;
	const uint32_t image = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpSurfaceConfig + base });

	if (image == kBad)
		return false;

	const uint32_t want = (image & ~kRotationMask) | (angleBits(degrees) << 8);

	if (want != image)
		compute.wr(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpSurfaceConfig + base }, want);

	ScanoutRotation &r = gRotation[hubp];

	if (r.degrees != degrees || r.variant != variant || r.pitch != pitch || r.width != width || r.height != height) {
		r = ScanoutRotation { degrees, variant, pitch, width, height };
		FLOG("HUBP%u: scanout turned %u degrees (DCSURF_SURFACE_CONFIG 0x%08x -> 0x%08x), %ux%u at %u bytes a row: flips from %s",
		     hubp, degrees, image, want, width, height, pitch,
		     variant == 2 ? "the last pixel" : variant == 3 ? "the last row" : "the first pixel");
	}

	return true;
}

bool findPipe(RDNA4Compute &compute, Surface &out, bool log) {
	auto dmuRead = [&](uint8_t segment, uint32_t dword) {
		return compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { segment, dword });
	};
	auto dmuRead2 = [&](uint32_t dword) { return dmuRead(2, dword); };
	auto dmuRead3 = [&](uint32_t dword) { return dmuRead(3, dword); };
	Surface found {};
	uint8_t opp = kNone;
	for (uint8_t i = 0; i < Pipe::kMaxOtg; i++) {
		const uint32_t control = dmuRead2(Pipe::Reg::kOtgControl + i * Pipe::Reg::kOtgStride);
		if (control != kBad && (control & 1) && (control & (1u << 16))) {
			found.otg = i;
			break;
		}
	}
	if (found.otg == kNone) {
		FLOG("skip: no running OTG");
		return false;
	}

	const uint32_t oppReg = Pipe::Reg::kOptcDataSource + found.otg * Pipe::Reg::kOdmStride;
	const uint32_t oppImage = dmuRead2(oppReg);
	if (oppImage == kBad) {
		FLOG("skip: OTG%u OPP source unreadable", found.otg);
		return false;
	}
	opp = static_cast<uint8_t>((oppImage >> 16) & 0xf);
	for (uint8_t i = 0; i < Pipe::kMaxOtg; i++) {
		const uint32_t id = dmuRead3(kMpccOppId + i * Pipe::Reg::kMpccStride);
		if (id != kBad && (id & 0xf) == opp) {
			// MPCC_TOP_SEL is the DPP/HUBP id; the MPCC index is not.
			const uint32_t top = dmuRead3(kMpccTopSel + i * Pipe::Reg::kMpccStride);
			if (top != kBad && (top & 0xf) < Pipe::kMaxOtg) {
				found.hubp = static_cast<uint8_t>(top & 0xf);
				break;
			}
		}
	}
	if (found.hubp == kNone) {
		FLOG("skip: OTG%u OPP%u has no HUBP", found.otg, opp);
		return false;
	}

	const uint32_t hubpBase = static_cast<uint32_t>(found.hubp) * Pipe::Reg::kHubpStride;
	const uint32_t viewport = dmuRead2(Pipe::Reg::kHubpViewportDim + hubpBase);
	const uint32_t pitchImage = dmuRead2(Pipe::Reg::kHubpSurfacePitch + hubpBase);
	const uint32_t oldLo = dmuRead2(kHubpAddressLo + hubpBase);
	const uint32_t oldHi = dmuRead2(kHubpAddressHi + hubpBase);
	if (viewport == kBad || pitchImage == kBad || oldLo == kBad || oldHi == kBad) {
		FLOG("skip: HUBP%u surface registers unreadable", found.hubp);
		return false;
	}
	found.width = viewport & 0x3fffu;
	found.height = (viewport >> 16) & 0x3fffu;
	found.pitch = (pitchImage & 0xffffu) + 1;
	found.desktop = static_cast<uint64_t>(oldLo) | (static_cast<uint64_t>(oldHi) << 32);
	const uint64_t bytes = surfaceBytes(found.pitch, found.height);
	if (!found.width || !found.height || !found.pitch || !bytes || bytes > kMaxAddressBytes) {
		FLOG("skip: invalid surface %ux%u pitch %u", found.width, found.height, found.pitch);
		return false;
	}

	out = found;
	if (log)
		FLOG("pipe OTG%u OPP%u HUBP%u surface 0x%llx, %ux%u pitch %u (%llu bytes)",
		     found.otg, opp, found.hubp, found.desktop, found.width, found.height, found.pitch, bytes);
	return true;
}

bool waitNextVblank(RDNA4Compute &compute, uint8_t otg, uint32_t timeoutMs,
                    uint64_t &frame) {
	auto dmuRead = [&](uint32_t dword) {
		return compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, dword });
	};
	const uint32_t reg = Pipe::Reg::kOtgFrameCount + otg * Pipe::Reg::kOtgStride;
	const uint32_t before = dmuRead(reg);
	if (before == kBad)
		return false;
	uint64_t span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(timeoutMs) * 1000000ull, &span);
	const uint64_t start = mach_absolute_time();
	for (;;) {
		const uint32_t current = dmuRead(reg);
		if (current != kBad && ((current - before) & 0xffffffu)) {
			frame = current & 0xffffffu;
			return true;
		}
		if (mach_absolute_time() - start >= span)
			return false;
		IOSleep(1);
	}
}

bool flipTo(RDNA4Compute &compute, const Surface &surface, uint64_t target,
            const char *name, uint64_t *latencyUs, bool async) {
	const bool detailed = !async || rdna4TraceLevel >= 2;
	auto dmuRead = [&](uint8_t segment, uint32_t dword) {
		return compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { segment, dword });
	};
	auto dmuRead2 = [&](uint32_t dword) { return dmuRead(2, dword); };
	auto dmuWrite2 = [&](uint32_t dword, uint32_t value) {
		compute.wr(IpDiscovery::HwDmu, GfxReg::Reg { 2, dword }, value);
	};
	if (surface.otg == kNone || surface.hubp == kNone)
		return false;
	const uint32_t hubpBase = static_cast<uint32_t>(surface.hubp) * Pipe::Reg::kHubpStride;
	const uint32_t lockReg = kOtgMasterUpdateLock + surface.otg * Pipe::Reg::kOtgStride;

	auto readAddress = [&](uint64_t &address) {
		const uint32_t lo = dmuRead2(kHubpAddressLo + hubpBase);
		const uint32_t hi = dmuRead2(kHubpAddressHi + hubpBase);
		if (lo == kBad || hi == kBad)
			return false;
		address = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
		return true;
	};
	auto readPosition = [&](uint32_t &frame, uint32_t &line) {
		const uint32_t f = dmuRead2(Pipe::Reg::kOtgFrameCount +
		                            surface.otg * Pipe::Reg::kOtgStride);
		const uint32_t p = dmuRead2(kOtgStatusPosition +
		                            surface.otg * Pipe::Reg::kOtgStride);
		if (f == kBad || p == kBad)
			return false;
		frame = f & 0xffffffu;
		line = (p >> 16) & 0xffffu;
		return true;
	};
	auto waitForPendingClear = [&]() {
		uint64_t span = 0;
		nanoseconds_to_absolutetime(100000000, &span);
		const uint64_t start = mach_absolute_time();
		for (;;) {
			const uint32_t control = dmuRead2(kHubpFlipControl + hubpBase);
			if (control != kBad && !(control & kHubpFlipPending)) {
				if (detailed)
					FLOG("%s: pending clear control 0x%08x", name, control);
				return true;
			}
			if (mach_absolute_time() - start >= span)
			{
				FLOG("failure: %s pending timeout control 0x%08x", name, control);
				return false;
			}
			IODelay(10);
		}
	};
	auto waitForFrameAdvance = [&](uint32_t before, uint32_t &after, uint32_t &line) {
		uint64_t span = 0;
		nanoseconds_to_absolutetime(100000000, &span);
		const uint64_t start = mach_absolute_time();
		for (;;) {
			uint32_t current = 0;
			if (readPosition(current, line) && ((current - before) & 0xffffffu)) {
				after = current;
				if (detailed)
					FLOG("%s: OTG frame advanced %u->%u line %u", name, before, after, line);
				return true;
			}
			if (mach_absolute_time() - start >= span) {
				after = current;
				FLOG("failure: %s vblank timeout frame %u->%u line %u", name,
				     before, after, line);
				return false;
			}
			IODelay(10);
		}
	};
	auto writeAddress = [&]() {
		const uint32_t before = dmuRead2(lockReg);
		if (before == kBad)
			return false;
		dmuWrite2(lockReg, before | 1);
		uint64_t span = 0;
		nanoseconds_to_absolutetime(1000000, &span);
		const uint64_t lockStart = mach_absolute_time();
		bool locked = false;
		while (mach_absolute_time() - lockStart < span) {
			const uint32_t lock = dmuRead2(lockReg);
			if (lock != kBad && (lock & (1u << 8))) {
				locked = true;
				break;
			}
			IODelay(10);
		}
		if (!locked) {
			FLOG("failure: %s update lock timeout state 0x%08x", name, dmuRead2(lockReg));
			dmuWrite2(lockReg, before & ~1u);
			return false;
		}

		bool wrote = false;
		const uint32_t flipReg = kHubpFlipControl + hubpBase;
		uint32_t flipControl = dmuRead2(flipReg);
		if (flipControl != kBad) {
			flipControl &= ~(kHubpFlipType | kHubpStereoMode | kHubpStereoIn);
			dmuWrite2(flipReg, flipControl);
			dmuWrite2(kHubpAddressHi + hubpBase, addressHi(target));
			dmuWrite2(kHubpAddressLo + hubpBase, addressLo(target));
			wrote = true;
		}
		dmuWrite2(lockReg, before & ~1u);
		return wrote && waitForPendingClear();
	};

	uint32_t beforeFrame = 0, beforeLine = 0;
	if (!readPosition(beforeFrame, beforeLine)) {
		FLOG("failure: %s could not read OTG position", name);
		return false;
	}
	if (detailed)
		FLOG("%s: begin target 0x%llx frame %u line %u", name, target, beforeFrame, beforeLine);
	uint64_t startNs = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &startNs);
	if (!writeAddress()) {
		FLOG("failure: %s did not latch within 100 ms", name);
		return false;
	}
	uint64_t readback = 0;
	if (!readAddress(readback) || readback != target) {
		uint32_t frame = 0, line = 0;
		(void)readPosition(frame, line);
		FLOG("failure: %s readback 0x%llx expected 0x%llx pending control 0x%08x frame %u line %u",
		     name, readback, target, dmuRead2(kHubpFlipControl + hubpBase), frame, line);
		return false;
	}
	uint64_t earliest = 0;
	const uint32_t earliestLo = dmuRead2(kHubpEarliestLo + hubpBase);
	const uint32_t earliestHi = dmuRead2(kHubpEarliestHi + hubpBase);
	if (earliestLo == kBad || earliestHi == kBad) {
		FLOG("failure: %s earliest-in-use unreadable (0x%08x/0x%08x)", name,
		     earliestLo, earliestHi);
		return false;
	}
	earliest = static_cast<uint64_t>(earliestLo) |
	           (static_cast<uint64_t>(earliestHi) << 32);
	// This is DC's hubp2_is_flip_pending test: pending clear plus the
	// DCSURF_SURFACE_EARLIEST_INUSE pair matching the requested address.  The
	// DCN 4.1.0 offsets are 0x061c/0x061d (BASE_IDX 2).
	if (earliest != target) {
		uint32_t frame = 0, line = 0;
		(void)readPosition(frame, line);
		FLOG("failure: %s earliest-in-use 0x%llx expected 0x%llx pending control 0x%08x frame %u line %u",
		     name, earliest, target, dmuRead2(kHubpFlipControl + hubpBase), frame, line);
		return false;
	}
	if (detailed)
		FLOG("%s: earliest-in-use 0x%llx", name, earliest);
	uint32_t afterFrame = 0, afterLine = 0;
	// The EARLIEST_INUSE pair above is the latch completion signal.  The OTG
	// frame counter is sampled separately so the caller's latency and scanline
	// report covers the frame containing the latch.
	if (!waitForFrameAdvance(beforeFrame, afterFrame, afterLine))
		return false;
	const uint32_t frameDelta = (afterFrame - beforeFrame) & 0xffffffu;
	const uint32_t vtotalImage = dmuRead2(Pipe::Reg::kOtgVTotal +
	                                      surface.otg * Pipe::Reg::kOtgStride);
	const uint32_t vtotal = vtotalImage == kBad ? 0 : (vtotalImage & 0x7fffu) + 1;
	const uint64_t scanlines = static_cast<uint64_t>(frameDelta) * vtotal +
	                          (afterLine >= beforeLine ? afterLine - beforeLine : 0);
	uint64_t endNs = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &endNs);
	const uint64_t elapsedUs = (endNs - startNs) / 1000;
	if (latencyUs)
		*latencyUs = elapsedUs;
	if (detailed)
		FLOG("%s latched: address 0x%llx, latency %llu us, %llu scanlines, frame %u->%u",
		     name, readback, elapsedUs, scanlines, beforeFrame, afterFrame);
	return true;
}

uint8_t hubpShowing(RDNA4Compute &compute, uint64_t mc) {
	for (uint8_t i = 0; i < Pipe::kMaxOtg; i++) {
		const uint32_t base = i * Pipe::Reg::kHubpStride;
		const uint32_t lo = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpAddressLo + base });
		const uint32_t hi = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpAddressHi + base });

		// a turned scanout's register holds the surface's address plus the offset (addressOffset)
		if (lo != kBad && hi != kBad && (static_cast<uint64_t>(lo) | static_cast<uint64_t>(hi) << 32) == mc + addressOffset(i))
			return i;
	}

	return kNone;
}

bool request(RDNA4Compute &compute, uint8_t hubp, uint64_t mc) {
	const uint32_t base = static_cast<uint32_t>(hubp) * Pipe::Reg::kHubpStride;
	const uint32_t control = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpFlipControl + base });

	if (hubp >= Pipe::kMaxOtg || control == kBad)
		return false;

	if (control & (kHubpFlipType | kHubpStereoMode | kHubpStereoIn))   // at vblank, not at once
		compute.wr(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpFlipControl + base }, control & ~(kHubpFlipType | kHubpStereoMode | kHubpStereoIn));

	// A turned scanout: the angle goes with every address (the plane's own programming latches DCSURF_SURFACE_CONFIG back to the
	// value it holds, so a write at mode-set time alone does not hold), and the address is the one the turned read starts at.
	if (rotation(hubp)) {
		const uint32_t image = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpSurfaceConfig + base });
		const uint32_t bits = angleBits(rotation(hubp)) << 8;

		// unconditional: the plane's own programming latches the field back, and one write a flip costs nothing
		if (image != kBad)
			compute.wr(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpSurfaceConfig + base }, (image & ~kRotationMask) | bits);
		static uint32_t seen;   // (diagnostic) what the field held before the write, the first few flips
		if (seen < 8 && image != kBad) {
			seen++;
			FLOG("HUBP%u: flip: DCSURF_SURFACE_CONFIG 0x%08x (angle %u) -> %u", hubp, image, (image >> 8) & 3, angleBits(rotation(hubp)));
		}
	}

	const uint64_t at = mc + addressOffset(hubp);

	compute.wr(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpAddressHi + base }, addressHi(at));
	compute.wr(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpAddressLo + base }, addressLo(at));

	return true;
}

bool shown(RDNA4Compute &compute, uint8_t hubp, uint64_t &requested, uint64_t &inUse) {
	const uint32_t base = static_cast<uint32_t>(hubp) * Pipe::Reg::kHubpStride;
	const uint32_t lo = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpAddressLo + base });
	const uint32_t hi = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpAddressHi + base });
	const uint32_t useLo = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpEarliestLo + base });
	const uint32_t useHi = compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, kHubpEarliestHi + base });

	if (hubp >= Pipe::kMaxOtg || lo == kBad || hi == kBad || useLo == kBad || useHi == kBad)
		return false;

	// the register holds the turned read's start address; callers deal in the surface's own
	const uint64_t offset = addressOffset(hubp);

	requested = (static_cast<uint64_t>(lo) | static_cast<uint64_t>(hi) << 32);
	inUse = (static_cast<uint64_t>(useLo) | static_cast<uint64_t>(useHi) << 32);
	requested = requested >= offset ? requested - offset : requested;
	inUse = inUse >= offset ? inUse - offset : inUse;

	return true;
}

bool underflowed(RDNA4Compute &compute, uint8_t hubp) {
	constexpr uint32_t kDchubpCntl = 0x05f4;
	constexpr uint32_t kUnderflowStatus = 7u << 28, kUnderflowClear = 1u << 31;
	const GfxReg::Reg reg { 2, kDchubpCntl + static_cast<uint32_t>(hubp) * Pipe::Reg::kHubpStride };
	const uint32_t v = hubp < Pipe::kMaxOtg ? compute.rd(IpDiscovery::HwDmu, reg) : kBad;

	if (v == kBad || !(v & kUnderflowStatus))
		return false;

	compute.wr(IpDiscovery::HwDmu, reg, v | kUnderflowClear);
	compute.wr(IpDiscovery::HwDmu, reg, v & ~kUnderflowClear);

	return true;
}

uint8_t otgOf(RDNA4Compute &compute, uint8_t hubp) {
	auto dmuRead = [&](uint8_t segment, uint32_t dword) {
		return compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { segment, dword });
	};
	uint8_t opp = kNone;

	for (uint8_t i = 0; i < Pipe::kMaxOtg && opp == kNone; i++) {
		const uint32_t top = dmuRead(3, kMpccTopSel + i * Pipe::Reg::kMpccStride);
		const uint32_t id = dmuRead(3, kMpccOppId + i * Pipe::Reg::kMpccStride);

		if (top != kBad && id != kBad && (top & 0xf) == hubp)
			opp = static_cast<uint8_t>(id & 0xf);
	}

	for (uint8_t i = 0; i < Pipe::kMaxOtg && opp != kNone; i++) {
		const uint32_t control = dmuRead(2, Pipe::Reg::kOtgControl + i * Pipe::Reg::kOtgStride);
		const uint32_t source = dmuRead(2, Pipe::Reg::kOptcDataSource + i * Pipe::Reg::kOdmStride);

		if (control != kBad && (control & 1) && source != kBad && ((source >> 16) & 0xf) == opp)
			return i;
	}

	return kNone;
}

bool scan(RDNA4Compute &compute, uint8_t otg, uint32_t &line, uint32_t &lines, uint32_t &blankStart) {
	auto dmuRead2 = [&](uint32_t dword) {
		return compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, dword + static_cast<uint32_t>(otg) * Pipe::Reg::kOtgStride });
	};

	if (otg >= Pipe::kMaxOtg)
		return false;

	const uint32_t position = dmuRead2(kOtgStatusPosition);   // OTG_VERT_COUNT [30:16]
	const uint32_t total = dmuRead2(Pipe::Reg::kOtgVTotal);     // lines - 1
	const uint32_t blank = dmuRead2(Pipe::Reg::kOtgVBlank);     // OTG_V_BLANK_START [14:0]

	if (position == kBad || total == kBad || blank == kBad)
		return false;

	line = position >> 16 & 0x7fff;
	lines = (total & 0x7fff) + 1;
	blankStart = blank & 0x7fff;

	return line < lines && blankStart < lines;
}

bool run(RDNA4Compute &compute) {
	uint32_t mode = 0;
	if (!PE_parse_boot_argn("rdna4-flip", &mode, sizeof(mode)) || !mode)
		return true;
	if (mode > 2) {
		FLOG("invalid mode %u; feature disabled", mode);
		compute.publishResult("flip", "FAIL invalid mode");
		return true;
	}
	if (!compute.dmaReady || !compute.devHeap.size() || !compute.rtLock) {
		FLOG("skip: SDMA/device heap unavailable");
		compute.publishResult("flip", "SKIPPED SDMA/device heap unavailable");
		return true;
	}

	Surface surface {};
	IOLockLock(compute.rtLock);
	const bool found = findPipe(compute, surface);
	IOLockUnlock(compute.rtLock);
	if (!found)
		compute.publishResult("flip", "SKIPPED no running display pipe");
	if (!found)
		return true;
	const uint64_t bytes = surfaceBytes(surface.pitch, surface.height);

	uint64_t bufferOffset = 0;
	IOLockLock(compute.rtLock);
	const bool allocated = compute.devHeap.alloc(bytes, bufferOffset);
	IOLockUnlock(compute.rtLock);
	if (!allocated) {
		FLOG("skip: device heap allocation of %llu bytes failed", bytes);
		compute.publishResult("flip", "FAIL device heap allocation");
		return true;
	}
	const uint64_t buffer = compute.vramMc(bufferOffset);
	FLOG("allocate: VRAM offset 0x%llx address 0x%llx size %llu", bufferOffset, buffer, bytes);

	bool touchedSurface = false;
	bool ok = true;
	auto runSdmaLocked = [&](const uint32_t *packet, uint32_t dwords, uint32_t timeoutMs) {
		IOLockLock(compute.rtLock);
		const bool completed = compute.sdmaRun(packet, dwords, timeoutMs);
		IOLockUnlock(compute.rtLock);
		return completed;
	};
	auto bootFlip = [&](uint64_t target, const char *trail, const char *name) {
		compute.trail(trail);
		IOLockLock(compute.rtLock);
		const bool completed = flipTo(compute, surface, target, name);
		IOLockUnlock(compute.rtLock);
		// W32: is the cursor plane still as programmed after a flip? Read-only and bounded; only set with rdna4-cursor.
		if (completed && compute.env.cursorProbe)
			compute.env.cursorProbe(compute.env.cursorProbeCtx, name);
		return completed;
	};

	uint32_t packet[Sdma::kCopyDwords];
	compute.trail("flip: SDMA copy");
	if (!Sdma::copyLinear(packet, surface.desktop, buffer, static_cast<uint32_t>(bytes)) ||
	    !runSdmaLocked(packet, Sdma::kCopyDwords, 2000)) {
		FLOG("failure: SDMA VRAM-to-VRAM copy did not complete");
		ok = false;
	} else {
		FLOG("copy: VRAM 0x%llx -> 0x%llx (%llu bytes)", surface.desktop, buffer, bytes);
	}

	if (ok && mode == 2) {
		constexpr uint32_t colours[8] = {
			0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffff00u,
			0xff00ffffu, 0xffff00ffu, 0xffffffffu, 0xff000000u,
		};
		const uint64_t rowBytes = static_cast<uint64_t>(surface.pitch) * 4;
		for (uint32_t bar = 0; bar < 8 && ok; bar++) {
			const uint32_t first = surface.height * bar / 8;
			const uint32_t last = surface.height * (bar + 1) / 8;
			const uint64_t fillBytes = static_cast<uint64_t>(last - first) * rowBytes;
			uint32_t fill[Sdma::kFillDwords];
			compute.trail("flip: SDMA pattern fill");
			if (!Sdma::constFill(fill, buffer + static_cast<uint64_t>(first) * rowBytes,
			                    colours[bar], static_cast<uint32_t>(fillBytes)) ||
			    !runSdmaLocked(fill, Sdma::kFillDwords, 2000)) {
				FLOG("failure: pattern bar %u did not complete", bar);
				ok = false;
			} else {
				FLOG("pattern: bar %u rows %u..%u", bar, first, last);
			}
		}
	}

	if (ok) {
		touchedSurface = true;
		ok = bootFlip(buffer, "flip: surface address", "flip");
	}
	if (ok && mode == 2) {
		FLOG("pattern displayed; holding for 1 second");
		IOSleep(1000);
	}
	if (ok) {
		touchedSurface = true;
		ok = bootFlip(surface.desktop, "flip: flip back", "flip back");
	}
	if (!ok) {
		FLOG("failure: page flip test failed");
		if (touchedSurface && !bootFlip(surface.desktop, "flip: restore original", "restore original"))
			FLOG("failure: restoring original surface did not latch");
		FLOG("feature disabled for this boot");
	}

	IOLockLock(compute.rtLock);
	const bool freed = compute.devHeap.free(bufferOffset);
	IOLockUnlock(compute.rtLock);
	if (!freed)
		FLOG("failure: device heap free of offset 0x%llx failed", bufferOffset);
	else
		FLOG("free: device heap offset 0x%llx", bufferOffset);
	compute.publishResult("flip", ok ? "PASS boot flip restored desktop" : "FAIL boot flip");
	return ok;
}

} // namespace Flip
