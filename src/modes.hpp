//
//  modes.hpp
//  AMDRadeonRDNA4
//
//  The display-mode table offered to macOS, built from the sink's EDID.
//  IOFramebuffer identifies modes by driver-chosen IDs that WindowServer
//  persists in its display preferences, so the table must be deterministic:
//  the same EDID and limits always yield the same modes under the same IDs,
//  and no two entries may look identical in the Displays pane (same size,
//  same whole-Hz refresh) — macOS would show both and the user could not
//  tell which timing they picked.
//
//  Sources, in the priority order used when two of them describe the same
//  size and rate (the first one wins): base-block DTDs (the first is the
//  preferred timing), CTA-861 extension DTDs, CTA short video descriptors
//  (CEA-861 timings), standard timings and established timings (VESA DMT).
//  Detailed timings are exact rasters the sink declared; everything else is
//  a table lookup, so it is additionally held to the EDID's own range
//  limits (vertical rate, max pixel clock) and the HDMI VSDB TMDS limit.
//
//  Freestanding: shared by the kext and the host test harness; no heap —
//  the caller owns the Mode array.
//

#ifndef Modes_hpp
#define Modes_hpp

#include "dsc.hpp"
#include "edid.hpp"

namespace Modes {

constexpr size_t MaxModes = 32;

// Where a mode came from, for logging (and, for the EDID sources, the
// deduplication priority described above).
enum Source : uint8_t {
	SourceDtd    = 0,   // base-block detailed timing
	SourceCtaDtd = 1,   // detailed timing in a CTA-861 extension
	SourceVic    = 2,   // CTA-861 short video descriptor -> CEA-861 table
	SourceStd    = 3,   // standard timing -> DMT table
	SourceEst    = 4,   // established timing -> DMT table
	SourceBoot   = 5,   // read back from the OTG the GOP programmed
	SourceDisplayId = 6,   // detailed timing in a DisplayID extension (Edid::displayIdTimings)
};

// Short lowercase tag ("dtd", "cta-dtd", "vic", "std", "est", "boot", "displayid").
const char *sourceName(uint8_t source);

struct Mode {
	uint32_t id;                 // stable 1..N, assigned in table order
	Edid::DetailedTiming t;
	uint32_t refreshMilliHz;     // rounded to the nearest mHz
	bool native;                 // EDID preferred timing
	uint8_t source;              // Source
	// rdna4-hdr=1 (device.cpp, RDNA4Device::buildModeTable): the same raster as the SDR mode it was made from, offered
	// with dynamicRange HDR10, colorimetry BT2020|BT2100 and 10 bits a component (getTimingInfoForDisplayMode's
	// colour hook). Appended after the SDR table under an ID of its own; false for every mode of a plain boot.
	bool hdr10;
	// U1 (docs/dsc-modes-u1.md, rdna4-dscmodes=1): this timing is above the pixel clock the link carries uncompressed
	// and is offered because the sink's own DSC caps say the compressor can carry it (Limits::maxPixelClockDscKHz and
	// dscLayout below). The three values are what plugin.cpp's getTimingInfoForDisplayMode hook stamps into
	// IODetailedTimingInformationV2 (dscCompressedBitsPerPixel = dscBpp * 16, dscSliceWidth, dscSliceHeight); every one
	// of them is 0 — and `dsc` false — for every mode of a boot that does not arm the option, and for every mode at or
	// below the uncompressed cap on a boot that does.
	bool dsc;
	uint8_t dscSlices;              // slices per line (1, 2 or 4: what the encoder offers of the sink's {1,2,4,6,8})
	uint8_t dscBpp;                 // whole bits per pixel (this sink takes integer bpp only, DPCD 0x06f)
	uint16_t dscSliceWidth;         // pixels per slice: ceil(hActive / dscSlices)
	uint16_t dscSliceHeight;        // lines per slice: the frame, DSC-over-DisplayPort has one vertical slice
};

struct Limits {
	uint32_t maxHActive, maxVActive;   // e.g. boot framebuffer size; 0 = no limit
	// min(TMDS 340000, DISPCLK cap, ...); 0 = no limit. build() already
	// holds table-derived modes to the EDID range-limit clock; folding that
	// in here as well also applies it to the sink's own DTDs.
	uint32_t maxPixelClockKHz;
	// U1: the highest pixel clock a *compressed* timing may reach, from dscCap() — min(DISPCLK the SMU is asked to
	// hold, the slices both ends offer times the sink's per-slice throughput, the link's budget at `dscBpp`). 0 = no
	// DSC mode is offered at all, which is every boot that does not arm rdna4-dscmodes=1 and every sink whose DPCD
	// block does not answer. It never raises maxPixelClockKHz: a timing at or below that cap is offered exactly as
	// before, uncompressed and unflagged.
	uint32_t maxPixelClockDscKHz;
	// The decoded block the two numbers above were computed from (device.cpp's dscCaps), kept for the per-mode slice
	// layout: dscLayout() reads the slice counts, the throughput and the max slice width out of it.
	Dsc::Caps dscSink;
};

// Build a deterministic, deduplicated, filtered table from an EDID blob
// (base block + up to 3 extensions; extensions with a bad checksum or an
// unknown tag are skipped, a bad base block yields 0 modes). Order: the
// native mode first, then descending hActive, vActive, refresh. Returns the
// number of modes written (at most cap). A native mode exists only if the
// preferred timing (or, failing that, a CTA SVD flagged native) survived
// the filters.
//
// With Limits::maxPixelClockDscKHz set, a second pass adds the timings the
// link carries only compressed (Mode::dsc). They come after every mode of
// the uncompressed table and in the same order among themselves, so the IDs
// macOS already holds keep their meaning; a DSC mode is never `native`, and
// a timing at or below the uncompressed cap is never marked DSC.
size_t build(const uint8_t *edid, size_t len, const Limits &lim, Mode *out, size_t cap);

// --- the compressed-mode rule (U1, docs/dsc-modes-u1.md) ---------------------
//
// What the whole-bpp choice is for an 8 bits a component stream: DSC's floor for RGB 4:4:4 is one byte a pixel, the
// same 8 the S2 PPS uses for the 4K60 raster (src/dsc_linux.inc: bits_per_pixel 128 = 8 x 16). A 10 or 12 bpc stream
// would take 12 (S5 of the plan), which is why the choice is a parameter and not a constant inside the arithmetic.
constexpr uint8_t kDscBpp8bpc = 8;

// The three terms of the cap, in kHz, and the cap itself. Zeroed when the sink's block does not offer a slice layout
// or the link or the clock is not known.
struct DscCap {
	uint32_t clockKHz { 0 };       // DISPCLK the SMU is asked to hold: the OTG still generates every pixel
	uint32_t throughputKHz { 0 };  // every slice both ends offer, at the sink's per-slice rate
	uint32_t linkKHz { 0 };        // the link's pixel-clock budget at `bpp`
	uint32_t kKHz { 0 };           // min of the three, 0 = DSC not offered
	uint8_t slices { 0 };          // the slice count the throughput term used (the most both ends offer)
	uint8_t bpp { 0 };
};

// dscCapKHz = min(clockKHz, throughputKHz, linkKHz),
//   clockKHz      = dispclkKHz                                    (device.cpp's dispclkTargetKHz)
//   throughputKHz = slices x sink.throughput0Mps x 1000           (slices from Dsc::usableSlices: 4 on this sink)
//   linkKHz       = linkKHz24 x 24 / bpp                          (linkKHz24 = device.cpp's bootDpLinkKHz)
// Pure: the sink's decoded caps and plain numbers in, numbers out, so the rule is tested without a card
// (test/atomdump.cpp::testDscModes).
DscCap dscCap(const Dsc::Caps &sink, uint32_t linkKHz24, uint32_t dispclkKHz, uint8_t bpp);

// The slice layout one raster would be stamped with, or slices = 0 when the sink's caps cannot carry it at `bpp`: the
// fewest slice count both ends offer whose per-slice throughput covers `pixelClockKHz`, slice width ceil(hActive /
// slices) within the sink's max slice width, and the whole frame in one vertical slice (the sink's DPCD block has no
// vertical slice field, and Linux's DisplayPort DSC config uses one).
struct DscLayout {
	uint8_t slices { 0 };
	uint8_t bpp { 0 };
	uint16_t sliceWidth { 0 };
	uint16_t sliceHeight { 0 };
};

DscLayout dscLayout(const Dsc::Caps &sink, uint32_t hActive, uint32_t vActive, uint32_t pixelClockKHz, uint8_t bpp);

// Index of the mode whose timing matches `t` (same active size and totals,
// pixel clock within 0.5%), or -1.
int match(const Mode *modes, size_t n, const Edid::DetailedTiming &t);

// Insert a timing (e.g. the timing read back from the GOP-programmed OTG)
// if no match exists; returns its index, or -1 if `t` is unusable. A new
// timing is appended with the next free ID so the EDID modes keep theirs;
// if an entry with the same size and whole-Hz rate but a different raster
// exists, `t` replaces its timing in place (ID and native flag kept): the
// running timing is proven on this sink, and adding it would create a
// duplicate. When the table is full the last entry is replaced.
int ensure(Mode *modes, size_t &n, size_t cap, const Edid::DetailedTiming &t, uint8_t source);

// 16.16 refresh for IODisplayModeInformation.
uint32_t refresh1616(const Edid::DetailedTiming &t);

// The sanity rules every table entry passes: progressive, even width,
// non-zero sync pulses, a raster OtgTiming::compute accepts, 1-1000 Hz.
bool usable(const Edid::DetailedTiming &t);

} // namespace Modes

#endif /* Modes_hpp */
