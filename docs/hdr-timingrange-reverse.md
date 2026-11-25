# `kIOFBTimingRangeKey` in macOS 26.6.2 (25G83, x86_64): what the build expects, and why 2026-11-26 00:30 panicked

Reverse-engineering of `/home/Celia/kc/SystemKernelExtensions.kc` (md5 `833d3691d2805c50b7bab52431ed9c66`,
361,922,560 bytes, `MH_FILESET`, 172 embedded kexts), cross-read against
`lib/MacKernelSDK/Headers/IOKit/{graphics,ndrvsupport}/*` and `ref/nvidia-macos-driver`.

Every address below is a **link-time vmaddr**, which in this collection is also the **file offset**
(each embedded image's `LC_SEGMENT_64.fileoff == vmaddr`; the images share one `__LINKEDIT`). The
mapping is *not* assumed identity in general — see §1.

---

## 0. Answer in one paragraph

In this build nothing in the kernel **reads** `kIOFBTimingRangeKey`. The literal appears seven times
(one per kext, listed in §9), always in a framebuffer driver that *publishes* it;
`IOGraphicsFamily` — the kext that owns `IOFramebuffer`, `IODisplay` and
`IOFramebuffer::setAttribute`/`getAttribute` — is **not** among the seven, so the attribute machinery
never looks at it, and the only kext that reads the property at all is `AMDFramebuffer` reading back
its own. The only kernel *writer* on the NDRV path is
`IONDRVFramebuffer::setInfoProperties()`, and in 26.6.2 that function is **dead code** (nothing calls
it or takes its address). It would have asked the NDRV driver for `cscGetTimingRanges` (status code
**30 / 0x1e**) into a **240-byte `VDDisplayTimingRangeRec`** (`0xf0`; the V1 shape of
`IODisplayTimingRange`, `version` at offset 8 = 0), and published those same 240 bytes as an **OSData**
property. Apple's native framebuffer drivers in the same collection publish the **V2** shape,
**312 bytes (`0x138`)**, and one of them reads it back with `OSDynamicCast(OSData)` — i.e. the type
contract is "OSData containing the struct", never a by-value struct or dict.

The 00:30 panics were **not** the kernel mis-parsing our blob. The backtrace is
`… → IONDRVFramebuffer::checkDriver → our wrapDoDriverIO → our attach(FbEntry&) → our rdna4Log →
vsnprintf → page fault`, with `CR2 == RAX == 0x17d7840`. The dereference happened **inside
`vsnprintf`**, i.e. in formatting **our own log call**: a `%s` conversion received the numeric field
`minPixelClock` (25,000,000) as its `char *`. No kernel frame ever touched the published structure.
The "kernel used our clock as a pointer" reading in `docs/hdr-30bpp-plan.md` (withdrawn section) and in
`docs/hw-logs/panics-20261126/README.md` is wrong, and so is the premise that a wrong struct version
caused it.

Confidence: **high** on "the fault is our own varargs/format mismatch" (backtrace + register file are
unambiguous about *which code* faulted); **high** on the NDRV contract (240-byte V1 record via status
0x1e); **high** that no kernel consumer exists (string absence is decisive for a `#define` key);
**medium** on two leftovers: whether CoreDisplay (userspace) accepts a V1 range from an
`IONDRVFramebuffer` at all, and the exact log statement that faulted (needs our kext binary, which is
not in this collection).

---

## 1. How the addresses were obtained, and the check that they are the loaded ones

The inherited helpers in `/tmp/trng-work/` are wrong for this file in two ways, both fixed in
`/tmp/trng-work2/kc2.py`:

* `fileset.py` looks for `LC_FILESET_ENTRY == 0x1f`; this collection uses **`0x80000035`** (172 of them).
* `LC_FILESET_ENTRY.entry_id` is an **`lc_str` offset relative to the load command**, not an inline
  C string. Reading it inline yields empty kext names (which is why the earlier attempt saw "no
  IONDRVSupport").
* symbol/section offsets in the embedded images are already **file-absolute** (`symoff 0x152ce630`,
  `stroff 0x153796d0`, shared by all images) — adding the image base double-counts.

The translation used throughout is per segment, `filepos(va) = seg.fileoff + (va - seg.vmaddr)`, never
an assumed identity.

**Validation against the panic** (`docs/hw-logs/panics-20261126/Kernel-2026-11-26-003058.panic`): the
three frames that name a symbol in a kext give one and the same KASLR slide:

| frame in panic | runtime address | symbol (this file) | link-time | slide |
|---|---|---|---|---|
| `IONDRVFramebuffer::checkDriver + 0xfc` | `0xffffff7fb00418da` | `0x1475c7de` | `+0xfc = 0x1475c8da` | `0xffffff7f9b8e5000` |
| `IONDRVFramebuffer::enableController + 0xa1` | `0xffffff7fb00407db` | `0x1475b73a` | `+0xa1 = 0x1475b7db` | `0xffffff7f9b8e5000` |
| `IOFramebuffer::open + 0x60b` | `0xffffff7fafffff81` | `0x1471a976` | `+0x60b = 0x1471af81` | `0xffffff7f9b8e5000` |

One slide for all three ⇒ this `.kc` is the collection those panics ran, and the symbolication below is
sound. (`n_value` of the already-confirmed
`__ZN17IONDRVFramebuffer27getTimingInfoForDisplayModeEiP19IOTimingInformation` = `0x1475e2cc`, whose
bytes are `55 48 89 e5 41 57 …` — a real prologue.)

Image bases: `com.apple.iokit.IOGraphicsFamily` `0x14701000`, `com.apple.iokit.IONDRVSupport`
`0x1475a000` (`__text` `0x1475a5b0..0x14761d5c`), `com.apple.kext.AMDFramebuffer` `0x277000`,
`com.apple.kext.AMDRadeonX6000Framebuffer` `0xbf…`.

---

## 2. The code path for `kIOFBTimingRangeKey` in this build

### 2.1 There is no consumer in `IOFramebuffer` / `IOGraphicsFamily`

`grep -abo IOFBTimingRange` over the 361 MB finds exactly **seven** copies of the literal, in seven
different kexts, and `IOGraphicsFamily` is **not** among them (§9 has the full per-kext reconciliation
and the reader analysis):

| file offset | owning image | what it does there |
|---|---|---|
| `0x2980b2` | `com.apple.kext.AMDFramebuffer` | publishes V2 (312 B), and reads it back |
| `0xc14033d` | `com.apple.kext.AMDRadeonX6000Framebuffer` | publishes V2 (312 B) / removes it |
| `0x13e66a1d`, `0x14063835`, `0x142266c7` | Intel CFL / ICL-LP / KBL framebuffers | publish |
| `0x142cb5d3` | `com.apple.driver.AppleParavirtGPU` | publishes |
| `0x147620c7` | `com.apple.iokit.IONDRVSupport` | publishes — **dead code**, see 2.2 |

`kIOFBTimingRangeKey` is a `#define` string (`IOGraphicsTypes.h:1456`), so any code that uses it must
carry the literal. `IOFramebuffer::setAttribute` (`0x1471f938`) and `IOFramebuffer::getAttribute`
(`0x1471fe26`) are real code in `__text`, but neither references it, and there is no *attribute
selector* for a timing range in the SDK at all (`grep` for `'trng'`/`*Range` in `IOGraphicsTypes.h`:
only the property key). So:

* `setAttribute`/`getAttribute` are **not** the path for this key — it is a **registry property**.
* The base `IOFramebuffer::getTimingInfoForDisplayMode` (`0x1472103a`), `validateDetailedTiming`
  (`0x14721046`), `setDetailedTimings` (`0x14721052`), … are 12-byte stubs that `mov eax, 0xe00002c7`
  (`kIOReturnUnsupported`) and return; the real behaviour is in the subclass.

### 2.2 The only NDRV-side writer: `IONDRVFramebuffer::setInfoProperties()` — and it is unreferenced

`IONDRVFramebuffer::setInfoProperties()` @ **`0x1475ccd0`** (`__ZN17IONDRVFramebuffer17setInfoPropertiesEv`):

```asm
1475ccd0  push rbp ; mov rbp, rsp ; push r14 ; push rbx ; sub rsp, 0x120
1475ccde  mov  rbx, rdi
1475cce1  or   byte ptr [rdi + 0x274], 0x40      ; a flag on the framebuffer object
1475cce8  mov  rax, qword ptr [rdi]              ; vptr
1475cceb  lea  rsi, [rip + 0x53d5]               ; -> 0x147620c7 "IOFBTimingRange"
1475ccf2  call qword ptr [rax + 0x2a0]           ; IORegistryEntry::removeProperty(key)
1475ccf8  lea  r14, [rbp - 0x130]                ; 0x130 bytes of stack for the record
1475ccff  mov  esi, 0xf0                         ; 0xf0 = 240 = sizeof(VDDisplayTimingRangeRec)
1475cd04  mov  rdi, r14
1475cd07  call 0xc0b4                            ; zero the buffer (import stub; rdi=buf, esi=0xf0)
1475cd0c  mov  dword ptr [r14], 0xf0             ; record->csRangeSize = sizeof(record)   [offset 0]
1475cd13  mov  rdi, rbx
1475cd16  mov  esi, 0x1e                         ; status code 0x1e = cscGetTimingRanges
1475cd1b  mov  rdx, r14
1475cd1e  call 0x1475baf6                        ; _doStatus(this, 0x1e, record)
1475cd23  test eax, eax
1475cd25  jne  0x1475cd46                        ; driver refused -> leave the property absent
1475cd27  mov  rax, qword ptr [rbx]
1475cd2a  lea  rsi, [rip + 0x5396]               ; -> "IOFBTimingRange"
1475cd31  lea  rdx, [rbp - 0x130]                ; the driver-filled record
1475cd38  mov  rdi, rbx
1475cd3b  mov  ecx, 0xf0                         ; 240 bytes
1475cd40  call qword ptr [rax + 0x288]           ; IORegistryEntry::setProperty(key, bytes, 0xf0)
1475cd46  …                                      ; then the same for "IOFBScalerInfo"
```

`_doStatus` @ `0x1475baf6` (`__ZN17IONDRVFramebuffer9_doStatusEPS_jPv`) is the generic NDRV status
trampoline. It builds a call record on the stack and hands it to the **virtual `doDriverIO`**:

```asm
1475bb56  lea  rdx, [rbp - 0x60]                 ; the NDRV call record
1475bb5a  mov  word  ptr [rdx + 0x1a], r15w      ; record->code   = status code (0x1e, 0x1f, 0xd, 0x24 …)
1475bb5f  mov  qword ptr [rdx + 0x20], r14       ; record->params = the 240-byte buffer
1475bb63  mov  rax, qword ptr [rbx]              ; vptr
1475bb66  mov  rdi, rbx                          ; this
1475bb69  mov  esi, 1                            ; commandID = 1 (status)
1475bb6e  mov  ecx, 0x85                         ; commandCode  = 0x85
1475bb73  mov  r8d, 4                            ; commandKind  = 4
1475bb79  call qword ptr [rax + 0xaf0]           ; doDriverIO(1, &record, 0x85, 4)
```

Vtable slot `+0xaf0` resolves (via the collection's rebase-fixup encoding) to
**`IONDRVFramebuffer::doDriverIO(UInt32, void*, UInt32, UInt32)` @ `0x1475c3a6`**, which is exactly the
virtual our plugin replaces with `wrapDoDriverIO(void*, unsigned, void*, unsigned, unsigned)`
(`__ZN12_GLOBAL__N_114wrapDoDriverIOEPvjS0_jj` in the panic). Slot `+0x2a0`/`+0x288` are the imported
`IORegistryEntry::removeProperty(const char *)` / `setProperty(const char *, const void *, unsigned)`
(argument shapes match; both slots are bind fixups, not local rebases).

Slot naming evidence: `+0x288` takes `(this, key, bytes, size)` and is the only 4-argument property
call — `IORegistryEntry::setProperty(const char *, const void *, unsigned)`; `+0x2a0` takes
`(this, key)` and is used by `AMDRadeonX6000_…::reportCapabilities` (`0xbf5e246`) precisely where it
also publishes the key, i.e. `removeProperty`; `+0x2b8` takes `(this, key)` and its result is
`OSDynamicCast` to `OSData` in `AMDFramebuffer` (`0x27c291`), i.e. `getProperty`. All three are bind
fixups (their low bits decode to `0x…98bd00`/`0x…98be60`/`0x…98bf60`, not to code), so they are
imports, as expected for IOKit base-class methods.

**`setInfoProperties` is dead in 26.6.2.** Exhaustive checks over the whole 361 MB:

* no `call rel32` to `0x1475ccd0` anywhere in `IONDRVSupport.__text` (349 direct calls scanned);
* no `lea rip+…` to it;
* no fixup-encoded pointer to it: this collection encodes local (`rebase`) pointers as
  `0x0040000000000000 | (0x40000000 + vmaddr)` — verified on the two known vtable slots: `+0xaf0` →
  `0x004000005475c3a6` → `doDriverIO 0x1475c3a6`; `+0xc10` → `0x004000005475d6b2` →
  `validateDisplayMode 0x1475d6b2`. A pointer to `setInfoProperties` would therefore contain the
  4-byte pattern `d0 cc 75 54` (`0x40000000 + 0x1475ccd0`), which occurs **nowhere** in the file.
  The only occurrence of the plain value `0x1475ccd0` is the symbol-table entry itself.

So on the NDRV path **macOS 26 no longer publishes a timing range at all** unless someone outside does
— and nothing in the kernel reads it either.

### 2.3 The record the driver is expected to answer (status 0x1e)

`cscGetTimingRanges = 30` — `IONDRVLibraries`' `IOMacOSVideo.h:400`: *"Takes a VDDisplayTimingRangePtr"*.
The record (`IOMacOSVideo.h:953`) is 240 bytes and is `IODisplayTimingRange` V1 with the first 32 bytes
re-purposed as a record header:

| offset | `VDDisplayTimingRangeRec` | `IODisplayTimingRangeV1` (same bytes once published) |
|---|---|---|
| `0x00` | `csRangeSize` (= `0xf0`) | `__reservedA[0]` |
| `0x04` | `csRangeType` | `__reservedA[1]` |
| `0x08` | `csRangeVersion` (0 ⇒ V1) | `version` |
| `0x0c` | `csRangeReserved` | `__reservedB[0]` |
| `0x10..0x1f` | `csRangeBlockIndex/Group/BlockCount/Flags` | `__reservedB[1..4]` |
| `0x20` | `csMinPixelClock` (u64) | `minPixelClock` |
| `0x28` | `csMaxPixelClock` (u64) | `maxPixelClock` |
| `0x30..` | pixel error, sync flags, signal levels/configs, frame/line rates, totals, char sizes, all min/max clocks, borders, link clocks | identical |
| `0xe4..0xf0` | `csReserved6/7/8` (u32×3) | `supportedPixelEncoding`, `supportedBitsPerColorComponent`, `supportedColorimetryModes`, `supportedDynamicRangeModes`, `__reservedF[1]` |

That last row is worth keeping: the NDRV record has **no named colour fields**, but its reserved tail is
byte-for-byte where V1 puts the colour capabilities (`0xe4` pixelEncoding, `0xe6` bpc, `0xe8`
colorimetry, `0xea` dynamicRange). So on this path the colour bits can only be transmitted by filling
`csReserved6/7` — undocumented, but layout-exact.

`sizeof(VDDisplayTimingRangeRec) = 240 = 0xf0`, matching the immediate the framework passes to
`bzero`/`setProperty` and its `csRangeSize` initialiser. `sizeof(IODisplayTimingRangeV1) = 240` too;
`sizeof(IODisplayTimingRangeV2) = 312 = 0x138` (V1 + `maxBandwidth` + the DSC fields + `__reservedC[6]`).

### 2.4 What Apple's own framebuffer drivers publish in the same build

`AMDFramebuffer::setInfoProperties()` — site `0x287797`:

```asm
287790  mov  rdi, [rbp-0x1b0]          ; this
287797  mov  rax, [rdi]                ; vptr
28779a  lea  rsi, [rip + 0x10911]      ; -> 0x2980b2 "IOFBTimingRange"
2877a1  lea  rdx, [rbp - 0x170]        ; blob just filled (via vtable+0xa28 = "get timing range")
2877a8  mov  ecx, 0x138                ; 312 = sizeof(IODisplayTimingRangeV2)
2877ad  call qword ptr [rax + 0x288]   ; setProperty(key, blob, 0x138)
```

`AMDRadeonX6000_AmdRadeonFramebuffer::reportCapabilities()` — sites `0xbf5e23c` / `0xbf5e251`:
same key, `ecx = 0x138` (312) on the success path, `call [rax+0x2a0]` (`removeProperty`) on the failure
path.

`AMDFramebuffer` also **reads it back**, twice, always as OSData:

```asm
27c287  mov  rax, [rdi]
27c28a  lea  rsi, [rip+0x1be21]      ; "IOFBTimingRange"      (inside validateDetailedTiming)
27c291  call qword ptr [rax + 0x2b8] ; getProperty(key) -> OSObject*
27c297  mov  rdi, rax
27c29a  mov  rax, [rip + 0x1ed8f]    ; -> 0x29b030 (OSData meta-class pointer)
27c2a1  mov  rsi, [rax]
27c2a4  call 0xc07e                  ; OSDynamicCast(OSData, prop)
```

(the same trio at `0x294d10` inside `getPropertiesForUserClient`). That fixes the **type** contract:
**OSData whose bytes are the struct** — not a dict, not an OSNumber, not a by-value struct in the
property table.

Consequence for the NDRV path: the blob a plugin publishes is consumed, if at all, **outside the
kernel** (CoreDisplay/SkyLight read the framebuffer's registry entry). The kernel half is a pure
publish-the-driver's-answer relay, and in 26.6.2 that relay is not even called.

---

## 3. Why the 00:30 boot panicked: the fault is in our own log call, not in the blob

All three reports are the same crash. From `Kernel-2026-11-26-003058.panic` (`macOSPanicString`):

```
panic(cpu 4 caller 0xffffff801b0d50da): Kernel trap at 0xffffff801b79e67e, type = 14=page fault
CR2: 0x00000000017d7840        RAX: 0x00000000017d7840        RDX: 0x000000007fffffff
RDI: 0x0000000000000000        RSI: 0xffffffabaad78690       Error code: 0x0000000000000000
Panicked task … pid 212: WindowServer
Backtrace (CPU 4), panicked thread: …
0xffffffabaad78210 : 0xffffff801b79e67e mach_kernel : _mac_schedule_telemetry + 0x348e   <- faulting RIP
0xffffffabaad783b0 : 0xffffff801b4e9b12 mach_kernel : _vsnprintf + 0x22
0xffffffabaad783d0 : 0xffffff801eeff59a com.hackintosh.AMDRadeonRDNA4 : __Z8rdna4LogPKcz + 0x9c
0xffffffabaad786c0 : 0xffffff801ef058cb com.hackintosh.AMDRadeonRDNA4 : __ZN12_GLOBAL__N_16attachERNS_7FbEntryE + 0x1e82
0xffffffabaad78910 : 0xffffff801ef02818 com.hackintosh.AMDRadeonRDNA4 : __ZN12_GLOBAL__N_114wrapDoDriverIOEPvjS0_jj + 0x34c
0xffffffabaad78980 : 0xffffff7fb00418da com.apple.iokit.IONDRVSupport : __ZN17IONDRVFramebuffer11checkDriverEv + 0xfc
0xffffffabaad78ab0 : 0xffffff7fb00407db com.apple.iokit.IONDRVSupport : __ZN17IONDRVFramebuffer16enableControllerEv + 0xa1
0xffffffabaad78b20 : 0xffffff7fafffff81 com.apple.iokit.IOGraphicsFamily : __ZN13IOFramebuffer4openEv + 0x60b
```

(00:33:28 and 00:40:36 are identical apart from cpu 2 / cpu 0 and the slide; same CR2, same frame
offsets, always pid 212 WindowServer — deterministic, one specific statement.)

Reading the frame chain bottom-up:

1. `IOFramebuffer::open` → `IONDRVFramebuffer::enableController` → `IONDRVFramebuffer::checkDriver`:
   WindowServer is opening the framebuffer.
2. `checkDriver` calls the virtual `doDriverIO` (`[rax+0xaf0]`) at `0x1475c8d4` with
   `(this, commandID = 0, contents = …, commandCode = 0x80, commandKind = 4)`; the return address
   `checkDriver + 0xfc = 0x1475c8da` is exactly the panic frame. So this is the NDRV **initialize**
   call, not a status call.
3. Our `wrapDoDriverIO` dispatches it into our `attach(FbEntry &)` — the NDRV translator's attach.
4. `attach + 0x1e82` calls **our** `rdna4Log(char const *, ...)` (`__Z8rdna4LogPKcz`), and
   156 bytes into it, `vsnprintf` is invoked.
5. `vsnprintf` dereferences `0x17d7840` and dies. `Error code 0x0` = read, page not present;
   `CR2 == RAX` = the value was loaded from the varargs save area into `RAX` and used as an address.

That is the whole of the evidence: **there is no IOFramebuffer/IONDRVFramebuffer frame between
`vsnprintf` and our own code.** If the kernel had been interpreting a published `IOFBTimingRange`, the
fault would be inside `IOFramebuffer`/`IOGraphicsFamily` code (or in `setProperty`/`OSData`), and
`vsnprintf` would not be in the stack at all. `RDX = 0x7fffffff` is the bounded-length idiom
(`strnlen`-class helper) used by a `%s` conversion, and `CR2 = RAX = 0x17d7840 = 25,000,000` is exactly
the `minPixelClock` we had just written into the structure — i.e. the `%s` argument was a **number from
our struct**, not a pointer.

Most likely shape of the defect (cannot be pinned without our binary — see §7): a format/argument
mismatch in that one `rdna4Log(...)` call, either

* a `%s` where a `%u`/`%llu` was meant and `minPixelClock` sat in that argument slot, or
* an argument-count shift — one argument missing before a `%s`, so the `%s` consumed the neighbouring
  numeric argument (the classic failure when a specifier is added to a long log line).

Either way it is our bug, it is deterministic, and it is **independent of the timing range being
published at all**: the same statement would fault on any boot that reaches that line. It also means
the blob's layout was never exercised in that crash.

Why the old reading was wrong: `docs/hw-logs/panics-20261126/README.md` inferred "the kernel used a
clock value from our blob as an address" from `CR2 == minPixelClock`. That inference ignores the frame
chain: a value can only be dereferenced by the code that holds it, and here that code is XNU's
`vsnprintf` called from our logger. `0x17d7840` being a *field of our blob* tells us where the number
came from, not who was interpreting the blob.

Note also what did **not** happen: no frame in the crash path is `IOService::setProperty`,
`OSData::withBytes`, or any IOFramebuffer property walk. Publishing the property itself never even
appears in the trace.

---

## 4. Verdicts

**(a) Wrong struct version/layout — true about the blob, false as the cause.**
`IODisplayTimingRangeV2` (`0x138` = 312 bytes on the native AMD path in this same build) is not what
the NDRV path would ever publish: `IONDRVFramebuffer::setInfoProperties` asks for `0xf0` = **240**
bytes and the record it fills is a `VDDisplayTimingRangeRec`, i.e. the **V1** shape (`version` at 0x08
= 0 = `kIOTimingRangeV1`). Confidence in the contract: **high**. Confidence that this caused the panic:
**very low** — refuted by §3. Extra nuance that matters for a retry: V2 is a strict superset of V1
(same fields, same offsets, `maxBandwidth`+DSC appended), so a V2 blob is *byte-compatible* with a V1
reader; the only thing a V1-only consumer can object to is `version == 2`.

**(b) Wrong object type — no.** The property is an `OSData` whose bytes are the struct;
`AMDFramebuffer::validateDetailedTiming` does `getProperty(key)` → `OSDynamicCast(OSData)` →
`getBytesNoCopy()` (`0x27c28a..0x27c2a4`, again at `0x294d10`). Not a dict, not an `OSNumber`, not a
by-value struct, not a `setAttribute` selector. Confidence: **high**.

**(c) "Must be answered by the NDRV driver through siid 0x12" — right principle, wrong code, and in
26.6.2 the consumer of that answer is gone.**
The selector is **`cscGetTimingRanges` = 30 = 0x1e** (`IOMacOSVideo.h:400`, "Takes a
VDDisplayTimingRangePtr"), delivered as `doDriverIO(1, &record, 0x85, 4)` with `record.code = 0x1e`
and `record.params` = the 240-byte buffer. `0x12` is not in this path at all; the plan's cited
`0x1475dfc4` is `IONDRVFramebuffer::getPixelInformation`, and `getTimingInfoForDisplayMode` uses 13
(`cscGetModeTiming`), 31 (`cscGetDetailedTiming`) and 36 (`cscGetScaler`) — see §6. Confidence in the
code numbers: **high**. But `setInfoProperties()` itself is unreferenced in this build (§2.2), so
answering 0x1e today changes nothing: the framework will not turn it into a property.

**(d) Something else — yes, for the panic.** Our own `rdna4Log`/`vsnprintf` varargs bug (§3).
Confidence: **high** for "the faulting dereference is our own log formatting"; **medium** for which
exact specifier/argument was mismatched.

Two consequences worth carrying into the HDR plan:

1. **Publishing this property cannot panic this kernel.** No kernel code loads it (no consumer, and the
   one publisher is dead). A malformed blob can at worst confuse *userspace* (a WindowServer crash, not
   a kernel trap). The 00:30 crash is therefore not evidence that the property is dangerous — it is
   evidence that our logging is.
2. The withdrawal of the range was based on a wrong diagnosis; the *reason to be careful* is different:
   on this path the property has no kernel-side contract at all, so its effect on mode generation is
   unproven, and the deterministic lever for HDR is the mode table plus the detailed-timing colour
   fields (§6).

---

## 5. Can the contract be honoured safely? The exact shape, if you publish at all

### 5.1 The exact structure for the NDRV path

**240 bytes, `version` = 0, `csRangeSize` = 0xf0.** The bytes are simultaneously
`VDDisplayTimingRangeRec` (what the driver would answer) and `IODisplayTimingRangeV1` (what the OS
reads) — same layout from `0x00`, different names for the first 32 bytes:

| offset | size | `VDDisplayTimingRangeRec` | `IODisplayTimingRangeV1` | note |
|---|---|---|---|---|
| `0x00` | 4 | `csRangeSize` = **0xf0** | `__reservedA[0]` | framework writes it before asking the driver |
| `0x04` | 4 | `csRangeType` = 0 | `__reservedA[1]` | |
| `0x08` | 4 | `csRangeVersion` = **0** (`kIOTimingRangeV1`) | `version` | the only version gate |
| `0x0c` | 4 | `csRangeReserved` = 0 | `__reservedB[0]` | |
| `0x10` | 16 | block index / group / count / flags | `__reservedB[1..4]` | |
| `0x20` | 8 | `csMinPixelClock` (Hz) | `minPixelClock` | **the field whose value appeared in `CR2`** |
| `0x28` | 8 | `csMaxPixelClock` | `maxPixelClock` | |
| `0x30` | 4 | `csMaxPixelError` | same | |
| `0x34..0x3c` | 12 | sync flags / signal levels / signal configs | same | |
| `0x40..0x4c` | 16 | min/max frame rate, min/max line rate | same | |
| `0x50..0x5c` | 16 | max h/v total + 2 reserved | same | |
| `0x60..0x6e` | 14+2 | 14 char sizes + 1 reserved u16 | same | |
| `0x70..0xaf` | 64 | min/max h active, h blank, h sync offset, h pulse, v active, v blank, v sync offset, v pulse | same | |
| `0xb0..0xcf` | 32 | min/max borders L/R/T/B | same | |
| `0xd0..0xe3` | 20 | max num links + 4 link clocks (kHz) | same | |
| `0xe4` | 2+2 | `csReserved6` (u32, offset 0xe4) | `pixelEncoding` @0xe4, `bitsPerColorComponent` @0xe6 | **colour pair 1** |
| `0xe8` | 2+2 | `csReserved7` (u32, offset 0xe8) | `colorimetry` @0xe8, `dynamicRange` @0xea | **colour pair 2** |
| `0xec` | 4 | `csReserved8` | `__reservedF[0]` | |

`sizeof(VDDisplayTimingRangeRec) = sizeof(IODisplayTimingRangeV1) = 240`; `sizeof(IODisplayTimingRangeV2)
= 312` (V1 + `maxBandwidth`, the DSC block and `__reservedC[6]`), which is why `AMDFramebuffer` and
`AMDRadeonX6000_AmdRadeonFramebuffer` pass `0x138` and the NDRV path passes `0xf0`.

**Fields macOS reads:** all of the envelope (clocks, totals, rates, link clocks) — the documented
purpose is "allow the OS to program detailed timings that fall within its range"
(`IOGraphicsTypes.h:608`) — plus, for the HDR question, the two colour pairs at `0xe4`/`0xe8`
(`supportedPixelEncoding`, `supportedBitsPerColorComponent`, `supportedColorimetryModes`,
`supportedDynamicRangeModes`). On the NDRV path those bytes have no field names in the NDRV header —
they are `csReserved6/7` — so using them is layout-compatible but unsanctioned.

### 5.2 The minimum-risk publication

If the range is to be published from the plugin at all:

1. **240 bytes, `OSData::withBytes(buf, 240)`** — not `sizeof(IODisplayTimingRangeV2)` (312). A 312-byte
   blob is not *wrong* for a V1 reader (V2 starts with V1's fields), but it advertises DSC that the NDRV
   path cannot honour, and it is not what this build's NDRV publisher would ever produce.
2. `csRangeSize = 0xf0`, `csRangeVersion = 0`, every reserved byte zero, `csRangeBlockCount = 1` if you
   want to mimic Apple's record; `minPixelClock`/`maxPixelClock` in Hz as `UInt64` at `0x20`/`0x28`.
3. Colour bits only in the `0xe4`/`0xe8` pairs (packed little-endian u16 pairs), e.g.
   `pixelEncoding = kIORangePixelEncodingRGB444`, `bitsPerColorComponent =
   kIORangeBitsPerColorComponent10`, `colorimetry = BT2020|BT2100`, `dynamicRange = HDR10`.
4. **It cannot panic the kernel** (§3/§4d): nothing in the kernel dereferences it. The worst case is a
   userspace (WindowServer) reaction; keep the boot-arg switch so it can be disabled without the kernel
   being involved, but there is no boot-loop risk of the 2026-11-26 kind.
5. **Do not expect the kernel to maintain it:** `IONDRVFramebuffer::setInfoProperties` is dead in this
   build, so the property will not be removed or overwritten by the framework — it stays exactly as
   published for the session.

Hard caveat, and the honest answer to "should we publish this property in this architecture": **on this
path the property has no kernel-side contract.** There is no kernel reader (IOGraphicsFamily has no such
string), the framework publisher is dead, and the only proven consumer of the key in the whole
collection is the framebuffer driver itself (`AMDFramebuffer` reading back its own property). Whether
CoreDisplay/SkyLight will use a timing range published by an `IONDRVFramebuffer` — and whether it gates
on `version` — is **unverified** (see §7). So the range is at best a userspace hint. The *deterministic*
lever for a 10-bit/HDR timing on this path is the detailed-timing record the driver already controls
(§6), plus the mode table; the range property should be treated as an optional experiment, not as the
mechanism the plan assumed.

---

## 6. Bonus: what `getTimingInfoForDisplayMode` relays, and which colour fields a driver can influence

`IONDRVFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID, IOTimingInformation *)` @
**`0x1475e2cc`** is a three-call relay:

```asm
1475e2e6  mov rax,[rdi] ; xor edx,edx ; xor ecx,ecx
1475e2ed  call [rax+0xc10]                    ; validateDisplayMode (slot verified: 0x1475d6b2)
1475e2fe  lea rdx,[rbp-0x48]                  ; VDTimingInfoRec
1475e31b  mov [rdx], r12d                     ; csTimingMode = displayMode
1475e328  mov qword [rdx+0x10], 0xaaaaaaaa6465636c   ; csTimingFormat = 'decl' (kDeclROMtables)
1475e334  call _doStatus(this, 0x0d, rdx)     ; 13 = cscGetModeTiming
1475e34a  cmp dword [rbp-0x38], 0x6465636c    ; driver must echo 'decl'
1475e358  mov eax,[rbp-0x34] ; mov [rbx], eax ; info->appleTimingID = csTimingData
1475e35d  cmp dword [rbx+4], 0 ; jns ret      ; only if kIODetailedTimingValid was requested
1475e367  lea r13,[rbx+8]                      ; &info->detailedInfo.v2  (160 bytes)
1475e374  memset(rbx+0x0c, 0, 0x9c)
1475e379  mov dword [rbx+8], 0xa0             ; record.csTimingSize = sizeof(VDDetailedTimingRec)
1475e380  mov [rbx+0x18], r12d                ; record.csDisplayModeID (record+0x10) = displayMode
1475e38f  call _doStatus(this, 0x1f, r13)     ; 31 = cscGetDetailedTiming  <-- driver fills the timing
1475e398  (on error) and byte [rbx+7], 0x7f   ; clear kIODetailedTimingValid
1475e3a3  mov [r13+8],0 ; mov [r13],0         ; clear record 0x00..0x0c  (V2 __reservedA[0..2])
1475e3ab  mov dword [rbx+0xa2],0 ; mov word [rbx+0xa6],0   ; clear record 0x9a..0xa0 (V2 __reservedB[2..4])
1475e3f2  call _doStatus(this, 0x24, scalerRec) ; 36 = cscGetScaler
1475e40c  (on success) or byte [rbx+7], 0x40  ; flags |= kIOScalingInfoValid
1475e410  record 0x1c/0x20/0x24/0x14/0x18 <- scalerRec 0x20/0x24/0x28/0x2c/0x30
```

So:

* the raster (`pixelClock`, active/blanking/sync/border, `numLinks`) comes **straight from the driver's
  `cscGetDetailedTiming` buffer** — `VDDetailedTimingRec` maps onto `IODetailedTimingInformationV2`
  field-for-field from offset `0x20` on (`csPixelClock` 0x28/record vs 0x28/V2, etc.);
* the framework rewrites only the **scaling** fields (`0x0c..0x1f` of V2) from `cscGetScaler`, sets
  `kIOScalingInfoValid`, and clears the NDRV header bytes and the V2 tail `0x9a..0xa0`;
* the colour fields — `pixelEncoding` @ `0x88`, `bitsPerColorComponent` @ `0x8a`, `colorimetry` @
  `0x8c`, `dynamicRange` @ `0x8e` (i.e. record `csReserved3` = u32 @ `0x88`, `csReserved4` = u32 @
  `0x8c`) — are **not touched**. Whatever the driver writes there reaches
  `IOTimingInformation.detailedInfo.v2` unchanged, and `bitsPerColorComponent`/`colorimetry`/
  `dynamicRange` are exactly what `IOFramebuffer` and userspace look at to describe a 10-bit/HDR timing.

`src/ndrv.cpp:350 getDetailedTiming()` currently writes `r = VDDetailedTimingRec{}` plus the raster and
leaves those two words zero ⇒ **every detailed timing this driver reports is 8 bpc / native RGB / SDR**,
which is precisely the "without the hook every timing comes back SDR" observation in the plan — but the
fix belongs in `getDetailedTiming` (fill record `0x88`/`0x8c`), not in a Lilu hook of
`getTimingInfoForDisplayMode`. Two lines, no new trampoline, and it is inside the documented record the
driver already answers.

Also corrected: the plan's "`0x1475dfc4`, NDRV siid 0x12" is `IONDRVFramebuffer::getPixelInformation`,
whose status code is indeed **18 = `cscGetVideoParameters`** (`0x1475e079: mov esi, 0x12`, right after
`_doStatus(0x0d)` for the mode record at `0x1475e068`). `0x12` has nothing to do with the timing relay.

---

## 7. What would settle the remaining doubt

1. **Which log statement faulted.** Needs the compiled HDR build: disassemble `attach(FbEntry&)` around
   `+0x1e82`, or `rdna4Log+0x9c`, and look at the format/argument list. Not available here: the kext is
   not in `SystemKernelExtensions.kc` (no `AMDRadeonRDNA4` fileset entry, no `rdna4Log` string anywhere
   in the 361 MB), there are no build artefacts under `a4/` (only `tools/build`), and the publisher was
   never committed — `git log -S IOFBTimingRange` yields only the two doc commits `dacd97e`/`8770e66`.
   Rebuilding the withdrawn `publishHdrRange()` and disassembling it would settle it in minutes.
2. **Whether CoreDisplay consumes `IOFBTimingRange` from an `IONDRVFramebuffer`, and whether it gates on
   `version`.** Needs the Mac's dyld shared cache (`CoreDisplay`, `SkyLight`) — not on this host, and the
   Mac is out of scope for this task. An offline `nm`/disassembly of `CoreDisplay` around the literal
   `IOFBTimingRange` would answer it; so would one instrumented boot reading back the property from a
   logged-in session (`ioreg -l -k IOFBTimingRange` plus AllRez's mode list).
3. **That `setInfoProperties` is dead in the shipping build.** The negative here is exhaustive for this
   file (no `call`, no `lea`, no rebase-fixup pointer), but a second opinion would be a boot with a probe
   on `doDriverIO` status 0x1e: if it never fires, the relay is confirmed dead on the running system.
4. Minor: `sizeof`/`version` behaviour for V2 on the native path would be confirmed by dumping
   `IOFBTimingRange` from an AMD Mac (X6000) and comparing `0x138` bytes with `version == 2`.

---

## 8. Address appendix (link-time vmaddr = file offset in this collection)

| address | symbol / item |
|---|---|
| `0x1475a000` | `com.apple.iokit.IONDRVSupport` image base (`__text` `0x1475a5b0..0x14761d5c`) |
| `0x147620c7` | `"IOFBTimingRange"` (only copy in this kext) |
| `0x147620d7` | `"IOFBScalerInfo"` |
| `0x1475ccd0` | `IONDRVFramebuffer::setInfoProperties()` — **unreferenced** |
| `0x1475cceb` / `0x1475ccf2` | `removeProperty("IOFBTimingRange")` call site (vtable +0x2a0) |
| `0x1475cd1e` | `_doStatus(this, 0x1e, buf)` — `cscGetTimingRanges` (30) |
| `0x1475cd40` | `setProperty("IOFBTimingRange", buf, 0xf0)` (vtable +0x288) |
| `0x1475baf6` | `IONDRVFramebuffer::_doStatus(OSObject*, UInt32 code, void *params)` |
| `0x1475bb5a` / `0x1475bb5f` | record `+0x1a` = code, `+0x20` = params |
| `0x1475bb79` | `doDriverIO(1, &record, 0x85, 4)` |
| `0x1475c3a6` | `IONDRVFramebuffer::doDriverIO(UInt32, void*, UInt32, UInt32)` — vtable `+0xaf0` |
| `0x1475c7de` | `IONDRVFramebuffer::checkDriver()`; NDRV init call at `0x1475c8d4` (`doDriverIO(0, …, 0x80, 4)`), return `0x1475c8da` = panic frame `+0xfc` |
| `0x1475b73a` | `IONDRVFramebuffer::enableController()` |
| `0x1471a976` | `IOFramebuffer::open()` (IOGraphicsFamily) |
| `0x1475e2cc` | `IONDRVFramebuffer::getTimingInfoForDisplayMode(int, IOTimingInformation*)` |
| `0x1475e334` / `0x1475e38f` / `0x1475e3f2` | status 0x0d (`cscGetModeTiming`) / 0x1f (`cscGetDetailedTiming`) / 0x24 (`cscGetScaler`) |
| `0x1475dfc4` | `IONDRVFramebuffer::getPixelInformation(iii…)` — status `0x12` at `0x1475e079` |
| `0x1475d6b2` | `IONDRVFramebuffer::validateDisplayMode(...)` — vtable `+0xc10` |
| `0x1471f938` / `0x1471fe26` | `IOFramebuffer::setAttribute(IOSelect, uintptr_t)` / `getAttribute(IOSelect, uintptr_t*)` — neither touches the key |
| `0x1472103a`, `0x14721046`, `0x14721052` | `IOFramebuffer::getTimingInfoForDisplayMode` / `validateDetailedTiming` / `setDetailedTimings` base stubs returning `0xe00002c7` (`kIOReturnUnsupported`) |
| `0x277000` | `com.apple.kext.AMDFramebuffer` base |
| `0x287797` | `AMDFramebuffer::setInfoProperties` — `setProperty(key, blob, 0x138)` (V2, 312 B) |
| `0x27c28a` / `0x294d10` | `AMDFramebuffer::validateDetailedTiming` / `getPropertiesForUserClient` — `getProperty` + `OSDynamicCast(OSData)` |
| `0xbf5e23c` / `0xbf5e251` | `AMDRadeonX6000_AmdRadeonFramebuffer::reportCapabilities` — `removeProperty` / `setProperty(…, 0x138)` |

Cross-kext calls such as `0xc0b4`, `0xc330`, `0xd080` and the `kalloc_type` helpers are not code in
any kext: they land in the fileset's own `__BRANCH_STUBS` (`0xc000..0x20000`, 16-byte `jmp [rip+…]`
entries) and `__BRANCH_GOTS` (`0x20000..0x38000`). Symbolicating them with a kext's symbol table — as
the inherited `diz.py` does — produces the nonsense `__ZZ23RegistryCStrEntryLookupE…` names seen above;
ignore those annotations, the imports themselves were not needed for any claim in this document
(the property-call identities come from their argument shapes and from Apple's own drivers' use of the
same slots).

Tooling used and left in `/tmp/trng-work2/`: `kc2.py` (fileset/Mach-O/sections/symbols, correct
`LC_FILESET_ENTRY` and `lc_str` handling), `dis2.py` (capstone with `skipdata`, symbol annotation),
plus the inherited `kcmod.py`/`diz.py` (unchanged; their `filepos` assumption is the one fixed here).

---

## 9. Reconciliation: all seven `IOFBTimingRange` occurrences, and who (if anyone) reads the property

**Correction first.** The summary in §0 said *"`IOGraphicsFamily` … does not contain the string
`IOFBTimingRange` at all"*, and that sentence has been read (reasonably) as a whole-collection claim.
It was meant scoped to `IOGraphicsFamily`, but the wording invited exactly that misreading, so it is
wrong as written and is corrected here: the literal occurs **seven** times in the 361 MB collection,
in **seven different kexts**. What is true is the narrower statement — `IOGraphicsFamily` is not one
of them, and no kext *reads and interprets* a foreign `IOFBTimingRange` (see the reader column below).

Method: each hit was mapped to its owning `LC_FILESET_ENTRY` with `kc2.py`, then that image's `__text`
was scanned with capstone for every rip-relative reference to the string address, and each reference
site was classified by the call it feeds — `IORegistryEntry::setProperty(const char *, const void *,
unsigned)` (vtable `+0x288`, 4 args, size immediate), `removeProperty` (`+0x2a0`, 2 args) or
`getProperty` (`+0x2b8`, 2 args, result then `OSDynamicCast`-ed to `OSData`). Every image keeps its own
`__cstring` inside its own `__TEXT` (no cross-kext string dedup in a KC), so "absent from a kext" is a
meaningful statement about that kext.

| file offset | string vmaddr | owning kext | sites | what the string does there |
|---|---|---|---|---|
| `0x2980b2` | `0x2980b2` | `com.apple.kext.AMDFramebuffer` | `0x28779a`, `0x27c28a`, `0x294d10` | **publish** in `AMDFramebuffer::setInfoProperties` (`setProperty`, `0x138` = 312 B) **plus the only two reads in the collection**: `validateDetailedTiming` and `getPropertiesForUserClient` do `getProperty` → `OSDynamicCast(OSData)` → `getBytesNoCopy`, i.e. the driver reads back *its own* property |
| `0xc14033d` | `0xc14033d` | `com.apple.kext.AMDRadeonX6000Framebuffer` | `0xbf5e23c`, `0xbf5e251` | publish in `reportCapabilities`: `removeProperty` on the "no HDR" path (`0xbf5e246`), `setProperty(…, 0x138)` on the capable path |
| `0x13e66a1d` | `0x13e66a1d` | `com.apple.driver.AppleIntelCFLGraphicsFramebuffer` | `0x13df640a`, `0x13df91c6` | `AppleIntelFramebuffer::initTimingRange` → `setProperty(…, 0x138)`; `updateTimingRange` → `removeProperty` then `setProperty(…, 0x138)` |
| `0x14063835` | `0x14063835` | `com.apple.driver.AppleIntelICLLPGraphicsFramebuffer` | `0x13ff3049`, `0x13ff8eaf` | same pair (`initTimingRange` tail-jumps into `setProperty(…, 0x138)`; `updateTimingRange` remove+set) |
| `0x142266c7` | `0x142266c7` | `com.apple.driver.AppleIntelKBLGraphicsFramebuffer` | `0x141b7411`, `0x141ba7aa` | same pair |
| `0x142cb5d3` | `0x142cb5d3` | `com.apple.driver.AppleParavirtGPU` | `0x142ba246`, `0x142ba5a1` | `AppleParavirtFramebuffer::start` publishes a 312-byte blob (`0x138`, after zeroing a stack copy); `free` removes it |
| `0x147620c7` | `0x147620c7` | `com.apple.iokit.IONDRVSupport` | `0x1475cceb`, `0x1475cd2a` | the **dead** NDRV relay (`setInfoProperties`, §2.2): `removeProperty` then `setProperty(…, 0xf0)` — **240 bytes, the only publisher that is not 312** |

Plainly, the three answers the lead asked for:

1. **Owners:** all seven are framebuffer drivers (2 AMD, 3 Intel, 1 paravirt, 1 NDRV relay). None is
   `IOGraphicsFamily`, none is IOKit core, none is a "sysctl"/userspace stub in the KC.
2. **Readers:** exactly one kext reads the property — `AMDFramebuffer`, twice, and both times it is
   reading back what it itself published, casting to `OSData`. There is **no** generic reader: no
   `IOFramebuffer::getAttribute` branch (no string in `IOGraphicsFamily` at all, and the key is a
   property key, not an attribute selector), no `IODisplay` reader, no other kext. And **no
   userspace code ships in this file**: `SystemKernelExtensions.kc` is a kext collection, so
   WindowServer/CoreDisplay/SkyLight are simply not in it — their use of the key cannot be answered
   from this binary (that is §7 item 2).
3. **Verdict, plainly: on this build `IOFBTimingRange` has no kernel reader; the key is only consumed
   by userspace (CoreDisplay/SkyLight), if at all.** The kernel's entire involvement is that four
   driver families *publish* it (312-byte V2, except the dead NDRV relay's 240-byte V1) and one driver
   reads its own back. Consequently the panic in §3 cannot have come from anyone "interpreting the
   blob": there is no such code in the kernel to run.

An independent bonus from this table: five of the seven publishers pass **`0x138` = 312** and the NDRV
relay alone passes **`0xf0` = 240**. Apple's own Intel and paravirt framebuffers therefore corroborate
the version/layout split in §2.3/§5.1 — V2 for the native `IOFramebuffer` subclasses, V1 for the NDRV
record — without relying on my size arithmetic.

### 9.1 The lines to quote when fixing `hw-logs/panics-20261126/README.md`, the plan and AGENTS.md

From `docs/hw-logs/panics-20261126/Kernel-2026-11-26-003058.panic` (`macOSPanicString`, verbatim):

```
panic(cpu 4 caller 0xffffff801b0d50da): Kernel trap at 0xffffff801b79e67e, type = 14=page fault, registers:
CR0: 0x000000008001003b, CR2: 0x00000000017d7840, CR3: 0x0000000c1670e000, CR4: 0x00000000003406e0
RAX: 0x00000000017d7840, RBX: 0xffffff801b79e890, RCX: 0x000000008000008a, RDX: 0x000000007fffffff
RDI: 0x0000000000000000, RSI: 0xffffffabaad78690
Fault CR2: 0x00000000017d7840, Error code: 0x0000000000000000, Fault CPU: 0x4, PL: 0, VF: 1
```

and the three frames that name the faulting and calling code (same report, `Backtrace (CPU 4)`):

```
0xffffffabaad78210 : 0xffffff801b79e67e mach_kernel : _mac_schedule_telemetry + 0x348e
0xffffffabaad783b0 : 0xffffff801b4e9b12 mach_kernel : _vsnprintf + 0x22
0xffffffabaad783d0 : 0xffffff801eeff59a com.hackintosh.AMDRadeonRDNA4 : __Z8rdna4LogPKcz + 0x9c
0xffffffabaad786c0 : 0xffffff801ef058cb com.hackintosh.AMDRadeonRDNA4 : __ZN12_GLOBAL__N_16attachERNS_7FbEntryE + 0x1e82
```

Reading: `Error code 0x0` = a **read** of a not-present page at `CR2 = 0x17d7840`; the value was in
`RAX` and equals `minPixelClock` = 25,000,000; the faulting code was reached from `_vsnprintf + 0x22`
and `_vsnprintf` from **our** `rdna4Log` (`__Z8rdna4LogPKcz`, `+0x9c`), which `attach` called. There is
no `IOFramebuffer`/`IONDRVFramebuffer`/`setProperty` frame between the fault and our logger, so the
published structure was never dereferenced. (`_mac_schedule_telemetry + 0x348e` is a mis-symbolisation:
the faulting RIP lies in a non-exported helper past the end of that symbol, and `mach_kernel` is not in
this collection, so its exact instruction cannot be disassembled here.)
