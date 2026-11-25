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
