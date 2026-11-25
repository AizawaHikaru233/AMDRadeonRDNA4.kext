# The NDRV gamma selectors: what IONDRVFramebuffer actually asks for

Status: **implemented, built, tested off-hardware; not yet run on the card** (2026-11-26).
This file is written first and appended as the work proceeds, so a killed session does not lose
the findings.

Live starting point, measured on the machine before any of this (lead's `ioreg`, 2026-11-26):

```
+-o .Display_boot  <class AMDRadeonRDNA4_AMDFramebuffer, ...>
    "IOFBGammaWidth"      = 8
    "IOFBGammaCount"      = 256
    "IOFBGammaHeaderSize" = 12
    "IOFBCurrentPixelClock" = 533250000
```

Those three numbers are **IONDRVFramebuffer's "there is no gamma table" defaults**, not a
description of this panel (§2.1). The driver answered none of the gamma selectors before this
work.

Why it matters: `docs/hdr-userspace-gating.md` §1.4 — CoreDisplay's plane capability word
(`CoreDisplay::IOPCapabilities` +`0xea`, bit 1) is the one gate our display fails, and the OS
says so 67 times in eight minutes: `Attempting to get capabilities from plane with no gamma`.
The gamma entry points of the NDRV are what a plane's gamma description is built from.
Until this change we answered **none** of `cscSetGamma` (4), `cscGetGamma` (8),
`cscGetGammaInfoList` (20) — every one of them fell through to `IOBootNDRV`, which answers them
the way a card without a programmable gamma ram does.

All addresses below are unslid vmaddrs in `/home/Celia/kc/SystemKernelExtensions.kc`
(x86_64, macOS 26.6.2 / 25G83). The parser is `kc2.py`/`dis3.py` (copies of the earlier
session's `/tmp/trng-work2/{kc2,dis2}.py` in `/tmp/gamma-work/`); this KC maps each fileset
member identity (`__TEXT` vmaddr == file offset), so `data[va:...]` is the image byte at `va`.

---

## 1. Where IONDRVFramebuffer touches gamma

Three functions matter, all in `com.apple.iokit.IONDRVSupport`
(`__TEXT` `0x1475a000`–`0x14763000`):

| symbol | VA | what it does |
|---|---|---|
| `IONDRVFramebuffer::setGammaTable(UInt32,UInt32,UInt32,void*,bool)` | `0x1475e7c0` | builds a `GammaTbl` **in the caller's buffer** and sends `cscSetGamma` (4) |
| `IONDRVFramebuffer::setGammaTable(UInt32,UInt32,UInt32,void*)` | `0x1475e7a8` | 0x18-byte tail thunk: `jmp [vtable+0x888]` with `r9d = 1` (the 5-argument form above) |
| `IONDRVFramebuffer::checkDriver()` | `0x1475c7de` | **the only reader**: asks `cscGetGamma` (8) and then enumerates `cscGetGammaInfoList` (20) + `cscRetrieveGammaTable` (21), and publishes `IOFBGammaWidth` / `IOFBGammaCount` / `IOFBGammaHeaderSize` |
| `IONDRVFramebuffer::_doStatus` | `0x1475baf6` | the status dispatcher (what our `doDriverIO` hook pre-empts) |
| `IONDRVFramebuffer::_doControl` | `0x1475bd8c` | the control dispatcher |

`IONDRVFramebuffer` does **not** override any getter: the whole read path is `checkDriver`,
which runs once (`[this+0x218]` is 0 → unchecked, set to 2 when it succeeds).

## 2. `checkDriver` — the exact sequence (0x1475c7de)

Decoded instruction by instruction. `rbx` = `this` throughout; the register the code keeps
gamma state in is a small struct at `[this+0x2f8]`, whose `+8` is *gamma width* and `+0xc` is
*gamma count* (both 32-bit ints).

```
0x1475c970  r14 = &[rbp-0xe8]                  ; a VDGammaRecord (one Ptr)
0x1475c98b  esi = 8                            ; cscGetGamma
0x1475c993  call _doStatus(this, 8, r14)
0x1475c998  rcx = [r14]                        ; csGTable
0x1475c99b  test rcx,rcx ; je 0x1475c9b8       ; NULL table -> defaults
0x1475c9a0  test eax,eax ; jne 0x1475c9b8      ; status != kSuccess -> defaults
0x1475c9a4  movsx edx, word [rcx+0xa]          ; GammaTbl.gDataWidth
0x1475c9af  mov   [info+8], edx                ;    -> IOFBGammaWidth
0x1475c9b2  movsx ecx, word [rcx+8]            ; GammaTbl.gDataCnt
0x1475c9cb  mov   [info+0xc], ecx              ;    -> IOFBGammaCount
--- the default branch (what happens today) ---
0x1475c9b8  mov dword [info+8], 8              ; IOFBGammaWidth  = 8
0x1475c9c6  mov ecx, 0x100                     ; IOFBGammaCount  = 256
--- enumeration ---
0x1475c9ce  eax = 0xfffffffe                   ; kGammaTableIDFindFirst
0x1475c9e1  [rbp-0xd0] = eax                   ; VDGetGammaListRec.csPreviousGammaTableID
0x1475ca27  [rbp-0xc0] = &[rbp-0xb0]           ; csGammaTableName = a 0xa0-byte stack buffer
0x1475ca31  esi = 0x14                         ; cscGetGammaInfoList (20)
0x1475ca39  call _doStatus(this, 20, &rec)
0x1475ca3e  test eax,eax ; jne 0x1475cac0      ; any error -> stop enumerating
0x1475ca42  cmp dword [rbp-0xcc], -3           ; csGammaTableID == kGammaTableIDNoMoreTables
0x1475ca49  je 0x1475cac0                      ;   -> stop
0x1475ca4b  edi = [rbp-0xc8]                   ; csGammaTableSize  (bytes)
0x1475ca51  call 0xc120                        ; IOMalloc(size)
0x1475ca64  [rbp-0xe0] = csGammaTableID        ; VDRetrieveGammaRec.csGammaTableID
0x1475ca6a  [rbp-0xd8] = r12                   ; VDRetrieveGammaRec.csGammaTablePtr
0x1475ca71  esi = 0x15                         ; cscRetrieveGammaTable (21)
0x1475ca80  call _doStatus(this, 21, &rec)
0x1475ca85  jne -> free, next
0x1475ca89  ecx = movsx word [r12+0xa]         ; gDataWidth of the retrieved table
0x1475ca96  cmp dword [info+8], ecx ; jae keep ; **keeps the widest table**
0x1475ca9b  mov   [info+8], ecx                ; info->gammaWidth = gDataWidth
0x1475ca9e  ecx = movsx word [r12+8]           ; gDataCnt
0x1475caa4  mov   [info+0xc], ecx              ; info->gammaCount = gDataCnt
0x1475caa7  call 0xc11a                        ; IOFree
0x1475cab5  eax = csGammaTableID ; loop        ; csPreviousGammaTableID = csGammaTableID
--- publish ---
0x1475cac0  edx = [info+8]  -> setProperty("IOFBGammaWidth",      edx)   ; 0x14762095
0x1475cae2  edx = [info+0xc] -> setProperty("IOFBGammaCount",      edx)   ; 0x147620a4
0x1475cb04  0xc             -> setProperty("IOFBGammaHeaderSize", 12)    ; 0x147620b3
```

### 2.1 What this settles about the live measurement

The live object shows `IOFBGammaWidth = 8`, `IOFBGammaCount = 256`,
`IOFBGammaHeaderSize = 12`. That is **exactly the fallback branch at `0x1475c9b8`**:
8 and 256 are hard-coded there, 12 is written unconditionally. So the three keys are not
evidence that anything answered — they are the "no gamma table" default. Nothing else has to
override them; they simply have to stop being the default. (`IOFBCurrentPixelClock` is a
different producer and says nothing about gamma; it is published on our own object from the
mode table, `0x1472c0d4` is the IOGraphicsFamily-side key string.)

Two consequences for the implementation:

* **`cscGetGamma` (8) is the one that decides the width/count default** — one answered table
  and the defaults are gone.
* **`cscGetGammaInfoList` (20) alone is not enough**, and it is *dangerous alone*: the kernel
  loops on it and only stops at `csGammaTableID == kGammaTableIDNoMoreTables`
  (`0xfffffffd`) or on an error. A selector 20 that always reports a table is an infinite
  loop inside `checkDriver`. Our answer must therefore be a two-step list (one table, then
  `NoMoreTables`), which is what `VDGetGammaListRec`'s `csPreviousGammaTableID` protocol is
  for.
* The table size the kernel allocates for `cscRetrieveGammaTable` is `csGammaTableSize` from
  the list record (`0x1475ca4b`), so it must be the true byte length of the `GammaTbl`.

### 2.2 When checkDriver runs — and why our answer arrives in time

`checkDriver` is a **virtual**, not a free function: it is vtable entry object-relative
`+0xbf0` (`__ZTV17IONDRVFramebuffer` at `0x14763cc8`, instance vtable base `0x14763cd8`; the
same table has `doDriverIO` at `+0xaf0` and `setGammaTable` at `+0x888`). Its **only** caller
in the whole KC is `IONDRVFramebuffer::enableController+0x9b` (`call qword ptr [rax+0xbf0]`
at `0x1475b7d5`) — no direct call, no other data reference exists.

The first two things `checkDriver` itself does are NDRV lifecycle commands, both through the
same virtual `doDriverIO` that this plugin routes:

| VA | what |
|---|---|
| `0x1475c81e`–`0x1475c829` | `doDriverIO(this, 0, [this+0x1d8], 0x87 /* kIONDRVInitializeCommand */, 4)` |
| `0x1475c8c9`–`0x1475c8d4` | `doDriverIO(this, 0, [this+0x1d8], 0x80 /* kIONDRVOpenCommand */, 4)` |
| `0x1475c8e2` | `[this+0x218] = 2` — the "checked" latch; `_doStatus`/`_doControl` refuse with `kIOReturnNotOpen` while it is 0 |
| `0x1475c906` | `_doStatus(10 /*cscGetCurMode*/)` — the first csc that reaches the translator |
| `0x1475c993` | `_doStatus(8 /*cscGetGamma*/)` — **ours to answer** |
| `0x1475ca39` | `_doStatus(20 /*cscGetGammaInfoList*/)`, then `0x1475ca80` `_doStatus(21)` per table |

`wrapDoDriverIO` already calls `attach(*e)` on `kIONDRVOpenCommand`
(`plugin.cpp`, `if (commandCode == kIONDRVOpenCommand && ret == kIOReturnSuccess && !e->tried) attach(*e);`),
i.e. **inside** `checkDriver` and **before** its first status call. So the gamma answer is in
place for the very first gamma query of the boot; no extra route and no ordering guard is
needed. (This also means the state today — attach on the first csc — is unchanged by this
work.)

`[this+0x218]` is 0 → `checkDriver` runs; that is why our machine has the 8/256 defaults at
all: the routine really runs, asks, gets `kIOReturnUnsupported` from `IOBootNDRV`, and falls
back. (`setAttribute` can write 0 back at `0x1475eb70`, so the check can run again after a
power cycle of the framebuffer.)

## 3. `setGammaTable` — the write path (0x1475e7c0)

`IOFramebuffer::setGammaTable(channelCount, dataCount, dataWidth, data, syncToVBL)`
(`IOGraphicsFamily`, `IOFramebuffer.h:432`) — `IONDRVFramebuffer` implements it as:

```
0x1475e7d1  cmp dword [rdi+0x214], 0 ; je -> return kSuccess
0x1475e7e1  test r8,r8 ; je -> return 0xe00002c2 (kIOReturnBadArgument)
0x1475e7f3  ...  [rdi+0x2f8]->byte[4] & 8  &&  [rdi+0x20c] != 0
0x1475e81f     esi = 0x1d                     ; cscGetClutBehavior (29) — refresh the cached mode
0x1475e848  word [data+0x0] = 0                ; GammaTbl.gVersion     = 0
0x1475e84c  word [data+0x2] = syncToVBL ? 1:0  ; GammaTbl.gType
0x1475e854  word [data+0x4] = 0                ; GammaTbl.gFormulaSize = 0
0x1475e859  word [data+0x6] = channelCount     ; GammaTbl.gChanCnt
0x1475e85e  word [data+0x8] = dataCount        ; GammaTbl.gDataCnt
0x1475e862  word [data+0xa] = dataWidth        ; GammaTbl.gDataWidth
0x1475e86a  [rbp-0x38] = data                  ; VDGammaRecord.csGTable
0x1475e870  esi = 4                            ; cscSetGamma
0x1475e875  call _doControl(this, 4, &rec)
0x1475e87a  test eax,eax ; je -> return eax
0x1475e87e  cmp word [data+2], 0 ; je -> return the error
0x1475e885  word [data+2] = 0                  ; gType = 0
0x1475e897  call _doControl(this, 4, &rec)     ; retry once without the "colour managed" flag
```

So the record the NDRV sees for selector 4 is a `VDGammaRecord` whose `csGTable` points at a
`GammaTbl` that the **kernel has already filled in**, in native (little-endian) byte order,
with the six 16-bit header fields; the entries follow at offset 12 (`gFormulaData[1]`,
`gFormulaSize == 0`). A NDRV that wants the table only has to read
`((GammaTbl *)rec.csGTable)` — it must not write to it, and it must cope with `gChanCnt == 1`
or `3` and `gDataWidth` 8 or 16.

Note the retry: if we answer selector 4 with an error and `gType != 0`, the kernel rewrites
`gType = 0` and asks **again**. A refusal must therefore be idempotent, and it is worth
answering the retry the same way (both are then logged once each).

`[this+0x214]` is the "NDRV present/opened" latch: when it is 0 the method returns
`kIOReturnSuccess` *without asking the NDRV anything*. This is one more reason failure here is
silent, and it is why the current behaviour (no selector answered) is invisible from userspace.

## 4. What the table must look like

`GammaTbl` (`lib/MacKernelSDK/Headers/IOKit/ndrvsupport/IOMacOSTypes.h:360`):

```c
struct GammaTbl {
    short gVersion;        /* +0x0  */
    short gType;           /* +0x2  */
    short gFormulaSize;    /* +0x4  */
    short gChanCnt;        /* +0x6  */
    short gDataCnt;        /* +0x8  number of values per channel   */
    short gDataWidth;      /* +0xa  bits per corrected value       */
    short gFormulaData[1]; /* +0xc  formula, then the values       */
};
```

* **Little-endian, naturally aligned, `sizeof` 12 + payload.** The kernel reads `+0xa` and
  `+0x8` with `movsx` (signed 16-bit) in `checkDriver` and writes `+0x0 … +0xa` with `mov word`
  in `setGammaTable`, so the layout is the C struct above on x86_64 — no byte-swapping, no
  mac68k `#pragma` (that only applies to 32-bit builds).
* **`gDataCnt` is per channel**, `gChanCnt` the channel count; the payload is
  `gChanCnt * gDataCnt * (gDataWidth > 8 ? 2 : 1)` bytes.
* `IOFBGammaHeaderSize = 12` is what the kernel tells userspace the header is, i.e. userspace
  builds exactly this structure too. It is hard-coded at `0x1475cb0e` (`mov edx, 0xc`), not
  derived from anything we return, so 12 is the contract on both paths — and `sizeof(GammaTbl)`
  in the SDK is 14, because of `gFormulaData[1]`; the header is the *offset* of `gFormulaData`,
  and the plugin static_asserts exactly that.

## 5. Is 8 bits the problem? What Apple's own framebuffer drivers publish

Every in-kernel framebuffer driver that publishes these keys was disassembled at the site
where it writes them (string xrefs on each image's own `IOFBGammaWidth` copy):

| driver | function | `IOFBGammaWidth` | `IOFBGammaCount` | `IOFBGammaHeaderSize` |
|---|---|---|---|---|
| `com.apple.driver.AppleIntelCFLGraphicsFramebuffer` | `AppleIntelFramebuffer::initGammaDepth` `0x13e038de` | **10** (modern) / 8 (legacy) | **1024** / 256 | — |
| `com.apple.kext.AMDFramebuffer` | `AMDFramebuffer::initializeDriverResources` `0x28794c` | **12** (from `getGammaParameters`) | **256** | 0 |
| `com.apple.kext.AMDRadeonX6000Framebuffer` | `AMDRadeonX6000_AmdRadeonFramebuffer::start` `0xbf64bee` | from the hardware object (`[+0x1b8](0x10)`) | from it (`(0xf)`) | 0 |
| `com.apple.iokit.IONDRVSupport` | `IONDRVFramebuffer::checkDriver` `0x1475cab5` | table's `gDataWidth`, else **8** | table's `gDataCnt`, else **256** | 12 |

The Intel one is the decisive A/B, because both branches are in the same function:

```
0x13e03931  cmp eax, 1 ; je 0x13e03955        ; GAMMA_MODE == 1 -> 0x13e03955
0x13e03936  test eax,eax ; jne 0x13e039eb     ; any other non-zero mode -> a third path
                                              ; (GAMMA_MODE == 0 falls through here)
0x13e0393e  [this+0xa4a0] = 0                 ; 0 = the 8 bit gamma
0x13e03948  r14d = 0x100                      ;   IOFBGammaCount  256
0x13e0394e  edx  = 8                          ;   IOFBGammaWidth  8
0x13e03955  [this+0xa4a0] = 1                 ; 1 = the 10 bit gamma  <- the je target
0x13e0395f  r14d = 0x400                      ;   IOFBGammaCount  1024
0x13e03965  edx  = 0xa                        ;   IOFBGammaWidth  10
0x13e0396d  setProperty("IOFBGammaWidth",  edx)
0x13e03985  setProperty("IOFBGammaCount",  r14d)
```

So the 8/256 branch is the one `GAMMA_MODE == 0` selects (and it is what our machine shows),
while the driver's 10 bit mode declares **10 / 1024**. The third path (`0x13e039eb`) publishes
`width = (flag == 1 ? 10 : 9)` and `count = (flag == 1 ? 1024 : 256)` — even it never says 8.
The log line the function then emits is `"IGFB:: %s:%d GAMMA_MODE=%#x\n"` — it is a *mode* of
the display engine, not a guess.

And AMD's own framebuffers ask their controller: `Vega10SharedController::getGammaParameters`
(`0x197df0`) and `BaffinSharedController::getGammaParameters` (`0x212430`) both fill
`(channels, count, width) = (3, 256, 12)`, and `initializeDriverResources` then
`setLinearRamp()`s the table before publishing (`0x2879c0`). So:

* **No Apple framebuffer publishes 8 bits as its normal configuration.** 8 is Intel's legacy
  gamma mode and IONDRVFramebuffer's "there is no table" default — the exact value our machine
  has today.
* The modern shapes are **10 bit / 1024** (Intel) and **12 bit / 256** (AMD). Both are > 8,
  so both are candidates for what CoreDisplay's `GammaChannelTableEntryCountAndType()` wants.
  This work offers 10/1024 by default and 12/256 as `rdna4-gamma=2`, so one boot can A/B them.

**[hyp]** that CoreDisplay requires a width above 8 is still a hypothesis about the
*userspace* predicate (`docs/hdr-userspace-gating.md` §1.4, `IOPCapabilities+0xea` bit 1); what
is proven here is the kernel side: the plane's declared gamma width/count come from this
single answer, and every Apple driver that has a gamma ram declares more than 8 bits.

---

## 6. What was implemented (src/ndrv.hpp, src/ndrv.cpp, src/plugin.cpp)

One switch, off unless armed, and four selectors behind it.

| piece | where |
|---|---|
| `GammaTbl`, `VDGammaRecord`, `VDGetGammaListRec`, `VDRetrieveGammaRec` mirrors + `GammaTableID` constants | `src/ndrv.hpp`, static_asserted against the SDK in `src/plugin.cpp` |
| the table itself (`kGammaTableBytes` = 12 + 3 × 1024 × 2 = **6156** bytes) | `Ndrv::Translator::gammaTable`, a member array — no allocation, ever |
| the ramp | `Translator::buildGammaRamp()`: per channel `round(i * (2^width - 1) / (entries - 1))`, 16-bit little endian, R then G then B |
| the answers | `Translator::gammaGet` (8), `gammaList` (20), `gammaRetrieve` (21), `gammaSet` (4) |
| the option | `rdna4-gamma=1\|2` (boot arg or the `rdna4-trial` NVRAM variable, through `Trial::option`), `debug.rdna4_gamma` sysctl; both default 0 |
| the log | `Backend::gammaLog` → `deviceGammaLog` in `src/plugin.cpp` |

Decisions worth naming, with the reason:

* **The ramp is linear (identity) and honest.** The plane's gamma is not programmed by this
  driver — nothing writes the DCN degamma/gamma LUT for the scanout — so the only table that
  is true is the pass-through one. Saying 10 bits and handing back a curve would be a lie about
  the picture; saying 10 bits and handing back identity is a statement about the *format the
  plane can accept*, which is what `IOFBGammaWidth`/`IOFBGammaCount` describe.
* **`cscGetGamma` answers before macOS ever sends `cscSetGamma`.** That is the requirement:
  `checkDriver` runs at `enableController`, long before anyone sets a table, and its first
  question is the getter. Measured order (this KC): `checkDriver` → Initialize → Open (plugin
  attaches) → `cscGetCurMode` → `cscGrayPage` → `cscGetClutBehavior` → **`cscGetGamma`** →
  enumerate → publish.
* **What macOS sets becomes the answer** (`gammaFromMacOS`): after a `cscSetGamma` the getter
  reports the table the OS installed, its width and its count, and the enumeration's size
  follows it — one table, described one way. Anything that does not fit (channels > 3,
  entries > 1024, a formula section, a version that is not 0, a channel count of 0, a total
  over 6156 bytes) is refused with `kBadArgument` **and changes nothing**; a null record is
  refused, never written through; `cscRetrieveGammaTable` copies at most the size the
  enumeration advertised (that is the only bound the record gives us), and refuses when the
  table has since become shorter.
* **The enumeration terminates**: `kGammaTableIDFindFirst` (0xfffffffe) → id 0, then
  `kGammaTableIDNoMoreTables` (0xfffffffd). Without the second answer `checkDriver` spins.
* **`csGammaTableName` is not written.** The kernel points it at a 0xa0-byte stack buffer
  (`0x1475ca27`) and never reads it back, and a NDRV cannot know how big a caller's buffer is;
  writing nothing is the only safe choice.
* **`rdna4-gamma=2`** exists only so one boot can A/B the two shapes Apple's drivers publish
  (10/1024 vs 12/256) without a second code change.
* **No `%s` anywhere near a number.** The translator passes an event and four `int32_t`; the
  format strings live in one `switch` in `src/plugin.cpp` and use `%d`/`%u`/`0x%x` only.

`IOFBGammaHeaderSize` stays 12 because the kernel hard-codes it (`0x1475cb0e`); nothing we
return changes it, and both directions (its `setGammaTable` writing the header into our buffer,
our buffer being read as a `GammaTbl`) assume it.

### 6.1 The gamma buffer overflows nothing by construction

* getter: hands the kernel a pointer to a member array that lives as long as the framebuffer;
* setter: reads at most 12 + 3 × 1024 × 2 bytes from the kernel's own buffer, and only after
  the six header fields pass the checks above;
* retrieve: writes at most the byte count the enumeration last advertised, which is itself
  capped by `kGammaTableBytes`.

## 7. Acceptance criteria for the next hardware boot

Arm it first (this is the only thing that has to happen outside the driver):

```
# boot-arg, alongside the existing ones
rdna4-gamma=1
# or at run time, no reboot:
sudo sysctl -w debug.rdna4_gamma=1
```

Note the run-time form only helps *future* `checkDriver` runs — the properties are published
once, at `enableController`. For a measurement of the plane capability, use the **boot arg**.

Then, after ≥ 10 minutes of uptime (AGENTS.md §一: a boot that changed the ESP needs that long
before it is called stuck):

1. **The defaults are gone — the single cleanest signal.** On our own framebuffer objects:

   ```bash
   ioreg -r -w0 -l -k IOFBGammaWidth | grep -E 'IOFBGamma(Width|Count|HeaderSize)'
   ```

   must show `IOFBGammaWidth = 10`, `IOFBGammaCount = 1024` (was 8 / 256) and
   `IOFBGammaHeaderSize = 12`, on `.Display_boot` and on every child framebuffer that is on
   our GPU. (A child on another GPU keeps 8/256 — `checkDriver` publishes per IONDRVFramebuffer
   instance and only ours is answered by this translator. `ioreg` shows the keys per node, so
   read the ones whose parent is our device.)

2. **Our own log lines** (kext ring log; this driver's `FBLOG` does not reach the unified log,
   AGENTS.md §四):

   ```bash
   sudo sysctl -n debug.rdna4_log | grep -E 'rdna4-gamma=|^.*gamma: '
   ```

   expected, in this order, once per framebuffer:
   - `ndrv: rdna4-gamma=1: answering cscGetGamma/cscGetGammaInfoList/cscRetrieveGammaTable with a 10 bit, 1024 entry linear ramp`
   - `gamma: cscGetGamma table 6156 bytes (macOS 0) width 10 entries 1024 -> 0x0`
   - `gamma: cscGetGammaInfoList after 0xfffffffe -> id 0x0 size 6156 -> 0x0`
   - `gamma: cscRetrieveGammaTable id 0x0 copied 6156 bytes -> 0x0`
   - `gamma: cscGetGammaInfoList after 0x0 -> id 0xfffffffd size 0 -> 0x0`

   `(macOS 0)` in the getter line means the OS had not set a table yet at that point; later
   `cscSetGamma` lines must show `chan 3 data 256|1024 width 8|10|16 -> 0x0` (or `-> 0xe00002c2`
   for something this driver refuses — which would be a finding, not a pass).

3. **The user-space half** (the actual goal, `tools/rig/hdr-evidence.sh`, or by hand):

   ```bash
   sudo log show --last 10m --info --debug --predicate 'process == "WindowServer"' > /tmp/ws.log
   grep -c 'plane with no gamma'          /tmp/ws.log   # must be 0   (baseline: 67 in 8 min)
   grep -c 'hdr-policy'                   /tmp/ws.log   # must be >= 1 (baseline: 0)
   grep -o 'InstallTiming - [a-zA-Z_]*'   /tmp/ws.log | sort | uniq -c   # the legacy* prefix should go
   ```

4. **With all three green**, run the in-session probe of `docs/hdr-userspace-gating.md` §4.1 in
   Terminal.app (`online >= 1`) and look for `SLSsupports=1`. That is the first time this
   machine could show it.

**Falsification, stated up front:** if (1) is green and (3) is not, then the plane capability
is *not* fed by this gamma description alone, and the next suspect is the one
`docs/hdr-userspace-gating.md` §3.2 item 5 names (`kIOFBTimingRangeKey`, V2). Do not stack the
two changes in one boot: each needs its own log.

## 8. What is not verified (and how it would be)

| claim | status | how to settle it |
|---|---|---|
| `checkDriver` publishes width/count from our answer on the real machine | **[inf]** from the disassembly + the fact that attach happens on its Open | acceptance §7.1/§7.2 — the registry keys and the getter log line |
| Width 10 (rather than 12) is what CoreDisplay's `GammaChannelTableEntryCountAndType()` wants | **[hyp]** — no Apple driver publishes 8, but the userspace predicate was not re-read | `rdna4-gamma=1` then `=2` in two boots, compare `plane with no gamma` |
| `IOPCapabilities +0xea` bit 1 is set by a non-empty gamma description | **[hyp]**, `docs/hdr-userspace-gating.md` §1.4 | acceptance §7.3/§7.4 |
| Nothing in the gamma path is read during a mode switch | **[inf]** — `checkDriver` runs once; `setGammaTable` is the only other user | watch the ring log for `gamma:` lines after the first minute |
| The 6 KB per-framebuffer table changes no timing | **[inf]** — it is a plain member, laid out in `setGammaOption` | boot time and the existing bring-up log lines |
| `debug.rdna4_gamma` registered at plugin start is visible | **[inf]** — `sysctl_register_oid` in `pluginStart`, as `device.cpp` does in its init | `sysctl -a \| grep rdna4_gamma` |
| A table macOS sets is what we report back | **tested off-hardware** (host test) | acceptance §7.2's `cscSetGamma` lines |

## 9. Rollback

The feature is **off unless armed**, so the rollback is "boot without the switch":

```bash
sudo sysctl -w debug.rdna4_gamma=0     # this boot, immediately; the next boot is unaffected
```

and for the persistent form, remove `rdna4-gamma=1` from the boot-args (or delete the
`rdna4-trial` NVRAM variable if it was armed that way). Nothing else in the driver changed
behaviour with the option off: `cscGetGamma`/`cscGetGammaInfoList`/`cscRetrieveGammaTable`/
`cscSetGamma` then return "not ours" exactly as before, and `IOBootNDRV` answers them as it
always did.

If a boot with the switch armed hangs or panics, the boot after it is already clean — but note
that a **panic report** is the thing to read first (AGENTS.md §一): the ring log dies with the
panic, the properties and the panic string do not.

## 10. How to re-check the kernel side

```bash
pip install --user capstone          # the two scripts only need this
# /tmp/gamma-work/{kc2,dis3}.py — kc2.py is a copy of the earlier session's /tmp/trng-work2/kc2.py
python3 - <<'PY'
import dis3, kc2
im = kc2.by_eid('IONDRVSupport')[0]
dis3.show(im, 0x1475c7de, 900, 'IONDRVFramebuffer::checkDriver')     # the gamma read
dis3.show(im, 0x1475e7c0, 480, 'setGammaTable jjjPvb')               # the gamma write
PY
```

`__ZTV17IONDRVFramebuffer` at `0x14763cc8` (instance vtable base `0x14763cd8`) decodes with the
chained-fixup rule "low 36 bits, minus 0x40000000 when that bit is set": `doDriverIO` at
object `+0xaf0`, `checkDriver` at `+0xbf0`, `setGammaTable` at `+0x888`.

## 11. Build and test provenance

| what | result |
|---|---|
| kext, clean build on the Mac (`~/rdna4/a4-gamma`, `make -j2 kext`) | **exit 0**, `AMDRadeonRDNA4.kext for x86_64 (min macOS 11.0)`, UUID `5BBD2A68-85A4-31BC-B4B3-58A8365D57D7` |
| warnings | none from `src/ndrv.cpp` or `src/plugin.cpp`; the only warnings in the build are the pre-existing ones in `compute.cpp`, `device.cpp`, `edid.cpp`, `gfxring.cpp`, `ih.cpp`, `pipe2.cpp`, `pvgpu.cpp`, `pvhooks.cpp`, `rdna4accel.cpp`, `runtime.cpp`, `vmtest.cpp` and Lilu's headers |
| host tests (`make -j2 CXX=g++ CC=gcc test` here; the box has no `clang++`, so `CXX` is overridden) | `ndrv: 9 modes …, EDID/DPMS ok` — every ndrv and gamma check passes; **2 checks fail, both pre-existing** (`navi44: gfx images: 0 built (mes header)` and `RLC_G must come last`), reproduced identically from the pristine tree (`git show HEAD:` of `test/atomdump.cpp` + `src/ndrv.*`, same compiler, same cwd) |
| new host tests | `test/atomdump.cpp`, `testNdrv()`: record layouts; option off → all four selectors not ours and nothing logged; option on → table header 3/1024/10, 6156 bytes, ramp 0…1023 monotonic, first 0 last 1023; enumeration one table then `NoMoreTables`; retrieve copies exactly the advertised bytes and refuses id 7 / a null buffer; `cscSetGamma` of an 8 bit/256 table becomes the getter's answer and the list size follows; oversized / formula / 4-channel / wrong-version / null sets refused with the table unchanged; null records refused; `rdna4-gamma=2` → 12 bit/256, 0…4095, 1548 bytes; turning the option off again |
