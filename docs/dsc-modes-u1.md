# U1: the OS-side half of DSC — compressed-only timings in the mode table, behind `rdna4-dscmodes=1`

Status: **code + host tests, kext builds clean on the Mac (`make -j2 kext`, no new warnings), not run on the
card.** This is step **U1** of `docs/dsc-4k120-plan.md` §7 (and the OS-side half of its S4, §3.1/§3.2). The
question it answers is *does macOS accept a timing whose raster this link can only carry compressed, from our
IONDRV framebuffer, when the timing carries the three `dsc*` words* — with **nothing programmed on the GPU**.

Nothing here touches the compressor: `src/dsc.cpp`, `src/dsc_linux.inc` and the S2 path (`rdna4-dsc=1`,
`RDNA4Device::tryEnableDsc`) are unchanged and independent. No DSC register is written, no DPCD byte is
written, no link is retrained, and no timing is switched by this code. The option is a trial (armed through
`rdna4-trial`, consumed before it acts) and off by default; with it off the mode table, every mode ID and every
`getTimingInfoForDisplayMode` record are the ones this kext served before.

---

## 1. The two halves of U1, and what each one is allowed to do

| Half | Code | What it does | What it must never do |
|---|---|---|---|
| Table | `src/modes.cpp` (`dscCap`, `dscLayout`, `Builder::addDsc`), `src/device.cpp` (`buildModeTable`) | admits a timing whose pixel clock is above the uncompressed cap of this link when the sink's own DSC caps say the compressor could carry it; marks it `Mode::dsc` and stores the slice layout | offer a timing the sink cannot decode, or change any mode that is offered today |
| Timing | `src/plugin.cpp` (`wrapGetTimingInfoForDisplayMode`) | writes `dscCompressedBitsPerPixel`, `dscSliceHeight`, `dscSliceWidth` of `IODetailedTimingInformationV2` for exactly those modes | write anything for a mode that is not `Mode::dsc`, or write when the caller did not ask for a detailed timing |

The switch-time half — "does macOS ever *ask* for one of them" — is a log line, not a program:
`RDNA4Device::applyModeRaw` refuses a DSC mode (it cannot be driven until the compressor is programmed) and
says so by name (§6).

## 2. What changed

| Path | What |
|---|---|
| `src/modes.hpp` | `Mode::dsc`, `dscSlices`, `dscBpp`, `dscSliceWidth`, `dscSliceHeight`; `Limits::maxPixelClockDscKHz` and `Limits::dscSink` (the decoded DPCD block the rule reads); `DscCap`, `DscLayout`, `dscCap()`, `dscLayout()`, `kDscBpp8bpc`. |
| `src/modes.cpp` | the cap and the slice choice as pure functions over the sink's decoded block; `Builder::addDsc()` and a second pass in `build()`; `before()` puts the compressed entries after every uncompressed one; the plain pass is the old code path unchanged. |
| `src/device.cpp` | `buildModeTable()`: reads the option, computes `Modes::dscCap()` from `dscCaps` + `bootDpLinkKHz` + `dispclkTargetKHz()`, logs the three terms; the mode-table log marks ` DSC` and prints the three stamp values per DSC mode. `offerHdrModes()`: a compressed-only mode gets no HDR10 variant. `applyModeRaw()`: one line naming a DSC mode that a refusal takes. |
| `src/device.hpp` | `dscModesEnabled()`, the `dscModes` static. |
| `src/plugin.cpp` | the three `static_assert`s on the `dsc*` offsets; the stamping block in the timing hook; two `static_assert`s proving the NDRV record has no room for them (§5). |
| `test/atomdump.cpp` | `testDscModes()`: the cap arithmetic at four DISPCLK values and two links, the slice choice for six rasters, `Modes::build()` twice on one EDID (cap 0 vs raised), the NDRV record's offsets. The five-timing DisplayID fixture moved into `buildFiveTimingEdid()` so both tests use the same bytes. |

`src/rdna4accel.cpp`, `src/ioaccel.hpp`, `src/dsc.cpp`, `src/dsc_linux.inc`, `src/ndrv.cpp` and the docs named in
the brief are untouched.

---

## 3. The cap rule, with the numbers that produced it

```
cap.kKHz = min( clockKHz, throughputKHz, linkKHz )           Modes::dscCap(), logged term by term

  clockKHz      = DISPCLK the kext asks the SMU to hold      = device.cpp's dispclkTargetKHz()
  throughputKHz = slices x sink.throughput0Mps x 1000        = the most slice count both ends offer
  linkKHz       = linkKHz24 x 24 / bpp                       = bootDpLinkKHz x 24 / 8
```

On this machine (`docs/dsc-s2.md` §3.1 is the S0 log, `docs/dsc-4k120-plan.md` §1.1 the decode, both from the
card's own DPCD block `01 21 03 7f 3b 07 01 00 00 1f 0e 11 08 00 00 04`):

| Term | Value | Where the number comes from |
|---|---|---|
| `clockKHz` | **650000** by default; **1070000** with `rdna4-dispclk=1070` (the S1 measurement); **1340000** with `=1340` | `dispclkTargetKHz()` (`src/clk.cpp:357`), `kRaised[0]` = 650 MHz |
| `throughputKHz` | 4 × 340 Mpix/s = **1360000** | DPCD `0x06b` = `0x11` → 340 Mpix/s *a slice*; the counts both ends offer are `0x064` = `0x3b` = {1,2,4,6,8} ∩ the encoder's 1..4 = **{1,2,4}**, so 4 is the most (`Dsc::usableSlices`) |
| `linkKHz` | 720000 × 24 / 8 = **2160000** | 4 lanes HBR2 → 17.28 Gbps → `bootDpLinkKHz` = 720000 kHz at 24 bpp; bpp = **8** (`kDscBpp8bpc`, DSC's floor for RGB 4:4:4 and the same 8 the S2 PPS uses) |
| **cap** | **650000 / 1070000 / 1340000** kHz | `clockKHz` decides at every DISPCLK up to 1360 MHz; at DISPCLK 2400 the throughput term binds at 1360000 |

The uncompressed cap stays `bootDpMaxKHz()` = max(min(720000, 650000), 533234) = **650000** kHz (the card's
own `modes: DisplayPort modes up to 650000 kHz …` line). With the default DISPCLK the DSC cap *equals* it, so
the option on its own offers nothing — deliberately: the clock, not the link, is what 4K120 needs, which is why
the plan puts S1 before S3/S4. The trial boot therefore arms both options (§7).

The second half of the rule is per raster, not a cap (`Modes::dscLayout`): the fewest slice count both ends
offer whose per-slice throughput covers the pixel rate (`Dsc::need`'s rule, same table),
`sliceWidth = ceil(hActive / slices)` ≤ the sink's max slice width (`0x06c` = `0x08` → 8 × 320 = **2560** px),
a whole bpp (the sink's `0x06f` = `4` means **1-bit steps**), and **one vertical slice** — the whole frame,
`sliceHeight = vActive`: DPCD has no vertical-slice field and Linux's DisplayPort DSC config uses one, which is
also what NVIDIA stamps (`dscSliceHeight = vVisible`).

A candidate is only admitted when **all** of these hold: it fits the console (`fbWidth`/`fbHeight`, unchanged
rule), it is `Modes::usable()`, its pixel clock is **above** `maxPixelClockKHz` and at most
`maxPixelClockDscKHz`, its source passes the EDID envelope rule that already applied, and `dscLayout()` finds a
layout. Otherwise it is dropped exactly as before.

---

## 4. Which modes this panel gets

The TCL 32X3B's DisplayID block 2 carries five Type I timings (`docs/hdr-30bpp-plan.md:113`, read from the
card): 1920x1080@240 (615.12 MHz), 2560x1440@240 (995.52 MHz), 3840x2160@144.05 (1333.33 MHz),
3840x2160@239.87 (2418.37 MHz), 3840x2160@120 (1066.56 MHz). Against the rule above:

| Raster | Pixel clock | Slices needed (clock / 340) | Slices offered → chosen | Slice layout | 8 bpp on the link | Joins at DISPCLK |
|---|---|---|---|---|---|---|
| 1920x1080@240 | 615120 | 2 | 2 | 960 × 1080 | 4.92 of 17.28 Gbps | **already offered** (≤ 650000), stays SDR, `dsc` false |
| 2560x1440@240 | 995520 | 3 | {1,2,4} → **4** | 640 × 1440 | 7.96 Gbps | **1070** |
| 3840x2160@120 | 1066560 | 4 | **4** | 960 × 2160 | 8.53 Gbps | **1070** |
| 3840x2160@144.05 | 1333333 | 4 | **4** | 960 × 2160 | 10.67 Gbps | **1340 only** |
| 3840x2160@239.87 | 2418370 | 8 | the encoder offers 4 at most → **none** | — | — | **never** (needs 8 slices) |

So, on this panel:

* at `rdna4-dispclk=1070` the table becomes **23 modes**: the 21 of today, then ID 22 = the DisplayID
  3840x2160 timing at 1066.56 MHz and ID 23 = the 2560x1440 one at 995.52 MHz, both `DSC` (the refresh each one
  works out to is on its `modes: id …` line — the DisplayID block carries rasters, not Hz);
* at `rdna4-dispclk=1340` it becomes **24 modes**: 4K144 joins as well (1333333 ≤ 1340000, and 1333333 ≤
  1360000 of the slice term — the tightest case in the plan, 6.7 kHz and 27 MHz of headroom);
* **4K240 is not offered on any setting**: 2418370 kHz needs 8 slices of 340, and the DCN 4.01 encoder can only
  be told 1..4 (`Dsc::kEncoderSlices`), so the throughput term caps the table at 1360000 kHz even if the SMU
  gave 2400 MHz. *All three* of the brief's rasters need 4 slices or fewer; 4K240 is the one that needs more
  than 4;
* with the option off, or at the default DISPCLK, the table is the same 21 modes with the same IDs.

Two further consequences of "the compressed entries go after every uncompressed one" (`Modes::before()`):

* an ID macOS already holds keeps pointing at the same timing — arming the option can never silently change the
  mode a display preference names (if `rdna4-hdr` is armed as well, its variants are appended after the
  compressed entries and take IDs after them);
* a preference saved while armed names ID 22/23, which does not exist on the next plain boot: CoreDisplay then
  falls back to the preferred timing instead of landing on a different mode.

Sink caps that do **not** constrain any of the above, checked because the PPS will need them later: line buffer
16 bpc (the encoder offers 13), block prediction on, RGB/4:4:4, 8/10/12 bpc, RC buffer 8 MiB (see
`docs/dsc-s2.md` §2 item 2), DSC 1.2.

---

## 5. What the OS is told, and what the NDRV record could say

`lib/MacKernelSDK/Headers/IOKit/graphics/IOGraphicsTypes.h` (`IODetailedTimingInformationV2`, size `0xa0`):

| Offset | Field | Written for a DSC mode | Asserted |
|---|---|---|---|
| `0x90` | `dscCompressedBitsPerPixel` | `dscBpp * 16` = **128** for 8 bpp (the 2018 timing features' encoding: NVKMS's 257 is 16.06 bpp) | `static_assert` in `plugin.cpp` |
| `0x92` | `dscSliceHeight` | **2160** (4K) / **1440** (1440p) — the frame, one vertical slice | `static_assert` |
| `0x94` | `dscSliceWidth` | **960** (4K, 4 slices) / **640** (1440p, 4 slices) | `static_assert` |

`bitsPerColorComponent` (`0x8a`), `colorimetry` (`0x8c`) and `dynamicRange` (`0x8e`) — the A8 colour hook's
words — are not touched by the DSC block, and the DSC block does not touch them. A mode is never both `hdr10`
and `dsc`: `offerHdrModes()` skips the compressed ones (a 10 bpc PPS would be 12 bpp, plan §5.1) and says so.

**The NDRV record carries none of this — proven, not assumed.** `VDDetailedTimingRec`
(`lib/MacKernelSDK/Headers/IOKit/ndrvsupport/IOMacOSVideo.h:1135-1186`) has 30 fields and no DSC field:
`grep -i dsc` over that header finds only `VDScalerRec`/`cscGetScaler` matches, nothing else. It is the *same
0xa0 bytes* as `IODetailedTimingInformationV2`, and its tail is `csNumLinks` (0x80) then `csReserved2..8`
(0x84…0x9c, "Init to 0"), so the V2 timing's `dscCompressedBitsPerPixel`/`dscSliceHeight` (0x90/0x92) fall on
`csReserved5` and `dscSliceWidth` (0x94) on `csReserved6`. `src/ndrv.cpp::getDetailedTiming` therefore has
nowhere to put a DSC word and fills none — it sets the raster, the polarities and the state only. The words go
into the `IOTimingInformation` the OS asks for, which is what `IONDRVFramebuffer::getTimingInfoForDisplayMode`
is. All of the above is asserted twice: `plugin.cpp` has `NDRV_SAME_SIZE(VDDetailedTimingRec)` (mirror == SDK)
plus two new `static_assert`s on `csReserved5`/`csReserved6` and the equal sizes, and `testDscModes()` checks
the mirror's offsets `0x28 / 0x40 / 0x80 / 0x90 / 0x94 / 0x9c` and `sizeof == 0xa0` on the host.

The hook's guard, in order: `org(...)` succeeded; `RDNA4Device::dscModesEnabled()` (the trial option was armed
— with it off no table entry is marked, so the block cannot run); the mode the ID maps to has `dsc`; the caller
set `kIODetailedTimingValid` (the offline probe passes `flags = 0`, and the union is not a V2 there). Nothing
else in the record is touched, so for every other mode the bytes are the original's.

---

## 6. The switch-time refusal, which is the answer U1 wants

A DSC mode cannot be driven in this build: nothing programs `DSCC*`, `DSCRM*`, `OPTC_DSC_*`,
`DP_PIXEL_FORMAT.PIXEL_ENCODING_TYPE` or `DPCD 0x160`. So `applyModeRaw` refuses it, and now says so:

```
dscmodes: the OS asked for id 22 3840x2160@… (1066560 kHz, 4 slices of 960 x 2160 px, 8 bpp): a
compressed-only timing, refused (no request timing from Linux for the raster, and the plane's does not deliver
its lines fast enough). The compressor is not programmed in this build: rdna4-dscmodes=1 offers the timing and
stamps its dsc fields, the encoder is the next step
```

Two refusals can take it, and each logs the line with its own reason: the missing Linux request timing (the one
that fires for 4K120/1440p240 — neither has a row in `tools/gen/pipegen/modes.txt`, plan §3.2) and
`ModeSet::build`'s "pixel clock above the one the link was brought up for" (`t.dpMaxKHz`, the *uncompressed*
cap). `Pipe2::modeKnown()` and the refusal itself are unchanged: the line is added *beside* them, so a later
boot can tell "macOS never asked" from "macOS asked and was refused".

---

## 7. Acceptance for the next boot on the card

Armed for one boot (the trial variable is consumed before any option acts, so a boot that does not come back is
followed by a plain one):

```bash
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial="armed rdna4-dscmodes=1 rdna4-dispclk=1070"
sudo reboot
```

`rdna4-dispclk=1070` is there because the cap is DISPCLK-bound (§3); with the default 650 MHz the mode list
must be the plain one, and that is itself a check (§7.2).

After the desktop is up (≥ 10 minutes on a changed kext: AGENTS.md §一):

```bash
sudo sysctl -n debug.rdna4_log | grep -E 'dscmodes:|modes: DisplayPort|modes: id|modes: [0-9]+ mode|hard minimum'
```

### 7.1 With the option armed (DISPCLK 1070)

| Line | Expected |
|---|---|
| `DISPCLK: hard minimum 1070 MHz met` (or `accepted` where the mailbox has no status bit) | the clock S1 measured, granted on **this** boot. The table is built before the SMU runs, so the cap is the *target*; this line is what proves the target was taken, and `applyModeRaw` is what refuses an above-boot clock when it was not |
| `dscmodes: rdna4-dscmodes=1: timings this link carries only compressed up to 1070000 kHz = min(DISPCLK 1070000, 4 slices x 340 Mpix/s = 1360000, link 720000 kHz x 24 / 8 bpp = 2160000); uncompressed stays at 650000 kHz, so only timings between the two can join` | the whole rule, with its three terms, from the sink's own block |
| `modes: id 22 3840x2160@… 1066560 kHz DSC` and `modes: id 23 2560x1440@… 995520 kHz DSC` | the two new entries, `DSC`-marked, after the 21 uncompressed ones (the `…` is the refresh the table computed from the DisplayID raster) |
| `dscmodes: id 22 3840x2160@… is offered compressed only: 4 slices of 960 x 2160 px, 8 bpp -> dscCompressedBitsPerPixel 128, dscSliceHeight 2160, dscSliceWidth 960` | exactly what the timing hook will stamp; the 1440p one says `4 slices of 640 x 1440 px … 128, 1440, 640` |
| `modes: 23 mode(s), switching on (rdna4-modeset=0 turns it off)` | the count |
| `dscmodes: mode 22: dscCompressedBitsPerPixel 128 (8 bpp x 16), dscSliceHeight 2160, dscSliceWidth 960 (flags 0x…)` | macOS **read** the timing and got the words (first 8 per boot) |
| `dscmodes: the OS asked for id 22 …: a compressed-only timing, refused (…)` | macOS tried to **switch** to it — the U1 result |
| no `modes: switch to id 22` success, no `underflow`, no AUX error, no retrain | nothing was programmed |

`sudo log show --last 5m --predicate 'process == "WindowServer"' | grep -iE 'switch|offline'` is the other half
of the method A1/A2 used: absent `dsc*` words, the plan's NVIDIA precedent is a display declared **offline**
(7 offline / 35 virtual framebuffers → 0/0 with the words set); with them, the mode should appear as a real one.
The result is whichever of these two happened:

* `dscmodes: the OS asked for id 22 …` present → macOS accepts a compressed timing from an IONDRV framebuffer,
  and U1 is answered **yes**; the compressor (S2/S3) is then worth building;
* the `dscmodes: mode 22 …` read line present but no "asked for" line → macOS enumerated it and never switched
  (the Displays pane is the place to try by hand);
* neither line, the two `modes: id 22/23 … DSC` lines present → macOS dropped them at enumeration; that is a
  **no** for U1, and the reply is Plan B (`IODisplayTimingRangeV2` + `kIOFBTimingRangeKey`, plan §7 U9), not more
  work on the hook.

### 7.2 With the option off (every other boot)

```bash
sudo sysctl -n debug.rdna4_log | grep -E 'dscmodes:|modes: [0-9]+ mode'
ioreg -r -w0 -l -k 'Modes,Count' | grep -o '"Modes,Count" = [0-9]*'   # "Modes,Count" = 21
```

* **no** `dscmodes:` line at all: every one of them is inside `if (dscModes)`, and the option being absent
  leaves it false;
* `21 mode(s)` and not one ` DSC` marker;
* the IDs of every mode identical to the pre-U1 table (`modes: id …` lines), which is what "no existing mode
  changes" means in the log.

### 7.3 Host tests and the build (done, this round)

```bash
make -j2 CXX=g++ CC=gcc test        # this tree: only the pre-existing `navi44: gfx images` failures remain
Build/host/atomdump | sed -n '/compressed-mode rule/,+16p'
```

What it prints here (the numbers this doc quotes, from the rule itself):

```
dsc: the compressed-mode rule (U1: the cap and the slice choice)
  DISPCLK 650 kHz (the kext's default) equals the uncompressed cap, so nothing can join -> 650000 kHz = min(650000, 1360000, 2160000), 4 slices, 8 bpp
  DISPCLK 1070 (rdna4-dispclk=1070, the S1 measurement)                        -> 1070000 kHz = min(1070000, 1360000, 2160000), 4 slices, 8 bpp
  DISPCLK 1340 (4K144 needs 1333.33)                                           -> 1340000 kHz = min(1340000, 1360000, 2160000), 4 slices, 8 bpp
  DISPCLK 2400: the slice term binds at 4 x 340 = 1360000, so 4K240 stays out  -> 1360000 kHz = min(2400000, 1360000, 2160000), 4 slices, 8 bpp
  3840x2160@60 (the S2 raster: its recorded PPS is 2 x 1920)     -> 2 slices of 1920 x 2160 px, 8 bpp
  2560x1440@240 (995.52 MHZ: 3 slices needed, 4 offered)         -> 4 slices of 640 x 1440 px, 8 bpp
  3840x2160@120 (1066.56 MHz)                                    -> 4 slices of 960 x 2160 px, 8 bpp
  3840x2160@144.05 (1333.33 MHz)                                 -> 4 slices of 960 x 2160 px, 8 bpp
  3840x2160@239.87 needs 8 slices: more than the encoder offers  -> 0 slices of 0 x 0 px, 0 bpp
  7680x2160 at 2 slices would be 3840 px a slice, above the sink's 2560 -> 0 slices of 0 x 0 px, 0 bpp
  table: 18 mode(s) uncompressed, 20 with the cap at 1340000 kHz
```

(the `18 → 20` pair is the Samsung five-timing fixture, not this panel: it has no 4K120 at 1066.56 MHz.)

The kext builds on the Mac with `make -j2 kext` (`~/rdna4/a4-dscm`, clang, x86_64, macOS 11 ABI) after
`rsync -a --exclude 'Build/' …`: **no new warnings** — `modes.cpp` and `plugin.cpp` produce none at all, and the
six `device.cpp` ones (`1133` shadow, `1964`/`4534` `snprintf`, `2271` int precision, `2315`/`2437`
`extendedFindPCICapability`) are pre-existing lines that exist byte for byte in `HEAD:src/device.cpp`.

---

## 8. What is unverified

1. **Whether macOS accepts any of it** — the whole point of U1. Nothing here has run on the card.
2. **Whether WindowServer even asks** for a 4K120/1440p240 mode: it may filter the mode list itself, or
   CoreDisplay may judge the timing offline despite the `dsc*` words. §7.1 separates those cases.
3. **The DISPCLK grant at 1070 MHz on this boot.** S1 measured it once; the mode table is built before the SMU
   runs, so it uses the target. `applyModeRaw` refuses above the boot clock unless `uhdClocks` is set, and
   `DISPCLK: hard minimum … met` is the line that says it was.
4. **Whether a compressed timing must also carry a request timing (DLG/TTU/DET) to be switchable.** 4K120 and
   1440p240 have no row in `modes.txt`, so even with everything above green the switch is refused by
   `Pipe2::modeKnown` — a hard prerequisite of plan §3.2, not an optimisation, and *out of scope here*.
5. **The exact refresh macOS shows** for the two new entries: the rasters are the DisplayID ones (1066.56 and
   995.52 MHz); the `modes: id …` line prints the refresh the table computed, and the Displays pane is the
   other witness.
6. **10 bpc (S5) and HDR together with DSC**: not offered — no HDR10 variant of a compressed mode is created,
   because its PPS would be 12 bpp (`dscCap`/`dscLayout` take bpp as a parameter, so S5 only has to pass 12).
7. **A second head** (`rdna4-head2`): `secondPipeModeLimits()` leaves both DSC fields zeroed, so a second pipe
   never offers a compressed timing. Untouched, and not exercised.
8. **The slice count vs `OPTC_DSC_MODE`** (plan U4): the enum value for "4 slices" is not needed here — this
   change writes no OPTC register — but the later PPS work must settle it against the sink's caps the same way.

## 9. Rollback

* **Nothing to undo on the machine**: the option is a trial variable that `src/trial.cpp` consumes before the
  option acts, so the next boot is plain even if this one hangs. If the option was put in `boot-args` instead,
  remove it there (OpenCore `NVRAM:Add:7C436110-…`, AGENTS.md §一).
* **In one boot**: no sysctl is needed and none exists — the mode table cannot be retracted at run time
  (macOS holds the IDs), and the stamping only ever touched timings the OS *asked about*, so a reboot is the
  rollback. A black screen with SSH alive is a normal reboot (AGENTS.md's ≥ 10-minute rule is about boots that
  look hung); a panic leaves `/Library/Logs/DiagnosticReports/Kernel-*.panic` — read `CR2`/`RAX` first.
* **Source**: the change is one commit-sized block in six files (`modes.*`, `device.*`, `plugin.cpp`, `test/atomdump.cpp`); nothing else was touched (§2). Reverting
  `src/modes.*` alone leaves `Mode::dsc` unused and every table identical to today's.

## 10. Where the pieces are

* Option: `rdna4-dscmodes=1` via `Trial::option` (boot-arg or `4D1FDA02-…:rdna4-trial`), read in
  `RDNA4Device::buildModeTable`.
* The rule: `Modes::dscCap()` and `Modes::dscLayout()` (`src/modes.cpp`), the second pass in `Modes::build()`,
  `Builder::addDsc()`.
* The stamp: `wrapGetTimingInfoForDisplayMode` (`src/plugin.cpp`).
* The refusal line: `logDscModeRefused()` + its two call sites in `RDNA4Device::applyModeRaw` (`src/device.cpp`).
* Host test: `testDscModes()` in `test/atomdump.cpp`.
* The plan this implements: `docs/dsc-4k120-plan.md` §3.1 (the three words), §3.2 (the per-mode cap), §6 S4
  (the OS-side half), §7 U1. The sink numbers: `docs/dsc-s2.md` §1/§3.1.
