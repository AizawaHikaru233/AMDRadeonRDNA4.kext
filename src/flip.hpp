//
//  flip.hpp
//  AMDRadeonRDNA4
//
//  The display page-flip smoke test. The implementation is kept separate
//  from the compute bring-up; compute only supplies the one post-stage-7
//  entry point and its DMA/device-heap state.
//

#ifndef AMDRadeonRDNA4_FLIP_HPP
#define AMDRadeonRDNA4_FLIP_HPP

#include <stdint.h>

class RDNA4Compute;

namespace Flip {

constexpr uint8_t kNone = 0xff;

// The active DCN path and its desktop surface. Register accessors do not
// take rtLock; callers serialize hardware access around them.
struct Surface {
	uint8_t otg { kNone };
	uint8_t hubp { kNone };
	uint32_t width { 0 };
	uint32_t height { 0 };
	uint32_t pitch { 0 };
	uint64_t desktop { 0 };
};

// Pure helpers are also exercised by the host harness. A primary linear
// surface is pitch pixels wide, height rows high and four bytes per pixel.
constexpr uint64_t surfaceBytes(uint32_t pitchPixels, uint32_t height) {
	return pitchPixels && height &&
	       static_cast<uint64_t>(pitchPixels) <= (~0ull / 4ull) / height
		? static_cast<uint64_t>(pitchPixels) * height * 4ull
		: 0;
}

constexpr uint32_t addressLo(uint64_t address) {
	return static_cast<uint32_t>(address);
}

constexpr uint32_t addressHi(uint64_t address) {
	return static_cast<uint32_t>(address >> 32);
}

// Discover the lit OPP/HUBP and its scanout surface. No trail is written;
// this function is also used by post-boot clients. `log` false: only a
// failure is logged (a client that presents asks once a frame).
bool findPipe(RDNA4Compute &compute, Surface &out, bool log = true);

// Poll the OTG frame counter until it advances. This is the temporary W5c
// vblank interface; replace its polling body with ihWaitVblank when W1b lands.
bool waitNextVblank(RDNA4Compute &compute, uint8_t otg, uint32_t timeoutMs,
                    uint64_t &frame);

// Latch one address and verify its vblank advance and readback. The caller
// serializes this with rtLock and supplies any boot trail before the call.
bool flipTo(RDNA4Compute &compute, const Surface &surface, uint64_t target,
			const char *name, uint64_t *latencyUs = nullptr, bool async = false);

// The HUBP whose primary surface is at MC address `mc` (a head's scanout), or kNone. The caller holds rtLock, as below.
uint8_t hubpShowing(RDNA4Compute &compute, uint64_t mc);

// HUBP `hubp` to the surface at `mc` at its next vblank (a vsync flip: the low address word's write requests it, as DC's
// hubp401_program_surface_flip_and_addr). Nothing is waited for.
bool request(RDNA4Compute &compute, uint8_t hubp, uint64_t mc);

// What HUBP `hubp` was last asked to show (`requested`) and what it reads now (`inUse`, DCSURF_SURFACE_EARLIEST_INUSE): the same
// once the request latched. False when they cannot be read.
bool shown(RDNA4Compute &compute, uint8_t hubp, uint64_t &requested, uint64_t &inUse);

// The scanout of HUBP `hubp` read turned by `degrees` (0 or 180 here), and which address such a read starts at (`variant`: 0 the
// surface's first pixel, 2 its last pixel, 3 the last row's first) -- rdna4-rot180 says, device.cpp passes it. `pitch` (bytes a
// row), `width` and `height` are the scanout's. Everything that hands the HUBP an address (request) or compares one (shown,
// hubpShowing) goes through the offset this keeps, so a caller never has to know about it. The angle is written with every flip:
// a write made once (at boot, at a mode set) is latched away by the plane's own programming.
bool setRotation(RDNA4Compute &compute, uint8_t hubp, uint32_t degrees, uint32_t variant, uint32_t pitch, uint32_t width,
                 uint32_t height);
uint32_t rotation(uint8_t hubp);
// DCSURF_SURFACE_CONFIG as the HUBP reads it now (ROTATION_ANGLE [9:8], H_MIRROR_EN [10]); 0xffffffff when unreadable. The
// register is double buffered, so this is how to tell whether the angle a flip wrote is the one latched.
uint32_t surfaceConfig(RDNA4Compute &compute, uint8_t hubp);

// Where a turned scanout's read starts, in bytes past the surface's first pixel (`variant` of setRotation): 1 the first pixel (as
// Linux DC programs a 180-degree plane, and the one verified on this card), 2 the last pixel, 3 the last row's first.
uint64_t addressOffset(uint8_t hubp);

// Whether HUBP `hubp` ran out of data since the last call (DCHUBP_CNTL.HUBP_UNDERFLOW_STATUS, sticky; cleared here): its picture then
// shows lines of garbage or of the frame before.
bool underflowed(RDNA4Compute &compute, uint8_t hubp);

// The OTG whose picture HUBP `hubp` feeds (MPCC_TOP_SEL, its MPCC_OPP_ID, the OTG's OPTC_SEG0_SRC_SEL), or kNone.
uint8_t otgOf(RDNA4Compute &compute, uint8_t hubp);

// Where OTG `otg` scans: its line now, its lines a frame and the first line of its vertical blank. False when they cannot be read.
bool scan(RDNA4Compute &compute, uint8_t otg, uint32_t &line, uint32_t &lines, uint32_t &blankStart);

// Run the rdna4-flip=<mode> test once. False means the requested test failed
// and the feature has been disabled after attempting to restore the desktop.
bool run(RDNA4Compute &compute);

} // namespace Flip

#endif /* AMDRadeonRDNA4_FLIP_HPP */
