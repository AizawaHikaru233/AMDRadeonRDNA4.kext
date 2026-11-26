# The AGDC capability answers: what Apple's drivers return, why ours leaves `GammaArray` empty

Status: **reversed (both sides, by disassembly), implemented behind `rdna4-agdccaps=1` (default off), built;
not run on the card by this change.** Written as the work proceeded (2026-11-26 09:29–…), so a killed
session keeps the findings.

Starting point is `docs/hdr-userspace-gating.md` §9: CoreDisplay decides "this plane has gamma" from
`plane[0]+0x88` = `CFArrayGetCount(plane["GammaArray"])`, the dictionary comes from IOPresentment, and
IOPresentment builds it from the **AGDC attribute `0x711`** (a single `IOConnectCallStructMethod`, 0x196C-byte
in/out buffer). §9 could not say *which* field of that answer produces the array count and marked it
**[hyp]**. This document replaces that hypothesis with the parser itself.

All addresses are unslid vmaddrs. Kernel side: `/home/Celia/kc/SystemKernelExtensions.kc` (x86_64, macOS
26.6.2 / 25G83). Userspace side: `/System/Library/PrivateFrameworks/IOPresentment.framework/Versions/A/IOPresentment`
as it sits in the **dyld shared cache**, read with `dyld_info -segments/-disassemble` on the card's machine
(image base `0x7FF81598D000`, `__TEXT,__text` 95 856 B — this is the "find out what the consumer wants"
half that §9 lacked).

---

## 1. The 0x711 answer, decoded from the consumer

`IOPresentmentCapabilities::__GatherAGDCDisplayPipelineCapabilities()` = **`0x7FF81599CC94`**. For each of 16
capability kinds it does exactly this (the first block, in full):

```
0x7FF81599CCC9  size = 0x196c
0x7FF81599CCD0  buf  = this + 0x69f0            ; the per-kind answer buffer
0x7FF81599CCDC  bzero(buf, 0x196c)
0x7FF81599CCE4  *(u32*)(buf + 0x00) = conn->p50          ; the framebuffer index
0x7FF81599CCEF  *(u64*)(buf + 0x04) = 0x8_00000010       ; ← low half = query 0x10, high half = 8
0x7FF81599CD18  IOConnectCallStructMethod(conn->p20 /* connect */, 0x711, buf, 0x196c, buf, &size)
0x7FF81599CD1D  if (ret != 0 || size == 0) → 0x2006      ; "capabilities not available"
0x7FF81599CD5D  os_log "Info: AGDC Plane Scaler successful"
0x7FF81599CD74  if (*(u32*)(buf + 0x08) >= 9) { os_log "Warning: Incorrect entry count"; *(u32*)(buf+8) = 8; }
0x7FF81599CDCB  this->p434 = 1                           ; "this kind is present"
```

The `+0x08` check is the whole story of the buffer's layout: **`+0x08` is the entry count on return**, the
caller offers `8` (`0x800000010` = `{query 0x10, max 8}` packed as one store), and anything above 8 is
clamped with a warning. `0x196C = 0xc + 8 * 0x32c`, i.e. an 8-entry array of 0x32c-byte records.

The 16 kinds, in call order (**[dis]**, all from the same function; `buf` = the buffer, `cnt` = where the
count lands, and the `os_log` line it prints on success):

| query | `buf` | `cnt` | success log | kinds |
|---|---|---|---|---|
| **0x10** | `this+0x69f0` | `+0x69f8` | `AGDC Plane Scaler` | per-plane scaler |
| 0x08 | `this+0x5084` | `+0x508c` | `AGDC Pipeline Scaler` | pipe scaler |
| **0x04** | `this+0x3718` | `+0x3720` | `AGDC Linearization` | **pre-CSC linearization** |
| **0x02** | `this+0x1dac` | `+0x1db4` | `AGDC Gamma` | **plane gamma** |
| 0x01 | `this+0x440` | `+0x448` | `AGDC CSC` | colour matrix |
| 0x40 | `this+0x9cc8` | `+0x9cd0` | `AGDC IB` | inline buffer |
| 0x20 | `this+0x835c` | `+0x8364` | `AGDC Rotation` | |
| 0x200 | `this+0xe90c` | `+0xe914` | `AGDC Mirror` | |
| 0x80 | `this+0xb634` | `+0xb63c` | `AGDC Queue` | |
| 0x100 | `this+0xcfa0` | `+0xcfa8` | `AGDC Queue` | (second queue) |
| 0x400 | `this+0x10278` | `+0x10280` | `AGDC Format Convert` | |
| 0x800 | `this+0x11be4` | `+0x11bec` | `AGDC HDRScaler` | |
| 0x1000 | `this+0x13550` | `+0x13558` | `AGDC PlaneCSC` | |
| 0x2000 | `this+0x14ebc` | `+0x14ec4` | `AGDC 3DLUT` | |
| 0x4000 | `this+0x16828` | `+0x16830` | `AGDC PixelProcessing` | |
| 0x8000 | `this+0x18194` | `+0x1819c` | `AGDC Cursor` | |

*(Correction to §9: the buffer at `this+0x14ebc` is the **3DLUT** query, not "logical device capabilities";
and §9's reading of the packed store `0x800000010` as "sub-query 0x10" is right, with the `0x8` beside it
being the requested entry count, not a second id.)*

### 1.1 The record, and the field that decides everything

`IOPresentmentCapabilitiesDisplayPipe::IOPresentmentCreateGammaDictionary(plane, pipe, array)` =
**`0x7FF8159A0A30`** (linearization: `…CreateLinearizationDictionary` = **`0x7FF8159A0424`**, same shape on the
count at `this+0x3720`):

```
0x7FF8159A0A4C  if (*(u32*)(this+0x1db4) == 0) return NULL      ; the count from query 0x02
0x7FF8159A0A67  mask = 1 << <arg3>                              ; <arg3> = the plane index
0x7FF8159A0AFD  for (i = 0; i < *(u32*)(this+0x1db4); i++, base += 0x32c) {
0x7FF8159A0B04      if ((mask & *(u64*)(this + 0x1dbc + i*0x32c)) == 0) continue;   ; ★ entry+0x04
0x7FF8159A0B1D      dict = …CreateGammaDictionary(plane, pipe, NULL)
0x7FF8159A0B3E      Flags            = *(u32*)(entry+0x00)  → AGDCFlagsToIOPresentment
0x7FF8159A0B58      NonAtomicApplicationPeriod = *(f32*)(entry+0x0c)  (CFNumber kCFNumberFloatType=12)
0x7FF8159A0B88      Type             = *(u32*)(entry+0x10)  → AGDCGammaTypeToIOPresentment
0x7FF8159A0BCD      FixedFormat      = *(u32*)(entry+0x14)  → AGDCGammaFixedFormatToIOPresentment
0x7FF8159A0C12      ConfigurationID  = *(u32*)(entry+0x18)
0x7FF8159A0C4A      MinimumValue     = *(f32*)(entry+0x20)
0x7FF8159A0C82      MaximumValue     = *(f32*)(entry+0x24)
0x7FF8159A0CBA      MaximumSize      = *(u64*)(entry+0x28)
0x7FF8159A0CFB      CFArrayAppendValue(array, dict)
                }
```

So, field by field, the record IOPresentment reads out of our `0x02` answer:

| record offset | type | becomes dictionary key | meaning |
|---|---|---|---|
| `+0x00` | `u32` | `Flags` (via `AGDCFlagsToIOPresentment`) | `AGDCExtendedCapabilityFlags_t` |
| **`+0x04`** | **`u64`** | — | **plane applicability mask: bit *p* set ⇒ this segment is for plane *p*** |
| `+0x0c` | `f32` | `NonAtomicApplicationPeriod` | |
| `+0x10` | `u32` | `Type` (via `AGDCGammaTypeToIOPresentment`) | `AGDCGammaType_t`, a **bitmask** |
| `+0x14` | `u32` | `FixedFormat` (low byte, 1:1) | `AGDCGammaFixedFormat_t` |
| `+0x18` | `u32` | `ConfigurationID` | |
| `+0x20` | `f32` | `MinimumValue` | |
| `+0x24` | `f32` | `MaximumValue` | |
| `+0x28` | `u64` | `MaximumSize` | **entry count of the table** |

`AGDCGammaTypeToIOPresentment` (`0x7FF81599E112`) and `AGDCLinearizationTypeToIOPresentment`
(`0x7FF81599E1C4`) are pure bit-mask translators: bit0→1, bit1→2, bit2→4, bit3→8, bit10→0x400, bit11→0x800,
bit12→0x1000, bit13→0x2000, bits 4…9→0x10…0x200, 0→0 (gamma checks bit 3 before 10…13, linearization after —
that ordering difference is the only asymmetry between the two functions).

### 1.2 The count CoreDisplay finally reads

`IOPresentmentCapabilities::__planesFromDictionary` / `__gammaFromDictionary` (§9.1) then put this array under
`GammaArray` in the plane dictionary, and CoreDisplay's `GammaChannelTableEntryCountAndType()`
(`CoreDisplay+0x117D2`) reads its count as `plane[0]+0x88`. So the chain is

```
count at answer+0x08  →  N iterations of the record loop  →  #records whose +0x04 has bit 0 set
                      →  CFArrayGetCount(GammaArray)  →  plane[0]+0x88
```

**Every term of that chain except the last is now proven by disassembly.** The last one is §9.1's.

---

## 2. What Apple's own drivers answer for 0x711 (and for the GPU capability)

### 2.1 `AMDRadeonX6000_AmdAgdcServices` — a real AMD card

`vendor_doDeviceAttribute` = `0xbf6f668` dispatches `attribute` through three jump tables
(`0xbf702b0` = 0x701…0x707, `0xbf702cc` = 0x920…0x928, `0xbf702f0` = 0x940…0x944, `0xbf70304` = 0x975…0x991)
plus a chain of compares. The three attributes that matter here:

| attribute | handler | symbol |
|---|---|---|
| **0x711** | `0xbf6fcb2` → `jmp 0xbf70dd4` | `AMDRadeonX6000_AmdAgdcServices::getFbExtendedCapability(AGDCFBGetExtendedCapability_t*, unsigned long) const` |
| **0x921** | `0xbf7001f` → `jmp 0xbf8d348` | `…::getLinkConfig(AGDCLinkConfig_t*, unsigned long)` |
| **0x980** | `0xbf6fd4e` → `jmp 0xbf7f916` | `…::getGpuCapability(AGDCGPUCapability_t*, unsigned long)` |

`getFbExtendedCapability` (`0xbf70dd4`) — input `+0x00` = framebuffer/display-path index, **`+0x04` = query**,
no size check; the per-query handlers write `*(u32*)(out+0x08) = count` and the records from `out+0x0c`:

| query | handler | symbol | answer |
|---|---|---|---|
| 1 | `0xbf70f14` (inline) | — | one CSC entry: `Flags=0x80005`, `+0x04=1`, `+0x0c=1`, `+0x18=0x800000000`, `+0x2c=CSC type` (0x50005/0x70007/… by colour mode), `+0x30 = -1.0f/1.0f` |
| **2** | `0xbf7101a` | `getFbCapabilityForGamma(AmdFbDisplayPath*, u32 &count, AGDCFBExtendedCapabilitesEntry_t*)` | **count = `virtual[0x118](0x17) + 1` = 2**; two records of 0x32c, see below |
| 4 | `0xbf71354` | `getFbCapabilityForDegamma(…)` | **count = 2**; each record: `Flags=0x80005`, `+0x04=1` (plane 0), `+0x0c=0.0f`, `+0x10=1` (Type = bit 0), `+0x14=0`, `+0x18=(const|this+0x2e798)`, `+0x20=0.0f`, `+0x24=1.0f`, `+0x28=1` |
| 0x10 | `0xbf71180` | `getFbCapabilityForScaler(…)` | count = 1, `bzero(record+8, 0x324)`, `Flags=0x80004`, then the path's scaler limits |
| 0x8000 | `0xbf7141e` | `getFbCapabilityForCursor(…)` | count = 1, `Flags=0x80004`, sizes from `virtual[0x1b8](6)` |

and the gamma records, field by field (`0xbf71089`…`0xbf71173`), with the record base at `out+0xc`:

| record | write | field IOPresentment reads |
|---|---|---|
| entry 0 `+0x00` | `0x88005` | `Flags` |
| `+0x04` | `1` | **plane mask = plane 0** |
| `+0x0c` | `0x1_00000000` (qword) | `NonAtomicApplicationPeriod = 0.0f`, `Type = 0x10` (bit 4) |
| `+0x14` | `0` | `FixedFormat = 0` |
| `+0x20` / `+0x24` | `0.0f` / `1.0f` | `MinimumValue` / `MaximumValue` |
| `+0x28` | `virtual[0x1b8](0xf)` (signed) | `MaximumSize` |
| entry 1 `+0x00…+0x28` (base `out+0x338`) | `0x88005`, `1`, `0`, `([this+0x2e798]|0x40)`, `pow(k,-25)`, `pow(k,7)`, `0x200` | a second (PQ-ish) segment: `MaximumSize = 0x200 = 512` |

So Apple's own AMD answer for a plane's gamma is **2 segments, both masked to plane 0, with `MaximumSize`
512 and `virtual[0x1b8](0xf)`** — not 0, and above all **not unmasked**.

### 2.2 `AppleIntelFramebufferController` — the second real driver in the same collection

`AppleIntelCFLGraphicsFramebuffer::AppleIntelFramebufferController::getDisplayPipeCapabilityExtended(AGDCFBGetExtendedCapability_t*,
AGDCFBGetExtendedCapability_t*)` = **`0x13e41692`** (same function in `AppleIntelICLLPGraphicsFramebuffer`
`0x14035064`-area and `AppleIntelKBLGraphicsFramebuffer`). It reads a *third* input field:

```
0x13e416a3  idx   = *(u32*)(in + 0x00)   ; must be < this+0x1e0d (the display path count)
0x13e416ed  plane = *(u32*)(in + 0x08)   ; 1-based, must be 1…8   ← the plane index
0x13e416fb  q     = *(u32*)(in + 0x04)   ; the query, a single bit
0x13e41715  jump table (1…8) → 1, 2, 4, 8 valid; 0x10/0x20/0x40/0x80/0x200/0x400/0x800/0x1000/0x2000/0x4000/0x8000
            → "IGFB:: %s:%d Currently unsupported feature request…" (0xe00002c7)
*out: +0x08 = count = 1, records from +0x0c, stride 0x32c
```

| query | record (`out+0xc…`) | value |
|---|---|---|
| 2 (gamma) | `+0x00` Flags | `0x101001` |
| | `+0x04` plane mask | `1` or `3` (`(¬this[0x2e59] & 1) * 2 + 1`) |
| | `+0x0c` | `const / path[0xa1c8] * const` (a normalisation factor) |
| | `+0x10` Type | `0x100` (bit 8) |
| | `+0x14` FixedFormat | `0` |
| | `+0x18` ConfigurationID | `0` |
| | `+0x20`/`+0x24` | `0.0f` / `1.0f` |
| | `+0x28` MaximumSize | **`0x100` = 256** for the gamma ramp, **`0x400` = 1024** (the `+0x1c = 0x100` / `+0x1c = 8` pair is the other table) |
| 4 (linearization) | `+0x00` Flags | `0x100000` |
| | `+0x04` plane mask | `1` or `2` |
| | `+0x10` Type | `1` (bit 0) |
| | `+0x28` MaximumSize | **`0x400` = 1024** |
| 8 (cursor) | `+0x00` Flags | `0xc5`, sizes from `path+0x219c…0x21a2` |

### 2.3 `AppleParavirtFramebuffer` — the dialect our kext speaks

`AppleParavirtFramebuffer::doDeviceAttribute` = `0x142bb5f4` (block `0x142bb69c`) is the object our
`RDNA4DeviceControl::framebufferQuery` was copied from, and it is the one that answers **attribute 0x711 with
`outLength == 0x196c`** (`0x142bb6d6`), with `+0x00` = framebuffer index and `+0x04` = query — exactly the
consumer's buffer. Its per-query answers (jump table `0x142bbcc8`):

| query | answer |
|---|---|
| 1 (CSC) | `count=1`; `Flags=5`, `+0x04=0`, `+0x10 Type=8`, `+0x2c=0x30300`, `+0x30 = -1.0f/1.0f` |
| **2 (gamma)** | `count=1`; `Flags=5`; **`+0x04 = (this[0x220] != 0)`** (1 if the display has a gamma table); `+0x0c = 0.0f`; `+0x10 Type = 8`; `+0x14 = 0`; `+0x18 = 0`; `+0x20 = 0.0f`; `+0x24 = 1.0f`; **`+0x28 = this[0x220]`** (the table size) |
| **4 (linearization)** | `count=1`, then `0x30` bytes of zeros — i.e. **mask 0, Type 0, MaximumSize 0** |
| 8 (pipeline scaler) | `count=1`, `bzero(record, 0x32c)` — all zeros |
| 0x10 (plane scaler) | `count=1`; `+0x28 = 1`, `+0x30/+0x34` from the detailed timing |

The three drivers agree on the record skeleton and disagree on the middle fields — the constants differ by
AGDC release (`Flags` 5 vs `0x88005` vs `0x101001`; `Type` 8 vs 0x10 vs 0x100). **They agree that `+0x04`
is the plane mask and that `+0x28` is the table's entry count.**

### 2.4 The GPU capability (0x921 here, 0x980 there) has no gamma field at all

For completeness, because item 1 of the task assumed otherwise:

* `AppleParavirtGPUControl::vendor_doDeviceAttribute` (`0x142b616a`, block `0x142b621a`) answers **0x921 =
  0xb0 bytes** and writes only: `[0x00]=0`, `[0x08]=1`, `[0x10]=0`, `[0x18]=0`, `[0x1c]=0x40000000500`,
  `[0x24..0x43]=0`, `[0x44]=0x500`, `[0x48]=0`, `[0x50]=0x40000000000`, `[0x58..0x83]=0`, `[0x84]=1`,
  `[0x8c]=0`, `[0x8e/0x96/0x9e/0xa6]=0`, `[0xae]=0`. That is *exactly* what our `case 0x921` writes.
* `AMDRadeonX6000_AmdAgdcServices::getGpuCapability` (`0xbf7f916`) answers **0x980 = 0xdc bytes** and its
  fields are: `+0x00/0x08/0x10/0x18` = u64 masks of display paths (`2 << path`), `+0x20` = path count,
  `+0x24/0x28/0x2c` = `path->vtable[0x1b8](0) & 0xff` three times, `+0x30` = path count again, `+0x34` = the
  PCIDevice pointer, `+0x3c + 8i` = the display-path object pointers. Same for `AtiDeviceControl::getGpuCapability`
  (`0x13ba5e60`, 0xdc): masks at `+0x00/+0x08/+0x10/+0x18`, `+0x20` = framebuffer count, `+0x34` = PCIDevice,
  `+0x3c + 8i` = framebuffers.
* The **GPU-level** AGDC object does not answer these at all: `AMDRadeonX6000_AmdGpuWrangler::vendor_doDeviceAttribute`
  (`0xbf9494e`) handles only attribute 1 (`getVendorInfo`) and 3 (load Controller/Accelerator/HWServices).

**Conclusion.** A GPU-capability answer is a set of path masks, counters and object pointers; neither version in
this kernel collection has a per-plane gamma/linearization field, and the only driver that writes the 0xb0
version (paravirt) leaves its `0x8c…0xaf` region zero. So there is nothing to fill in `0x921` that Apple's own
drivers fill — and inventing bits there would be a guess in a structure IOPresentment parses (a wrong shape is
a WindowServer failure, not a kernel panic). The per-plane capability lives **only** in the `0x711` records.
That is a correction to the task's item 2 and to `hdr-userspace-gating.md` §9.4 item 1.

---

## 3. Why ours is empty — the bug, exactly

`src/rdna4accel.cpp`'s `framebufferQuery` answers query 2 (gamma) with

```cpp
case 2:
    u32(8, 1), u32(0xc, 5), u64(0x10, 0), u32(0x18, 0), u32(0x1c, 0x80), u32(0x20, 0), u64(0x24, 0), u32(0x2c, 0);
    u32(0x30, 0x3f800000u), u64(0x34, 0);   // 1.0
```

which in §1.1's layout is

| field | paravirt | ours | effect |
|---|---|---|---|
| count `+0x08` | 1 | 1 | the loop runs once |
| `Flags +0x00` | 5 | 5 | same |
| **plane mask `+0x04`** | `(has gamma)` = 1 | **0** | **`(1<<plane) & 0` = 0 → the only segment is skipped** |
| `Type +0x10` | 8 | 0x80 | |
| `MaximumSize +0x28` | the table size | **0** | a segment with 0 entries, if it survived |

So `CreateGammaDictionary` returns without appending anything, `GammaArray` is an empty CFArray, and
`CFArrayGetCount` = 0 → `plane[0]+0x88 = 0` → `Attempting to get capabilities from plane with no gamma`.
Query 4 (linearization) is worse: our record is `bzero`ed except the count, so its mask is 0 too.

This is the same defect `AppleParavirtFramebuffer` has for queries 4 and 8 (its records are zeroed), which is
consistent with a paravirtual display never opening the HDR path. **The zero was never the *table*; it was the
*applicability mask* of a segment that was already there.**

---

## 4. What was implemented

One file: `src/rdna4accel.cpp`. `src/ioaccel.hpp` and `src/ioaccel_symbols.inc` were **not** touched — no Apple type
or symbol is referenced by this change (the record is a byte layout, not a KDK object), and `ioaccel.hpp` is
regenerated by `tools/gen/accelgen.py`, where a hand-added struct would be lost on the next run.

### 4.1 The option

`rdna4-agdccaps`, `Trial::option` semantics (`src/trial.cpp`: a boot-arg of that name, or `armed
rdna4-agdccaps=1` in the `rdna4-trial` NVRAM variable, which is written back as `used …` before it is acted on,
so a boot that dies loses it). Read once in `accelTo`, next to `rdna4-accelgamma` and for the same reason (before
any pipe exists, no NVRAM work under the family's lock), into `gAgdcCapsKind`.

* **Default off.** `gAgdcCapsKind` stays 0, `capabilityRecord()` is never called, and the two answers are the
  bytes they were; the AGDC log line is the old one, throttled the same way (32 lines). A boot without the option
  is the boot it was.
* `=1`: the **paravirtual-shaped** record — `Flags 5`, gamma `Type 8` (both `AppleParavirtFramebuffer`'s own
  values for query 2 at `0x142bbb07`, i.e. the dialect our attribute-1 answer advertises), linearization
  `Type 1` (`AMDRadeonX6000::getFbCapabilityForDegamma`), `MaximumSize 1024` for both
  (`AppleIntelCFLFramebufferController` answers `0x400` for both).
* `=2`: **`AMDRadeonX6000`'s own numbers** instead — gamma `Flags 0x88005`, `Type 0x10`; linearization
  `Flags 0x80005`, `Type 1`, `MaximumSize 1`. Same mask and the same layout; it exists so one boot can show
  whether the middle fields matter to the consumer, without a rebuild.

### 4.2 The record

`RDNA4DeviceControl::capabilityRecord(o, size, query, kind)` writes, at `o + 0xc`, for query 2 (gamma) and 4
(linearization) — the two kinds that become `GammaArray` and `LinearizationArray`:

| offset | value | from |
|---|---|---|
| `o+0x08` | `1` | the answer's count (IOPresentment clamps ≥9 to 8; it offers 8) |
| `+0x00` | `Flags` 5 (kind 1) / `0x88005` or `0x80005` (kind 2) | paravirt / X6000 |
| **`+0x04`** | **`0xff`** | the plane mask — *the field that was zero* |
| `+0x0c` | `0.0f` | `NonAtomicApplicationPeriod` |
| `+0x10` | `Type` 8 / 1 (kind 1) or `0x10` / 1 (kind 2) | paravirt, X6000 |
| `+0x14` | `0` | `FixedFormat` |
| `+0x18` | `0` | `ConfigurationID` |
| `+0x20` | `0.0f` | `MinimumValue` |
| `+0x24` | `1.0f` | `MaximumValue` |
| `+0x28` | `1024` (kind 1) / `1024`, `1` (kind 2) | the entry count |

Two deliberate choices, both stated because they are the only places where the answer is not literally Apple's:

1. **The mask is `0xff`, not `1`.** `CreateGammaDictionary` indexes the mask by the *pipe* argument of
   `CreatePlaneDictionary(plane, pipe, array)` (`0x7FF8159A0A67` builds `1 << edx`, and `edx` is the pipe), and the
   same 0x711 answer is reused for every pipe of that framebuffer — our driver cannot know which pipe index the
   planelist will be built for. Bits 0…7 cover every index the consumer can test; a superset cannot hurt, because
   the mask is used for nothing else in the image (single use, `0x7FF8159A0B04`). Whether Apple's `1` (path 0) is
   enough is the first thing the armed boot's log will tell us.
2. **1024 entries.** Our framebuffer already publishes `IOFBGammaCount=1024, IOFBGammaWidth=10`
   (`docs/hdr-gamma.md`), so the AGDC answer and the framebuffer gamma description agree. The records carry no
   table — only the entry count — so nothing here has to match the width bit-for-bit; `FixedFormat` stays 0
   because no driver in the collection fills it for a plain ramp.

### 4.3 Bounds

`capabilityRecord` refuses (`false`, nothing written) unless `size >= 0xc + 0x30`, which is the header plus the
last field it writes (`+0x28`, 8 bytes); the caller then gets `kIOReturnUnsupported` — the same "refuse rather
than half-answer" rule the rest of the function follows, one log line, no partial record. Everything else in
`framebufferQuery` is unchanged: unknown sub-queries still return `kIOReturnUnsupported`, `in[0]` must still be a
framebuffer of ours, and the sizes are still the ones each case was written for.

### 4.4 The log line (item 3 of the task)

With the option armed, *every* `0x711`/`0x921` call logs one line:

```
AMDRadeonRDNA4: accel: agdccaps=1: AGDC attribute 0x711 (in 6508, out 6508): 0x0; in[0..3] 0x0 0x2 0x8 0x0; out[0..3] 0x0 0x2 0x1 0x5
```

`in[0]` = framebuffer index, `in[1]` = query, `in[2]` = the entry count the caller offered; `out[0..3]` = the
answer's first four words (`out[2]` is the count we returned, `out[3]` the record's `Flags`). Reading `in`/`out`
is bounded by `inLength`/`size` (a lambda each), and no `%s` is ever given a number — the format string takes
`%u`/`%#x`/`%lu` only, which is the rule the 2026-11-26 00:30 panic cycle cost us.

### 4.5 0x921 was left alone, on purpose

Item 2 of the task asked to fill "the gamma/linearization capability fields of our `0x921` answer". **That
structure has no such fields** (§2.4): paravirt's 0xb0 GPU-capability answer is masks and counts (we already
match it byte for byte), and `AMDRadeonX6000`/`AtiDeviceControl`'s 0xdc one is path masks, three counts, the
PCIDevice pointer and the display-path object pointers. The only gamma/linearization capability that exists in
this protocol is the `0x711` record. Filling bits in `0x921` because a plan said so would be exactly the
"invented field" the same plan forbids — and the wrong shape there is parsed in WindowServer, where the failure
mode is a dead display session, not a log line. What the option does for 0x921 is log it, so the next boot says
whether IOPresentment asks it at all.

### 4.6 Build

| what | result |
|---|---|
| kext on the card's machine (`~/rdna4/a4-agdc`, `make -j2 kext`) | **exit 0**, `Built …AMDRadeonRDNA4.kext for x86_64 (min macOS 11.0)`, UUID `E8A2FD86-0177-3AC1-B38E-1D8C02816CC2` |
| new warnings | **none**: `rdna4accel.cpp` compiled twice with the same flags, this change and `HEAD`'s file, gives 5 warning lines both times (3 from Lilu's `kern_util.hpp`, 2 pre-existing `-Wsign-conversion` at `rdna4accel.cpp:310/336`); the diff between the two sets is the file name |
| the new code is in the binary | `strings` of the built kext has `rdna4-agdccaps`, the armed line and the per-call line |
| the record's byte layout, against the consumer's reader | a scratch replay of `capabilityRecord` + the disassembled reader (`count at +8`, `mask & (1<<pipe)` at `+4`, stride `0x32c`) yields **1 segment for pipe 0 and for pipe 3**, kind 1 and 2, gamma and linearization; the *old* answer yields **0 segments**; a 0x20-byte answer is refused with nothing written |
| host tests (`make -j2 CXX=g++ CC=gcc test` here) | the pre-existing failure `navi44: gfx images: RLC_G must come last` / `0 gfx images … FAILED` (the one `hdr-accel-gamma.md` §11 recorded from the pristine tree). Nothing in `test/` includes `rdna4accel.cpp` |

---

## 5. The one boot that would prove it

Scope: this change is armed with a trial option and nothing else in the driver changed, so one boot answers it.

```bash
# 0  arm it (the boot-arg rdna4-agdccaps=1 does the same; the variable consumes itself, so a bad boot is clean)
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial="armed rdna4-agdccaps=1"
#    … reboot, wait for the desktop (≥10 min on this machine) …

# 1  the option was read and the answers were built
sudo sysctl -n debug.rdna4_log | grep 'rdna4-agdccaps'
#    expect exactly 1 line: "… rdna4-agdccaps=1: the 0x711 gamma and linearization records get a plane mask, …"

# 2  ★ the carrier, named: every AGDC call with its query and its first answer words
sudo sysctl -n debug.rdna4_log | grep 'agdccaps=1: AGDC attribute'
#    expect ≥16 lines: "AGDC attribute 0x711 (in 6508, out 6508): 0x0; in[0..3] <fb> 0x10 0x8 0x0; …",
#    then 0x8, 0x4, 0x2, 0x1, 0x40, … — the order is IOPresentment's (query 0x10 first).
#    the 0x2 line's out[2] must be 0x1 (our count) and out[3] the Flags word (0x5 with =1).

# 3  the gate itself
sudo log show --last 10m --info --debug --predicate 'process == "WindowServer"' > /tmp/ws.log
grep -c 'plane with no gamma' /tmp/ws.log       # baseline 38 per boot (60 on the 8-bit/256 boot)
grep -c 'hdr-policy' /tmp/ws.log                 # baseline 0
grep -c 'Warning: Incorrect entry count' /tmp/ws.log   # must stay 0: it would mean our count is >8
```

| 2 | 3 (`plane with no gamma`) | conclusion |
|---|---|---|
| 0x711 answered, `out[2] = 1` on the 0x2 line | **0** and `hdr-policy` appears | the mask was the whole bug: the plane has a gamma segment, CoreDisplay's count is non-zero, and `word[IOPCapabilities+0xEA]` is non-zero too — then `hdr-policy = 0x…` names what is still missing for HDR (EOTF/CSC/limits), which is the next, separate question |
| 0x711 answered | still 38 | the mask was necessary but not sufficient: the segment exists and is dropped later, i.e. the *dictionary* path from `PlaneArray` to `GammaArray` loses it — then the next measurement is `0x1` (CSC) and `0x1000` (PlaneCSC), whose records our driver still answers with paravirt's zero mask |
| `0x711` missing from the log, or `in[0]` ≥ our framebuffer count (the return code in the same line is `0xe00002c2` = `kIOReturnBadArgument`) | unchanged | the whole answer is being refused before any record is built: the framebuffer index IOPresentment asks for is not one of `framebuffers()`. That is a different fix and the log says which index |
| `0x921` appears in the log | — | IOPresentment *does* ask the GPU-capability attribute; its `in[1]`/`in[2]` then say whether it expects a sub-query we answer at all |

Do not arm it twice in a row without reading the log: the trial variable is consumed on use, so the boot after it is
already clean, and a second armed boot would mix two measurements.

---

## 6. What stayed hypothesis

| claim | status | how it would be settled |
|---|---|---|
| `+0x88` is `CFArrayGetCount(GammaArray)`, and the dictionary comes from IOPresentment | **proven** (`hdr-userspace-gating.md` §9.1, re-read here) | — |
| the answer layout: `+0 count`, records at `+0xc`, stride `0x32c`, plane mask at record `+4` | **proven** twice: the reader (`CreateGammaDictionary` / `CreateLinearizationDictionary`, `0x7FF8159A0A30` / `0x7FF8159A0424`) and three writers (paravirt `0x142bbb07`, `AMDRadeonX6000` `0xbf7101a`/`0xbf71354`, Intel `0x13e41692`) | — |
| our zero mask is *why* `GammaArray` is empty | **proven** by the two above: with mask 0 the reader's `testq` fails for every record, and our answer's only record is that one | acceptance 2/3 confirms end to end |
| the mask is indexed by the **pipe**, not the plane | **inferred** (`0x7FF8159A0A67` shifts `edx`, and `CreatePlaneDictionary(plane, pipe, …)` passes the pipe there). Which index a planelist is built with, and whether Apple's `1` suffices, is not known — hence `0xff` | acceptance 3; if it still fails, arm `=2` and/or narrow the mask to `1` in a follow-up |
| `Type`/`FixedFormat`/`Flags` values matter to CoreDisplay beyond the count | **unknown**. `GammaChannelTableEntryCountAndType()` walks the segments and picks the widest entry type among {16,12,10,8} bits, but which field that walk reads on its side (and what it requires for HDR) is CoreDisplay's own, still unreversed | acceptance 3 (`hdr-policy`), then CoreDisplay `+0x117D2`'s segment struct |
| 1024 entries | **inferred but consistent**: our framebuffer publishes `IOFBGammaCount=1024`, Intel answers `0x400` for both tables, and a 1024×10-bit table is what the pipe's gamma LUT holds | a real HDR session, which would program the table |
| that `0x921` is what the consumer calls the GPU capability, and 0x980 the framebuffer list | **inferred from our own answers**: paravirt answers 0x921 (0xb0) and 0x980 (0xdc) exactly as we do, and our attribute-1 answer advertises paravirt's vendor id/version | acceptance 2 (the log names the attributes actually asked) |
| nothing here runs at boot with the option off | **verified by inspection and by the identical warning/binary check**, not by a boot | any ordinary boot: the old log line and no `agdccaps` strings in the ring log |

---

## 7. Rollback

There is no sysctl: the option is read once, in `accelTo`, and it is a **trial**, so

* **the next boot without the trial is already clean** (the variable wrote itself back as `used …` when it was
  read), and
* to stop arming it from a boot-arg, drop `rdna4-agdccaps=1` from `boot-args` and reboot.

With the option read as 0 nothing else in the driver behaves differently: `capabilityRecord()` is not called, the
two answers are the bytes they were, and the AGDC log line is the old throttled one. No property, no object, no
symbol lookup was added — verified by the two-file compile check (§4.6), not by a boot.

If a boot with it armed hangs or panics, read the panic report first (`ls -t
/Library/Logs/DiagnosticReports/Kernel-*.panic`, `macOSPanicString`). A `rdna4Log`/`_vsnprintf` frame would mean a
format-string mistake; the two new formats are the only new ones and neither takes `%s`.

---

## 8. How to re-check the kernel side, and the consumer-side one

Kernel collection (this box, `pip install --user capstone`):

```bash
mkdir -p /tmp/agdc-work && cp /tmp/trng-work2/{kc2,dis2}.py /tmp/gamma-work/dis3.py /tmp/agdc-work/
cd /tmp/agdc-work && python3 - <<'PY'
import kc2, dis3
x6 = kc2.by_eid('AMDRadeonX6000')[0]
dis3.show(x6, 0xbf6f668, 420, 'AmdAgdcServices::vendor_doDeviceAttribute (the 0x711/0x921/0x980 dispatch)')
dis3.show(x6, 0xbf70dd4, 260, 'getFbExtendedCapability (query -> handler)')
dis3.show(x6, 0xbf7101a, 300, 'getFbCapabilityForGamma  (the two records)')
dis3.show(x6, 0xbf71354, 220, 'getFbCapabilityForDegamma')
dis3.show(x6, 0xbf7f916, 420, 'getGpuCapability: 0xdc, no gamma field')
pv = kc2.by_eid('AppleParavirtGPU')[0]
dis3.show(pv, 0x142bb69c, 400, 'AppleParavirtFramebuffer::doDeviceAttribute: 0x711 needs outLength 0x196c')
dis3.show(pv, 0x142bbb07, 130, 'paravirt query 2 (gamma)')
dis3.show(pv, 0x142bbb83, 120, 'paravirt query 4 (linearization): all zeros')
cf = kc2.by_eid('AppleIntelCFLGraphicsFramebuffer')[0]
dis3.show(cf, 0x13e41692, 400, 'Intel getDisplayPipeCapabilityExtended: fb, query, plane')
PY
```

Consumers (on the card's machine, from the dyld shared cache — no extraction needed):

```bash
F=/System/Library/PrivateFrameworks/IOPresentment.framework/Versions/A/IOPresentment
dyld_info -disassemble "$F" > /tmp/iop.dis          # 95 856 B of __text, image base 0x7FF81598D000
grep -n 'AGDC .* successful' /tmp/iop.dis           # the 16 gather blocks and their queries
sed -n '/__ZN25IOPresentmentCapabilities39__GatherAGDCDisplayPipelineCapabilitiesEv/,+60p' /tmp/iop.dis
sed -n '/IOPresentmentCreateGammaDictionaryEjjP9__CFArray:/,+140p' /tmp/iop.dis   # the mask test and the 7 fields
```
