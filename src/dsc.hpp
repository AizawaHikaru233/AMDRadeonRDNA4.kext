//
//  dsc.hpp
//  AMDRadeonRDNA4
//
//  Display Stream Compression (DSC): what the sink says it can do (DPCD 0x060..0x06f, DP_DSC_ENABLE 0x160), and what
//  a raster would need from it. The fields and their arithmetic are Linux's (include/drm/display/drm_dp.h and
//  drivers/gpu/drm/amd/display/dc/dsc/dc_dsc.c, dc_dsc_parse_dsc_dpcd), so the driver's decode can be held against
//  the same numbers amdgpu computes.
//
//  Freestanding (no IOKit): shared by the kext and test/atomdump.cpp. The register side lives in src/dsc_linux.inc
//  (recorded by tools/gen/pipegen from Linux's own DCN 4.01 DSC code) and is applied by RDNA4Device.
//

#ifndef Dsc_hpp
#define Dsc_hpp

#include <stddef.h>
#include <stdint.h>

namespace Dsc {

// DP_DSC_ENABLE (DPCD 0x160), DP 1.4: bit 0 is the only one a DP sink needs.
constexpr uint32_t kDpcdEnable      = 0x160;
constexpr uint32_t kDpcdSupport     = 0x060;
constexpr uint32_t kDpcdCapBytes    = 16;

// What this project's DCN 4.01 encoder offers (dcn401_dsc.c, dsc401_get_single_enc_caps): slices 1, 2, 3, 4; line
// buffer 13 bpc; block prediction; RGB/YCbCr; 8/10/12 bpc; DSC 1.2.
constexpr uint8_t kEncoderVersion   = 0x21;   // v1.2, the same byte the sink reports for DSC 1.2
constexpr uint8_t kEncoderSlices    = 4;      // the most slices per line the encoder can be told
constexpr uint8_t kEncoderLineBuf   = 13;     // bits per component
constexpr uint16_t kLinkPayloadMbps = 17280;  // 4 lanes HBR2 after 8b/10b: the boot link's payload

// The sink's capability block, decoded as dc_dsc_parse_dsc_dpcd does.
struct Caps {
	uint8_t  raw[kDpcdCapBytes] {};   // 0x060..0x06f as read
	bool     read { false };          // the block answered on AUX
	bool     supported { false };     // 0x060 bit 0, decompression
	bool     passthrough { false };   // 0x060 bit 1
	bool     dynamicPps { false };    // 0x060 bit 2 or 3
	uint8_t  version { 0 };           // 0x061: minor << 4 | major
	uint8_t  rcBlockKiB { 0 };        // 0x062 bits[1:0]: 1, 4, 16 or 64 KiB blocks; 0 not a value Linux knows
	uint32_t rcBufferBytes { 0 };     // (0x063 + 1) * block size
	uint8_t  slices1 { 0 };           // 0x064 raw: bit0 1, bit1 2, bit3 4, bit4 6, bit5 8, bit6 10, bit7 12
	uint8_t  lineBufBpc { 0 };        // 0x065 bits[3:0] -> 9..16, 8 at 8; 0 not a value Linux knows
	bool     blockPred { false };     // 0x066 bit 0
	bool     rgbConvBypass { false }; // 0x066 bit 1
	uint16_t edpMaxBppX16 { 0 };      // 0x067 | 0x068 << 8 (eDP only; 0 = not declared)
	uint8_t  colorFormats { 0 };      // 0x069 raw: RGB bit0, YCbCr444 bit1, 422 simple bit2, 422 native bit3, 420 bit4
	uint8_t  colorDepths { 0 };       // 0x06a raw: 8 bpc bit1, 10 bit2, 12 bit3
	uint16_t throughput0Mps { 0 };    // 0x06b bits[3:0], per slice (+ 0x062 bits[7:3] * 2)
	uint16_t throughput1Mps { 0 };    // 0x06b bits[7:4], per slice
	uint16_t maxSliceWidth { 0 };     // 0x06c * 320 px
	uint8_t  slices2 { 0 };           // 0x06d raw (16/20/24)
	uint8_t  bppDeltaVersion { 0 };   // 0x06e raw
	uint8_t  bppIncrementDiv { 0 };   // 0x06f bits[2:0]: 16, 8, 4, 2, 1; 0 not a value Linux knows
	bool     enableRead { false };    // 0x160 answered
	uint8_t  enable { 0 };            // 0x160 raw (bit 0 = DP_DECOMPRESSION_EN)

	uint8_t major() const { return version & 0xf; }
	uint8_t minor() const { return (version >> 4) & 0xf; }
};

// Decode the DPCD block `raw` (16 bytes at 0x060). `enable`/`enableRead` are the separate 0x160 read.
Caps decode(const uint8_t raw[kDpcdCapBytes], bool blockRead, bool enableRead, uint8_t enable);

// The slice counts, 1/2/4/6/8/10/12, in `buf` as "{1,2,4,6,8}" (or "{}"); 0 for a count, not a bit.
const char *slicesText(const Caps &c, char *buf, size_t cap);

// "v1.2" (or "v1.0" / "unread"), from 0x061.
const char *versionText(const Caps &c, char *buf, size_t cap);

// The slice counts both sides offer: the sink's 1/2/4/6/8/10/12 against the encoder's 1..4.
uint8_t usableSlices(const Caps &c);

// What one raster needs from a sink decoded above: the slices its throughput covers, and whether the link carries
// `bpp` bits a pixel at `linkMbps` payload (the plan's "4K120 needs 4 slices").
struct Need {
	uint32_t pixelKHz { 0 };      // the raster's pixel rate
	uint32_t slices { 0 };        // per-slice throughput needed (0: no answer)
	bool     slicesOffered { false };   // the sink offers that many and the encoder can be told it
	bool     linkFits { false };        // pixel rate * bpp <= the link's payload
	uint32_t linkMbpsNeeded { 0 };      // pixelKHz * bpp / 1000
};
Need need(const Caps &c, uint32_t pixelKHz, uint16_t bpp, uint16_t linkPayloadMbps = kLinkPayloadMbps);

// The 4K120 raster the plan works towards (3840x2160@120: 1066.56 MHz), and the line the S0 log prints:
// "4K120 (1066.56 MHz): 4 slices, ..." — kept here so the test and the kext state the same verdict.
constexpr uint32_t kFourK120KHz = 1066560;

// ---- the register table recorded from Linux (src/dsc_linux.inc, tools/gen/pipegen/mkdsc.py) ----
//
// One step each. `block` says which instance stride the driver adds to `dword` (which is instance 0's):
enum Block : uint8_t { kFixed = 0, kDsc = 1, kDscRm = 2, kOptc = 3, kDig = 4 };
constexpr uint32_t kDscStride = 0x5c;    // DSC_TOP/DSCC/DSCCIF per instance
constexpr uint32_t kDscRmStride = 1;     // one dword per DSC instance
constexpr uint32_t kOptcStride = 0x10;   // ODM/OPTC per OPP
constexpr uint32_t kDigStride = 0x124;   // DP*/VPG*/AFMT* per DIG
struct Gen {
	char        op;      // 'W' write, 'U' update, 'T' wait until (reg & mask) == value (arg us), 'D' delay (arg us),
	                     // 'A' DPCD write of `value` at `dword`
	uint8_t     block;
	uint8_t     seg;
	uint16_t    dword;
	uint32_t    mask, value, arg;
	const char *what;
};

// The table and its metadata (3840x2160, 2 slices of 1920, 8 bpp, RGB 8 bpc).
const Gen *gen(size_t &count);
uint32_t address(const Gen &g, uint8_t dscInst, uint8_t oppInst, uint32_t digOff);
const uint32_t *ppsWords();      // the 23 DSCC_PPS_CONFIG words, in order
uint16_t picWidth();
uint16_t picHeight();
uint8_t  slices();
uint16_t sliceWidth();
uint8_t  bitsPerPixel();
uint8_t  bitsPerComponent();

} // namespace Dsc

#endif /* Dsc_hpp */
