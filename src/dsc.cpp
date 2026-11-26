//
//  dsc.cpp
//  AMDRadeonRDNA4
//
//  See dsc.hpp. The capability decode is Linux's dc_dsc_parse_dsc_dpcd field for field; the register table is Linux's
//  own DCN 4.01 DSC code recorded by tools/gen/pipegen (src/dsc_linux.inc, generated — never edited by hand).
//

#include "dsc.hpp"

namespace Dsc {

#include "dsc_linux.inc"

const Gen *gen(size_t &count) {
	count = sizeof(kGen) / sizeof(kGen[0]);
	return kGen;
}

uint32_t address(const Gen &g, uint8_t dscInst, uint8_t oppInst, uint32_t digOff) {
	switch (g.block) {
	case kDsc:    return g.dword + kDscStride * dscInst;
	case kDscRm:  return g.dword + kDscRmStride * dscInst;
	case kOptc:   return g.dword + kOptcStride * oppInst;
	case kDig:    return g.dword + digOff;
	default:      return g.dword;
	}
}

const uint32_t *ppsWords() { return kPps; }
uint16_t picWidth() { return kPicWidth; }
uint16_t picHeight() { return kPicHeight; }
uint8_t  slices() { return kSlices; }
uint16_t sliceWidth() { return kSliceWidth; }
uint8_t  bitsPerPixel() { return kBitsPerPixel; }
uint8_t  bitsPerComponent() { return kBitsPerComponent; }

// ---- the sink's side ----

namespace {

// 0x06b bits[3:0] / [7:4], Linux's dsc_throughput_from_dpcd: code 0 unsupported, 1 = 340, 2 = 400, ...
// 14 = 1000 and 15 = 170 Mpix/s a slice.
uint16_t throughput(uint8_t code) {
	static const uint16_t kMps[14] = { 340, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000 };

	if (code == 0)
		return 0;
	if (code == 15)
		return 170;
	return code <= 14 ? kMps[code - 1] : 0;
}

} // namespace

Caps decode(const uint8_t raw[kDpcdCapBytes], bool blockRead, bool enableRead, uint8_t enable) {
	Caps c;

	if (blockRead)
		for (uint32_t i = 0; i < kDpcdCapBytes; i++)
			c.raw[i] = raw[i];

	c.read = blockRead;
	c.enableRead = enableRead;
	c.enable = enable;
	if (!blockRead)
		return c;

	c.supported = (raw[0] & 0x01) != 0;       // DP_DSC_DECOMPRESSION_IS_SUPPORTED
	c.passthrough = (raw[0] & 0x02) != 0;
	c.dynamicPps = (raw[0] & 0x0c) != 0;
	c.version = raw[1];

	switch (raw[2] & 0x03) {                 // DP_DSC_RC_BUF_BLK_SIZE: 1, 4, 16 or 64 KiB
	case 0: c.rcBlockKiB = 1; break;
	case 1: c.rcBlockKiB = 4; break;
	case 2: c.rcBlockKiB = 16; break;
	case 3: c.rcBlockKiB = 64; break;
	default: break;
	}
	if (c.rcBlockKiB)
		c.rcBufferBytes = (static_cast<uint32_t>(raw[3]) + 1) * c.rcBlockKiB * 1024;

	c.slices1 = raw[4];

	const uint8_t lb = raw[5] & 0x0f;         // Linux: 0..7 -> 9..16 bits, 8 -> 8 bits
	if (lb <= 7)
		c.lineBufBpc = static_cast<uint8_t>(lb + 9);
	else if (lb == 8)
		c.lineBufBpc = 8;

	c.blockPred = (raw[6] & 0x01) != 0;
	c.rgbConvBypass = (raw[6] & 0x02) != 0;
	c.edpMaxBppX16 = static_cast<uint16_t>(raw[7] | (raw[8] << 8));
	c.colorFormats = raw[9];
	c.colorDepths = raw[10];

	c.throughput0Mps = throughput(raw[11] & 0x0f);
	c.throughput0Mps = static_cast<uint16_t>(c.throughput0Mps + ((raw[2] >> 3) * 2));
	c.throughput1Mps = throughput((raw[11] >> 4) & 0x0f);
	c.maxSliceWidth = static_cast<uint16_t>(raw[12] * 320);   // DP_DSC_SLICE_WIDTH_MULTIPLIER
	c.slices2 = raw[13];
	c.bppDeltaVersion = raw[14];

	switch (raw[15] & 0x07) {                  // DP_DSC_BITS_PER_PIXEL_INC
	case 0: c.bppIncrementDiv = 16; break;
	case 1: c.bppIncrementDiv = 8; break;
	case 2: c.bppIncrementDiv = 4; break;
	case 3: c.bppIncrementDiv = 2; break;
	case 4: c.bppIncrementDiv = 1; break;
	default: break;
	}
	return c;
}

namespace {

// Freestanding (the kext builds with -nostdinc): the two short strings below are written by hand.
char *putChar(char *p, char *end, char c) {
	if (p < end)
		*p++ = c;
	return p;
}

char *putUInt(char *p, char *end, uint32_t v) {
	char digits[4];
	uint32_t n = 0;

	if (!v)
		digits[n++] = '0';
	while (v && n < sizeof(digits)) {
		digits[n++] = static_cast<char>('0' + v % 10);
		v /= 10;
	}
	while (n)
		p = putChar(p, end, digits[--n]);
	return p;
}

void terminate(char *buf, char *p, char *end) {
	if (p < end)
		*p = 0;
	else if (end > buf)
		end[-1] = 0;
}

} // namespace

const char *versionText(const Caps &c, char *buf, size_t cap) {
	char *end = buf + cap;

	if (!c.read) {
		const char *unread = "unread";
		char *p = buf;
		for (const char *s = unread; *s; s++)
			p = putChar(p, end - 1, *s);
		terminate(buf, p, end);
		return buf;
	}
	char *p = putChar(buf, end - 1, 'v');
	p = putUInt(p, end - 1, c.major());
	p = putChar(p, end - 1, '.');
	p = putUInt(p, end - 1, c.minor());
	terminate(buf, p, end);
	return buf;
}

const char *slicesText(const Caps &c, char *buf, size_t cap) {
	static const uint8_t kBit[8] = { 1, 2, 0, 4, 6, 8, 10, 12 };
	char *end = buf + cap;
	char *p = putChar(buf, end - 1, '{');
	uint32_t n = 0;

	for (uint32_t b = 0; b < 8; b++) {
		if (!(c.slices1 & (1u << b)) || !kBit[b])
			continue;
		if (n++)
			p = putChar(p, end - 1, ',');
		p = putUInt(p, end - 1, kBit[b]);
	}
	p = putChar(p, end - 1, '}');
	terminate(buf, p, end);
	return buf;
}

uint8_t usableSlices(const Caps &c) {
	uint8_t m = 0;
	for (uint32_t b = 0; b < 8; b++) {
		static const uint8_t kBit[8] = { 1, 2, 0, 4, 6, 8, 10, 12 };
		if ((c.slices1 & (1u << b)) && kBit[b] && kBit[b] <= kEncoderSlices)
			m |= static_cast<uint8_t>(1u << b);
	}
	return m;
}

Need need(const Caps &c, uint32_t pixelKHz, uint16_t bpp, uint16_t linkPayloadMbps) {
	Need n;

	n.pixelKHz = pixelKHz;
	n.linkMbpsNeeded = static_cast<uint32_t>((static_cast<uint64_t>(pixelKHz) * bpp) / 1000);
	n.linkFits = n.linkMbpsNeeded && n.linkMbpsNeeded <= linkPayloadMbps;
	if (!c.read || !c.throughput0Mps)
		return n;
	// Linux: total_sink_throughput = max_slices * throughput_mode_0_mps, and a slice may not clock
	// faster than one slice's throughput. The smallest slice count that covers the raster is the answer; if the
	// sink (or the encoder) does not offer it, say how many would be needed and that it is not offered.
	const uint32_t perSlice = static_cast<uint32_t>(c.throughput0Mps) * 1000;
	static const uint8_t kCounts[3] = { 1, 2, 4 };
	static const uint8_t kBit[3] = { 0, 1, 3 };
	const uint32_t required = (pixelKHz + perSlice - 1) / perSlice;

	n.slices = required ? required : 1;
	for (uint32_t i = 0; i < 3; i++) {
		if (kCounts[i] < required || !(usableSlices(c) & (1u << kBit[i])))
			continue;
		n.slices = kCounts[i];
		n.slicesOffered = true;
		break;
	}
	return n;
}

} // namespace Dsc
