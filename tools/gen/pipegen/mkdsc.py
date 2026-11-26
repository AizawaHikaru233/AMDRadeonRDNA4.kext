#!/usr/bin/env python3
"""mkdsc.py <dcn_4_1_0_offset.h> <pipegen trace> <linux commit> > src/dsc_linux.inc

Turns the DSC part of a pipegen trace into the table src/dsc.cpp applies: the PPS SDP the stream encoder sends, the
DSCCLK DTO, the DSC block's PPS registers and forward enable, and OPTC's three DSC registers. Every step is one
Linux's own DCN 4.01 code made while pipegen ran its `dsc N BPP` scenario; nothing here is worked out by hand.

Only the registers of the DSC chain are accepted. Each entry names the block it belongs to (DSC, DSCRM, OPTC, DIG,
fixed) so the driver can add the instance stride of the pipe it enables.
"""
import re, sys, collections

hdr, trace, commit = sys.argv[1:4]

base, off = {}, {}
for line in open(hdr):
    m = re.match(r'#define (reg\w+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)\s*$', line)
    if m:
        (base if m.group(2) else off)[m.group(1)[3:]] = int(m.group(3), 0)
names = collections.defaultdict(list)
for n, o in off.items():
    if n in base:
        names[(base[n], o)].append(n)

# Blocks and their instance strides (dcn_4_1_0_offset.h): the DSC blocks are 0x5c dwords apart, OPTC/ODM 0x10,
# DSCRM one dword each; the DIG registers (DP*, VPG*, AFMT*) are materialised by the driver's DIG offset.
DSC_BASE, DSC_STRIDE = 0x3000, 0x5c
DSCRM_BASE = 0x1a64
OPTC_BASE, OPTC_STRIDE = 0x1acc, 0x10
K_FIXED, K_DSC, K_DSCRM, K_OPTC, K_DIG = 0, 1, 2, 3, 4
BLOCK_NAME = {K_FIXED: 'fixed', K_DSC: 'dsc', K_DSCRM: 'dscrm', K_OPTC: 'optc', K_DIG: 'dig'}

def classify(seg, dword, name):
    if name.startswith(('DSC_TOP0_', 'DSCC0_', 'DSCCIF0_')):
        assert DSC_BASE <= dword < DSC_BASE + DSC_STRIDE, f'{name} outside DSC0'
        return K_DSC, dword - DSC_BASE
    if name.startswith('DSCRM0_'):
        assert dword == DSCRM_BASE, f'{name} outside DSCRM0'
        return K_DSCRM, dword - DSCRM_BASE
    if name.startswith('ODM0_OPTC_'):
        assert OPTC_BASE <= dword < OPTC_BASE + OPTC_STRIDE, f'{name} outside ODM0'
        return K_OPTC, dword - OPTC_BASE
    if name.startswith(('DP0_', 'VPG0_', 'AFMT0_', 'DIG0_AFMT_')):
        return K_DIG, dword
    if seg == 1 and name.startswith(('DCCG_', 'DSCCLK')):
        return K_FIXED, dword
    if name.startswith('OTG0_'):
        # optc3_set_dsc_config's OTG_V_SYNC_MODE = 0 (the HDMI FRL branch does not apply to this sink)
        return K_FIXED, dword
    sys.exit(f'refused: {name} (segment {seg} dword 0x{dword:04x}) is not part of the DSC chain')

def name_of(seg, dword, want=None):
    ns = names.get((seg, dword))
    if not ns:
        sys.exit(f'refused: no register name for segment {seg} dword 0x{dword:04x}')
    if want:
        ns = [n for n in ns if n.startswith(want)] or ns
    return ns[0]

lines = [l.split() for l in open(trace) if l.strip()]
cfg = lines[0]
assert cfg[0] == 'CONFIG', 'no CONFIG line'
pipe, dig, link, hpd = map(int, cfg[1:5])
ha, hfp, hsw, hbp, va, vfp, vsw, vbp, khz = map(int, cfg[5:14])
dsc_slices, dsc_bpp, dsc_bpc = 2, 8, 8          # what run.sh asks pipegen for
assert (ha, va, khz) == (3840, 2160, 533250), f'the trace is {ha}x{va} at {khz} kHz, not the 4K60 raster'
assert (pipe, dig, hpd) == (0, 0, 1), 'the DSC table is recorded on the boot pipe (pipe 0, DIG0, HPD1)'

entries = []
def emit(kind, block, seg, dword, mask, value, arg, what):
    entries.append((kind, block, seg, dword, mask, value, arg, what))

# 1. The stream encoder's DP_PIXEL_FORMAT as Linux leaves it for a DSC stream. It is written while the stream is
#    set up, before begin:dsc; take the write that says PIXEL_ENCODING_TYPE = 1.
seen = []
for p in lines[1:]:
    if p[0] in ('W', 'U'):
        seg, dword, mask, value = int(p[1]), int(p[2], 0), int(p[3], 16), int(p[4], 16)
        nm = name_of(seg, dword)
        if nm == f'DP{dig}_DP_PIXEL_FORMAT':
            seen.append((p[0], seg, dword, mask, value, p[-1]))
assert seen, 'no DP_PIXEL_FORMAT write in the trace'
enc = [(a, b, c, d, e, f) for (a, b, c, d, e, f) in seen if e & 1]
assert enc, 'DP_PIXEL_FORMAT never selects the compressed encoding'
assert len({e for _, _, _, _, e, _ in enc}) == 1, 'DP_PIXEL_FORMAT has more than one compressed value'
k, seg, dword, mask, value, func = enc[-1]
assert k in ('W', 'U'), 'DP_PIXEL_FORMAT expected as a register write'
emit(k, K_DIG, seg, dword, mask, value, 0, f'{func} DP{dig}_DP_PIXEL_FORMAT (PIXEL_ENCODING_TYPE = 1)')

# 2. Everything between begin:dsc and end:dsc, in order.
inside = False
pps = collections.OrderedDict()
for p in lines[1:]:
    if p[0] == 'MARK':
        if p[1] == 'begin:dsc':
            inside = True
        elif p[1] == 'end:dsc':
            inside = False
        continue
    if not inside:
        continue
    if p[0] == 'DELAY':
        emit('D', K_FIXED, 0, 0, 0, 0, int(p[1]), p[3])
        continue
    seg, dword = int(p[1]), int(p[2], 0)
    nm = name_of(seg, dword)
    block, off = classify(seg, dword, nm)
    what = f'{p[-1]} {nm}'
    if p[0] == 'WAIT':
        mask, value, arg = int(p[3], 16), int(p[4], 16), int(p[5])
        emit('T', block, seg, off, mask, value, arg, what)
    else:
        mask, value = (0xffffffff, int(p[4], 16)) if p[0] == 'W' else (int(p[3], 16), int(p[4], 16))
        arg = int(p[5])
        emit(p[0], block, seg, off, mask, value, arg, what)
        if nm.startswith('DSCC0_DSCC_PPS_CONFIG'):
            pps[nm.split('_', 1)[1]] = value
assert inside is False, 'no end:dsc mark'
assert len(pps) == 23, f'{len(pps)} PPS words recorded, not 23'
# The PPS words are the 23 dwords; log them in order for the test and the boot log.
pps_words = [pps[f'DSCC_PPS_CONFIG{i}'] for i in range(23)]

# 3. The sink: DP_DSC_ENABLE (0x160) = 1, Linux's own DPCD write before the stream is unblanked. The table applies
#    it after the encoder is programmed and before the stream is unblanked; the driver keeps that order.
enable_writes = [p for p in lines if p[0] == 'MARK' and p[1] == 'aux:w:160:01']
assert len(enable_writes) >= 1, 'the sink is never told DSC is on'
emit('A', K_FIXED, 0, 0x160, 0xff, 0x01, 0, 'dm_helpers_dp_write_dsc_enable DP_DSC_ENABLE (DPCD 0x160)')

# The table must contain each block once and the chain in Linux's order: PPS SDP (VPG/DP_SEC), DTO, DSCC, DSCRM,
# OPTC. A missing piece is a table that would leave the sink without a PPS or the encoder unconnected.
kinds = collections.Counter(e[0] for e in entries)
blocks = collections.Counter(e[1] for e in entries)
assert blocks[K_DSC] >= 23 + 5, blocks           # 23 PPS words, DSCC_CONFIG0/1, DSC_TOP, DSCCIF, ...
assert blocks[K_DSCRM] == 1 and blocks[K_OPTC] == 3, blocks
assert kinds['W'] + kinds['U'] >= 40, kinds
assert any(e[7].endswith('DSCRM0_DSCRM_DSC_FORWARD_CONFIG') for e in entries), 'no forward enable'
assert any(e[7].endswith('DSCC0_DSCC_CONFIG1') for e in entries), 'no rate-control buffer size'
assert any(e[7].endswith('ODM0_OPTC_BYTES_PER_PIXEL') for e in entries), 'no OPTC bytes a pixel'
assert any('DP0_DP_GSP11_CNTL' in e[7] for e in entries), 'no PPS SDP enable line'
assert sum('VPG0_VPG_GENERIC_PACKET_DATA' in e[7] for e in entries) >= 36, 'the PPS SDP data is not there'

print(f'''// GENERATED by tools/gen/pipegen from Linux {commit[:12]} (drivers/gpu/drm/amd/display). Do not edit:
// regenerate with tools/gen/pipegen/run.sh. Every step is one Linux's DCN 4.01 code made when asked to compress
// this stream with DSC (link_dpms.c's PPS SDP, dcn32_update_dsc_on_stream's block, dcn401_dsc.c's registers).
//
// DSC on the boot display's raster: {ha}x{va} at {khz} kHz, {dsc_slices} slice(s) of {(ha + dsc_slices - 1)//dsc_slices} px,
// {dsc_bpp} bpp, RGB {dsc_bpc} bpc. Recorded on pipe {pipe} (OTG{pipe}, OPP{pipe}, DIG{dig}, HPD{hpd}), so the block
// numbers below are instance 0; the driver adds the instance stride of the pipe it enables it on.
//
// block: 0 fixed (DCCG), 1 DSC_TOP/DSCC/DSCCIF (stride 0x5c), 2 DSCRM (stride 1), 3 OPTC/ODM (stride 0x10),
//        4 DIG/DP/VPG/AFMT (the driver's DIG offset). {len(entries)} entries.

constexpr uint16_t kPicWidth = {ha};
constexpr uint16_t kPicHeight = {va};
constexpr uint8_t kSlices = {dsc_slices};
constexpr uint8_t kBitsPerPixel = {dsc_bpp};
constexpr uint8_t kBitsPerComponent = {dsc_bpc};
constexpr uint16_t kSliceWidth = {(ha + dsc_slices - 1)//dsc_slices};

// The 23 Picture Parameter Set dwords as Linux wrote them (DSCC_PPS_CONFIG0..22): what the sink has to be told, and
// what the boot log prints.
constexpr uint32_t kPps[23] = {{
	{', '.join(f'0x{w:08x}' for w in pps_words)}
}};

static const Gen kGen[] = {{''')
for kind, block, seg, dword, mask, value, arg, what in entries:
    esc = what.replace('"', '\\"')
    print(f"\t{{ '{kind}', {block}, {seg}, 0x{dword:04x}, 0x{mask:08x}, 0x{value:08x}, {arg}, \"{esc}\" }},")
print('};')
sys.stderr.write(f'mkdsc: {len(entries)} entries, ' + ', '.join(f'{BLOCK_NAME[b]}:{n}' for b, n in sorted(blocks.items()))
                 + f'; PPS words {len(pps_words)}\n')
