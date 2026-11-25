# The crash loop of 2026-11-26 00:30-00:40: our own `rdna4Log` call, not the kernel reading the range

**Corrected 2026-11-26 04:40.** This file first said the kernel had dereferenced a clock value out of
our `IODisplayTimingRangeV2`. That reading was wrong: the fault is inside `vsnprintf`, on a `%s`
conversion in a log line emitted while the plugin attached. No kernel frame ever touched the published
structure, so the property was never tested at all.

Three panics, identical, while `rdna4-hdr=1` was in the boot-args and the plugin published an
`IODisplayTimingRangeV2` under `kIOFBTimingRangeKey` ("IOFBTimingRange"):

```
panic(cpu 4): Kernel trap at ..., type = 14=page fault
  CR2: 0x00000000017d7840   RAX: 0x00000000017d7840   RDX: 0x000000007fffffff
  Panicked task ... pid 212: WindowServer
  ...
  0xffffff801b79e67e mach_kernel : _mac_schedule_telemetry + 0x348e      <- RIP, the faulting load
  0xffffff801b4e9b12 mach_kernel : _vsnprintf + 0x22
  0xffffff801eeff59a com.hackintosh.AMDRadeonRDNA4 : __Z8rdna4LogPKcz + 0x9c
  0xffffff801ef058cb com.hackintosh.AMDRadeonRDNA4 : __ZN12_GLOBAL__N_16attachERNS_7FbEntryE + 0x1e82
  0xffffff801ef02818 com.hackintosh.AMDRadeonRDNA4 : __ZN12_GLOBAL__N_114wrapDoDriverIOEPvjS0_jj + 0x34c
  0xffffff7fb00418da com.apple.iokit.IONDRVSupport : __ZN17IONDRVFramebuffer11checkDriverEv + 0xfc
```

What that says:

- the panic is our `attach(FbEntry&)` -> `rdna4Log` -> `vsnprintf` -> a load from `0x17d7840`;
- `0x17d7840` = **25,000,000** is not a random address: it is the `minPixelClock` (Hz) our HDR block
  wrote. A `%s` (or a `%.*s`) conversion consumed a *number* as the `char *`;
- three occurrences, same stack, same address: a deterministic varargs/format mismatch in that one log
  call, which is why it happened on every boot with `rdna4-hdr=1` and never without it;
- the panic took WindowServer (pid 212) with it, and the machine rebooted into the same state
  (00:30:58, 00:33:28, 00:40:36).

So: **publishing `kIOFBTimingRangeKey` was never shown to be unsafe, and never shown to work.** The
line that crashed was our diagnostic, not the publication. What is still unknown (and now being
reversed properly, `docs/hdr-timingrange-reverse.md`) is the record shape this build actually consumes
— NDRV `cscGetTimingRanges` / status 0x1e, and what userspace reads back — and whether an
`IONDRVFramebuffer`-based plugin can supply it at all.

Recovery: the kext's `Kernel -> Add` entry was disabled to boot, then the plugin's
`publishHdrRange()` was reverted, the boot-arg `rdna4-hdr=1` removed, and the kext re-enabled with the
verified build (md5 `e53b71e1…`). The boot after that is stable: accelerator registered, `runtime up`,
rot180 on, EDID 384 bytes, no new panic.

Lessons (in AGENTS.md):

1. **A panic in `rdna4Log`/`vsnprintf` is a format-string bug, and it is fatal in the kernel.** Read the
   backtrace before blaming the mechanism that was being tested. Treat any `%s` added to a log line as
   code under review.
2. **Do not power-cut a boot that is still progressing.** This machine's first boot after an ESP kext
   change takes 4–5 minutes; cutting power at five minutes corrupted the auxiliary kernel collection
   (the `Kernel com.apple.driver.IOVersatile - can't resolve dependencies` storm on screen) and turned
   one bad boot into a loop.
3. **Read `Kernel-*.panic` before power-cycling.** The register dump names the faulting frame
   (`CR2`/`RAX`/`RIP`), which the ring log can never show (the ring dies with the panic).
