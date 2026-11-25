# A8: HDR10 as a mode colour property (30bpp scanout)

Status: **code written, builds clean; not run on the card yet.** This file is the
record for the change and the acceptance test of the next hardware boot.

The plan this implements is `docs/hdr-30bpp-plan.md` §4 (items 1, 3, 4) after A6/A7.
The `kIOFBTimingRangeKey` route of §4 item 2 is **not** used: it panicked this
machine three times (plan, "The IOFBTimingRange attempt, and why it is withdrawn")
and is forbidden. No IORegistry property is added.

## What changes

Option, off by default:

| Where | Name | Effect |
|---|---|---|
| boot | `rdna4-hdr=1` | the mode table also offers the modes' HDR10 variants |
| run time | `sudo sysctl -w debug.rdna4_hdr=1` | appends the variants to the live table (the table only grows) |

When off, every code path is the one that ran before this change (no modes added,
the timing hook passes the original answer through, the plane format is decided by
the OS depth exactly as in A4).

1. **Mode table** (`src/modes.hpp`, `src/device.cpp`): `Modes::Mode` has `bool hdr10`.
   `RDNA4Device::offerHdrModes()` appends one copy of every SDR table entry (same
   raster/refresh, `native = false`, an ID of its own) with `hdr10 = true`, up to
   `Modes::MaxModes`. If the table is full the entries that fit are added and one
   line says how many were skipped; if none fit, one line and nothing else.
2. **Timing colour** (`src/plugin.cpp`): a Lilu route on
   `__ZN17IONDRVFramebuffer27getTimingInfoForDisplayModeEiP19IOTimingInformation`.
   It calls the original and, only when the mode ID maps to one of our `hdr10`
   entries, overwrites three fields of `IOTimingInformation.detailedInfo.v2`:
   `bitsPerColorComponent = kIOBitsPerColorComponent10` (0x4),
   `colorimetry = kIOColorimetryBT2020 | kIOColorimetryBT2100` (0x300),
   `dynamicRange = kIODynamicRangeHDR10` (0x2). `appleTimingID` and `flags` are left
   as the original set them; the colour is only written when the caller asked for
   detailed timing (`flags & kIODetailedTimingValid`), which is how
   `IOFramebuffer::extGetInformationForDisplayMode` calls it (Apple's
   `IOFramebuffer.cpp:11488`).
   The values come from `lib/MacKernelSDK/Headers/IOKit/graphics/IOGraphicsTypes.h`;
   the offsets are static-asserted against that header, nothing is hard-coded.
3. **Pipe** (`src/device.cpp`, `src/ndrv.cpp`): one place decides the plane format,
   `applyScanoutFormat()`: ARGB2101010 (0xa) when the OS selected the second depth
   (`scanout10bpc`, A3/A4) **or** the live mode is an HDR10 variant (`scanoutHdr`);
   ARGB8888 (0x8) otherwise. `Ndrv::Translator::switchMode` tells the device which
   entry won (`Backend::setHdrMode`) after a successful switch, so the synthetic
   re-applies of the running timing (display wake, link retrain, cold start) keep the
   mode's colour instead of clearing it; `applyMode()` re-applies the format after
   every mode set, as it already did for the OS depth. The plane format is the only
   thing that follows HDR; the DP stream stays as the firmware left it (A4 measured
   it already sends 10 bpc).
4. **Fail safe**: if the route symbol is missing, `RDNA4Device::hdrTimingHook` stays
   false and no HDR10 mode is offered (a mode that cannot carry its colour would be
   a duplicate of an SDR mode). A null `info`, a mode ID we do not know, a timing
   without `kIODetailedTimingValid` and a failed original call all return the
   original answer untouched, with at most one log line. The option can never stop
   a boot: nothing is written outside our own mode table and the three colour words
   of a timing we are asked about.

Explicitly not done: `kIOFBTimingRangeKey` / `IODisplayTimingRangeV2` in any form,
`'hdrm'` (nobody asks for it; plan A5), the HDR static-metadata infoframe, DSC, and
any new ioreg property.

### One correction to the task's numbers

The task text said `bitsPerColorComponent = kIODisplayRGBColorComponentBits10 (0x2)`.
In this SDK both 10-bit constants are **0x4**:
`kIODisplayRGBColorComponentBits10 = 0x00000004` and
`kIOBitsPerColorComponent10 = 0x00000004`; 0x2 is the 8-bit value in both enums
(`IOGraphicsTypes.h:566-571, 1150-1156`). `0x2` in that field would describe 8 bpc,
so the code uses the field's own constant, `kIOBitsPerColorComponent10` (0x4).

## Files

- `src/modes.hpp` — `Mode::hdr10`.
- `src/device.hpp` — `hdrEnabled()`, `hdrModes`/`hdrTimingHook`, `scanoutHdr`,
  `setHdrOption()`, `offerHdrModes()`, `modeTableGrew` callback.
- `src/device.cpp` — boot option, sysctl `debug.rdna4_hdr`, `offerHdrModes()`,
  the format decision in `applyScanoutFormat()`, the re-apply in `applyMode()`.
- `src/ndrv.hpp`/`src/ndrv.cpp` — `setModeCount()` / `modeForDisplayId()`, and the
  `Backend::setHdrMode` report from `switchMode`.
- `src/plugin.cpp` — route and wrapper, mode count hand-off to the translator.

## Acceptance: next boot on the card

The Mac is in use: **do not install or reboot from this change without the owner's
go-ahead.** The cheap first test needs no boot at all (below).

### A. Run time, no reboot

```
sudo sysctl -w debug.rdna4_hdr=1
sudo sysctl -n debug.rdna4_log | grep -E 'hdr:|modes: .*HDR10|colour: plane format'
ioreg -r -w0 -l -k 'Modes,Count' | head
ioreg -r -w0 -l -k 'AMDRadeonRDNA4,Build' | head    # the -dirty build this run is (UUID from kextstat)
```

Expected lines (in this order):

```
AMDRadeonRDNA4: hdr: routed IONDRVFramebuffer::getTimingInfoForDisplayMode (HDR10 colour fields)
AMDRadeonRDNA4: hdr: debug.rdna4_hdr=1: the HDR10 variants are offered from now on (the table only grows)
AMDRadeonRDNA4: hdr: 11 HDR10 mode(s) offered (IDs 22..32); 10 of 21 skipped, the table is full (32)   [numbers vary]
AMDRadeonRDNA4: hdr: the NDRV translator serves 32 mode(s) now
AMDRadeonRDNA4: hdr: mode 122: dynamicRange HDR10, colorimetry BT2020|BT2100, 10 bits a component (flags 0x80000000)
AMDRadeonRDNA4: hdr: the live mode is an HDR10 variant; the plane format is ARGB2101010
AMDRadeonRDNA4: colour: plane format ARGB2101010 (0xa) [HDR10 mode]: HUBP ... (after the mode is switched to)
```

The per-mode `modes: id ... HDR10` lines are printed only where the table is built
(the boot, section B); at run time the `hdr: N ... offered (IDs A..B)` line is the
evidence of what was appended.

`Modes,Count` in ioreg must have grown by the number of `hdr:`-offered modes; every `id` line for the new
entries must carry ` HDR10`. A second `debug.rdna4_hdr=1` must append nothing
(`hdr: debug.rdna4_hdr=1: the HDR10 variants are already in the table (32 mode(s))`), and
`debug.rdna4_hdr=0` must leave the table alone (`hdr: debug.rdna4_hdr=0: the variants already in the table stay ...`)
and stop the colour overwrite.

Caveat on the run-time route: the translator serves the new IDs immediately, but macOS may keep the mode list it
read at boot until the display is reconfigured (reconnect or a `Displays` change). If the new `HDR10` lines appear
in `sudo sysctl -n debug.rdna4_log` but not in CoreGraphics' mode list, the table did grow and the OS simply did not
re-enumerate; the boot option (B) is then the test.

Then, in a logged-in GUI session, ask CoreDisplay what it thinks of the display
(the snippet from the plan, A7):

```python
python3 - <<'PY'
import ctypes
from ctypes import c_uint32, c_bool, byref
cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
cg.CGMainDisplayID.restype = c_uint32
cg.CGGetOnlineDisplayList.argtypes=[c_uint32, ctypes.POINTER(c_uint32), ctypes.POINTER(c_uint32)]
sl = ctypes.CDLL("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight")
sl.SLSDisplaySupportsHDRMode.argtypes=[c_uint32]; sl.SLSDisplaySupportsHDRMode.restype=c_bool
sl.SLSDisplayIsHDRModeEnabled.argtypes=[c_uint32]; sl.SLSDisplayIsHDRModeEnabled.restype=c_bool
ids=(c_uint32*8)(); n=c_uint32(0); cg.CGGetOnlineDisplayList(8, ids, byref(n))
for i in range(n.value):
    d=ids[i]; print(d, bool(sl.SLSDisplaySupportsHDRMode(d)), bool(sl.SLSDisplayIsHDRModeEnabled(d)))
PY
```

- `supportsHDR=False` for the TCL: CoreDisplay does not take the mode list as an
  HDR declaration; the mode table alone is not the lever (report this; it is a
  negative result, not a failure of the change).
- `supportsHDR=True`: the mode list was the missing declaration. Then select one of
  the `HDR10` modes in Displays (or by mode ID through `cscSwitchMode`), and the
  criterion is a **correct picture** with `colour: plane format ARGB2101010 (0xa)`
  and the HDR10 timing current. Wrong colours mean the OS is still compositing
  8 bpc into a 10 bpc read: `sudo sysctl -w debug.rdna4_hdr=0` at once and report.

### B. Boot option (only when the owner allows a reboot)

Add `rdna4-hdr=1` to `boot-args` (same place as `rdna4-rot180=1`), reboot, then:

```
sudo sysctl -n debug.rdna4_log | grep -E 'hdr:|modes: .*HDR10'
ioreg -r -w0 -l -k 'Modes,Count'
ioreg -r -w0 -l -k 'AMDRadeonRDNA4,Build' | head    # must name this build (dirty: its UUID identifies it)
```

Expected: the route line, `hdr: rdna4-hdr=1: ...`, the `hdr: N HDR10 mode(s) offered`
line before the `modes:` list (one `modes:` line per table entry, the new ones ending
in ` HDR10`, e.g. `modes: id 22 3840x2160@60.000 533250 kHz HDR10`), and `Modes,Count`
grown by that N (the table caps at 32, so with this sink's 21 SDR modes it reads 32,
not 42).
If the route is missing (a different KC), there must be exactly one
`hdr: no IONDRVFramebuffer::getTimingInfoForDisplayMode route (error ...)` line and
**no** HDR10 mode in the table.

## Unverified / risks

- **Never run on hardware.** No boot, no mode switch, no picture. Only the compile
  is verified (`make -j2 kext` on the Mac, see "Build" below).
- The route has not been exercised on the running card. Its name was checked against
  this machine's KC (`/home/Celia/kc/SystemKernelExtensions.kc`): the string is in the
  `com.apple.iokit.IONDRVSupport` fileset entry, and the code at `0x1475e2cc` is the
  relay the wrapper assumes (NDRV `cscGetModeTiming` for `appleTimingID`, then
  `cscGetDetailedTiming` into `detailedInfo`, only when the caller set
  `kIODetailedTimingValid`). A different KC needs the boot A/B in section B to show
  the route line.
- **Depth vs mode colour**: the pipe reads ARGB2101010 whenever an HDR10 mode is
  live, even if macOS keeps the framebuffer at depth 128 (8 bpc). A3/A4 measured
  that macOS re-derives the depth and returns to 128 on its own. If that happens
  while an HDR10 mode is live, the picture will have wrong colours. That is the
  first thing to look at; the option off is the fallback.
- The colour fields are written **after** IONDRVFramebuffer filled the timing (the
  KC relay at `0x1475e2cc` zeroes the V2 reserved words and copies the NDRV
  raster; it does not touch `pixelEncoding/bitsPerColorComponent/colorimetry/dynamicRange`).
  `pixelEncoding` stays 0 (not supported) because the task says colour fields only;
  if CoreDisplay requires `kIOPixelEncodingRGB444` (0x1) before it uses the colour
  fields, that is the next field to try.
- Mode IDs are `100 + Mode::id`; the HDR copies take IDs after the SDR table, so
  WindowServer display preferences that name an old ID still resolve to the same
  SDR mode. With the option off the IDs are byte-for-byte today's.
- `Modes::MaxModes` is 32: on this sink's 21-mode EDID only 11 variants fit (32 −
  21). The plan's "append variants of the table's modes" therefore covers the first
  11 in table order (native, largest, fastest first); the rest are skipped with the
  one `skipped` line. If all 21 are wanted, the constant or the mode count has to
  grow — not done here.
- The wrapper trusts the SDK struct layout; `static_assert`s pin
  `sizeof(IODetailedTimingInformationV2) == 0xa0`, the union at offset 8 and the
  three field offsets. A different header fails the build, not the boot.

## Build

Build host: `Celia@192.168.31.254`, tree `~/rdna4/a4-hdr` (never `~/rdna4/a4`),
`make -j2 kext`. Not installed; nothing on the Mac's ESP, boot-args or `/Library`
was touched.

```
Built Build/AMDRadeonRDNA4-0.4.0-RELEASE/AMDRadeonRDNA4.kext for x86_64 (min macOS 11.0)
Build/AMDRadeonRDNA4-0.4.0-RELEASE/AMDRadeonRDNA4.kext/Contents/MacOS/AMDRadeonRDNA4: Mach-O 64-bit kext bundle x86_64
```

Exit 0. The tree is uncommitted, so the build string says `-dirty`; the kext UUID is
the only way to tell this build from another (`dwarfdump --uuid`). The four touched
objects (`plugin.cpp`, `device.cpp`, `ndrv.cpp`, `modes.cpp`) compile without a
warning of their own: `plugin.cpp`, `ndrv.cpp` and `modes.cpp` report none, and
`device.cpp`'s six warnings (lines 1128, 1959, 2266, 2310, 2432, 4033:
`-Wshadow`/`-Wsign-conversion`/`-Wimplicit-int-conversion`) are all in untouched
code that was there before this change (the lines added here are 23-43, 2599-2607,
2689-2692, 2856-2956, 3595-3664 and 3695-3716). Compiling `git show HEAD:src/device.cpp`
with the same flags reports the same six warnings at its own lines (1118, 1949, 2256,
2300, 2422, 3883), so the change adds none.

`make -j2 test` (host tests) passes everything except a pre-existing firmware
check, `FAIL: navi44: gfx images: RLC_G must come last` ("0 gfx images built"):
this tree has no navi44 blobs in `firmware/amdgpu`, and the check lives in
`test/atomdump.cpp`, untouched by this change (CI's linux-firmware job runs it).
The `modes`/`ndrv` host tests pass.

## Rollback (one command)

Run time, the option only:

```
sudo sysctl -w debug.rdna4_hdr=0
```

Next boot, if it was put in `boot-args`:

```
sudo nvram boot-args="$(nvram -p boot-args | sed 's/ *rdna4-hdr=1//')"
```

Either leaves the kext loaded and the display on the SDR table; a reboot with the
option absent is the plain pre-A8 behaviour.

<!-- appended while working; keep the newest at the bottom -->
