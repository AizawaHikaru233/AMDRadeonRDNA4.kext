# DSC S0 + S2 on DCN 4.01 (RX 9070 XT, gfx1201): what was built, and how to accept it

Status: **code + generated table + host tests done and build-verified; not yet run on the card.** This file is the
boot-time checklist for the first card test. It follows `docs/dsc-4k120-plan.md` (the specification) and does not
change it.

Nothing here installed, rebooted, or touched the ESP/`/Library`/NVRAM. The Mac was used only to build
(`~/rdna4/a4-dsc`, `make -j2 kext`).

---

## 1. What changed

| Path | What |
|---|---|
| `src/dsc.hpp`, `src/dsc.cpp` | The sink's DPCD 0x060..0x06f block decoded as Linux's `dc_dsc_parse_dsc_dpcd` does (DSC revision, RC buffer block/size, slice caps, line-buffer depth, block prediction, colour formats/depths, per-slice throughput, max slice width, bpp increment, `0x160`). The 4K120 verdict (`need()`). The generated register table consumer, with the instance strides (`DSC` 0x5c, `DSCRM` 1, `OPTC` 0x10, `DIG` 0x124). Freestanding: no IOKit, no libc formatting. |
| `src/dsc_linux.inc` | **Generated** by `tools/gen/pipegen` (`dsc 2 8` + `mkdsc.py`) from Linux `6addb4f38557`: 96 steps of Linux's own DCN 4.01 DSC code — the PPS SDP (`enc3_dp_set_dsc_pps_info_packet`), DSCCLK DTO, `DSC_TOP`/`DSCC`/`DSCCIF`/`DSCRM`, the 23 `DSCC_PPS_CONFIG` words, OPTC's three registers, and the sink's `DPCD 0x160 = 1`. Metadata: 3840×2160, 2 slices × 1920, 8 bpp, RGB 8 bpc. |
| `tools/gen/pipegen/pipegen.c` | New `dsc N BPP` scenario: gives the fake stream `flags.DSC`, a `dc_dsc_config`, and runs Linux's DSC chain (`link_set_dsc_pps_packet` → `set_dto_dscclk` → `dsc_set_config` → `dsc_enable` → `set_dsc_config`). |
| `tools/gen/pipegen/mkdsc.py` | Turns the DSC part of a trace into `src/dsc_linux.inc`; refuses a trace whose chain is incomplete, whose PPS is not 23 words, or that touches a register outside the DSC chain. |
| `tools/gen/pipegen/run.sh` | Emits `src/dsc_linux.inc`; also `shim/dsc_pps_pack.c` (Linux's `drm_dsc_pps_payload_pack` + `drm_dsc_compute_rc_parameters`, copied MIT, for the host recorder). |
| `tools/gen/pipegen/linux-sources.txt`, `linux-noops.txt` | The DSC sources the recorder now links (`dc_dsc.c`, `dcn20_dsc.c`, `rc_calc.c`, `rc_calc_dpi.c`, `dml/dsc/rc_calc_fpu.c`) and the no-op FPU/assert shims. |
| `src/device.cpp`, `src/device.hpp` | **S0**: `readSinkDsc()` runs once a boot for a DisplayPort boot sink and prints the raw block, the decoded caps + 4K120 verdict, and the detail line; it keeps the decode for S2. **S2**: `tryEnableDsc()` behind `rdna4-dsc=1` (trial option), and `disableDsc()` behind `debug.rdna4_dsc = 0`. Registry property `AMDRadeonRDNA4,Dsc` = 1/0. |
| `src/kext.mk`, `Makefile` | `dsc.o` in `USER_OBJS`; `src/dsc.cpp` in the host test. |
| `test/atomdump.cpp` | `testDsc()`: decodes the captured DPCD fixture (the machine's own block), checks every field and the 4K120 verdict, and checks the generated table (96 entries, metadata, 23 PPS words and their values, block counts, instance strides, `0x160` last). |

The option is off by default. With it off, the visible change is the S0 log lines and two AUX reads at attach. The
one register write the plain boot can make is defensive: if a previous DSC boot left the sink's `DP_DSC_ENABLE`
(`0x160`) at 1, init writes it back to 0 and reads it back, because the plain path sends an uncompressed stream;
on a sink that was never told DSC (the normal case) nothing is written.

---

## 2. Three things the generator found that the plan did not have

These matter for the first card test, and they are the first things to look at if the picture is wrong.

1. **The DSC block does not send the PPS SDP; the stream encoder does.** On DCN 4.01 the 128-byte PPS goes out as
   DP generic packet 11 (`VPG` 11..14, `DP_GSP11_CNTL`, `DP_MSA_VBID_MISC`, `DP_SEC_CNTL2.DP_SEC_GSP11_PPS`,
   `DP_SEC_CNTL.DP_SEC_STREAM_ENABLE`) — `enc3_dp_set_dsc_pps_info_packet`. The plan's U3 asked whether the DSC
   block emits it; it does not. A table with only `DSCC_*`/`DSCRM` would leave the sink with no PPS: `DSCC_STATUS`
   would latch and the screen would still be wrong. All of those writes are in `src/dsc_linux.inc` (59 DIG entries).
2. **The sink's RC buffer is 8 MiB, not 8 KiB.** DPCD `0x062` = 3 means **64 KiB** blocks (Linux:
   `DP_DSC_RC_BUF_BLK_SIZE_64` → `64 * 1024`), and `0x063` = 0x7f means 128 blocks, so
   `rc_buffer_size = 8 MiB`. The plan's §1.1 row ("64-byte blocks; 128 blocks ⇒ 8192-byte buffer") multiplied the
   wrong units; the 8192 is the PPS's `rc_model_size`, which is a different number. The decode follows Linux.
   Consequence for S2: none (the sink's buffer is larger than the model), but the S0 log now states the right
   number, so the books are straight.
3. **Linux writes `OTG_V_SYNC_A_CNTL.OTG_V_SYNC_MODE = 0`** as part of `optc3_set_dsc_config` (the HDMI-FRL branch
   is not taken for this sink, but the register is still written). The table keeps it; it is a no-op at value 0.

---

## 3. Boot-time acceptance criteria (run by hand)

### 3.1 S0, every boot (no option needed)

The boot sink must be DisplayPort. In `sudo sysctl -n debug.rdna4_log`:

```
dsc: sink DPCD 0x060..0x06f (AUX0): 01 21 03 7f 3b 07 01 00 00 1f 0e 11 08 00 00 04
dsc: sink caps: v1.2, slices {1,2,4,6,8}, max slice 2560, lb 16 bpc, blkpred 1, throughput 340/340 Mpix/s a slice, bpp step 1/1, 0x160 0x00; 4K120 needs 4 slices (the encoder and sink offer 4), 8 bpp = 8.53 of 17.28 Gbps
dsc: sink detail: decomp 1 pass-through 0 dynamic PPS 0; rc buffer 64 KiB blocks x 128 = 8388608 B; ...
```

If the previous boot ran S2, a line before those says the leftover bit was cleared:

```
dsc: the sink was left with DP_DSC_ENABLE = 0x01 (an earlier DSC boot): written 0, read back 0x00 (cleared)
```

Accept when the 16 bytes match the earlier dump, the caps line says `v1.2`, `{1,2,4,6,8}`, `340/340`, `bpp step
1/1`, `4K120 needs 4 slices`, and the detail line says `8388608 B` and `depths 0x0e (8 1 10 1 12 1)`. `0x160` is
the live value: `0x00` on the plain path, `0x01` after a successful S2 boot.

### 3.2 S2: one trial boot

```bash
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial="armed rdna4-dsc=1"
sudo reboot
```

`src/trial.cpp` consumes the variable before any option acts: if the boot does not come back, the next boot is
plain without anyone touching the machine.

After the desktop is up (≥ 10 minutes on a boot with a changed kext: AGENTS.md §一):

```bash
sudo sysctl -n debug.rdna4_log | grep -E 'dsc:|DPCD 0x160|DSCC_STATUS|DSCRM|modes: now'
sudo sysctl -n debug.rdna4_dsc            # 1
ioreg -r -d 1 -w0 -l | grep 'AMDRadeonRDNA4,Dsc'   # 1
```

Expected lines, in this order (values from the table; the register read-backs are the acceptance):

| Line | Expected |
|---|---|
| `dsc: DSCC_STATUS 0x… (pending clear)` | pending bit 0 cleared within 100 ms of the PPS writes |
| `dsc: DSCRM_DSC_FORWARD_CONFIG 0x…: forward en 1, en status 1, opp 0` | **both** forward enable and its status are 1, OPP 0 |
| `dsc: DPCD 0x160 wrote 1, read back 0x01 (DSC on)` | the sink's own read-back, not just the write |
| `dsc: ON at the 3840x2160 raster: 2 slices of 1920 px, 8 bpp, RGB 8 bpc; stream running (0x…); sink 0x160 = 0x01` | the final verdict |
| `dsc: DSC_TOP_CONTROL … DSCC_CONFIG0 … DSCC_CONFIG1 … DSCCIF_CONFIG0 …` | `DSCC_CONFIG0` low nibble = 1 (2 slices − 1), bits[11:8] = 0xf (reset at EOL), bit 8 = 1 (alternate ICH); `DSCC_CONFIG1` = the RC buffer model size |
| `dsc: OPTC_DATA_FORMAT_CONTROL … OPTC_BYTES_PER_PIXEL 0x10000000 … OPTC_WIDTH_CONTROL … DP_PIXEL_FORMAT …` | OPTC DSC mode = 1 (444), bytes/pixel = 1.0 in u3.28, slice width 1920; `DP_PIXEL_FORMAT` bit 0 = 1 |
| `dsc: PPS 0..11: …` and `dsc: PPS 12..22: …` | the 23 words of §1.1's table; PPS0 `0x8d000002`, PPS1 low byte `0x80` (8 bpp × 16), PPS2/PPS3 the 3840×2160 / 1920×2160 pair |

Any of these not as stated means **do not** call S2 green:

* `dsc: FAILED (…) at 0x…, read 0x…: undoing the block` — the read-back check rejected it; the code has already
  written `0x160 = 0`, turned the compressor and OPTC DSC off, restored `DP_PIXEL_FORMAT`, and restarted the
  stream. The picture must be the plain 4K60 desktop. The `read 0x…` names the value that failed.
* `dsc: not enabled: …` — a precondition (sink caps, raster, pipe) refused before anything was written.

Also expect **no** `dsc: off` line (that is the rollback path), no `modes: switch` to another mode, no
`underflow`/`OTG underflow`, no AUX error, no retrain, and the measured refresh still ~59.99 Hz (`modes: now
3840x2160@59.99x, measured …` if a mode set logged it).

### 3.3 What "correct picture" means

The stream is still 3840×2160@60 on 4 HBR2 lanes that were never retrained; only the bits on the wire changed.
Accept the picture when **all** of these hold:

1. The desktop is the same 3840×2160 layout as the plain boot (no scaling, no black border, no offset).
2. A full-screen gradient (or a photo) shows no green/pink/blocky regions and no tiled/patchwork areas — the
   classic wrong-PPS signature; no repeated horizontal bands.
3. Text is crisp at the same size as before, and a 1-pixel checkerboard test image does not shimmer or crawl.
4. The panel's own OSD/info still says 3840×2160@60 (or 59.9 Hz), and the machine stays up for at least 10 minutes
   with the desktop in use.
5. `pgrep WindowServer` is one stable process (no restart loop); no new `WindowServer`/CoreDisplay `.ips` in
   `/Library/Logs/DiagnosticReports`.

A correct picture with DSC on is the S2 pass. "Not black" alone is not enough: the failure mode this step exists to
find is a wrong PPS that still lights the panel.

### 3.4 If it is wrong: rollback (one command, no reboot needed)

```bash
sudo sysctl -w debug.rdna4_dsc=0
```

That runs `disableDsc("debug.rdna4_dsc = 0")`: stops the stream, writes `DPCD 0x160 = 0` and reads it back, turns
`DSCRM` forward-enable and `DSC_TOP_CONTROL.DSC_CLOCK_EN` off, sets OPTC `OPTC_DSC_MODE = 0`, stops the PPS SDP
(`DP_GSP11_CNTL`, `DP_SEC_CNTL2`, `DP_SEC_CNTL`), restores `DP_PIXEL_FORMAT`, and restarts the stream. The log
line is `dsc: off (debug.rdna4_dsc = 0): sink 0x160 = 0x00 (acked), …`.

If the screen is black and SSH is not reachable, the trial variable is already consumed: **power-cycle and boot
without the trial**; the next boot is the plain 4K60 desktop. (AGENTS.md's ≥ 10-minute rule applies to a boot that
looks hung; a black picture with a live SSH is a normal reboot.) A panic would leave a report in
`/Library/Logs/DiagnosticReports/Kernel-*.panic`: read `CR2`/`RAX` first; every new `dsc:` line uses only
`%u`/`%d`/`0x%x` (and `%s` only for real strings), because the 2026-11-26 panic was a `%s` fed a number.

---

## 4. What was verified, and what was not

Verified on this Linux host:

* `make -j2 CXX=g++ CC=gcc test`: the DSC test passes; the only remaining failure is the pre-existing
  `navi44: gfx images` one (no navi44 firmware blobs in this tree), unrelated to DSC.
* `tools/gen/pipegen` built (gcc, at Linux `6addb4f38557`) and its `dsc 2 8` scenario recorded 900 trace lines;
  `mkdsc.py` accepted it and produced `src/dsc_linux.inc` (96 entries, PPS 23 words).
* The kext builds on the Mac (`make -j2 kext`, x86_64, macOS 11 ABI): no new compiler warnings from `dsc.cpp` or
  the DSC code in `device.cpp` (the remaining `device.cpp` warnings are pre-existing lines).

Not verified (needs the card):

* **The sink actually decodes DSC** (the plan's U6). Its DPCD says yes; nothing on this machine has ever received
  a DSC stream. This is what the picture test above decides.
* Whether the DCN 4.01 DSC block emits the PPS SDP and the sink locks on the first frame after the blank (the
  table is Linux's order; our blank/unblank handshake is new).
* Whether `DSCC_STATUS`'s pending bit clears within 100 ms on this hardware, and whether `DSCRM`'s
  `FORWARD_EN_STATUS` reads 1 as Linux expects.
* Whether DSCCLK runs from the DTO (`dccg401_set_dto_dscclk` phase/modulo = 1) without the DCN35 root-gate writes:
  the DCN32/401 Linux path used here does not call `enable_dsc`, so the kext follows it. If the picture is wrong
  while all read-backs are right, this is the first suspect (plan U5).
* The `DP_PIXEL_FORMAT` change from the firmware's 10 bpc MSA (`0x200`) to 8 bpc (`UNCOMPRESSED_COMPONENT_DEPTH =
  1`) that the 8 bpc PPS needs. It should be invisible on an 8 bpc desktop; if the desktop was actually 10 bpc, the
  picture may band differently. This is intended for S2 (RGB 8 bpc).
* Any raster other than 3840×2160@60, any pipe other than OTG0/OPP0/DIG0/HPD1, and any sink without 2 slices,
  8 bpc, RGB, block prediction, ≥13-bit line buffer and 340 Mpix/s a slice: the code refuses all of them with a
  `dsc: not enabled: …` line before writing anything.
* 4K120/1440p240 (S3/S4/U1): **not implemented here**. S2 is the compressor-on proof; the mode-table cap and the
  `getTimingInfoForDisplayMode` stamp are a separate step (plan §3.2/§6 S3-S4).

---

## 5. Where the pieces are

* Option: `rdna4-dsc=1` via `Trial::option` (boot-arg or `4D1FDA02-…:rdna4-trial`); off by default.
* Enable: `RDNA4Device::tryEnableDsc()`, called once after `buildModeTable()`; refusal reasons are logged.
* Undo: `RDNA4Device::disableDsc()`, called by a failed enable and by `debug.rdna4_dsc = 0`.
* Table: `src/dsc_linux.inc` (generated) consumed by `src/dsc.cpp`; regenerate with
  `tools/gen/pipegen/run.sh <linux tree>`, never edit by hand.
* Host test: `test/atomdump.cpp::testDsc()`.
* The plan this implements: `docs/dsc-4k120-plan.md` §1.1 (sink decode), §2.1/§2.2 (registers and order), §5.2
  (S2), §6 S2 (acceptance), §7 U3/U5/U6/U8.
