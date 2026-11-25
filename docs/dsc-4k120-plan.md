# DSC on DCN 4.01 (RX 9070 XT, gfx1201) → TCL 32X3B over DisplayPort

Status: **analysis + implementation plan only** (no driver code written).
Started 2026-11-26 06:50 by subagent `dsc-plan`. This file is the only thing it wrote.
Target tree: `/home/Celia/rdna4/a4` @ `devel/hdr-30bpp`-era working tree (kext 0.5.0 line).

Evidence labels, used on every claim:

| Label | Meaning |
|---|---|
| **[T]** | **proven from tables** — a register offset/field/value read out of Linux's DCN 4.1.0 headers or its display code, or a value the driver logged on this machine (`docs/hw-logs/…`, ring log). |
| **[L]** | **proven from Linux source** — behaviour read in the file named (fetched, not guessed; see §8 for the exact revisions/URLs). |
| **[inf]** | **inferred** — follows from a proven fact plus documented behaviour, no line in this tree proves it. |
| **[hyp]** | **hypothesis** — plausible, unmeasured; listed with the measurement that would settle it. |

Nothing in this file was verified on the card except what is quoted from the existing ring logs.
The Mac was **not** touched: no reboot, no boot-args, no NVRAM, no `/Library`, no WindowServer restart.

---

## Answers at a glance

| Question | Where | One-line answer |
|---|---|---|
| Q1 sink: what must be told, what it reports, SST or MST | §1 | DPCD `0x160` bit0 = 1 and a software-built PPS; the sink reports DSC **1.2**, whole-bpp only, 4 slices max usable, 340 Mpix/s/slice; **SST, no MST change** |
| Q2 source: DCN401 registers, what we already write, minimal program | §2 | `DSC_TOP0..3` `0x3000`…, `DSCC0..3` `0x300a`…, `DSCRM0..3` `0x1a64`…, OPTC `0x1acc`…, `DP_PIXEL_FORMAT` `0x211f+digOff()`; we write **none** of the DSC programming today (only power/clock gating); minimal = §2.4, six items |
| Q3 timing plumbing, the cap, the right cap value | §3 | `dscCompressedBitsPerPixel/SliceHeight/SliceWidth` at `0x90/0x92/0x94` of `IODetailedTimingInformationV2`, stamped in the `getTimingInfoForDisplayMode` hook (NDRV records have no DSC field); cap computed at `device.cpp:2801-2810`/`bootDpMaxKHz()` and applied at `modes.cpp:52-56`; raise it **per DSC mode only**, to `min(DISPCLK, 4×340 Mpix/s, 17.28 Gbps/bpp)` = DISPCLK-bound |
| Q4 smallest step, risks, rollback | §5, §6 | **S1 first** (can the SMU give ≥1066.56 MHz?), then **S2** = DSC on at the existing 4K60 raster; risks and per-step rollback in §5.3 |
| Q5 precedents | §4 | NVIDIA's `dsc*` timing-field trick is the only precedent (`nvrm-fb.cpp:3844-3860`); `archive/AMDRadeonRDNA4` has **no** DSC code (its `dsc_*.py` are dyld-cache tools) |

## 0. Machine facts (input; not re-derived) and three corrections

| Fact | Value | Source |
|---|---|---|
| GPU | RX 9070 XT, gfx1201, DCN 4.01 | ring log |
| Sink | TCL 32X3B, 4K panel, DisplayPort, product 0x2014 | `docs/hw-logs/2026-11-25-dpcd-dsc.txt:4` |
| Link | DPCD 1.2, 4 lanes, rate `0x14` = HBR2: 5.4 Gbps/lane on the wire, 8b/10b ⇒ **17.28 Gbps payload** = a 720 MHz pixel clock at 24 bpp (the tree's own arithmetic: `DpTrain::bandwidthKbps`, `src/dptrain.hpp:48-49`, `static_assert(… == 17280000)`) | `dptrain: sink DPCD 1.2, up to 4 lane(s) at rate 0x14`; `src/device.cpp:2806` |
| Sink DSC | present | `dptrain: sink DSC 0x060..0x06f: 01 21 03 7f 3b 07 01 00 00 1f 0e 11 08 00 00 04` |
| Mode cap today | 650000 kHz (8 bpc budget 720000, DISPCLK 650000, boot 533234) | `modes: DisplayPort modes up to 650000 kHz …` |
| Pipeline | our own code: `dcn401_init_hw` 168 steps, `pipe2.cpp`, `otgtiming.cpp`, `modeset.cpp` | `docs/index.md:54,56,62` |
| DMUB | present, **not used for DSC anywhere in the tree** | ring log + `grep -ri dsc src/` (§2.3) |

Bandwidth arithmetic — payload **17.28 Gbps** (4×HBR2 after 8b/10b). The driver divides it by 24
only (`src/device.cpp:2806`, `:4850`, `:4939`, `:4992`), i.e. it assumes 8 bpc:

| Raster | Pixel rate | 8 bpc RGB (×24) | 10 bpc RGB (×30) | Fits 17.28 Gbps? |
|---|---|---|---|---|
| 3840x2160@60 | 533.25 MHz | 12.80 Gbps | 15.99 Gbps | **both fit** (26 % and 7.5 % margin) |
| 3840x2160@120 | 1066.56 MHz | 25.60 Gbps | 31.99 Gbps | neither |
| 3840x2160@144 | 1333.33 MHz | 32.00 Gbps | 40.00 Gbps | neither |
| 2560x1440@240 | 995.52 MHz | 19.11 Gbps | 23.89 Gbps | neither (8 bpc is 11 % over) |
| 3840x2160@240 | 2418.37 MHz | 58.04 Gbps | — | no, and not with DSC either (§5.1) |

Compressed bits-per-pixel that would make each raster fit 17.28 Gbps (DSC's floor is 6 bpp, and the
sink accepts whole bpp only, §0.2):

| Raster | max bpp that fits | a sane choice |
|---|---|---|
| 4K120 8 bpc | 16.2 | **8** (8.53 Gbps) or 12 (12.80) |
| 4K120 10 bpc | 13.5 | 12 (12.80 Gbps) |
| 4K144 8 bpc | 12.9 | 12 (16.00 Gbps — 7 % margin) |
| 1440p240 8 bpc | 17.3 | 12 (11.95 Gbps) |
| 4K240 8 bpc | 7.1 | *moot*: the sink's 4×340 Mpix/s cannot decode 2418 Mpix/s |

### Corrections to the brief (all three matter for the implementation)

1. **The sink's DSC revision is 1.2, not 2.1.** DPCD `0x061` = `0x21`; DP's
   revision byte is `minor<<4 | major` (Linux `include/drm/display/drm_dp.h:265-270`
   `DP_DSC_MAJOR_MASK = 0xf<<0`, `DP_DSC_MINOR_MASK = 0xf<<4`), so `0x21` = major 1, minor 2 =
   **DSC 1.2** — and DCN401's encoder reports exactly the same `0x21`
   (`dcn401_dsc.c:69`: `dsc_enc_caps->dsc_version = 0x21; /* v1.2 - DP spec defined it in reverse order and we kept it */`).
   **[T]** There is no `0x00 0x1f` version pair in the dump; `0x067`/`0x068`
   (the eDP max-bpp bytes) are `00 00`.
2. **`0x06F` = `0x04` means the sink accepts only integer bits-per-pixel**
   (`DP_DSC_BITS_PER_PIXEL_MASK 0x7`, value `4` = `DP_DSC_BITS_PER_PIXEL_1_1`,
   `drm_dp.h:365-371`). So the bpp we pick must be a whole number (8, 10, 12 …),
   not e.g. 8.5. **[T]**
3. **4K60 at 10 bpc does *not* need DSC.** 533.25 MHz × 30 = 15.99 Gbps against a 17.28 Gbps
   payload — it fits with 7.5 % margin, and the firmware already runs the DP stream encoder at
   10 bpc (`DP0_DP_PIXEL_FORMAT` = `0x200`, `DP0_DP_MSA_COLORIMETRY` = `0x40000000`,
   `docs/hdr-30bpp-plan.md:221-226`). **DSC's payoff on this machine is refresh rate, not bit
   depth**: everything above 60 Hz at 4K, plus 4K120 at 10 bpc. The HDR track (`docs/hdr-*.md`)
   and the DSC track are therefore *independent* — do not couple them, and do not justify DSC work
   with HDR.

---

## 1. What the sink and the source must do (Q1)

### 1.1 The sink's DSC block, decoded (DPCD 0x060..0x06F)

Raw, from the card (`docs/hw-logs/2026-11-25-dpcd-dsc.txt:8`, read by `device.cpp:3142-3152`):

```
0x060: 01 21 03 7f 3b 07 01 00 00 1f 0e 11 08 00 00 04
```

Decoded with the offsets/values of `include/drm/display/drm_dp.h:244-371` and the same decode
amdgpu does in `dc/dsc/dc_dsc.c:701-770` (`dc_dsc_parse_dsc_dpcd`):

| DPCD | Field | Raw | Meaning | Consequence for us |
|---|---|---|---|---|
| 0x060 | `DSC_DECODER_CAPABILITY` | `0x01` | decompression supported (bit 0); **no** pass-through, **no** dynamic-PPS-update bits | DSC is a real path **[T]** |
| 0x061 | `DSC_REV` | `0x21` | **DSC 1.2** | PPS `DSC_VERSION_MINOR` = 2, `ALTERNATE_ICH_ENCODING_EN` = 1 (`dcn20_dsc.c:431`) **[T]** |
| 0x062 | `RC_BUF_BLK_SIZE` | `0x03` | 64-byte buffer blocks; bits[7:3] (DP 2.1a `THROUGHPUT_MODE_0_DELTA`) = 0 | `rc_buffer_size` = 8192 B |
| 0x063 | `RC_BUF_SIZE` | `0x7f` | 128 blocks ⇒ **8192-byte rate-control buffer** | `DSCC_CONFIG1.DSCC_RATE_CONTROL_BUFFER_MODEL_SIZE` derives from this (`dc_dsc.c:713-727`) |
| 0x064 | `DSC_SLICE_CAP_1` | `0x3b` | 1, 2, 4, 6, **8** slices per line (bits 0,1,3,4,5) | usable sets = {1,2,4} ∩ encoder {1,2,3,4} |
| 0x065 | `LINE_BUF_BIT_DEPTH` | `0x07` | **16 bits/component** | encoder offers 13 → PPS `line_buf_depth` = **13** (`dcn401_dsc.c:76`, `dcn20_dsc.c:430`) |
| 0x066 | `BLK_PREDICTION_SUPPORT` | `0x01` | block prediction supported; RGB-conv bypass **not** | `block_pred_enable = 1` |
| 0x067/0x068 | `MAX_BITS_PER_PIXEL` (eDP only) | `00 00` | not declared | no constraint from here |
| 0x069 | `DEC_COLOR_FORMAT_CAP` | `0x1f` | RGB, YCbCr444, simple 422, native 422, native 420 | we use RGB |
| 0x06A | `DEC_COLOR_DEPTH_CAP` | `0x0e` | 8, 10, 12 bpc | 10 bpc for HDR is allowed |
| 0x06B | `PEAK_THROUGHPUT` | `0x11` | mode0 = mode1 = **340 Mpixel/s per slice** | **4 slices are required for 4K120** (4×340 = 1360 > 1066.56; 2 slices = 680 < 1066.56) |
| 0x06C | `MAX_SLICE_WIDTH` | `0x08` | 8 × 320 = **2560 px** | 3840/4 = **960** px per slice fits |
| 0x06D | `DSC_SLICE_CAP_2` | `0x00` | no 16/20/24-slice | — |
| 0x06E | `MAX_BPP_DELTA_VERSION` | `0x00` | no delta | — |
| 0x06F | `BITS_PER_PIXEL_INC` | `0x04` | **integer bpp only** | pick 8 / 10 / 12 |

> The "340 Mpix/s" figure is *per slice*: amdgpu multiplies by the slice count
> (`dc_dsc.c:1212` `total_sink_throughput = max_slices * throughput_mode_0_mps`, and validates
> per-slice at `:1870` `pix_clk_per_slice_khz <= sink_per_slice_throughput_mps * 1000`). Reading it
> as a total would wrongly rule out even 4K60. **[L]**

### 1.2 What the sink must be told, and where

| What | Where | Value | Note |
|---|---|---|---|
| Decompression on | **DPCD 0x160** `DP_DSC_ENABLE`, bit 0 `DP_DECOMPRESSION_EN` (`drm_dp.h:755-757`) | `0x01` | The only mandatory sink-side register for DP 1.4 DSC |
| DP 2.0 DSC configuration | DPCD 0x161 `DP_DSC_CONFIGURATION` (`drm_dp.h:758`) | **not written** | exists in DP 2.0 only; our sink reports DPCD 1.2 and the link is 8b/10b HBR2 |
| The PPS itself | **in the main stream, as a DSC PPS secondary-data packet** — *not* DPCD | 128-byte PPS | see below |
| MSA colour depth | DPCD none; `DP0_DP_MSA_COLORIMETRY.DP_MSA_MISC0` (seg2 `0x2120`, bits 31:24) | already `0x40` = 10 bpc on this boot (`docs/hdr-30bpp-plan.md:221-226`) | firmware's value |

**PPS route — "native PPS vs our own".** For an **external DP sink** the PPS is not a DPCD
register: `drm_dp.h` defines DSC DPCD up to `0x06F` (capabilities), `0x160`/`0x161` (enable) and
nothing that holds a PPS, and the DP 1.4 PPS DPCD window (`0x0A0`…, used by eDP) is **not** what
`device.cpp:3146` reads or what a DP sink decodes. amdgpu builds the 128-byte PPS in software
(`dsc2_get_packed_pps` → `dsc_prepare_config` → `drm_dsc_pps_payload_pack`,
`dcn20_dsc.c:223-241`) **and** programs the same numbers into the encoder's `DSCC_PPS_CONFIG0..22`.
So the answer is: **our own PPS**, computed from the same struct the encoder registers get — one
source of truth, two destinations (registers + SDP). Which of the two the hardware actually
transmits on DCN 4.01 is **[inf]**: the DCN 4.1.0 header has no register that would make the
source build an SDP by hand, so the DSC block is presumed to emit it; the check is in §7 (U3).

> **Correction to the brief's "PPS payload at 0x10A..":** `0x10A` is
> `DP_EDP_CONFIGURATION_SET` (`drm_dp.h:664`), not a PPS address. There is no PPS register block
> at `0x10A` for a DP sink. If the intent was the *eDP* PPS window, that window is `0x0A0`-relative
> and only applies to an internal panel — not to this TV over DisplayPort.

### 1.3 MST vs SST

**SST, unchanged.** The tree only ever speaks SST (`Pipe::Signal::DpSst`; `device.cpp:3121`
refuses to train anything else), DP 1.4 allows DSC in SST, and nothing in DCN 4.01 requires an MST
topology to compress a single stream. No `DP_MSTM_CTRL` (0x111) write, no payload allocation —
the only DSC-related DPCD write in the whole plan is `0x160 = 1`. **[L]/[inf]**

### 1.4 What the sink-side sequence looks like at the end

1. Link stays exactly as trained (4 lanes, HBR2). **No retrain** is required to enable DSC — the
   compressed stream simply uses less of the same 17.28 Gbps. **[inf]** (and it is what makes this
   plan low-risk: the retrain path is not entered at all).
2. `DPCD 0x160 = 0x01` (AUX write, the mechanism already exists: `ModeSet::Op::Aux`,
   `modeset.hpp:78`, executed at `device.cpp:3071` and `:4028`).
3. Stream re-programmed with DSC on (§2), which also carries the new PPS.
4. On leaving DSC: `0x160 = 0` and the encoder disabled — in that order for "safe" frames.

---

## 2. What the DCN 4.01 source side needs (Q2)

### 2.1 The register map (all offsets are **segment + dword**, as this tree writes registers)

From `dcn_4_1_0_offset.h` / `dcn_4_1_0_sh_mask.h` (Linux, DCN 4.1.0) — **[T]**. Instance stride for
the DSC block is `0x5c` dwords (`0x3000 → 0x305c → 0x30b8 → 0x3114`), OTG/OPTC instance stride `0x10`.

| Block (seg 2 unless said) | Address (dword) | Fields that matter |
|---|---|---|
| `DSC_TOPi_DSC_TOP_CONTROL` | `0x3000 + 0x5c*i` | `DSC_CLOCK_EN`(0,`0x1`), `DSC_DISPCLK_R_GATE_DIS`(4), `DSC_DSCCLK_R_GATE_DIS`(8), `DSC_FGCG_REP_DIS`(12), `DSC_DSCCLK_DYNAMIC_CLOCK_GATE_EN`(16) |
| `DSC_TOPi_DSC_DEBUG_CONTROL` | `0x3001 + 0x5c*i` | `DSC_DBG_EN`(0) |
| `DSCCIFi_DSCCIF_CONFIG0` | `0x3005 + 0x5c*i` | `INPUT_PIXEL_FORMAT`(12, `0x7000`), `BITS_PER_COMPONENT`(16, `0xF0000`) |
| `DSCCi_DSCC_CONFIG0` | `0x300a + 0x5c*i` | `ICH_RESET_AT_END_OF_LINE`(0,`0xF`), `NUMBER_OF_SLICES_PER_LINE`(4,`0x30` = h−1), `ALTERNATE_ICH_ENCODING_EN`(8), `NUMBER_OF_SLICES_IN_VERTICAL_DIRECTION`(16, `0xFFFF0000` = v−1) |
| `DSCCi_DSCC_CONFIG1` | `0x300b + 0x5c*i` | `DSCC_RATE_CONTROL_BUFFER_MODEL_SIZE`(0,`0x3FFFF`), `DSCC_DISABLE_ICH`(24) |
| `DSCCi_DSCC_STATUS` | `0x300d + 0x5c*i` | `DSCC_DOUBLE_BUFFER_REG_UPDATE_PENDING`(0) — the "PPS latched" bit |
| `DSCCi_DSCC_PPS_CONFIG0..22` | `0x3012 .. 0x3028 + 0x5c*i` | the 23 PPS words (below) |
| `DSCCi_DSCC_INTERRUPT_CONTROL0/1` | `0x300e/0x300f + 0x5c*i` | RC-buffer-overflow ints; Linux programs 0s here |
| `DSCCi_DSCC_MEM_POWER_CONTROL0/1` | `0x3029/0x302a + 0x5c*i` | leave as init left them |
| `DSCRMi_DSCRM_DSC_FORWARD_CONFIG` | `0x1a64 + i` | `DSCRM_DSC_FORWARD_EN`(0), `DSCRM_DSC_OPP_PIPE_SOURCE`(4,`0x70`), `DSCRM_DSC_FORWARD_EN_STATUS`(12) |
| `ODMi_OPTC_DATA_FORMAT_CONTROL` | `0x1acc + 0x10*i` | `OPTC_DSC_MODE`(4, `0x30`) |
| `ODMi_OPTC_BYTES_PER_PIXEL` | `0x1acd + 0x10*i` | `OPTC_DSC_BYTES_PER_PIXEL`(0, `0x7FFFFFFF`, **u3.28**) |
| `ODMi_OPTC_WIDTH_CONTROL` | `0x1ace + 0x10*i` | `OPTC_DSC_SLICE_WIDTH`(16, `0x1FFF0000`) |
| `DPi_DP_PIXEL_FORMAT` | `0x211f + digOff()` (stride `0x124`, `Pipe::Reg::kDigStride` = `0x124`, `src/pipe.hpp:75`; helper `src/device.hpp:516`) | `PIXEL_ENCODING_TYPE`(0) = **1 when DSC**, `COMPRESSED_PIXEL_FORMAT`(16), `UNCOMPRESSED_COMPONENT_DEPTH`(8) |
| `DPi_DP_MSA_COLORIMETRY` | `0x2120 + digOff()` | `DP_MSA_MISC0`(24) |
| DCCG `DSCCLK{0,1,2}_DTO_PARAM` | **seg 1** `0x006c/0x006d/0x006e` | DSCCLK DTO phase/modulo |
| DCCG `DSCCLK_DTO_CTRL` | **seg 1** `0x00a7` | DSCCLK DTO control |
| DCCG `DCCG_GATE_DISABLE_CNTL` | **seg 1** `0x0074` | `DSCCLK_GATE_DISABLE`(11) |
| DCCG `DCCG_GATE_DISABLE_CNTL6` | **seg 1** `0x00a9` | `DSCCLK{0..3}_ROOT_GATE_DISABLE`(8..11) |

The **OPTC** block in DCN 4.01 is named `ODM0..3` (that is why `dcn_init_linux.inc` shows
`ODM0_OPTC_DATA_SOURCE_SELECT` at `0x1acb`); "OTG" holds no DSC field at all in this IP block
(`grep -c "OTG0_OTG.*DSC" dcn_4_1_0_sh_mask.h` = 0).

### 2.2 The programming order, as DCN401's own driver does it

**[L]** `dcn401_dsc.c` (DCN401 DSC) + `dcn20_dsc.c` (shared PPS builder) + `dcn20_optc.c`:

```
dsc401_set_config  →  dsc_write_to_registers:            dcn401_dsc.c:222-408
    DSC_DEBUG_CONTROL   = DSC_DBG_EN
    DSCCIF_CONFIG0      = INPUT_PIXEL_FORMAT | BITS_PER_COMPONENT
    DSCC_CONFIG0        = (slices_h-1), ALTERNATE_ICH_ENCODING_EN, (slices_v-1)   [+ ICH_RESET_AT_END_OF_LINE on DCN4]
    DSCC_CONFIG1        = DSCC_RATE_CONTROL_BUFFER_MODEL_SIZE
    DSCC_INTERRUPT_CONTROL0 = 0
    DSCC_PPS_CONFIG0..22 = the 23 PPS words
dsc401_enable      →  DSC_TOP_CONTROL.DSC_CLOCK_EN = 1;                       dcn401_dsc.c:161-183
                      DSCRM_DSC_FORWARD_CONFIG = FORWARD_EN | OPP_PIPE_SOURCE(op)
dsc401_disable     →  DSCRM_DSC_FORWARD_EN = 0; DSC_TOP_CONTROL.DSC_CLOCK_EN = 0;   :186-203
                      (wait FORWARD_EN_STATUS == 0 afterwards)
optc3_set_dsc_config → OPTC_DATA_FORMAT_CONTROL.OPTC_DSC_MODE;                 dcn20_optc.c:127-142
                       OPTC_BYTES_PER_PIXEL.OPTC_DSC_BYTES_PER_PIXEL;          (via dcn30_optc.c:184-199)
                       OPTC_WIDTH_CONTROL.OPTC_DSC_SLICE_WIDTH
enc401_stream_encoder_dp_set_stream_attribute:                                 dcn401_dio_stream_encoder.c:537-586
                       DP_PIXEL_FORMAT = PIXEL_ENCODING_TYPE(DSC?1:0) | COMPRESSED_PIXEL_FORMAT(0 for RGB)
sink:                  DPCD 0x160 bit0 = 1                                     (DP 1.4, no register)
```

The **PPS arithmetic** the kext has to reproduce (all from `dcn20_dsc.c:387-476`, **[L]**):

| PPS/register value | Formula | 4K120 8 bpc example |
|---|---|---|
| `pps.bits_per_pixel` (and `DSCC_PPS_CONFIG1.BITS_PER_PIXEL`, `bpp_x32>>1` for RGB) | bpp × 16 | 8 bpp → **128** |
| `pps.slice_width` | `ceil((pic_width + padding) / slices_h)` | 3840/4 → **960** |
| `pps.slice_height` | `pic_height / slices_v` (must divide exactly) | 2160/1 → **2160** |
| `pps.dsc_version_minor` | sink's minor | **2** |
| `pps.bits_per_component` | 8/10/12 from the stream's depth | **8** |
| `pps.line_buf_depth` | min(sink 16, encoder 13) | **13** |
| `pps.block_pred_enable` | both sides support | **1** |
| `alternate_ich_encoding_en` | `version_minor == 2` | **1** |
| `ich_reset_at_eol` | `slices_h > 1 or ODM` → 0xF | **0xF** (4 slices) |
| `OPTC_DSC_BYTES_PER_PIXEL` | bytes/pixel in **u3.28** | 1.0 → `0x10000000` |
| `rc_buffer_size`, `rc_model_size`, bpg offsets, 15 `rc_range_params` | `calc_rc_params` + `dscc_compute_dsc_parameters` (`dc/dsc/rc_calc.c`, `dscc_types.h`) | table-driven per bpc/bpp |

**Do not re-derive these by hand in the kext.** The project's established method (§5 of
`docs/index.md`) is to *record* Linux's own code: extend `tools/gen/pipegen` so its fake stream
carries `flags.DSC = 1` and a `dsc_cfg`, and let `mkinc.py` emit the DSC register writes and PPS
words as a table for each mode of `modes.txt`. That turns the whole PPS computation into data with
Linux as ground truth, exactly as `pipe2_linux*.inc` already does for the rest of the pipe.

### 2.3 What our driver already writes today

`grep -ri dsc src/` over the kext finds **only** these, and none of them turn DSC on:

| Already written | Where | Meaning |
|---|---|---|
| `DSC_TOP{0..3}_DSC_TOP_CONTROL` bit 12 (`DSC_FGCG_REP_DIS` = 1) | `dcn_init_linux.inc:49-52` (`dsc401_set_fgcg`) | fine-grain clock gating disabled — power/clock plumbing, not DSC |
| `DOMAIN16..19_PG_CONFIG`/`PG_STATUS` (`dcn32_dsc_pg_control`) | `dcn_init_linux.inc:169-184` | the four DSC power domains are **ungated** at init, so the block is reachable |
| `DP0_DP_MSA_COLORIMETRY` = `0x40000000` and `DP0_DP_PIXEL_FORMAT` = `0x200` | left by the firmware; logged in `docs/hdr-30bpp-plan.md:221-226` | MSA already says 10 bpc; `UNCOMPRESSED_COMPONENT_DEPTH` = 2 = 10 bpc |
| DPCD writes over AUX (`Op::Aux`) | `modeset.hpp:78`, `device.cpp:3071/:4028`, tables in `pipe2_linux_dp*.inc` (`0x300`, `0x303`, `0x600`) | the transport for `0x160 = 1` exists; **`kMaxAux` is only 4** (`pipe2.hpp:137`) |
| nothing at all in `DSCC*`, `DSCRM*`, `DSC_TOP*_CLOCK_EN`, `ODM*_OPTC_*DSC*`, `DSCCLK*` | — | the encoder is never configured |

### 2.4 The minimal 4K120-with-DSC source program (what a first working version needs)

1. **Clocks**: `DISPCLK` and `DPPCLK` ≥ 1066.56 MHz (`clk.cpp:109-111` currently asks the SMU for
   650 MHz; `rdna4-dispclk=<MHz>` overrides it, `clk.cpp:110`, `smuSetHardMin` at `clk.cpp:332`).
   `DSCCLK` from its DTO (seg1 `0x006c..0x006e`, `0x00a7`), and **its gates opened first**: the
   cold-start table deliberately *allows* every DCCG gate —
   `dcn_init_linux.inc:186` writes `DCCG_GATE_DISABLE_CNTL` (seg1 `0x0074`) = `0` with a full mask,
   which includes `DSCCLK_GATE_DISABLE` (bit 11) — and `DCCG_GATE_DISABLE_CNTL6` (seg1 `0x00a9`,
   `DSCCLK{0..3}_ROOT_GATE_DISABLE` bits 8..11) is **never written by the init table at all**, so it
   holds the firmware's value. Whatever DSC turns on must clear those gates and put them back on the
   way out. **The clock ceiling is the gating unknown — measure it first (§6 S1).**
2. **Encoder** (DSC instance = the OPP instance of the pipe; `DSCRM_DSC_OPP_PIPE_SOURCE` ties them):
   the `dsc401_set_config` block from §2.2 with the PPS for {3840×2160, 4 slices × 960, 8 bpp, RGB,
   8 bpc}, then `DSC_TOP_CONTROL.DSC_CLOCK_EN = 1` and `DSCRM_DSC_FORWARD_CONFIG = EN | OPP`.
3. **OPTC/OTG**: `OPTC_DSC_MODE` (enum `optc_dsc_mode`, `dc/dsc/dc_dsc.h`), `OPTC_DSC_BYTES_PER_PIXEL`
   (`0x10000000`), `OPTC_DSC_SLICE_WIDTH` (960).
4. **DP stream encoder**: `DP_PIXEL_FORMAT.PIXEL_ENCODING_TYPE = 1`, `COMPRESSED_PIXEL_FORMAT = 0`
   (RGB / YCbCr444), `UNCOMPRESSED_COMPONENT_DEPTH` unchanged.
5. **Sink**: `DPCD 0x160 = 1` before unblank; `0` after the encoder is off when leaving DSC.
6. **Timing**: unchanged raster — the OTG keeps generating 3840×2160@120 (H total 1066.56 MHz); only
   the link now carries fewer bits. 2200×1125-style totals must be recomputed from the EDID's
   DisplayID entry (`modes.txt`), not invented.

Program-size effect on the existing planner: `ModeSet::kMaxSteps = 128` (`modeset.hpp:89`) for the
**lit-pipe** plan and `Pipe2::kMaxSteps = 448`, `Pipe2::kMaxAux = 4` (`pipe2.hpp:135-137`) for a
second pipe. ~30 extra register writes + 1 DPCD write ⇒ either raise `kMaxSteps` to 192 and
`kMaxAux` to 8, or emit DSC as its own step list. Raising them is a `sizeof` change on a static
buffer (`device.cpp:3810` uses `Pipe2::kMaxModeSteps`; `ModeSet::Plan` is stack/static) — check each
allocation site for stack size before bumping.

---

## 3. Timing / mode plumbing (Q3)

### 3.1 The OS-side fields that exist

`lib/MacKernelSDK/Headers/IOKit/graphics/IOGraphicsTypes.h`:

| Offset in `IODetailedTimingInformationV2` | Field | Note |
|---|---|---|
| `0x90` | `dscCompressedBitsPerPixel` | **bpp × 16** (`drm_dsc`/NVKMS convention: 257 = 16.06 bpp) |
| `0x92` | `dscSliceHeight` | lines |
| `0x94` | `dscSliceWidth` | pixels |
| (struct size `0xa0`) | — | asserted already in `src/plugin.cpp:1292` |

Fields `bitsPerColorComponent` (`0x8a`), `colorimetry` (`0x8c`), `dynamicRange` (`0x8e`) sit directly
before them and are already written by the A8 hook — the same function, three more words.

Also present, and relevant later rather than now:

* `struct IODisplayTimingRangeV2` (`:795-893`) with `maxBandwidth`, `dscMin/MaxSliceHeight`,
  `dscMin/MaxSliceWidth`, `dscMin/MaxSlicePerLine`, `dscMin/MaxBPC`, `dscMin/MaxBPP`, `dscVBR`,
  `dscBlockPredEnable`, under `kIOFBTimingRangeKey` (`:1456`). This is the *documented* way to let
  macOS generate DSC detailed timings itself — but see `docs/hdr-timingrange-reverse.md`: this
  build's contract for that property is unproven, and the one attempt at publishing it ended in a
  panic caused by our own log format string (`docs/hdr-30bpp-plan.md:403-431`). **Not part of the
  minimal path.**
* `kIODSCBlockPredEnable = 0x20` in **`signalConfig`** (`:976`) — a PPS-block-prediction hint in the
  timing's signal config, i.e. `VDDetailedTimingRec.csSignalConfig` (`ndrv.hpp:182`). cosmetically
  settable, not required for DSC to work. **[inf]**

There is **no** DSC field in `VDDetailedTimingRec` (`src/ndrv.hpp:176-215`, checked field by field)
and **no** `kDisplayMode…DSC…` mode flag (`IOGraphicsTypes.h:160-186`). So:

> **Reading-time caveat.** This tree was being edited by another agent while this plan was written
> (working tree at `HEAD 213f0da` plus a concurrent `src/ndrv.hpp` change adding the gamma selectors
> of `docs/hdr-gamma.md`). Every `file:line` here was read from that working tree; the gamma change
> does not touch `VDDetailedTimingRec` (`git diff src/ndrv.hpp | grep -c VDDetailedTimingRec` = 0),
> but re-check the line numbers before coding.


> **The only place our driver can tell macOS "this timing is DSC" is the
> `IONDRVFramebuffer::getTimingInfoForDisplayMode` hook in `src/plugin.cpp:1306-1342`**, which
> already stamps three colour words at `v2.bitsPerColorComponent…dynamicRange`. Add
> `v2.dscCompressedBitsPerPixel / dscSliceHeight / dscSliceWidth` there for DSC modes, gated by the
> same trial option, after the original call (`org(...)`), with the same
> `kIODetailedTimingValid` guard that is already there.

### 3.2 Where the cap lives, and what it must become

`device.cpp:2801-2810` (DisplayPort boot display):

```cpp
const bool known = bootDpLinkKnown || readBootDpLink(sink, link);
bootDpLinkKHz = known ? DpTrain::bandwidthKbps(link) / 24 : 0;   // 720000 kHz at 8 bpc, 4×HBR2
lim.maxPixelClockKHz = bootDpMaxKHz();                            // = max(min(720000, DISPCLK 650000), boot 533234)
```

`modes.cpp:52-56` then drops every mode above `lim.maxPixelClockKHz`:

```cpp
if ((lim.maxHActive && t.hActive > lim.maxHActive) || …
    (lim.maxPixelClockKHz && t.pixelClockKHz > lim.maxPixelClockKHz))
    return;
```

That is why 4K120 (1066.56 MHz), 4K144 and 4K240 are filtered out of the 21-mode table while the
EDID already carries them (`docs/hdr-30bpp-plan.md:113`), and why `applyModeRaw` would still refuse
them: `modeset.cpp:224` `if (t.to.pixelClockKHz > t.dpMaxKHz) …` with `t.dpMaxKHz = bootDpMaxKHz()`.

**Shape of the fix (per-mode, not a global raise):**

```
Limits gains:   uint32_t maxPixelClockDscKHz;   // 0 = DSC not offered
Mode gains:     bool dsc;                       // set by the builder, not by the EDID

maxPixelClockDscKHz = min3(
    dispclkTargetKHz(),                                   // 650000 today; the real ceiling to measure
    slicesCommon * sinkThroughputPerSliceMps * 1000,      // 4 slices × 340 = 1360000 kHz
    linkPayloadKbps(bootDpLink) / bppChosen)              // 17280000/8 = 2160000 kHz at 8 bpp
```

with `slicesCommon = {1,2,4} ∩ sink{1,2,4,6,8}` and `bppChosen` = the integer bpp we pick
(8 for 4K120 8 bpc; 10 or 12 for 10 bpc variants). For this machine the binding term is
**`dispclkTargetKHz()`** — i.e. the DSC cap is a *clock* question, not a bandwidth question, and
that is exactly why §6 starts by measuring it.

`Modes::build` then admits a mode when
`t.pixelClockKHz <= lim.maxPixelClockKHz || (dscCapable && t.pixelClockKHz <= lim.maxPixelClockDscKHz)`
and marks `m.dsc = true` in the second case. **Modes below the plain cap stay non-DSC** — nothing
that works today changes behaviour, which is the whole point of doing this per-mode.

Two more plumbing facts that bite at the switch, not at enumeration:

* `device.cpp:3818-3829`: a DP switch to a timing with no Linux request timing in the table is
  **refused** unless the plane's current request timing already delivers lines fast enough. 4K120
  is ~2× the boot raster's line rate, so **`modes.txt` must gain the 4K120/144 timings and
  `pipegen` must be re-run** before the switch can be attempted. This is a hard prerequisite, not
  an optimisation.
* `ModeSet::kMaxSteps = 128` + the DSC block ≈ 30 writes ⇒ plan storage must grow (§2.4).

---

## 4. What exists elsewhere (Q5): precedents, and what does *not* exist

### 4.1 NVIDIA reference — **the precedent that matters**

`ref/nvidia-macos-driver` @ 87e524a, `kexts/NVRMFB/fb/nvrm-fb.cpp:3844-3860`:

```cpp
// NVKMS validated this mode, so above DP 1.4's uncompressed limit (HBR3 x4 = 25.92 Gbit/s payload) it drives it
// with DSC. Without DSC fields macOS judges the mode by its uncompressed bandwidth, declares the display offline and
// the panel goes black (issue #77, RTX 5080 4K@120: 7 offline / 35 virtual framebuffers -> 0 / 0 with these
// values). 257 = 16.06 bpp x16, what NVKMS itself picked there ("EVO-DSC bppX16 257 sliceCount 4"); …
const NvU64 dscLimit = nvrmDscThresholdBps(…);
if (dscLimit && (NvU64)k.pixelClockHz * 24 > dscLimit && !b.dscCompressedBitsPerPixel) {
    const UInt16 slices = k.hVisible >= 3840 ? 4 : 2;
    b.dscCompressedBitsPerPixel = 257;
    b.dscSliceWidth = (UInt16)((k.hVisible + slices - 1) / slices);
    b.dscSliceHeight = (UInt16)k.vVisible;
```

with the threshold `nvrm-fb-safety.h:52-58`: DP/eDP ⇒ `25.92 Gbit/s` (HBR3×4), HDMI 2.1 FRL ⇒
`42.67 Gbit/s`, other connectors ⇒ 0 (no DSC hint at all). Three consequences for us:

1. **macOS does read these three words**, and their absence is what makes it declare a
   4K@120 display *offline* (7 offline displays / 35 virtual framebuffers → 0/0 with the fields
   set). That is the empirical basis for §3.1. **[inf → from NVIDIA's measured issue #77]**
2. NVIDIA's rule is "**the type of the timing decides**": the same raster gets bpp×16 = 0 when the
   link can carry it uncompressed, and a real value when it cannot. We should copy that shape.
3. NVIDIA does **not** program any DSC hardware — NVKMS/the GPU firmware does. We have no such
   layer: on this card everything in §2 is ours to write. That is the real difference in cost
   between the two projects.

`nvrm-fb.cpp:3898` also *clears* the three fields when it clones a timing at a derived refresh rate
(the half/quarter-rate copies), i.e. the fields are per-timing, never inherited.
`publishTimingCaps()` (`:4422-4462`) publishes `IODisplayTimingRange` **V1** (no `dsc*` fields at
all) — so NVIDIA's DSC path needs no timing-range property. That is the precedent for keeping
`kIOFBTimingRangeKey` out of our minimal step.

### 4.2 `archive/AMDRadeonRDNA4` — **no DSC code**

Checked: `grep -ril dsc src/ docs/` in that tree returns only

* `src/pipe2_linux*.inc`, `src/dcn_init_linux.inc` — the same DCN tables this tree has
  (`DSCL*` = the *scaler*, `DSC_TOP*_CONTROL` fgcg, `dcn32_dsc_pg_control`);
* `src/device.cpp` — the same DPCD capability log line;
* `docs/ARCH-apple-transaction-machinery.md:4210,4228,6364-6365` — the names `dsc_find.py`,
  `dsc_dis.py`, `dsc_refs2.py`, `dsc_ins.py`, `dsc_cal.py`. **These are reverse-engineering scripts
  for the macOS *dyld shared cache* ("dsc" = dyld shared cache, not Display Stream Compression)**;
  they read CoreDisplay/SkyLight out of the 4.8 GB cache. Not a DSC precedent.
* `docs/cursor-ttu.md`, `docs/linux-ground-truth.md` — `DSCL*` (scaler) again.

So: **the older project does not contain Display Stream Compression code, and neither does this
one.** `docs/ARCH-apple-transaction-machinery.md:6963` only notes that "pixel clock (~2.1–2.4 GHz)
… on DP 2.1 needs UHBR20 or DSC" as an aside about the platform's pixel-clock ceiling — it is a
useful number for §6 S1 (the hardware *can* clock above 1 GHz; whether the SMU will is separate).

### 4.3 `ref/liberator`, `archive/refs/Cobalt64`

* Cobalt64 has a DCN 4.1 register census (`src/dcn41/dcn41_allow_ranges.h:72,82`) whose ranges cover
  "OPP..DSCRM3" and "DLPC..DSCC3" — i.e. it knows the DSC register blocks exist, but it programs
  none of them (`grep -i dsc` → only `DSCL`/`DSCRM` range names).
* Nothing in `ref/` drives DSC.

**Conclusion for Q5: the only DSC precedent available to this project is NVIDIA's timing-field
trick (§4.1). There is no AMD/macOS DSC driver to copy — the source side has to be built from
Linux's DCN401 code.**

---

## 5. Feasibility and risk ranking (Q4)

### 5.1 What is provably reachable on this link

| Wanted | Needs | Verdict |
|---|---|---|
| **4K60 8 bpc with DSC** (no visible change; a *validation* step) | nothing beyond the DSC chain itself — the raster already fits uncompressed | **safest possible test of the whole chain** — smallest step, §6 S2 |
| 4K60 10 bpc | **no DSC at all**; 15.99 Gbps fits, MSA already 10 bpc | the HDR track's job, not this one (§0.3) |
| **4K120 8 bpc with DSC** | 4 slices × 340 = 1360 Mpix/s ≥ 1066.56; link at 8 bpp = 8.53 Gbps; **DISPCLK ≥ 1066.56 MHz** (today 650) | **feasible iff the SMU grants the clock** — S1 then S4 |
| **4K120 10 bpc with DSC** | 12 bpp → 12.80 Gbps (fits, 26 % margin); same 4 slices, same clock | feasible after S4 |
| **4K144 8 bpc with DSC** | 12 bpp → 16.00 Gbps (7 % margin); 4×340 = 1360 ≥ 1333 ✓; DISPCLK ≥ 1333 MHz | plausible; needs S1 at 1333 — S5 |
| **1440p240 8 bpc with DSC** | 12 bpp → 11.95 Gbps; 1360 ≥ 995.5 ✓; DISPCLK ≥ 995.5 MHz | an easier high-refresh win than 4K120 if 4K120's request timing is missing |
| **4K240** | 2418 Mpix/s needs ≥ 8 slices at 340 Mpix/s/slice; **the DCN401 encoder offers at most 4** (`dcn401_dsc.c:71-74` sets `NUM_SLICES_1..4` only) ⇒ 4×340 = 1360 ≪ 2418; and DISPCLK ≈ 2418 MHz | **not possible on this hardware**, DSC or not. Say it once, drop it |

### 5.2 The smallest step that produces a measurable win, in order

**S0 (measurement only)** → **S1 (clock ceiling)** → **S2 (DSC on at the boot raster, 4K60 8 bpc)**
→ **S3 (the first *higher-refresh* mode the measured clock allows — 1440p240 or 4K120)** →
**S4 (4K120, the headline)** → S5 (4K120 at 10 bpc / 4K144) optional.

S2 is the smallest step that *proves* anything: it changes no raster, no clock, no link rate, and if
the PPS or the register block is wrong the screen either stays correct (compression is transparent)
or goes black and the next boot drops the option. S1 is not optional before S3/S4 because without a
measured DISPCLK ≥ the target raster every hour spent on 4K120 is wasted.

Note what is **not** on this list: 4K60 at 10 bpc. It needs no DSC (§0.3) and belongs to the HDR
track; bundling it in would only make two independent mechanisms fail at once.

### 5.3 What can break, per step, and the way back

| Failure | Symptom | Which step | Rollback |
|---|---|---|---|
| Encoder programmed but sink not enabled | picture freezes/tears at the switch, `DSCC_STATUS` stuck | S2-S5 | trial option (a `rdna4-*` option in `rdna4-trial` is cleared before it acts, `src/trial.cpp`); reboot without it → plain 4K60 |
| PPS wrong (slices/bpp/linebuf) | **green/pink garbage or black**, often with the link still "up" | S2-S5 | same: option off next boot. Do **not** put the DSC path in the default boot |
| `DP_DSC_ENABLE` written to a sink that silently ignores it | sink shows an uncompressed-looking but wrong picture, or nothing | S2 | read `0x160` back (must read 1) before unblanking; abort the switch if not |
| DISPCLK too low for the new raster | DCN underflow → flicker/black bars, `OTG underflow` in the log | S3, S4 | the switch is refused before it starts if `dispclkTargetKHz() < pixelClockKHz`; keep the boot mode as the fallback |
| Kernel panic in the switch path | machine down | any | the plan limits: keep every new register write inside the existing `applyMode` transaction (already panic-free on this card), and **audit every new `rdna4Log` format string** — the 2026-11-26 panic was a `%s` fed an integer (`docs/hdr-30bpp-plan.md:403-431`) |
| WindowServer assert / display declared offline | black screen, WS restart loop; `.ips` in `/Library/Logs/DiagnosticReports` | S3/S4 (the mode becomes visible to CoreDisplay) | remove the mode from the table (option off) — the display goes back to 4K60 which CoreDisplay accepts today |
| Link retrain storm | AUX errors, "training failed" rings, no picture | *should not happen* | do **not** retrain for DSC; if a step needs a retrain, stop and reconsider |

**Blast radius by construction:** every step is behind a boot-armed trial option, the option is
consumed before acting, and nothing changes the *default* mode table. The one irreversible-ish risk
is a WindowServer restart loop, which the project treats as "reboot and drop the option" — and
restarting WindowServer by hand is already forbidden on this machine (AGENTS.md §五).

### 5.4 What I could not verify (see §7 for the full list, this is the short version)

* Whether the SMU will grant DISPCLK ≈ 1070 MHz (no log on this machine shows DISPCLK above 650 as
  a *granted* value; the "2.1–2.4 GHz" in `archive/…/ARCH-apple-transaction-machinery.md:6963` is
  about the platform, not a measured DALSMC grant).
* Whether **this** sink actually demodes a DSC stream at all — its DPCD *says* yes, but it also
  reports DPCD 1.2, and no Linux/macOS/Windows run on this panel is recorded in the tree.
* Whether the DSC block emits the PPS SDP itself on DCN 4.01, or whether a DIO register we have not
  found must be told (no such register exists in the DCN 4.1.0 header — that is evidence, not
  proof).
* What `OPTC_DSC_MODE`'s enum values are for "4 slices" (`enum optc_dsc_mode`, `dc/dsc/dc_dsc.h`;
  the DCN401 header gives only the 2-bit field).
* Whether macOS accepts a 4K120 mode from an **IONDRV** framebuffer with `dsc*` set — NVIDIA did it
  from an `IOFramebuffer` subclass where it also controlled the mode list. This is the single
  biggest unknown of the whole plan and it is *cheap to test* (§7 U1).

---

## 6. The plan, as an ordered checklist

**Start here: S1.** It is one trial boot, needs no code, and answers the only question that can
stop this plan outright (can the SMU give DISPCLK ≥ 1066.56 MHz?). S0 is a half-day of logging that
makes every later number checkable and should ride along with it.

Every step: an acceptance criterion that can be read off a log, an evidence pointer, and a rollback
that needs no physical access. All code lands behind a trial option; nothing is enabled by default until S2 is
green on the card. Follow `CONTRIBUTING.md` (branch `devel/dsc`, three-part entry in
`/home/Celia/rdna4/Changelog.md` before/after, `-j2` builds only).

### S0 — Read the sink properly (no behaviour change)

* [ ] Extend the existing DPCD dump (`device.cpp:3142-3152`) into a decoded log line per
      DP 1.4 field: revision, buffer block/size, slice caps, line-buffer depth, block-pred,
      colour formats/depths, peak throughput per slice, max slice width, bpp increment.
      Also read `0x160` (current `DP_DSC_ENABLE`), `0x161`, and `0x000`/`0x001` for the rev byte.
* [ ] Add the same decode to `test/atomdump.cpp`'s host harness over a captured DPCD fixture, so the
      decode is testable without the card.
* [ ] Log a single verdict line: `dsc: sink caps: v1.2, slices {1,2,4,6,8}, max slice 2560,
      lb 16bpc, blkpred, throughput 340 Mpix/s/slice, bpp/1 -> 4K120 needs 4 slices`.
* **Acceptance**: the line appears once per boot on AUX0 and the values match §1.1 exactly; the
  decode agrees with `dc_dsc_parse_dsc_dpcd` field for field; `make test` passes.
* **Evidence**: §1.1 (the decode table), `device.cpp:3142-3152` (what is read today),
  `dc_dsc.c:701-770` (the decode to mirror).
* **Rollback**: none needed (log only). **Risk**: zero.
* **Why first**: the brief's summary of these bytes was wrong about the revision and about where the
  PPS goes (§0.1, §1.2), and every later step keys off them.

### S1 — Measure the DISPCLK ceiling (no display change)

* [ ] Trial boot: `sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial="armed rdna4-dispclk=1070"`
      then reboot (the option is consumed and cleared by `src/trial.cpp`), read
      `sudo sysctl -n debug.rdna4_log | grep -i 'hard minimum\|DISPCLK\|divider'`.
* [ ] If 1070 is refused, bisect down (1000, 900, 800) in later trial boots; note the highest
      accepted value and whether `DENTIST_DISPCLK_CNTL`'s divider can express it (`clk.cpp:292`).
* **Acceptance**: a log line stating DISPCLK ≥ 1066.56 MHz was *granted* (not asked for), plus the
  desktop still correct at 4K60 (raising DISPCLK must not change the raster — the OTG's pixel clock
  comes from the DP DTO: `readDpDto` in `device.cpp`, `scaleDpDto` at `modeset.hpp:157`).
* **Evidence**: `clk.cpp:109-111` (650 MHz today), `clk.cpp:332-390` (how a hard minimum is asked
  for and logged), `src/trial.cpp` (why the option cannot outlive the boot).
* **Rollback**: the trial variable does not survive the boot; reboot without it.
* **Risk**: low — a clock raise with an unchanged mode. If the picture breaks, the next boot has no
  option. This is the measurement that decides whether 4K120 is even on the table.

### S2 — DSC on at the boot raster (4K60, 8 bpc, 2 slices) — **the smallest viable step**

* [ ] `tools/gen/pipegen`: give the fake stream `flags.DSC = 1` + `dsc_cfg` (version 1.2, RGB,
      8 bpc, 2 slices × 1920, 8 bpp, linebuf 13, block-pred on) and record the DSC writes into a new
      table (DSC_TOP/DSCC/DSCCIF/DSCRM/ODM OPTC/DP_PIXEL_FORMAT + the 23 PPS words).
* [ ] Kext: a `rdna4-dsc=1` trial option that, in `applyModeRaw` (or a dedicated path), executes those
      steps + `Op::Aux` for `DPCD 0x160 = 1`, reads `0x160` back, and only then unblanks. Program the
      DSC instance that belongs to this pipe's OPP.
* [ ] Log: the PPS words, `DSCC_STATUS` before/after, `DSCRM` forward-enable status, and the
      `0x160` readback.
* **Acceptance**: desktop is **correct** (not just "not black") at 3840×2160@60 with DSC enabled on
  a 4-lane HBR2 link that was never retrained; `0x160` reads `1`; `DSCRM_DSC_FORWARD_EN_STATUS` = 1;
  no underflow lines; `debug.rdna4_log` shows the DSC block programmed. Option off ⇒ byte-identical
  behaviour to today.
* **Rollback**: option off (next boot is plain).
* **Evidence**: §2.2 (programming order), §2.1 (addresses), §1.2 (`0x160`).
* **Risk**: medium — this is the first time the encoder, OPTC and sink all get told at once. Failure
  mode is a black screen at the next boot *only if the option is defaulted on*; keep it off.
* **Recovery note**: a black screen with SSH alive is a normal reboot (the AGENTS.md ≥10-minute rule
  is about *boots*, not about this); a Kernel panic leaves a report in
  `/Library/Logs/DiagnosticReports/Kernel-*.panic` — read `CR2`/`RAX` before anything else.

### S3 — the first higher-refresh mode with DSC (raster changes, link does not)

Which mode is chosen is decided by S1: take the fastest of the EDID's DisplayID timings whose pixel
clock the measured DISPCLK covers — 2560×1440@240 (995.52 MHz, 19.11 Gbps uncompressed → needs DSC at
≤17.3 bpp, so 12 bpp = 11.95 Gbps) or 3840×2160@120 (1066.56 MHz, 8 bpp = 8.53 Gbps).

* [ ] `tools/gen/pipegen/modes.txt` gains that timing (from the EDID — totals from the DisplayID
      detail, never invented), pipegen re-run, `pipe2_linux_p0_dp1_uhd.inc` regenerated so
      `Pipe2::modeKnown()` is true and `device.cpp:3818-3829` stops refusing the switch.
* [ ] DSC instance for the pipe's OPP, 4 slices, `OPTC_DSC_*`, `DP_PIXEL_FORMAT`, `DPCD 0x160 = 1` —
      all from the S2 mechanism, with the S3 raster's PPS.
* **Acceptance**: the panel really is at the new rate (its own on-screen info / a 120 Hz motion test,
  not just our log); picture correct; no underflow; no AUX error; `0x160` reads 1.
* **Evidence**: §3.2 (cap and per-mode flag), `device.cpp:3818-3829` (the request-timing refusal
  that must be cleared first), `Pipe2::modeKnown` (`pipe2.hpp:168-172`).
* **Rollback**: option off ⇒ the 21-mode SDR table and the boot mode come back; reboot if WS asserts.
* **Risk**: medium-high — first raster change through the DSC path. This is where the request timing
  (DLG/TTU/DET) and the clock meet the compressed stream.

### S4 — 4K120 with DSC (the headline goal) and its OS-side gate

* [ ] S1 shows DISPCLK ≥ 1066.56 MHz grantable; S3 has shown a DSC raster change working.
* [ ] `Modes::Limits::maxPixelClockDscKHz` + `Mode::dsc` (§3.2); the 650 MHz cap stays for SDR modes.
* [ ] `plugin.cpp` `wrapGetTimingInfoForDisplayMode`: stamp `dscCompressedBitsPerPixel = 128`
      (8 bpp × 16), `dscSliceWidth = 960`, `dscSliceHeight = 2160` for `m->dsc` modes, under the same
      option, same `kIODetailedTimingValid` guard.
* **Acceptance**: (a) `modes:` log shows the raised cap and the 4K120 entry; (b) WindowServer offers
  and switches to it (`cscSwitchMode` with the new ID) and the three DSC words appear in the
  `getTimingInfoForDisplayMode` log; (c) the panel is at 120 Hz with a correct picture; (d) no
  underflow, no AUX error, no retrain.
* **Note**: if S3 already reached 4K120, S4 degenerates to the OS-side half only — which is exactly
  the experiment U1 (§7) and should be run **before** the hardware half, because it is one boot and
  no code.
* **Evidence**: §3.1 (the three words and their offsets), §4.1 (NVIDIA's measured outcome), and
  §7 U1 (run this half first, it is one boot and no code).
* **Rollback**: option off ⇒ mode not offered ⇒ nothing selectable.

### S5 (optional) — 4K120 at 10 bpc, and 4K144

Same mechanism, different bpp: 12 bpp for 4K120@10bpc (12.80 Gbps) and for 4K144@8bpc (16.00 Gbps,
7 % margin — the tightest case in this plan). Only after S4 is green, and only if the DISPCLK S1
measured covers 1333 MHz.

### Cross-cutting, before any of it

* [ ] `ModeSet::kMaxSteps` 128 → 192 and `Pipe2::kMaxAux` 4 → 8, checking every allocation site
      (`device.cpp:3810`, `:4966`) for stack budget.
* [ ] A "DSC off" path that is exercised: from a DSC mode, switch back to 4K60 SDR and watch
      `0x160` go to 0 and the encoder disable without a black frame.
* [ ] Every new `rdna4Log` reviewed for format/argument agreement (the 2026-11-26 panic).

---

## 7. Unknown / needs a measurement

| # | Unknown | Why it matters | Cheapest measurement |
|---|---|---|---|
| **U1** | Does **macOS** accept a 4K120 mode from the IONDRV route with `dsc*` set, or does CoreDisplay declare the display offline? | This decides whether the whole plan is worth S4 | Offer a 4K120 entry **without** touching hardware: raise only the table cap + stamp `dsc*` in the hook, keep the pipe at 4K60, and watch whether `cscSwitchMode` ever carries the new ID (the A1/A2/A3 method: enumeration first, hardware later). Costs one boot, no DSC code |
| **U2** | Will the SMU grant DISPCLK ≈ 1070/1333 MHz? | 4K120/144 are impossible without it | S1 (`rdna4-dispclk=1070`) |
| **U3** | Does the DCN401 DSC block emit the PPS SDP itself, or is a DIO register needed? | If not, no picture even though the sink is enabled | S2 with the sink enabled and a known-good PPS; if the sink shows garbage while `DSCC_STATUS` latched, suspect the SDP. Cross-check by recording what Linux writes for a DSC stream in pipegen (it would show any extra register) |
| **U4** | The `enum optc_dsc_mode` value for "4 slices" (`dc/dsc/dc_dsc.h`) | Wrong value = encoder sits in single-slice mode | read the enum in a Linux tree, or take the recorded value from pipegen |
| **U5** | Is `DSCCLK` = DISPCLK/3 (so 355 MHz at 4K120), and does DCCG's DSCCLK DTO need DISPCLK's parent PLL? | If DSCCLK is wrong the encoder underruns even when the PPS is right | record `dccg401_set_dscclk` in pipegen; or probe the DSCCLK DTO registers read-only on the card (`seg1 0x6c..0x6e`, `0xa7`) and compare with what the firmware left |
| **U6** | Does this panel really decode DSC, given it reports DPCD **1.2** while carrying DP 1.4 DSC caps? | Everything here assumes yes | S0's readback + S2's picture. A Linux live-USB run on the same cable would settle it independently (not available in this session) |
| **U7** | For 4K120 at 10 bpc (S5), is a wrong picture the sink's DSC colour-depth path or the *plane* format? | the two failure modes are indistinguishable otherwise | run the HDR track's 10 bpc plane test at 60 Hz first (`docs/hdr-30bpp-plan.md:209-240`), so only the stream is new when S5 runs; log the `0x06A` decode |
| **U8** | Does the boot display's DSC instance = its OPP instance? | Writing the wrong `DSCCi`/`DSCRMi` does nothing (or breaks another pipe) | read `DSCRM{0..3}_DSC_FORWARD_CONFIG` read-only and correlate with `pipe.opp`; the `pipe` discovery already knows OPP (`src/pipe.hpp`) |
| **U9** | `IODisplayTimingRangeV2` + `kIOFBTimingRangeKey` as the *documented* route for DSC timings (instead of stamping `dsc*`) | If U1 fails this becomes Plan B | `docs/hdr-timingrange-reverse.md` first — the contract is still unproven and the last attempt panicked for an unrelated-but-real reason |

---

## 8. Where every claim comes from (so it can be re-checked)

### In this tree (read, not modified)

| Claim | File:line |
|---|---|
| DPCD 0x060..0x06F read + log; boot DP link sink parse | `src/device.cpp:3114-3170` |
| Mode cap computed (link ÷ 24 = 8 bpc pixel clock; min with DISPCLK) | `src/device.cpp:2801-2810`, `:3115-3121` |
| `Limits::maxPixelClockKHz` filter | `src/modes.cpp:52-56`, `src/modes.hpp:61-67` |
| Switch refused when `pixelClockKHz > dpMaxKHz` | `src/modeset.cpp:224`, `src/device.hpp:286-288` |
| DP switch refused without a Linux request timing | `src/device.cpp:3818-3829` |
| Plan step limits / Aux writes | `src/modeset.hpp:78-95`, `src/pipe2.hpp:135-137` |
| OPTC/OTG and DPCD step execution | `src/device.cpp:3071`, `:4028` |
| DISPCLK/DPPCLK hard minimum 650 MHz, `rdna4-dispclk` option | `src/clk.cpp:109-111`, `smuSetHardMin` `:332`, `raiseDisplayClocks` `:367`, `dispclkTargetKHz` `:357` |
| DSC block already power/clock-gated at init (no programming) | `src/dcn_init_linux.inc:49-52`, `:169-184` |
| `getTimingInfoForDisplayMode` hook that stamps colour words | `src/plugin.cpp:1286-1342` |
| `VDDetailedTimingRec` has no DSC field | `src/ndrv.hpp:166-205` |
| `IODetailedTimingInformationV2` DSC fields at 0x90/0x92/0x94, size 0xa0 | `lib/MacKernelSDK/Headers/IOKit/graphics/IOGraphicsTypes.h:465-519`, `src/plugin.cpp:1292` |
| `IODisplayTimingRangeV2.dsc*` + `kIOFBTimingRangeKey` | `IOGraphicsTypes.h:795-893`, `:1456`, `:976` |
| Sink DPCD dump (the 16 bytes + link + cap lines) | `docs/hw-logs/2026-11-25-dpcd-dsc.txt:8-10` |
| Previous gates/limits for HDR work | `docs/hdr-30bpp-plan.md:105-140`, `:403-431` |

### Linux (fetched read-only from `raw.githubusercontent.com/torvalds/linux/master`, 2026-11-26; hashes of nothing pinned — the revision is "master at fetch time", which is why the plan says "re-check the numbers before coding")

| File | Used for |
|---|---|
| `include/drm/display/drm_dp.h` | DPCD DSC field map (`:244-371`), `DP_DSC_ENABLE 0x160` (`:755`), `DP_DSC_CONFIGURATION 0x161` (`:758`) |
| `drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_offset.h` (1.69 MB) | every DCN 4.01 register address in §2.1 |
| `…/dcn_4_1_0_sh_mask.h` (15.6 MB) | every field shift/mask in §2.1-2.2 |
| `…/display/dc/dsc/dcn401/dcn401_dsc.c` | DCN401 DSC enable/disable/set_config order, encoder caps (`:69-92`), PPS write (`:216-408`) |
| `…/display/dc/dsc/dcn20/dcn20_dsc.c` | `dsc_prepare_config` (`:387-476`) — the PPS arithmetic in §2.2; `dsc2_get_packed_pps` (`:223-241`); `dsc2_enable` (`:244-266`) |
| `…/display/dc/dsc/dc_dsc.c` | sink capability decode (`:701-770`), throughput per slice (`:1212`, `:1870`) |
| `…/display/dc/dsc/dscc_types.h`, `rc_calc.c` | `struct dsc_parameters`, `calc_rc_params` (the RC model inputs to the PPS) |
| `…/display/dc/optc/dcn20/dcn20_optc.c` | `optc2_set_dsc_config` (`:127-142`) → the three OPTC registers |
| `…/display/dc/optc/dcn30/dcn30_optc.c` | `optc3_set_dsc_config` (`:184-199`) and the DCN401 vtable's use of it |
| `…/display/dc/dio/dcn401/dcn401_dio_stream_encoder.c` | `DP_PIXEL_FORMAT` DSC fields (`:537-586`), `dp_set_dsc_pps_info_packet` wiring (`:763-764`) |

### Reference projects (read-only)

| Claim | File:line |
|---|---|
| macOS needs the three `dsc*` words or it declares a 4K@120 display offline | `ref/nvidia-macos-driver/kexts/NVRMFB/fb/nvrm-fb.cpp:3844-3860` (issue #77) |
| bpp is ×16 (`257` = 16.06), slices = 4 above 3840 wide, slice height = full frame | same block |
| DSC hint threshold: DP 25.92 Gbps, HDMI 2.1 42.67 Gbps, else none | `ref/nvidia-macos-driver/kexts/NVRMFB/fb/nvrm-fb-safety.h:52-58` |
| NVIDIA publishes timing range **V1** (no dsc fields) — DSC needs no range property | `nvrm-fb.cpp:4422-4462` |
| `archive/AMDRadeonRDNA4` has no DSC code; its `dsc_*.py` are dyld-shared-cache tools | `archive/AMDRadeonRDNA4/docs/ARCH-apple-transaction-machinery.md:4210,4228,6364-6365` |
| The platform's pixel clock is good for ~2.1-2.4 GHz; DP 2.1 needs UHBR20 or DSC above that | same file, `:6963` |

---

## 9. One-paragraph summary for the changelog

> DSC is real on this sink, and it is what any refresh rate above 60 Hz at 4K needs (4K60 at 10 bpc
> does **not** need it — 15.99 Gbps fits the 17.28 Gbps payload; DSC is a refresh-rate tool here,
> not a bit-depth one):
> the sink reports DSC 1.2, integer bpp, slice counts {1,2,4,6,8}, 340 Mpix/s per slice, 2560 px max
> slice width, 16-bit line buffer, block prediction — which makes 4K120 need exactly 4 slices of
> 960 px at 8 bpp (8.53 Gbps of a 17.28 Gbps link), and 4K240 impossible on any settings
> because the encoder offers only 4 slices. The source side is entirely ours: DCN401 has a
> full DSC block (seg2 `0x3000`… , `DSCC0..3` at `0x300a`, `DSCRM0..3` at `0x1a64`, OPTC
> `OPTC_DSC_*` at `0x1acc`…, `DP_PIXEL_FORMAT` bit 0), the kext writes none of it today, and Linux's
> `dcn401_dsc.c` gives the exact order. The OS side is one hook we already own
> (`getTimingInfoForDisplayMode`, three words at `0x90/0x92/0x94`) with an NVIDIA precedent that
> proves macOS reads them. The gating unknown is not bandwidth but **clock**: 4K120 needs
> DISPCLK ≥ 1066.56 MHz and the kext asks the SMU for 650. Smallest viable step: turn DSC on at the
> **existing** 4K60 raster (`DPCD 0x160 = 1` + the DSC block + 2 slices at 8 bpp) and keep the
> desktop correct — no raster change, no clock change, no retrain, and a trial option as the way back.
