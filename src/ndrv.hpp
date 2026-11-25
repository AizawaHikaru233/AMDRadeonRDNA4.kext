//
//  ndrv.hpp
//  AMDRadeonRDNA4
//
//  Answers Apple's IONDRVFramebuffer the way a display NDRV would. On a GPU
//  without a macOS driver, IONDRVFramebuffer drives the GOP framebuffer
//  through IOBootNDRV, which knows one fixed mode (ID 100) and nothing about
//  the monitor. The plugin (plugin.cpp) hands the csc Status/Control
//  requests for our framebuffer to a Translator first; it answers the mode
//  list, timings, EDID, connection and DPMS requests from the AMDRadeonRDNA4 mode
//  table and the sink's EDID, and everything it does not handle (CLUT, power
//  state, cursor, ...) stays with IOBootNDRV. The gamma selectors (4, 8, 20, 21)
//  are answered only when rdna4-gamma arms them: without them
//  IONDRVFramebuffer::checkDriver publishes the "no gamma" defaults 8 / 256, and
//  CoreDisplay then refuses the plane any HDR capability
//  (docs/hdr-gamma.md, docs/hdr-userspace-gating.md §1.4).
//
//  Mode IDs: the boot mode keeps IOBootNDRV's 100 (IONDRVFramebuffer may
//  have cached it before the first request reached us); every other table
//  entry gets 100 + its Modes::Mode::id. They stay below 0x80000000, which
//  Apple reserves for programmable (detailed-timing) modes. There is one
//  depth, kDepthMode1 = 32 bpp direct; rdna4-depth2=1 adds kDepthMode2 as a second one (docs/hdr-30bpp-plan.md).
//
//  The csc records are mirrored locally (the SDK's IOMacOSVideo.h needs the
//  kernel headers). IOMacOSVideo.h selects mac68k alignment only for 32-bit
//  builds; on x86_64 the records are naturally aligned, and plugin.cpp
//  static_asserts every mirror against the SDK type. Freestanding: shared by
//  the kext and the host test harness (LP64 hosts only).
//

#ifndef Ndrv_hpp
#define Ndrv_hpp

#include <stddef.h>
#include <stdint.h>

#include "modes.hpp"

namespace Ndrv {

// csc selectors (IOMacOSVideo.h).
enum : uint16_t {
	// Status
	cscGetGamma           = 8,
	cscGetCurMode         = 10,
	cscGetSync            = 11,
	cscGetConnection      = 12,
	cscGetModeTiming      = 13,
	cscGetNextResolution  = 17,
	cscGetVideoParameters = 18,
	cscGetGammaInfoList   = 20,
	cscRetrieveGammaTable = 21,
	cscSupportsHardwareCursor = 22,
	cscGetHardwareCursorDrawState = 23,
	cscGetDDCBlock        = 27,
	cscGetDetailedTiming  = 31,
	// Control
	cscSetEntries         = 3,
	cscSetGamma           = 4,
	cscSwitchMode         = 10,
	cscSetSync            = 11,
	cscSetHardwareCursor  = 22,
	cscDrawHardwareCursor = 23,
};

// IOReturn values used here.
enum : int32_t {
	kSuccess     = 0,
	kBadArgument = static_cast<int32_t>(0xe00002c2),
	kUnsupported = static_cast<int32_t>(0xe00002c7),
};

constexpr int32_t  kBootModeId                = 100;   // IOBootNDRV's kIOBootNDRVDisplayMode
constexpr int32_t  kDisplayModeIDCurrent      = 0;
constexpr int32_t  kDisplayModeIDInvalid      = -1;    // 0xFFFFFFFF
constexpr int32_t  kDisplayModeIDFindFirst    = -2;    // kDisplayModeIDFindFirstResolution
constexpr int32_t  kDisplayModeIDNoMore       = -3;    // kDisplayModeIDNoMoreResolutions
constexpr uint16_t kDepthMode1                = 128;
constexpr uint16_t kDepthMode2                = 129;   // the second depth rdna4-depth2=1 offers
constexpr uint32_t kDeclROMtables             = 0x6465636c;   // 'decl'
constexpr uint32_t kTimingInvalid             = 0;
constexpr uint32_t kDisplayModeValidFlag      = 0x1;
constexpr uint32_t kDisplayModeSafeFlag       = 0x2;
constexpr uint32_t kDisplayModeDefaultFlag    = 0x4;
constexpr int16_t  kRGBDirectPixels           = 2;     // kIORGBDirectPixels
constexpr uint16_t kGenericLCD                = 20;
constexpr uint32_t kHasDirectConnection       = 1u << 3;
constexpr uint32_t kReportsDDCConnection      = 1u << 7;
constexpr uint32_t kHasDDCConnection          = 1u << 8;
constexpr uint32_t kConnectionInactive        = 1u << 9;    // nothing connected: IONDRVFramebuffer goes offline
constexpr uint32_t kDDCBlockTypeEDID          = 0;
constexpr uint32_t kSyncPositivePolarityMask  = 1;
constexpr uint32_t kDMSModeReady              = 0;
constexpr uint8_t  kDPMSSyncOn                = 0;
constexpr uint8_t  kDPMSSyncOff               = 7;
constexpr uint8_t  kDPMSSyncMask              = 7;     // H, V, composite sync
constexpr uint8_t  kSyncDisableHV             = 3;     // kHorizontalSyncMask | kVerticalSyncMask

// --- csc records (LP64 layout) ----------------------------------------------
static_assert(sizeof(void *) == 8, "csc record mirrors assume LP64");

struct Rect {
	int16_t top, left, bottom, right;
};

struct VPBlock {
	uint32_t vpBaseOffset;
	uint32_t vpRowBytes;          // UInt32 on LP64
	Rect     vpBounds;
	int16_t  vpVersion;
	int16_t  vpPackType;
	uint32_t vpPackSize;
	uint32_t vpHRes;
	uint32_t vpVRes;
	int16_t  vpPixelType;
	int16_t  vpPixelSize;
	int16_t  vpCmpCount;
	int16_t  vpCmpSize;
	uint32_t vpPlaneBytes;
};

struct VDSwitchInfoRec {
	uint16_t  csMode;             // depth mode
	int32_t   csData;             // display mode ID
	uint16_t  csPage;
	uintptr_t csBaseAddr;         // 1 | physical address
	uintptr_t csReserved;
};

struct VDTimingInfoRec {
	int32_t   csTimingMode;
	uintptr_t csTimingReserved;
	uint32_t  csTimingFormat;
	uint32_t  csTimingData;
	uint32_t  csTimingFlags;
};

struct VDDisplayConnectInfoRec {
	uint16_t  csDisplayType;
	uint8_t   csConnectTaggedType;
	uint8_t   csConnectTaggedData;
	uint32_t  csConnectFlags;
	uintptr_t csDisplayComponent;
	uintptr_t csConnectReserved;
};

struct VDSyncInfoRec {
	uint8_t csMode;
	uint8_t csFlags;
};

struct VDResolutionInfoRec {
	int32_t   csPreviousDisplayModeID;
	int32_t   csDisplayModeID;
	uint32_t  csHorizontalPixels;
	uint32_t  csVerticalLines;
	uint32_t  csRefreshRate;      // Fixed 16.16 Hz
	uint16_t  csMaxDepthMode;
	uint32_t  csResolutionFlags;
	uintptr_t csReserved;
};

struct VDVideoParametersInfoRec {
	int32_t   csDisplayModeID;
	uint16_t  csDepthMode;
	VPBlock  *csVPBlockPtr;
	uint32_t  csPageCount;
	uint32_t  csDeviceType;
	uint32_t  csDepthFlags;
};

struct VDDDCBlockRec {
	uint32_t ddcBlockNumber;      // 1-based
	uint32_t ddcBlockType;
	uint32_t ddcFlags;
	uint32_t ddcReserved;
	uint8_t  ddcBlockData[128];
};

struct VDDetailedTimingRec {
	uint32_t csTimingSize;
	uint32_t csTimingType;
	uint32_t csTimingVersion;
	uint32_t csTimingReserved;
	int32_t  csDisplayModeID;
	uint32_t csDisplayModeSeed;
	uint32_t csDisplayModeState;
	uint32_t csDisplayModeAlias;
	uint32_t csSignalConfig;
	uint32_t csSignalLevels;
	uint64_t csPixelClock;        // Hz
	uint64_t csMinPixelClock;
	uint64_t csMaxPixelClock;
	uint32_t csHorizontalActive;
	uint32_t csHorizontalBlanking;
	uint32_t csHorizontalSyncOffset;
	uint32_t csHorizontalSyncPulseWidth;
	uint32_t csVerticalActive;
	uint32_t csVerticalBlanking;
	uint32_t csVerticalSyncOffset;
	uint32_t csVerticalSyncPulseWidth;
	uint32_t csHorizontalBorderLeft;
	uint32_t csHorizontalBorderRight;
	uint32_t csVerticalBorderTop;
	uint32_t csVerticalBorderBottom;
	uint32_t csHorizontalSyncConfig;
	uint32_t csHorizontalSyncLevel;
	uint32_t csVerticalSyncConfig;
	uint32_t csVerticalSyncLevel;
	uint32_t csNumLinks;
	uint32_t csReserved2, csReserved3, csReserved4;
	uint32_t csReserved5, csReserved6, csReserved7, csReserved8;
};

// IOMacOSVideo.h cursor csc records (Apple IOGraphics).
struct VDSetHardwareCursorRec {
	void    *csCursorRef;
	uint32_t csReserved1;
	uint32_t csReserved2;
};

struct VDDrawHardwareCursorRec {
	int32_t  csCursorX;
	int32_t  csCursorY;
	uint32_t csCursorVisible;
	uint32_t csReserved1;
	uint32_t csReserved2;
};

struct VDSupportsHardwareCursorRec {
	uint32_t csSupportsHardwareCursor;
	uint32_t csReserved1;
	uint32_t csReserved2;
};

struct VDHardwareCursorDrawStateRec {
	int32_t  csCursorX;
	int32_t  csCursorY;
	uint32_t csCursorVisible;
	uint32_t csCursorSet;
	uint32_t csReserved1;
	uint32_t csReserved2;
};

// --- gamma (docs/hdr-gamma.md) -----------------------------------------------
//
// IONDRVFramebuffer::checkDriver (0x1475c7de in the 26.6.2 System KC) asks
// cscGetGamma (8) for a GammaTbl and reads its gDataCnt and gDataWidth into
// IOFBGammaCount and IOFBGammaWidth; without one it publishes the hard-coded
// defaults 256 and 8 (the branch at 0x1475c9b8), which is why the plane
// reports no gamma. It then enumerates tables with cscGetGammaInfoList (20) and
// fetches them with cscRetrieveGammaTable (21), keeping the widest table it is
// offered. cscSetGamma (4) is what IONDRVFramebuffer::setGammaTable sends, with
// a GammaTbl the kernel has already laid out in front of the entries.

// IOMacOSTypes.h GammaTbl: the six 16-bit fields, then gFormulaData[1].
struct GammaTbl {
	int16_t gVersion;
	int16_t gType;
	int16_t gFormulaSize;
	int16_t gChanCnt;
	int16_t gDataCnt;      // values per channel
	int16_t gDataWidth;    // bits per value (8, 10, 12, 16)
	// int16_t gFormulaData[]; the values follow the header.
};

// IOMacOSVideo.h VDGammaRecord: the parm block of cscGetGamma/cscSetGamma.
struct VDGammaRecord {
	void *csGTable;        // GammaTbl * (in and out)
};

// IOMacOSVideo.h VDGetGammaListRec: the parm block of cscGetGammaInfoList.
struct VDGetGammaListRec {
	uint32_t csPreviousGammaTableID;   // in: kGammaTableIDFindFirst, then the previous answer
	uint32_t csGammaTableID;           // out: the next table, or kGammaTableIDNoMoreTables
	uint32_t csGammaTableSize;         // out: bytes the retrieve must copy
	char    *csGammaTableName;         // out: a name (not written by this driver)
};

// IOMacOSVideo.h VDRetrieveGammaRec: the parm block of cscRetrieveGammaTable.
struct VDRetrieveGammaRec {
	uint32_t csGammaTableID;
	void    *csGammaTablePtr;          // GammaTbl * into a buffer of csGammaTableSize bytes
};

// IOMacOSVideo.h GammaTableID values.
constexpr uint32_t kGammaTableIDFindFirst    = 0xFFFFFFFE;
constexpr uint32_t kGammaTableIDNoMoreTables = 0xFFFFFFFD;
constexpr uint32_t kGammaTableIDSpecific     = 0;

constexpr uint16_t kGammaVersion   = 0;   // what IONDRVFramebuffer::setGammaTable writes
constexpr uint16_t kGammaHeaderSize = 12; // sizeof(GammaTbl); IOFBGammaHeaderSize
// The table this driver offers: a linear (identity) ramp, 10 bits, 1024 values a
// channel, three channels — 2 bytes a value, so 12 + 3 * 1024 * 2 = 6156 bytes.
constexpr uint16_t kGammaChannels  = 3;
constexpr uint16_t kGammaEntries   = 1024;
constexpr uint16_t kGammaDataWidth = 10;
constexpr size_t   kGammaTableBytes =
	kGammaHeaderSize + static_cast<size_t>(kGammaChannels) * kGammaEntries * 2;

// One line per gamma selector (rdna4-gamma=1). `ev` names the selector and says
// what the four numbers are; plugin.cpp formats the line, so no format string
// leaves the kernel build (a %s fed a number is a panic, AGENTS.md §一).
enum class GammaEvent : uint8_t {
	Set,        // a = gChanCnt, b = gDataCnt, c = gDataWidth, d = gFormulaSize
	Get,        // a = bytes the table has, b = 1 if macOS set it, else 0
	List,       // a = previous ID, b = returned ID, c = size
	Retrieve,   // a = requested ID, b = bytes copied
};


// --- backend -----------------------------------------------------------------

// The memory macOS draws into for one mode.
struct Surface {
	uint64_t physBase;
	uint32_t rowBytes;
	uint32_t width, height;
};

// What the hardware side provides. `isBoot` marks the mode the GOP lit.
struct Backend {
	void *ctx;
	// Surface for `m`; false = the mode cannot be displayed.
	bool (*surfaceFor)(void *ctx, const Modes::Mode &m, bool isBoot, Surface &out);
	// Program `m` (an IOReturn value; the current mode stays on failure).
	int32_t (*switchTo)(void *ctx, const Modes::Mode &m, bool isBoot);
	// DPMS: display on or blanked.
	void (*setPower)(void *ctx, bool on);
	bool (*supportsHardwareCursor)(void *ctx);
	int32_t (*setHardwareCursor)(void *ctx, void *cursorRef);
	int32_t (*drawHardwareCursor)(void *ctx, int32_t x, int32_t y, uint32_t visible);
	int32_t (*getHardwareCursorDrawState)(void *ctx, VDHardwareCursorDrawStateRec &state);
	// The depth a switch selected (kDepthMode1 = 128, kDepthMode2 = 129): the plane's pixel format follows it.
	void (*setScanoutDepth)(void *ctx, uint16_t depth);
	// The mode a switch selected is one of the HDR10 variants (Mode::hdr10): the plane reads ARGB2101010 while it is
	// live, whatever the OS depth. Called after the switch succeeded, so a refused mode leaves the old colour alone.
	void (*setHdrMode)(void *ctx, bool on);
	// rdna4-gamma=1: one line per gamma selector, with its arguments and the verdict. Null = no logging (the host
	// test's backends leave it out).
	void (*gammaLog)(void *ctx, GammaEvent ev, int32_t a, int32_t b, int32_t c, int32_t d, int32_t ret);
};

class Translator {
public:
	// `modes` (count entries, bootModeId = Modes::Mode::id of the live
	// mode) and `edid` must outlive the Translator. count 0 leaves every
	// mode request to IOBootNDRV; edidLen 0 leaves the DDC request to it.
	void init(const Modes::Mode *modes, size_t count, uint32_t bootModeId,
	          const uint8_t *edid, size_t edidLen, const Backend &backend);

	// Handle one csc Status/Control request. Returns false if the request
	// is not ours to answer (the caller then passes it on to IOBootNDRV);
	// otherwise `ret` holds the IOReturn for IONDRVFramebuffer.
	bool status(uint16_t code, void *params, int32_t &ret);
	bool control(uint16_t code, void *params, int32_t &ret);

	int32_t currentModeId() const { return current; }

	// Whether a display is on the connector. Without one cscGetConnection
	// says so and there is no DDC block: macOS takes the framebuffer offline
	// until a connect interrupt makes it ask again.
	void setConnected(bool on) { connected = on; }
	// A second depth (30bpp). Off unless a test boot arms it: rdna4-depth2=1.
	void setDepth2(bool on) { depth2 = on; }
	size_t  modeCount() const { return count; }
	// NDRV mode ID of table entry `m`.
	int32_t idFor(const Modes::Mode &m) const;
	// The table entry an NDRV mode ID names, or nullptr (the HDR10 colour hook, plugin.cpp).
	const Modes::Mode *modeForDisplayId(int32_t id) const { return modeFor(id); }
	// The table grew at run time (debug.rdna4_hdr appended the HDR10 variants): serve the entries up to `n`. Never
	// shrinks — the IDs macOS already holds must stay answered.
	void setModeCount(size_t n);
	// rdna4-gamma=1 / debug.rdna4_gamma: answer cscSetGamma (4), cscGetGamma (8), cscGetGammaInfoList (20) and
	// cscRetrieveGammaTable (21). 0 = off (the selectors stay with IOBootNDRV), 1 = a 10 bit / 1024 entry ramp,
	// 2 = a 12 bit / 256 entry ramp (the A/B of docs/hdr-gamma.md §5). Off at boot unless armed, so a failure cannot
	// change the shipped behaviour.
	void setGammaOption(uint32_t level);
	uint32_t gammaOption() const { return gammaLevel; }

private:
	const Modes::Mode *modes { nullptr };
	size_t   count { 0 };
	uint32_t bootModeId { 0 };
	const uint8_t *edid { nullptr };
	size_t   edidLen { 0 };
	Backend  be {};
	int32_t  current { kBootModeId };
	uint8_t  syncState { kDPMSSyncOn };
	bool     connected { true };
	bool     depth2 { false };
	uint8_t  currentDepth { kDepthMode1 };
	// The gamma table this driver serves: the linear ramp until macOS sends one of its own (cscSetGamma), that one
	// after. cscGetGamma, cscGetGammaInfoList and cscRetrieveGammaTable all describe this one table.
	bool     gamma { false };
	bool     gammaFromMacOS { false };
	uint16_t gammaLen { 0 };        // bytes the table occupies (header + values)
	uint16_t gammaListSize { 0 };   // what cscGetGammaInfoList last advertised, which bounds the cscRetrieveGammaTable copy
	uint32_t gammaLevel { 0 };      // rdna4-gamma: 0 = off, 1 = 10 bit / 1024, 2 = 12 bit / 256
	uint16_t gammaEntries { kGammaEntries };
	uint16_t gammaDataWidth { kGammaDataWidth };
	// Never allocated, never resized: laid out in setGammaOption, read by the kernel as a GammaTbl, so it is
	// aligned for the 16-bit header fields.
	alignas(8) uint8_t gammaTable[kGammaTableBytes] {};

	const Modes::Mode *modeFor(int32_t id) const;
	bool surface(const Modes::Mode &m, Surface &out) const;

	int32_t getCurMode(VDSwitchInfoRec &r) const;
	int32_t getNextResolution(VDResolutionInfoRec &r) const;
	int32_t getVideoParameters(VDVideoParametersInfoRec &r) const;
	int32_t getModeTiming(VDTimingInfoRec &r) const;
	int32_t getDetailedTiming(VDDetailedTimingRec &r) const;
	int32_t getConnection(VDDisplayConnectInfoRec &r) const;
	int32_t getDDCBlock(VDDDCBlockRec &r) const;
	int32_t getSync(VDSyncInfoRec &r) const;
	int32_t switchMode(VDSwitchInfoRec &r);
	int32_t setSync(const VDSyncInfoRec &r);

	// gamma (docs/hdr-gamma.md)
	void    buildGammaRamp();
	int32_t gammaGet(VDGammaRecord &r) const;
	int32_t gammaList(VDGetGammaListRec &r);
	int32_t gammaRetrieve(VDRetrieveGammaRec &r) const;
	int32_t gammaSet(const VDGammaRecord &r);
	void    gammaEvent(GammaEvent ev, int32_t a, int32_t b, int32_t c, int32_t d, int32_t ret) const {
		if (be.gammaLog)
			be.gammaLog(be.ctx, ev, a, b, c, d, ret);
	}
	GammaTbl *gammaHeader() const {
		return const_cast<GammaTbl *>(reinterpret_cast<const GammaTbl *>(gammaTable));
	}
};

// --- a head with no IOBootNDRV behind it (docs/second-head-ndrv.md) ----------

// What IOBootNDRV answers to a csc request the Translator leaves alone: the
// CLUT and gamma writes succeed and do nothing, everything else is
// unsupported (IOBootNDRV::doControl / doStatus).
int32_t bootReply(bool isStatus, uint16_t code);

// Place a width x height, 32 bpp surface for a second head in the memory
// range [rangeBase, rangeBase + rangeLen) that holds the console framebuffer
// [fbBase, fbBase + fbLen): 1 MiB aligned and at least 1 MiB past the
// console's end (the cursor sprite sits right behind it), with the 128 bytes
// IONDRVFramebuffer::getApertureRange adds. False if it does not fit.
bool spareSurface(uint64_t fbBase, uint64_t fbLen, uint64_t rangeBase, uint64_t rangeLen,
                  uint32_t width, uint32_t height, Surface &out);

} // namespace Ndrv

// Opaque bridges to IONDRVFramebuffer's VSL services. The service is made
// by Apple's VSLNewInterruptService; W1 only invokes VSLDoInterruptService
// from its deferred IH work-loop action.
namespace Ndrv {
using VslDoInterruptService = int32_t (*)(void *service);
using VslPrepareCursor = bool (*)(void *cursorRef, void *descriptor, void *info);
void vslInit();
void vslServiceCreated(void *service, VslDoInterruptService doService);
bool vslServiceDisposed(void *service);
bool vslServicePresent();
void vslPrepareCursorInstalled(VslPrepareCursor prepare);
void setVblankEnabled(bool enabled);
void signalVblank();
bool prepareCursor(void *cursorRef, void *descriptor, void *info);
}

#endif /* Ndrv_hpp */
