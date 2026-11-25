# 30bpp on the IONDRV path: the A1 experiment

Question: does the IONDRVFramebuffer + our own NDRV translator route carry a **second depth**
(10 bits a component, `kIO30BitDirectPixels`) far enough that macOS asks for it? Our HDR work
(30bpp scanout, PQ/BT.2020 metadata, DSC) is only worth starting if the seam exists, and it has
never been measured: upstream's translator answers one depth (`kDepthMode1`, 32 bpp direct,
`src/ndrv.hpp`) and nothing in this tree offers a second one.

## Why this is not a subclassing question

`IONDRVFramebuffer` **is** an `IOFramebuffer` (`MacKernelSDK/Headers/IOKit/ndrvsupport/IONDRVFramebuffer.h:49`)
and declares the modern virtuals (`getPixelInformation` :242, `setAttributeForConnection` :306).
The IOFramebuffer-side declarations that HDR and DSC need exist
(`IOKit/graphics/IOGraphicsTypes.h`: `kIOFBHDRMetaDataAttribute = 'hdrm'` :290,
the `dsc*` fields of `IODetailedTimingInformationV2` :517-519, `kIODynamicRangeHDR10` :591).
What has never been measured is whether the NDRV records can drive that machinery: they carry
`cscGetVideoParameters` (`VPBlock` with `vpCmpSize`) and `VDSwitchInfoRec.csMode` (a depth mode),
and IONDRVFramebuffer builds its pixel formats from those.

A subclass is the *other* route, and this project has already measured its cost: `fb/` (the
`RDNA4Framebuffer` of commits `6f1bafa`..`c784121`) loaded from `/Library/Extensions`, was opened
by WindowServer, and the desktop did not draw (`accel: rdna4-accel=4 at boot: FAILED (19)`).
`src/plugin.cpp:14-18` records why upstream stays with the Lilu route: on macOS 11+
IOGraphicsFamily lives only in the System kernel collection, so an OpenCore-injected kext cannot
link it.

## The change

`rdna4-depth2=1` (a test boot's option, `src/trial.cpp`) makes the translator:

- answer `cscGetVideoParameters` for `kDepthMode2` with `vpPixelSize 32`, `vpCmpCount 3`,
  `vpCmpSize 10` (`src/ndrv.cpp`);
- report `csMaxDepthMode = kDepthMode2` from `cscGetNextResolution`;
- accept a switch to `kDepthMode2` in `cscSwitchMode` and report it as current in `cscGetCurMode`.

Nothing about the scanout changes: the pipe still outputs 8 bpc, so if macOS switches to the
second depth the picture may be wrong. That is deliberate — the experiment measures what macOS
asks for, not a working HDR desktop.

`src/plugin.cpp` logs every `cscGetVideoParameters`, `cscGetCurMode`, `cscSwitchMode` and
`cscGetNextResolution` while the option is armed:

```
AMDRadeonRDNA4: depth: status code 18 mode 100 depth 129 answered 1 ret 0x0
```

## Criteria

1. **The seam exists** — macOS asks for `depth 129` (`kDepthMode2`) at least once, and a
   `cscSwitchMode` with `csMode 129` follows. Then the NDRV path can declare 30bpp, and the next
   steps are the pipe's 10 bpc output and `'hdrm'`.
2. **The seam does not exist** — every request carries `depth 128`, or macOS never switches.
   Then the NDRV route cannot expose a second depth, and the IOFramebuffer-subclass route has to
   be faced again, with the accelerator bring-up (`rdna4-accel=4`) as its first problem.

Either result goes into `docs/` and `/home/Celia/rdna4/Changelog.md`.

## How to run

```
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial="armed rdna4-depth2=1"
# reboot; the kext must be injected (OpenCore Kernel -> Add) for the plugin to run
sudo sysctl -n debug.rdna4_log | grep 'depth:'
```

The trial variable is cleared by the driver before it acts on any option, so the boot after a
test that hangs runs without it (`src/trial.cpp`).

## A1 result (2026-11-25 19:35, on the card)

Ran on the RX 9070 XT, Tahoe 26.6.2, Samsung 4K over DisplayPort, kext 0.5.0 + `devel/hdr-30bpp`,
injected by OpenCore (`Kernel -> Add`: Lilu 1.7.3 + `AMDRadeonRDNA4.kext`), trial armed with
`rdna4-trial="armed rdna4-depth2=1"` (the driver marked it `used`). Full trace:
`hw-logs/2026-11-25-hdr30bpp-a1-depth2.txt`.

**What happened**

- `IONDRVFramebuffer` **enumerates six depths for every one of the 21 modes**
  (`cscGetVideoParameters`, code 18): `depth 128` and `depth 129` answered `0x0`, and `130`..`133`
  answered `0xe00002c2` (we only offer two). So the depth table macOS shows comes from the NDRV
  records, and a second depth is accepted through this route — 41 requests for `depth 129`, none
  refused.
- **No switch to the second depth followed**: all four `cscSwitchMode` calls (code 10, control)
  carried `depth 128`. The desktop stayed 32 bpp (pixel clock 533.25 MHz, 4K60).
- The driver came up normally (`runtime PASS`, `gfx PASS`, WindowServer running, displays present).

**Verdict**

Criterion 1 of this plan is half met, and the half that failed is not the interface: the NDRV
route *can* declare 30bpp, and IONDRVFramebuffer accepts it. What is missing is the reason for
macOS to *choose* it — HDR has to be declared to CoreDisplay (CTA-861 HDR static metadata in the
EDID we serve, `kIOFBHDRMetaDataAttribute = 'hdrm'` on the connection) before the second depth is
wanted, and the pipe has to output 10 bpc when it is. So "IONDRV cannot support HDR at all" is
refuted at the interface level; "declaring a depth is enough" is refuted too.

**A2, in the order that decides fastest**

1. Check whether the served EDID (256 bytes, two blocks) carries a CTA-861 HDR static metadata
   block; if it does not, serve one (the block is opaque bytes to macOS — `src/ndrv.cpp:401`).
2. Route `IONDRVFramebuffer::getPixelInformation` and `setAttributeForConnection` (the class is
   already ours to patch: `src/plugin.cpp` routes `doDriverIO` and `getApertureRange`) and log
   what WindowServer asks for each depth, and whether `'hdrm'` ever arrives.
3. Only then program the DCN pipe for 10 bpc and see whether the picture survives.

## A2 inputs: what the sink actually says (2026-11-25, measured)

The monitor is a TCL 32X3B over DisplayPort. Everything below is from the card, not from a spec sheet.
Evidence: `hw-logs/2026-11-25-dpcd-dsc.txt`.

| Where | What | Value |
|---|---|---|
| EDID CTA-861 | HDR Static Metadata (extended tag 6) | `06 07 01 73 4f 02` — EOTF bits `0x07` (SDR + HDR gamma + **PQ**), static metadata type 1, BT.2020 in the colorimetry block `05 c0 01` |
| EDID extension 2 (DisplayID, tag 0x70) | read by the driver, **not served to macOS** | 1920x1080@240 (615.12 MHz), 2560x1440@240 (995.52 MHz), 3840x2160@144.05 (1333.33 MHz), 3840x2160@239.87 (2418.37 MHz), 3840x2160@120 (1066.56 MHz) |
| DPCD 0x00060..0x0006f | **DSC supported** | `01 21 03 7f 3b 07 01 00 00 1f 0e 11 08 00 00 04` — 0x060 bit 0 set, algorithm revision 2.1 at 0x061 |
| DPCD 0x00000.. | link | DPCD 1.2, sink max rate `0x14` (HBR2, 5.4 Gbps/lane), 4 lanes |
| driver's mode budget | `bootDpMaxKHz()` | 650000 kHz (8 bpc over HBR2 = 720000 kHz, DISPCLK 650000 kHz) |

Consequences, in order:

1. **HDR is already declared to macOS by the EDID we serve** (PQ + static metadata type 1 + BT.2020).
   So the missing piece for HDR is not the EDID: it is the framebuffer side (a 30bpp pixel format the OS
   chooses, and `kIOFBHDRMetaDataAttribute`) — A1 showed the OS never switched to depth 129 even when the
   NDRV record offered it.
2. **DSC is real on this sink**, but only over a link that is HBR2 today. 4K120 needs ~23.9 Gbps at
   8 bpc while HBR2×4 carries 17.28 Gbps ⇒ without DSC it cannot exist; with DSC at ~5.8 bpp it fits.
   4K240 (~47.8 Gbps) needs ~2.9 bpp, at the edge of the sink's throughput fields.
3. **The high-refresh modes are already in our table** but filtered out by `bootDpMaxKHz()`, which
   assumes 8 bpc: 4K120 (1066.56 MHz) and 1440p240 (995.52 MHz) exceed the 650/720 MHz budget. They
   come back the moment the budget accounts for DSC, which is the same switch that has to program the
   DCN401 DSC encoder and hand the OS a timing with its `dsc*` fields (`IODetailedTimingInformationV2`).
4. The DisplayID block should also be **served** rather than clamped away: today `getDDCBlock` answers
   two blocks and rewrites the extension count 2 → 1 (`src/ndrv.cpp:401`), while the driver has the
   third block in `edidMore` (`src/device.cpp:665`).

## Order of work that this implies

1. Serve all EDID blocks (extend the served buffer to `edidData + edidMore`, keep the count) — small.
2. DSC: program the DCN401 DSC block, put the PPS and the compressed-bpp into the stream, retrain at
   HBR2 with DSC enabled, and report the timing with `dsc*` set; the mode budget then multiplies by the
   compression ratio. This is the only way to 4K above 60 Hz on this link.
3. 30bpp + `'hdrm'`: hook `IONDRVFramebuffer::getPixelInformation` / `setAttributeForConnection`, see
   what WindowServer asks with a 10 bpc pipe behind it.

## A2 result (2026-11-25 20:49, on the card)

Ran with `rdna4-depth2=1` and three routes added to the plugin (observation only, `src/plugin.cpp`):
`IONDRVFramebuffer::getPixelInformation`, `setAttributeForConnection`, `getAttributeForConnection` — symbol
names read from the kernel collection, not guessed (`__ZN17IONDRVFramebuffer19getPixelInformationEiiiP18IOPixelInformation`,
`…25setAttributeForConnectionEijm`, `…25getAttributeForConnectionEijPm`). Evidence:
`hw-logs/2026-11-25-a2-depth-attributes.txt`.

**What was measured**

- **IONDRVFramebuffer builds a real 30bpp pixel format from our NDRV answer**:
  `depth 1 -> 0x0: 32 bpp type 2 comp 3×10 mask 3ff00000 000ffc00 000003ff 3840x2160 row 15360`.
  The masks are `ARGB2101010`. So the "IONDRV cannot represent 10-bit" claim is refuted at the format level:
  the records carry it and Apple's own class turns them into the modern format.
- **The OS looks at it and does not take it**: depth 1 is asked 67 times, depth 0 (3×8) stays current, and
  no `cscSwitchMode` carried `csMode 129` (A1's finding stands).
- **No HDR metadata traffic**: `'hdrm'` (`kIOFBHDRMetaDataAttribute`) never appears — 0 requests on the
  connection, set or get. What does arrive on the connection are colour/gamma attributes:
  `'flgs'`, `'colr'` (= 0x10001001, refused 0xe00002c7), `' bpc'` (= 6), `'dflg'`, and the three colour-matrix
  channels `'rgsc' 'ggsc' 'bgsc'` plus `'vblm' 'flus'` (all 0x10000).

**Verdict**

The seam is real and wider than A1 showed: the NDRV route can hand macOS a 10-bit pixel format, and macOS
inspects it. What is missing is the *reason* for WindowServer to choose it, and that reason is HDR being
declared to CoreDisplay at the display level — not `'hdrm'` (nobody asks for it) but the display's own
capability path. A `' bpc'` = 6 attribute arriving before anything else is worth understanding: either the
display asks for 6 bits a component, or that constant means something else; the values are in the log.

**Order from here**

1. Decode the connection attributes the OS does send (`' bpc'` 6, `'colr'` 0x10001001) against
   `IOGraphicsTypes.h` / `IOFramebuffer.h` and Apple's IONDRVSupport behaviour: these are the levers the OS
   pulls before it would pick a 10-bit depth.
2. Only then the pipe: program DCN for 10 bpc (and a 10 bpc DP pixel encoding) so a chosen depth 1 can
   actually be scanned out.
3. DSC stays the separate, larger track for 4K above 60 Hz (see the DPCD section above).

## The connection attributes, decoded (2026-11-25 21:05)

The fourccs from the A2 log against `MacKernelSDK/Headers/IOKit/graphics/IOGraphicsTypes.h`:

| Attribute | Constant | What macOS did with it |
|---|---|---|
| `' bpc'` (0x20627063) | `kConnectionColorDepthsSupported` (IOGraphicsTypes.h:1075) | **set to 6** — the only value the OS wrote on this connection |
| `'colr'` (0x636f6c72) | `kConnectionColorModesSupported` (:1074) | **set to 0x10001001 — refused, 0xe00002c7** |
| `'flgs'` / `'dflg'` | `kConnectionFlags` / `kConnectionDisplayFlags` | 0 / 1 |
| `'rgsc' 'ggsc' 'bgsc'` | red/green/blue gamma scale | 0x10000 each |
| `'vblm'` / `'flus'` | VBL multiplier / flush parameters | 0x10000 / 1 |
| `'cena'` / `'pcnt'` / `'lddc'` / `'hddc'` / `'asns'` | check-enable, param count, DDC sense, Apple sense | 1 / 4 / E00002C7 / 0 / 0 |

**This is the lever.** macOS asks the framebuffer which colour modes and colour depths it supports
(`kConnectionColorModesSupported`, `kConnectionColorDepthsSupported`) and then tries to select one:
`'colr' = 0x10001001` was **refused** because nothing in our stack answers those two attributes —
IONDRVFramebuffer's own implementation does not know them, and our translator only answers csc records.
`' bpc' = 6` is the depth the OS settled on in the absence of an answer.

So the next experiment is not a guess: handle `'colr'` and `' bpc'` in the plugin's
`setAttributeForConnection` / `getAttributeForConnection` (the routes are already in place), report the
10-bit colour modes/depths in the encoding IONDRVFramebuffer expects, and watch whether WindowServer then
takes depth 1 (which A2 showed is already built as ARGB2101010). Only after that does the DCN pipe need
10 bpc — the OS has to want it first.
