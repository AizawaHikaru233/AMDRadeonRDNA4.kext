# The crash loop of 2026-11-26 00:30-00:40: our IOFBTimingRange, used as a pointer

Three panics, identical, while `rdna4-hdr=1` was in the boot-args and the plugin published an
`IODisplayTimingRangeV2` under `kIOFBTimingRangeKey` ("IOFBTimingRange") on the framebuffer:

```
panic(cpu 0): Kernel trap ... type = 14=page fault
  CR2: 0x00000000017d7840          <-- 25,000,000 decimal
  RAX: 0x00000000017d7840
  Panicked task ... pid 212: WindowServer
```

`0x17d7840` = **25,000,000** = the `minPixelClock` (Hz) we wrote into that structure. The kernel
dereferenced that clock value as an address, i.e. the blob we published was consumed in a layout the
OS did not expect (wrong structure version/interpretation for this build), and the fault took
WindowServer down with it; the machine then rebooted into the same state (three panics 00:30:58,
00:33:28, 00:40:36).

Recovery: the kext's `Kernel -> Add` entry was disabled to boot, then the plugin's
`publishHdrRange()` was reverted, the boot-arg `rdna4-hdr=1` removed, and the kext re-enabled with the
verified build (md5 `e53b71e1…`). The boot after that is stable: accelerator registered, `runtime up`,
rot180 on, EDID 384 bytes, no new panic.

Lessons recorded in AGENTS.md:

1. **Do not publish `IOFBTimingRange` without the exact structure this macOS expects.** A wrong layout
   is not ignored: a field is taken as a pointer and the kernel page-faults.
2. **Do not power-cut a boot that is still progressing.** This machine's first boot after an ESP kext
   change takes 4–5 minutes; cutting power at five minutes corrupted the auxiliary kernel collection
   (the `Kernel com.apple.driver.IOVersatile - can't resolve dependencies` storm on screen) and turned
   one bad boot into a loop.
3. **Read `Kernel-*.panic` before power-cycling.** The register dump pointed straight at our own value
   (`CR2`), which the ring log could never have shown (the ring dies with the panic).
