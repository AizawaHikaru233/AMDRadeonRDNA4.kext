# WindowServer aborts with the 0.5.0-test2 accelerator (2026-11-25)

Three reports, all identical, from the boots where `/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle`
(upstream v0.5.0-test2, SHA256 `3c42ec1e…`) was installed and the accelerator published:

| File | Boot | Pair |
|---|---|---|
| `WindowServer-2026-11-25-210522.ips` | first bundle boot | our 0.5.0 kext + official bundle |
| `WindowServer-2026-11-25-210531.ips` | same boot, next spawn | same |
| `WindowServer-2026-11-25-210901.ips` | A/B boot | **official** 0.5.0-test2 kext + official bundle |

Every one: `EXC_CRASH SIGABRT` → `__assert_rtn` → `abort`, faulting thread:

```
#3  libsystem_c        __assert_rtn
#4  CoreDisplay        CoreDisplay_CreateDisplayForCGXDisplayDevice
#5  CoreDisplay        UpdateCGXDisplayDevice
#6  CoreDisplay        AddCGXDisplayDeviceToDeviceList
#7  CoreDisplay        CGXDisplayDriverInitialize
#8  SkyLight           WS::Displays::CoreDisplayManager::initialize()
#9  SkyLight           WSInitialize
```

No `rdna4`/`metal` images are loaded in the crashed process: the assert fires **before** the Metal plug-in
runs, so it is the display-device side, not the user-space driver.

The last CoreDisplay lines before the abort (unified log, `process == "WindowServer"`):

```
[ERROR] - Setting offline display 0x00000000 main in CGXConstructVirtualFramebuffer
[ERROR] - Setting main display 0x00000000 offline
```

i.e. CoreDisplay has a **null display device** for the accelerated framebuffer and asserts on it.

Notes:
- The iop daemon (`org.rdna4.iop`, "useIOP on, as this boot needs") could not be tested cleanly: the boot
  with it disabled happened to miss the accelerator's runtime gate (`runtime NOT up after 30 s`), so
  WindowServer stayed up on the plain framebuffers and no crash was produced. The correlation observed so
  far is "accelerator publishes → WindowServer aborts in CoreDisplay".
- `shutdown_stall_2026-11-25-210348_celia-mac.shutdownStall` (930 KB) is in
  `/Library/Logs/DiagnosticReports` on the machine, from the same window.
