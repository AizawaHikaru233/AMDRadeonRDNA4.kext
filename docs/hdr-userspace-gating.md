# What `SLSDisplaySupportsHDRMode` / `CGSIsHDRSupported` actually check (macOS 26.6.2, 25G83, x86_64)

Investigation date: 2026-11-26 03:24–04:30, Mac live at a desktop session (`ssh Celia@192.168.31.254`),
read-only. Every claim below is tagged:

* **[dis]** proven by disassembly of the shipped binaries (addresses given so it can be re-checked),
* **[log]** measured at runtime from this machine's unified log,
* **[inf]** inferred from strings / imports / structure of the code,
* **[hyp]** hypothesis, with the experiment that would settle it.

The short version, for the impatient:

> The verdict is computed in **userspace, inside WindowServer, by CoreDisplay.framework** — not by the
> kernel, not by SkyLight. `SLSDisplaySupportsHDRMode` and `CGSIsHDRSupported` are thin client stubs
> that both land on **one** predicate (`sub_51b7e` in CoreDisplay). That predicate ANDs two independent
> capability words:
>
> 1. **the display's EDID capability word** — CTA-861.3 HDR static metadata block present **and** EOTF
>    bit 2 (SMPTE ST 2084 / PQ) set. *We already satisfy this* — the OS parses our block every boot.
> 2. **the plane's capability word** (`CoreDisplay::IOPCapabilities` +`0xea`, bit 1) — this is derived
>    from the **framebuffer plane's gamma / linearization tables**, i.e. from the kernel side of
>    `IOFramebuffer`, not from the EDID, and not from `'hdrm'`.
>
> Our display fails (2). The OS says so itself, in two log messages that are present on every boot of
> this machine: `Attempting to get capabilities from plane with no gamma` (38×) and
> `InstallTiming - legacyRng_legacyEnc_legacyBit_legacyClr FAILED ... code 0xe00002c7`
> (= `kIOReturnUnsupported`) (115×). There is **no vendor/product whitelist that a TCL panel has to
> pass** — the only vendor table in the predicate is an Apple-only *exclusion* list (vendor `0x0610`),
> and it is skipped entirely for a non-Apple vendor.

---

## 0. Where the code lives (and one correction to the starting points)

| Path | Reality in this build |
|---|---|
| `/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight` | **directory contains only `Resources/` and `_CodeSignature/`**; the Mach-O lives only in the dyld shared cache. `nm`/`otool` fail on the path; `dyld_info` works (it consults the cache). |
| **`/System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay`** | **it exists — as a cache-only image.** It is under `Frameworks/`, *not* `PrivateFrameworks/`; `ls` shows no file, `dyld_info -segments` resolves it. This is the binary that holds the verdict. |
| `CoreGraphics.framework` | no on-disk Mach-O either; it **re-exports** the SkyLight entry points (see §1.1). |
| `/System/Library/Extensions/IOGraphics.kext` | irrelevant to this question — see §1.5. |

Shared cache: `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h`
(+ `.01` … `.06`, ~4.8 GB total). Nothing needs to be copied off the machine to read it: a ~200-line
Python reader (in `/tmp` on the Mac, see §6) parses the cache header → mapping table → the image's
`LC_SEGMENT_64`/`LC_SYMTAB`/`LC_FUNCTION_STARTS`, and dumps the sections you want.

Image bases (unslid, equals what `dyld_info -segments` calls `unslid-addr`):

| Image | base | `__text` | size |
|---|---|---|---|
| CoreDisplay | `0x7FF80535B000` | `0x7FF80535C3F0` (+0x13F0) | 987 460 B |
| SkyLight | `0x7FF8091FE000` | `0x7FF809200790` (+0x2790) | 4 727 347 B |

**Limitation of `dyld_info` on cache images** (measured): `-exports`, `-imports`, `-segments`,
`-load_commands`, `-uuid`, `-lookup_va` work; `-fixups`, `-function_starts`, `-fixup_chain*`,
`-disassemble` return **nothing** for a cache image. Local symbols (26 537 for SkyLight, i.e. all the
static functions) and function starts are still recoverable: `LC_SYMTAB.symoff`/`stroff` and
`LC_FUNCTION_STARTS.dataoff` are **file offsets into the cache file** (not vmaddrs), so reverse-map
`fileOffset → (mapping.address + delta)` and read them directly. That is what the scripts in §6 do.
So: **nothing needed for this analysis was unreadable.**

---

## 1. The predicate chain

### 1.1 From the public-ish entry points down to the decision

```
CGSIsHDRSupported(display)            [CoreGraphics]   = re-export of _SLSIsHDRSupported
                                                                  │  [dis]
SLSIsHDRSupported(display)            SkyLight @0x1D91B4 ─────────┤  IPC stub ("IsHDRSupported")
SLSDisplaySupportsHDRMode(display)    SkyLight @0x1DC516 ─────────┤  IPC stub ("DisplaySupportsHDRMode")
SLSDisplayIsHDRModeEnabled(display)   SkyLight @0x1DC869            IPC stub ("DisplayIsHDRModeEnabled")
SLSDisplayGetPreferHDR10(display)     SkyLight @0x1DB80B            IPC stub ("DisplayGetPreferHDR10")
SLSGetDisplayMaximumHDRValue          SkyLight @0x1D8E20
SLSEnableHDR / SLSIsHDREnabled        SkyLight @0x1D8FEE / 0x1D9098
                                                                  │
                                    WindowServer ─────────────────┘
                                                                  ▼
CoreDisplay_Display_SupportsHDRMode_Server   CoreDisplay @0x51B42
        └─ sub_51B7E  ← THE predicate
CoreDisplay_Display_SetHDRModeEnabled_Server CoreDisplay @0x52010  calls sub_51B7E too
```

**[dis]** `CGSIsHDRSupported` is literally listed by `dyld_info -exports` on CoreGraphics as
`[re-export] _CGSIsHDRSupported (_SLSIsHDRSupported from SkyLight)`. So both names the question asks
about are the *same* decision, reached through two different stubs. That is why both were `False`
together.

**[dis]** The SkyLight functions are **IPC stubs, not local logic**: each builds a message whose first
dword is `0x1513` and whose selector half is `0x772a00000000` (`SLSDisplaySupportsHDRMode`),
`0x80f800000000` (`SLSIsHDRSupported`), then `call 0x7FF809684DB8` (the send/receive helper) and tests a
reply status (`cmp dword [r14+0x14], 0x47`). The server-side selector *names* are in `__cstring` right
next to each other, which is how they are matched up:
`'DisplaySupportsHDRMode'` (`0x7FF8096F0031`), `'DisplayIsHDRModeEnabled'`, `'DisplayGetPreferHDR10'`,
`'SLSDisplaySupportsHDRMode'` (`0x7FF8096CCD2F`), … **[inf]**: the `SLS…` names are the client names, the
`Display…` names the server handlers; both live in the same image (SkyLight is linked into
WindowServer).

**[dis]** `CoreDisplay_Display_SupportsHDRMode` (`0x519EC`) is the client-side half of CoreDisplay's own
API: it resolves a *function pointer* out of a global (`0x7FF843AD5798`) once, and calls it — i.e. in a
process that is not WindowServer it RPCs, in WindowServer it runs the `_Server` variant inline. The
`_Server` variant does:

```
0x51B42  call sub_db29c              ; display-record lookup by CGDirectDisplayID (rdi = edi)
         test rax,rax ; je  ret 0
         rdi = rax ; call sub_3c881  ; deref to the per-display record
         rdi = rax ; call sub_51B7E  ; ← the predicate  (returns al)
```

### 1.2 The predicate itself — `sub_51B7E` (CoreDisplay +0x51B7E, unslid `0x7FF8053ACB7E`)

Decompiled by hand from the disassembly (all offsets are byte offsets in the objects):

```c
bool supportsHDRMode(DisplayRecord *d) {
    CGXDisplayDevice *dev = d->vtable->m20(d);                 // the CGXDisplayDevice
    if (dev->flags[0x64] & 0x80) return false;                  // (1) hard exclusion
    uint32_t did  = d->conn[0x68]->sr[0x160]->dword[0x58];      // CGDirectDisplayID
    Capabilities    *caps = lookupByDisplayID(did)->p[0x128];   // CoreDisplay::IOPCapabilities*
    bool a = caps && (caps->bits[0xea] & 0x02);                  // (2) PLANE capability bit 1
    bool b = (d->byte[0x200] == 1) && (d->byte[0x1e4] & 0x04);   // (3) EDID: HDR block + PQ
    ... mode two-value comparison, must DIFFER ...               // (4)
    if (!(a && b))            return false;
    if (modeValue1 == modeValue2) return false;
    // (5) Apple-only product exclusion list, see §2
    return true;
}
```

Evidence, instruction by instruction (**[dis]**):

| VA | instruction | meaning |
|---|---|---|
| `…3ACBA0` | `mov rax,[rdi]; call [rax+0x20]` | virtual getter → `CGXDisplayDevice*` |
| `…3ACBA6` | `test byte [rax+0x64], 0x80 ; jne → xor eax,eax; ret` | **gate (1)**: a flag bit that short-circuits everything to `false`. **[hyp]** "not a real/local display" class of flag (AirPlay/virtual/offline). It is *not* set for us — see note below. |
| `…3ACBB0-BBF` | `rax=[rbx+0x68]; rax=[rax+0x160]; esi=[rax+0x58]` | pull the `CGDirectDisplayID` out of the connect record |
| `…3ACBD7` | `call sub_F2526+0x12E` | dictionary lookup `globalMap[displayID] → record` |
| `…3ACBE6` | `rcx=[rax+0x128]; je` | `record->p128` — the per-display capability object; **[inf]** its type is `CoreDisplay::IOPCapabilities` (see §1.4 for why), **[dis]** its field offsets and every access to them |
| `…3ACBEB` | `r13b=[rcx+0xea]; and 2; shr 1` | **gate (2)**: `IOPCapabilities +0xea`, **bit 1** |
| `…3ACBFE` | `cmp byte [rax+0x200],1 ; jne → r15=0` | **gate (3a)**: `record +0x200` = "HDR static metadata block was parsed" |
| `…3ACC07` | `r15b=[rax+0x1e4]; and 4; shr 2` | **gate (3b)**: `record +0x1e4` = **EOTF mask**, bit 2 = **PQ / ST 2084** |
| `…3ACC7E` | `and r13b, r15b ; cmp r13b,1 ; jne → return 0` | gates 2 **AND** 3 must both hold |
| `…3ACC89` | `cmp r14d,[rbp-0x40]; je → return 0` | **gate (4)**: two 32-bit mode values (`[connect+0x6c]` mapped through `sub_a188c(...,2)` vs. the raw one from `sub_db451`) must **differ** — i.e. the HDR10 timing must be a *different mode* from the SDR one, not the same raster re-labelled |
| `…3ACC8F-CCA` | Apple vendor/product table | §2 |
| `…3ACCF0` +0x… | stack-cookie check, `ret` | return `al` |

**Notes on gates (1) and (4).**
(1) `[dev+0x64] & 0x80` would short-circuit everything (it is written by `_CDVirtualDisplayCreate`,
`0x7FF80535FADD: mov word [r8+0x64], ax`, which fits a "virtual/wireless/offline display" reading).
I **cannot exclude** it from outside WindowServer, but it is unlikely to be set for a real DP display
that the OS drives at 4K60 and that it parses an EDID for. **[hyp]** — the same probe that answers §5
**H2** answers this.
(4) compares two 32-bit values and returns `false` when they are equal: the mode's value from
`sub_db451` (getter at `sub_F2526+0x350`) against the same value mapped through
`sub_a188c(conn[0x6c], value, 2)`. I read it as NVIDIA's "an HDR10 timing must be a *different* mode
from the SDR one" (`ref/nvidia-macos-driver` `nvrm-fb.cpp:3578,3662`), but the exact semantics of the
two values is unresolved. It can only be reached *after* (2) and (3) pass, so it is not the current
failure. **[inf]**

### 1.3 Gate (3) — the EDID half. **This one we already pass.**

`record + 0x1e4` and `+ 0x200` are written by the CTA-861 parsers **inside CoreDisplay itself** (the
kernel's EDID parser is duplicated in userspace here):

| symbol (from `__func__` strings, CoreDisplay `__cstring`) | image offset | VA |
|---|---|---|
| `void ParseCEAExtendedHDR10DataBlock(IOFBConnectRef, UInt8 *, IOByteCount, bool)` | 0x22007 | `0x7FF80537D007` |
| `void ParseCEAColorimetryDataBlock(IOFBConnectRef, UInt8 *, IOByteCount, bool)` | 0x22007 (same block) | — |
| `void InstallDisplayIDEXT(IOFBConnectRef, EDID *, DisplayIDEXT *, bool, bool)` | **[inf]** same region | — |

**[dis]** In `ParseCEAExtendedHDR10DataBlock`, the field writes are
`mov [rbx+0x1e4], r14b` with `and r14b,0x3f` beforehand (`+0xC97`), a luminance triple at
`+0x1e8/+0x1f0/+0x1f8` (`movss`), presence/option bytes at `+0x1ec/+0x1f4/+0x1fc`, the static-metadata
descriptor bytes copied to `+0x21a…`, and `mov byte [rbx+0x200],1` (`+0xCDC`). The 6-bit mask is the
**CTA-861.3 EOTF bitfield** (bit0 SDR, bit1 HDR-gamma, bit2 PQ, bit3 HLG, bits 4-5 reserved) — exactly
`SupportsEOTF`'s input.

**[log]** On this machine, on every boot of the current driver:

```
WindowServer: (CoreDisplay) [DEBUG] - ParseCEAColorimetryDataBlock, block with length 3
WindowServer: (CoreDisplay) [DEBUG] - ParseCEAExtendedHDR10DataBlock, block with length 6
```

`length 3` = our `05 c0 01` (colorimetry, BT.2020), `length 6` = our `06 07 01 73 4f 02` (HDR static
metadata; EOTF `0x07` ⇒ **bit 2 = PQ is set**). So gates (3a) and (3b) are satisfied — **the EDID is
not the reason the answer is `False`**, and re-serving more EDID blocks will not change it. (This
confirms the earlier assumption in `hdr-30bpp-plan.md` §A2 that "HDR is already declared by the EDID",
now with the parse confirmed at runtime rather than inferred.)

### 1.4 Gate (2) — the plane half. **This is what fails.**

`record->p128` is the per-display capability object. **[inf]** it is `CoreDisplay::IOPCapabilities` —
this image contains `CoreDisplay::IOPCapabilities::…` `__func__` strings for exactly the capabilities
that are missing (gamma/linearization/EOTF/CSC per *plane*), and the object is populated by a
"capabilities from plane" routine; but no instruction ties the class name to offset `+0x128`
directly. Method inventory (Apple's own names, CoreDisplay `__cstring`):

```
bool  CoreDisplay::IOPCapabilities::SupportsEOTF(uint32_t, bool, TransferFunctionCharacteristics) const
bool  CoreDisplay::IOPCapabilities::SupportsLinearization(uint32_t, bool) const
bool  CoreDisplay::IOPCapabilities::IsLinearizationTableBased(uint32_t) const
bool  CoreDisplay::IOPCapabilities::SupportsCSC(uint32_t, bool) const
bool  CoreDisplay::IOPCapabilities::CanCSC() const
std::tuple<uint32_t,uint64_t> CoreDisplay::IOPCapabilities::GammaChannelTableEntryCountAndType() const
std::tuple<uint32_t,uint64_t> CoreDisplay::IOPCapabilities::LinearizationChannelTableEntryCountAndType(uint32_t) const
uint64_t CoreDisplay::IOPCapabilities::PlaneCount() const
std::vector<uint32_t> CoreDisplay::IOPCapabilities::GetPlaneIndices() const
```

and the object is populated by a "capabilities from plane" routine that CoreDisplay logs about:

**[log]** `WindowServer: (CoreDisplay) [ERROR] - Attempting to get capabilities from plane with no gamma`
— 38 occurrences in the last 3 h, for our display. (The same routine has a sibling message
`Attempting to get capabilities when none exist` next to `IOPCapabilities::PlaneCount()`.)

**[dis]** The consumer side of the same 4-bit field is visible elsewhere:
`test byte [rcx+0xea], 0x1e` (`sub_50772+0x81`) — i.e. bits 1-4 of `+0xea` are a **4-way HDR/EOTF
capability mask**, and our predicate wants **bit 1**.

So gate (2) reads: *"does this plane expose a gamma / linearization table (and hence an EOTF
programmable path) on the framebuffer?"* For a display with none, CoreDisplay never sets the bit, and
`SLSDisplaySupportsHDRMode` is `False` — **regardless of EDID, of 10 bpc being offered, of `'hdrm'`, and
of the mode table's colour fields.** **[inf]** on the "hence an EOTF" wording; the *fact* that the bit
is 0 and that the plane reports no gamma is **[log]**, the field identity `+0xea bit 1` is **[dis]**.

Whether we fail because `record->p128 == NULL` or because `p128` exists with `+0xea == 0` cannot be
read from outside WindowServer — **both** produce `r13 = 0` in the same instruction. The distinguishing
experiment is in §5 (**H2**).

### 1.5 The mode-install path confirms the same story, independently

`InstallTiming` (CoreDisplay `+0x24398`, unslid `0x7FF80537F398`, `__func__` = `'InstallTiming'`) is
called when CoreDisplay installs a timing/mode on the connect record, and it **starts with the same two
lookups as the predicate** (**[dis]**):

```
0x7FF80537F3C0   mov rax,[rdi+0x128]      ; IOPCapabilities*
0x7FF80537F3C7   test rax,rax ; je  → 0x…F49A          ; no caps → legacy path
0x7FF80537F3D0   cmp word [rax+0xea], 0 ; je  → 0x…F49A ; caps word zero → legacy path
0x7FF80537F3DE   rdi = [r14+0x70]         ; the display's *preferences* dictionary
                 CFDictionaryGetValue(prefs, CFSTR("hdr-policy")) → get bytes
0x7FF80537F45E   log "%s: hdr-policy = 0x%x"
0x7FF80537F469   al = policy & 1 ; if set → log "%s: Disable HDR" and skip
```

**[log] The `hdr-policy` line never appears on this machine** (0 occurrences in 3 h, vs. 115 mode
installs). Because the only way to skip that log is `p128 == NULL` or `word[p128+0xea] == 0`, this is a
second, independent runtime confirmation that **`IOPCapabilities` is empty for our display**.

What the legacy path then does, verbatim from the log (**[log]**, 115×):

```
InstallTiming - legacyRng_legacyEnc_legacyBit_legacyClr FAILED for mode 0x80001000 (640x480 to 640x480@60.0)
    for 0x1000004cb with code 0xe00002c7 ()
```
`0xe00002c7` = `kIOReturnUnsupported`. And in `InstallTimingImpl` (`+0x24B8F`, `0x7FF80537FB8F`):

```
InstallTimingImpl: driverSupportsMAT=N overridesDisabled=Y vrrNotEnabled=Y refreshRate30or60=Y
    refresh rate=59.952381 supportedSignalConfigs=0 timing=0
```

`legacyRng_legacyEnc_legacyBit_legacyClr` is CoreDisplay's own summary of **which capability query
answered, and it answered the legacy way, for all four**: timing **R**ange, pixel **Enc**oding,
**Bit**s-per-colour, **C**o**l**orimetry. `supportedSignalConfigs=0` = our framebuffer advertises no
signal-configuration capability at all. **[dis]** the fields these four flags name exist verbatim as
strings in CoreDisplay: `supportedDynamicRangeModes`, `supportedColorimetryModes`,
`supportedBitsPerColorComponent`, `supportedPixelEncoding` (the `IODisplayTimingRangeV2` field names)
and `dynamicRange`, `bitsPerColorComponent`, `colorimetry` (the `IODetailedTimingInformationV2` field
names).

Everything above agrees with `/home/Celia/rdna4/a4/docs/hdr-30bpp-plan.md` §3 ("HDR is a mode property,
not a depth") and with the measured `'colr'` refusal `0xe00002c7` in §A2 — same status code, same
mechanism.

### 1.6 What is *not* part of the chain (verified negatives, so nobody spends time there)

* **The kernel.** No in-kernel HDR advertisement is consulted by the verdict; the KC's `'hdrm'`
  64-byte sink is a dead end for *support* (unchanged from the earlier conclusion). **[inf]** from the
  fact that the entire chain above is userspace and terminates in `IOPCapabilities`.
* **`'hdrm'` / `kIOFBHDRMetaDataAttribute`.** Never sent for us (measured earlier, §A2) and not part of
  `sub_51B7E`. **[dis]**
* **The depth / 30bpp path.** `sub_51B7E` contains no depth or bpc term. `kDepthMode2` being accepted
  and switched to (measured in §A3) does not feed this verdict. **[dis]**
* **The EDID already has the HDR block** — parsed every boot (§1.3). **[log]**
* **`force8BPC`** is `NO` for our display, so the "force 8 bpc" override is not what blocks us. **[log]**
  (`get_display_device_info: Display 0x1b080500 force8BPC=NO isBDW=YES isSKL=NO isCNL=YES isAcc=YES
  isMUX=NO isBuiltIn=NO isMobile=YES isSkylakeMacBook=NO` — note the Intel generation probes answering
  `YES` on an AMD GPU; **[hyp]** that classification is fooled by whatever `get_display_device_info`
  matches on, but it does not gate HDR — the log line itself shows no refusal.)

---

## 2. Whitelists / blacklists: can a non-Apple TCL panel pass at all?

**Yes — the only vendor table in the verdict is an Apple-only exclusion list.** **[dis]**
(`sub_51B7E` tail, `0x7FF8053ACC8F` onward):

```c
if (!d->conn[0x68])                 return true;      // 0x…ACC95
rcx = d->conn[0x68]->p[0x10];
if (dword[rcx+0x11c] != 0x0610)     return true;      // 0x…ACCA3  ← vendor != Apple ⇒ PASS
uint32_t prod = dword[rcx+0x120];                     // 0x…ACCAA
// prod - 0xAE21 in [0,14]:  mask 0x7007 >> (prod-0xAE21) → if bit set, return FALSE
//                          (i.e. 0xAE21..0xAE23 and 0xAE2D..0xAE2F are excluded)
// prod & ~7 == 0xAE38  →  return TRUE                   0xAE38..0xAE3F
// prod & ~1 == 0xAE42  →  return TRUE                   0xAE42..0xAE43
// otherwise fall through to more checks at 0x…ACD0D
```

* `0x0610` is the EDID manufacturer id of **"APP" (Apple)**; `[rcx+0x11c]`/`[rcx+0x120]` are the
  vendor and product code fields of the parsed EDID. **[inf]** (high confidence: the two fields are
  dwords 4 bytes apart, compared against the APP id and against a contiguous Apple product-id range).
* The table's effect is `return false` for a handful of *Apple* product ids — a **blacklist of Apple
  displays that must not offer HDR**, not a whitelist of ones that may.
* A **TCL 32X3B** (`EDID vendor ≠ 0x0610`) hits `0x…ACCA3` and returns `true` — i.e. **for us this
  table is skipped**; it can never be the reason for `False`.

Other per-display controls found (not vendor lists, all of them *opt-outs*):

| key | where | effect |
|---|---|---|
| `hdr-policy` | per-display preferences dict at `[CGXDisplayDevice+0x70]`, read in `InstallTiming` (**[dis]**) | bit 0 ⇒ log `Disable HDR`, skip HDR entirely. **Currently absent** (`hdr-policy` never logged, **[log]**) — so it is not blocking us. |
| `force8BPC`, `forcePipeForHDR`, `enableHDR10`, `nominalNits2084`, `m_force2084Mode` | CoreDisplay cstring/defaults (**[inf]**) | CoreDisplay-side debug overrides. Untested; they can only short-circuit *after* the capability gates. |
| `kCDDisplayPresetBT2020CapableKey`, `kCDDisplayPresetMaxHDRLuminanceKey` | exported data from CoreDisplay (**[dis]**) | display *preset* descriptions (colour preset dictionaries), not support gates. |

---

## 3. Minimum driver-side change set that would flip the verdict

Ordered by what the evidence says the OS actually reads. "Necessary" = the predicate reads it;
"sufficient" = I can show it is enough (I cannot, for anything below).

### 3.1 Necessary, and already done — do not spend time here

1. **CTA-861.3 HDR static metadata block with EOTF bit 2 (PQ)** in the EDID we serve.
   Already shipped; `ParseCEAExtendedHDR10DataBlock, block with length 6` proves the OS parsed it. **[log]**
2. Colourimetry block advertising BT.2020 (`05 c0 01`, length 3) — parsed every boot. **[log]**
3. `IODisplayEDID` being present and readable as 384 B (base + CTA + DisplayID) — unchanged requirement.

### 3.2 Necessary, and missing — `IOPCapabilities +0xea` bit 1 (the plane/pipe capability word)

This is *the* failing term. It is not fed by the EDID and not by NDRV mode-table colour fields: it is
fed by the **plane's gamma / linearization** capability, which CoreDisplay reads through
`IOFramebuffer`. Concretely:

4. **Give the framebuffer plane a real gamma (degamma) table.** The OS currently reports
   `Attempting to get capabilities from plane with no gamma`. For an `IONDRVFramebuffer` that means the
   NDRV gamma entry points (`cscGetEntries` / `cscSetEntries`, `VDGetGammaListRec`/`VDSetGammaListRec`)
   plus whatever `IOFramebuffer` exposes as the per-plane gamma channel table
   (`CoreDisplay::IOPCapabilities::GammaChannelTableEntryCountAndType()` /
   `LinearizationChannelTableEntryCountAndType(plane)` must become non-zero, and
   `IsLinearizationTableBased(plane)` true). **[inf]** on which exact IOFB attribute is read — it is
   `IOFramebuffer`'s gamma-table API, not an NDRV record; the experiment in §5 (**H1**) names it in one
   boot by logging what the OS asks for.
5. **Publish a modern timing range**: `kIOFBTimingRangeKey` on the framebuffer with an
   `IODisplayTimingRangeV2` whose `supportedPixelEncoding = kIORangePixelEncodingRGB444`,
   `supportedBitsPerColorComponent |= kIORangeBitsPerColorComponent10`,
   `supportedColorimetryModes |= BT2020|BT2100`,
   `supportedDynamicRangeModes |= kIORangeDynamicRangeHDR10`.
   This is exactly NVIDIA's mechanism (`ref/nvidia-macos-driver` `nvrm-fb.cpp:4454-4461`) and it is what
   the four `legacy*` flags in `InstallTiming - legacyRng_legacyEnc_legacyBit_legacyClr` are about.
   **Note:** the earlier belief that this property panics the machine is **wrong** — AGENTS.md §一
   (04:40 correction) records that the panic was our own `rdna4Log("%s", 25000000)` format bug, and the
   kernel never read the property. So this option is open again, with the logging bug fixed and with a
   reversible arming mechanism.
6. **Make each mode's `IODetailedTimingInformationV2` carry the colour fields** —
   `bitsPerColorComponent = 10`, `colorimetry = BT2020|BT2100`,
   `dynamicRange = kIODynamicRangeHDR10` — via `getTimingInfoForDisplayMode` (the plugin already knows
   how to route `IONDRVFramebuffer` virtuals; NDRV siid 0x12 is a pure relay, `0x1475dfc4`).
   Needed for gate (4) ("the HDR10 mode must differ from the SDR mode") and for the four
   `supported*` queries to be believable *per mode*.

### 3.3 Optional / later

7. `'hdrm'` (`kIOFBHDRMetaDataAttribute`) as a 64-byte sink — cosmetic for the verdict; CoreDisplay may
   send it once HDR is *on*. Not required to flip `SupportsHDRMode`. **[dis]**
8. Program the DCN pipe for ARGB2101010 + HDR infoframe — required for a *correct picture*, not for the
   verdict.

### 3.4 Necessary vs. sufficient

| item | necessary? | proven sufficient? |
|---|---|---|
| EDID HDR block + PQ | yes (gate 3) — **done** | not sufficient alone (measured: done, verdict still `False`) |
| plane gamma/linearization capability (gate 2) | **yes** | unknown; if it is the only missing term, yes |
| modern `kIOFBTimingRangeKey` (V2, four `supported*` fields) | probably (it is the source of the four `legacy*` flags) | no — and it may be *downstream* of item 4: CoreDisplay may refuse to consider a modern range while the plane has no linearization |
| distinct HDR10 mode with colour fields | yes for gate (4) | no — gate (4) can only be reached after 2 and 3 pass |
| `'hdrm'` | no | — |

**The honest statement:** gate (2) must go from 0 to 1; nothing else on the list is provably the
blocker, because gates (2) and (3) short-circuit gate (4). I would not claim "HDR will work" after
items 4–6 — only "the verdict has a chance to flip", which the §5 measurement decides per step.

---

## 4. Measurement procedure (re-run after each change)

### 4.1 The verdict itself — must be run **inside** the GUI session

Put this in a file on the Mac (it is `/tmp/hdrprobe.py` right now) and paste-run it in **Terminal.app in
the logged-in desktop session** (that is how the `0x1b080500` number in the plan doc was obtained):

```python
import ctypes
from ctypes import c_uint32, c_bool, byref
cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
cg.CGGetOnlineDisplayList.argtypes=[c_uint32, ctypes.POINTER(c_uint32), ctypes.POINTER(c_uint32)]
cg.CGMainDisplayID.restype=c_uint32
sl = ctypes.CDLL("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight")
for n in ("SLSDisplaySupportsHDRMode","SLSDisplayIsHDRModeEnabled"):
    f=getattr(sl,n); f.argtypes=[c_uint32]; f.restype=c_bool
cd = ctypes.CDLL("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay")
for n in ("CoreDisplay_Display_SupportsHDRMode","CoreDisplay_Display_IsHDR10",
          "CoreDisplay_Display_IsHDRModeEnabled","CoreDisplay_Display_PreferHDR10"):
    f=getattr(cd,n); f.argtypes=[c_uint32]; f.restype=c_bool
ids=(c_uint32*8)(); n=c_uint32(0); cg.CGGetOnlineDisplayList(8, ids, byref(n))
print("CGMainDisplayID 0x%x ; online=%d" % (cg.CGMainDisplayID(), n.value))
for i in range(n.value):
    d=ids[i]
    print("0x%08x SLSsupports=%d SLSenabled=%d CDsupports=%d CDisHDR10=%d CDenabled=%d CDpreferHDR10=%d" % (
        d, bool(sl.SLSDisplaySupportsHDRMode(d)), bool(sl.SLSDisplayIsHDRModeEnabled(d)),
        bool(cd.CoreDisplay_Display_SupportsHDRMode(d)), bool(cd.CoreDisplay_Display_IsHDR10(d)),
        bool(cd.CoreDisplay_Display_IsHDRModeEnabled(d)), bool(cd.CoreDisplay_Display_PreferHDR10(d))))
```

`CDsupports` is the *same* predicate as `SLSsupports`, one layer lower — if they ever disagree, the
SkyLight RPC is the problem, not the display.

### 4.2 The false-negative signature (measured today, so you can recognise it)

Running the same script **over SSH** gives:

```
CGMainDisplayID 0x0 ; online=0
```

* plain `ssh` → `online=0`
* `sudo launchctl asuser $(id -u Celia) python3 /tmp/hdrprobe.py` → `online=0`
* `sudo launchctl bsexec $(pgrep -x WindowServer) python3 /tmp/hdrprobe.py` → `online=0`

So **`online == 0` or `CGMainDisplayID == 0` means the measurement is void** — there is no CG
connection, so "no displays" is not "HDR not supported". A real negative is `online >= 1` with
`SLSsupports=0`. If you see a line with a display id and all-zero flags, that is the real answer.

### 4.3 The SSH-safe proxy measurement (works today, no session needed)

The same objects the predicate reads are logged by `InstallTiming`, so these three greps are an
over-the-wire proxy for gate (2). Baseline on the current driver (measured 04:20):

```bash
sudo log show --last 10m --info --debug --predicate 'process == "WindowServer"' > /tmp/ws.log
grep -c 'hdr-policy'                    /tmp/ws.log   # 0  → IOPCapabilities empty (gate 2 fails)
grep -c 'Disable HDR'                   /tmp/ws.log   # 0
grep -c 'plane with no gamma'           /tmp/ws.log   # 38 → plane has no gamma
grep -c 'InstallTiming - legacy'        /tmp/ws.log   # 115, all "legacyRng_legacyEnc_legacyBit_legacyClr"
grep -o 'InstallTiming - [a-zA-Z_]*'    /tmp/ws.log | sort | uniq -c
grep -c 'ParseCEAExtendedHDR10DataBlock' /tmp/ws.log  # >=1 → EDID half already works
```

**Success criteria for the next step, in this order:**

1. `plane with no gamma` count → **0**, and `logGammaTransferInfoForDevice` /
   `Pipe CSC [...]` lines appear for `0x1b080500` ⇒ gate (2) has a chance.
2. `hdr-policy = 0x0` line **appears** at all ⇒ `InstallTiming` got past `word[caps+0xea] == 0`, i.e.
   `IOPCapabilities` is non-empty. This is the single cleanest signal that the change landed.
3. `InstallTiming - ` names lose the `legacy*` prefix (or a non-`FAILED` install appears) ⇒ the range /
   encoding / bpc / colorimetry queries are answering the modern way.
4. Only then re-run §4.1 in the session and look for `SLSsupports=1`.

---

## 5. Experiments that would settle what is still open

**H1 — what exactly does CoreDisplay ask the plane for?** Route two more `IONDRVFramebuffer`/`IOFramebuffer`
virtuals in `src/plugin.cpp` (the pattern already exists for `getPixelInformation` /
`set/getAttributeForConnection`) and log every call with arguments:
`getGammaTableByIndex`, `setGammaTable`, `getAttributeForConnection(0, …)` for
`kIOFBGammaHeaderSize`/`kIOFBGammaWidth`/`kIOFBGammaCount`/`kIOFBCurrentPixelClock` and, above all,
`getTimingRanges`/`setAttribute(kIOFBTimingRangeKey)`. One boot names the query that produces
`Attempting to get capabilities from plane with no gamma`.

**H2 — is `p128` NULL or just `+0xea == 0`?** Two ways, both read-only-ish:
(a) if we answer a plausible gamma table and the `hdr-policy` line appears in the log, then `p128` was
already allocated and only its bits were missing; (b) if CoreDisplay's `IOPCapabilities` object is
built by `get_display_device_info` from the *framebuffer* (not the connect record), then the number of
`plane with no gamma` messages per boot should equal the plane count — compare with
`system_profiler SPDisplaysDataType` / the `IOFramebuffer` node's plane count.

**H3 — the modern timing range, safely.** With the `rdna4Log("%s", …)` bug fixed (AGENTS.md §一,
04:40) and a trial-boot arming variable, publish `kIOFBTimingRangeKey` (V2, 312 B) with the four
`supported*` fields and watch for (i) `hdr-policy` appearing, (ii) the `legacy*` names changing,
(iii) panic reports. This is the mechanism NVIDIA uses; the property was never proven harmful.

**H4 — the "different mode" gate.** Only meaningful once (2)/(3) pass: serve the HDR10 timing as a
*separate* mode id with `dynamicRange/colorimetry/bitsPerColorComponent` set, and check whether
`CDisHDR10` and `SLSsupports` both flip.

---

## 6. How this was obtained (so it can be repeated)

Scripts left on the Mac in `/tmp` (read-only w.r.t. the system; only `/tmp` was written):

| file | purpose |
|---|---|
| `/tmp/cachedump.py` | cache header + mapping table, dump an image's sections by address |
| `/tmp/cachedump2.py` | same + `LC_SYMTAB` local symbols + `LC_FUNCTION_STARTS` decode |
| `/tmp/cachesyms.py` | SkyLight local symbols + function starts (26 537 syms, 10 742 starts) |
| `/tmp/cdsec.py` | dump a cache image's section table as JSON (`/tmp/cdsec.json`) |
| `/tmp/cacherev.py` | file-offset → vmaddr reverse mapping (needed because `LC_SYMTAB.symoff` is a *file* offset) |
| `/tmp/hdrprobe.py` | the §4.1 measurement |
| `/tmp/hdrgate-ws.log` | 3 h of WindowServer log used for §1.3/§1.5 |

Offline analysis (this box, `/tmp/hdrgate-work/`): `sl_dis.py` / `cd_dis.py` (capstone disassembly with
symbolisation), `cd_xref.py` (string xrefs / call graph by walking `LC_FUNCTION_STARTS`, not a linear
sweep — a linear sweep desynchronises in this binary and silently loses instructions), `cd_callers.py`.

Tooling note for the next person: `dyld_info -exports <cache path>` prints **offsets from the image
base**, and `nm`/`otool` cannot open cache-only images at all.

---

## 7. Confidence summary

| Claim | Basis | Confidence |
|---|---|---|
| The verdict is userspace, computed in WindowServer by CoreDisplay | **[dis]** §1.1 | proven |
| `CGSIsHDRSupported` ≡ `_SLSIsHDRSupported` (same predicate as `SLSDisplaySupportsHDRMode`) | **[dis]** re-export table | proven |
| `sub_51B7E` is the predicate, and its terms are the four listed | **[dis]** | proven |
| Gate (3) (EDID HDR block + PQ) is satisfied for us | **[log]** parse lines | proven (parse ran); **[inf]** that the bits are set |
| Gate (1) (`dev+0x64` bit 7) is not what fails for us | no direct evidence | **hypothesis**, low likelihood |
| Gate (2) (`IOPCapabilities+0xea` bit 1) is 0 for us | **[dis]** field identity + **[log]** `hdr-policy` never logged, `plane with no gamma` | high |
| The plane capability comes from gamma/linearization tables on the framebuffer | **[log]** + **[dis]** `IOPCapabilities` method names | high (**[inf]** on the exact IOFB call) |
| No vendor/product whitelist blocks a non-Apple panel; the table is Apple-only exclusion | **[dis]** §2 | proven (control flow); **[inf]** on the "vendor/product" reading of the two dwords |
| `hdr-policy` per-display pref can disable HDR, currently unset | **[dis]** + **[log]** | proven |
| The minimal fix is a plane gamma capability + modern timing range + per-mode colour fields | synthesis of the above | **hypothesis**, testable with §4.3/§5 |

---

# §8 (follow-up, 2026-11-26 07:40–08:05): which value is zero at "plane with no gamma"

The lead implemented the NDRV gamma selectors (`cscGetGamma` 8, `cscGetGammaInfoList` 20,
`cscRetrieveGammaTable` 21, `cscSetGamma` 4) behind a trial option. The kernel accepted them
(`IONDRVFramebuffer::checkDriver` walked Get → List(FindFirst) → Retrieve(6156 B) → List(0) →
NoMoreTables) and the framebuffer publishes `IOFBGammaWidth=10`, `IOFBGammaCount=1024`,
`IOFBGammaHeaderSize=12` instead of the 8/256 fallback. **`plane with no gamma` still fires and
`hdr-policy` is still never logged.** This section says why, with the exact code.

Measurement note: this boot is the plain one (`rdna4-dispclk=1070` consumed by the trial mechanism),
so the framebuffer is back on the 8/256 fallback — measured `IOFBGammaWidth=8`, `IOFBGammaCount=256`,
`IOFBGammaHeaderSize=12` on 4 nodes, and `logGammaTransferInfoForDevice: Gamma values red: 0.000000 ...
0.501961 ... 1.000000` (0.501961 = 128/255 = the 256-entry 8-bit middle). The counts below are from
that boot, i.e. the gamma-table-present boot should be re-measured with `rdna4-gamma=1` (§8.6).

## 8.1 The zero is a **count**, not a null pointer

`CoreDisplay::IOPCapabilities::GammaChannelTableEntryCountAndType() const`
= CoreDisplay **+0x117D2**, unslid `0x7FF80536C7D2` (identified by its own `__func__` string, which
sits immediately before the message in `__cstring`). **[dis]**

```
0x7FF80536C7E6   lock(this+0x10)
0x7FF80536C7EB   rax = [this+8]            ; the sPipelineDictionary (see 6.2); NULL → "capabilities when none exist"
0x7FF80536C7F8   rax = [rax+0xB0]          ; the planelist object          (NO null check — it must exist)
0x7FF80536C7FF   rcx = [rax+0x90]          ; the plane array base          (NO null check)
0x7FF80536C806   rax = [rcx+0x88]          ; ★ plane[0].gammaEntryCount   ← THE VALUE
0x7FF80536C80D   test rax,rax ; je 0x7FF80536C9AB   ; zero → log "Attempting to get capabilities from plane with no gamma"
0x7FF80536C816   rcx = [rcx+0x90]          ; plane[0].gammaEntries (array)
0x7FF80536C81D   rcx += 0x30
0x7FF80536C96C   add rcx,0x38 ; dec rax ; jne 0x7FF80536C82C   ; walk `count` entries of 0x38 bytes
                 …picks the widest entry type among {16,12,10,8} bits (cmp rdx,0x10/0xc/0xa/8)
```

So the value tested at the print site is **`plane[0] + 0x88`, the number of gamma-channel-table entries
CoreDisplay was told this plane has**. It is zero. It is *not* a NULL pointer: `[dict+0xB0]` and
`[[dict+0xB0]+0x90]` are dereferenced unconditionally before the test, and we reach the log instead of
crashing — so **the planelist and plane[0] both exist, and only the gamma table is missing.** The log
message is `IOPCapabilities.cpp` line `0x2D8` (immediate at `0x7FF80536C98F`).

Adjacent fields of the same plane record (stride **0xA8**, **[dis]** from
`sub_11A04`/`sub_11CD2`):

| plane offset | meaning |
|---|---|
| `+0x10` | 64-bit capability mask (`test [plane+0x10], rax` in `sub_11CD2`) |
| `+0x78` / `+0x80` | (count, entries) pair used by `sub_11A04` — a *different* capability |
| **`+0x88` / `+0x90`** | **(entries, pointer) for the gamma channel table — our zero** |
| `+0xEA` (in `IOPCapabilities`, not the plane) | the 4-bit HDR/EOTF mask read by `sub_51B7E` gate 2 (§1.2) — `test byte [x+0xEA], 0x1E` in `sub_50772+0x81` |

The log's *sibling* messages name the other terms: `... plane with no 3-channel gamma table support`,
`... plane with no 3-channel linearization table support`, `... device with no planes`,
`... device without plane index %u`. **[dis]** On the measured boot: `device with no planes` = **0**,
`plane with no gamma` = **60** — i.e. planes exist, gamma does not. **[log]**

## 8.2 Who is supposed to fill it — and it is not the framebuffer

`plane[0]+0x88` is written by the code that parses `sPipelineDictionary`:

```
DisplayPipe ctor  sub_D470 (= CoreDisplay +0xD470, 0x7FF805368470)          [dis]
  0x7FF8053684A2  new(0x9B0)                      ; the pipe impl
  0x7FF8053685A6  new(0xB8) → zeroed, [obj+0xB0]=0 ; ★ the empty "sPipelineDictionary"
  0x7FF8053685FC  call LoadCapabilities(presentmentRef=[rbx+0x80], dict=[rbx+0x88])
  0x7FF80536860C  if (did not load) { release dict; [rbx+0x88] = NULL; }     ; then a long field init

LoadCapabilities = CoreDisplay +0x10CC9 (0x7FF80536BCC9)                    [dis]
  if (!dict)                                → log "LoadCapabilities" (IOPCapabilities.cpp:0x1B)
  rbx = alloc(presentmentRef)               ; 0x7FF80536BD0F's callee
  status = <external call>(presentmentRef, rbx, dict)    ; ★ THE PARSER, 0x7FF80536BD0F
  if (status != 0 && status != 0x2002)      → log "Failed to get capabilities: %s"
  if (dict->qword[0xA8] == 0)               → fail (IOPCapabilities.cpp:0x31)
  if ([[dict+0xB0]+0x88] == 0)              → log "Attempting to get capabilities from device with no planes" (:0x37)
  …
```

The external call at `0x7FF80536BD0F` is a stub into **`/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator`**.
Proof: CoreDisplay `dlopen`s that exact path (`__cstring` `0x7FF805472141`, at `0x7FF8053E8387`) and then
`dlsym`s **14** symbols from a relative-offset name table at `0x7FF805451E3C` (`__TEXT,__const`), storing
them in a global API table at `0x7FF843AD8380`; if any `dlsym` fails the whole table is abandoned and the
global `0x7FF843AD83F8` is still set (`…+0x3F1`). Decoded names (**[dis]**, offsets rel. to the table):

```
IOAccelDisplayPipeCreate              IOAccelDisplayPipeRelease
IOAccelDisplayPipeTransactionBegin    IOAccelDisplayPipeTransactionSetPlaneEmpty
IOAccelDisplayPipeTransactionSetPlaneIOSurface
IOAccelDisplayPipeTransactionSetOptions
IOAccelDisplayPipeTransactionEnd      IOAccelDisplayPipeTransactionWait
IOAccelDisplayPipeCreateSetVBLNotifyPort   IOAccelDisplayPipeRequestVBLNotifyCallback
IOAccelDisplayPlaneGammaTable         ← ★ the gamma query
IOAccelDisplayPipeTransactionSetPipePreGammaTable
IOAccelDisplayPipeTransactionSetPipePostGammaTable
IOAccelDisplayPipeTransactionSetPipeColorMatrix
```

**Conclusion: the plane capability word — including its gamma entry count — comes from the
accelerator's `IOAccelDisplayPipe`, through IOAccelerator.framework. The framebuffer/NDRV side cannot
fill it.**

## 8.3 Q1 — do `IONDRVFramebuffer`/`IOFramebuffer` gamma entry points answer this? **No.**

Kernel-collection inventory (`strings /home/Celia/kc/SystemKernelExtensions.kc`, **[dis]** on symbols):

| symbol | kind |
|---|---|
| `IONDRVFramebuffer::setGammaTable(uint32,uint32,uint32,void*)` (+ `…,bool)` variant) | **set** |
| `IOFramebuffer::setGammaTable(…)[,bool]`, `updateGammaTable(...)`, `saveGammaTables()`, `setCLUTWithEntries(...)`, `checkDeferredCLUTSet()`, `deferredCLUTSetTimer(...)` | all **set/update** |
| `IOFramebuffer::extSetGammaTable`, `extSetCLUTWithEntries`, `extSetColorConvertTable`, `extSetHibernateGammaTable` | user-client **setters** |
| `IODisplay::getGammaTableByIndex(unsigned int*,unsigned int*,unsigned int*,void**)` | the **only** "get … by index" in the whole KC — and it is on **IODisplay**, not on `IOFramebuffer`, and is not part of `IOFramebufferUserClient` |

* There is **no `getGammaTable`/`getGammaTableByIndex` on `IOFramebuffer` or `IONDRVFramebuffer`** in
  this build, and **no gamma getter in the IOFramebuffer user client** — so nothing we implement in
  `setGammaTable`/`cscGetGamma` can be *read back* by CoreDisplay through the framebuffer.
* CoreDisplay does not import any IODisplay gamma symbol either (its imports contain only
  `IOServiceMatching`/`IOServiceGetMatchingServices`/`IOConnectCall*` and `_kCGDisplayEDIDKey`); the only
  gamma API it uses is the dlsym'd IOAccelerator set above.
* Our gamma work is real but on a **different path**: the CLUT/colour-management path, which is what
  `CGXSetDisplayColorProfileAndTransfer` / "using last created GPU Gamma2 Table" (19 078 lines on this
  boot) / `logGammaTransferInfoForDevice` show. It will matter for actually *programming* a plane gamma
  once one exists (`PerformPlaneGammaTransaction`), not for the capability word.

## 8.4 Q2 — is the "plane" an accel plane? **Yes, and we already have the pipe but not the gamma objects.**

Registry census on this machine (read-only `ioreg`, **[log]**):

| class / key | count | note |
|---|---|---|
| `IOAccelerator` | 1 | our `RDNA4Accelerator`, id `0x1000005ab` |
| `IOAccelDisplayPipe` | **1** | an `IOAccelDisplayPipe` **exists** |
| `RDNA4DisplayPipe` | 4 | our subclass instances |
| `IOAccelDisplayPipeUserClient2` | 1 | created (child of `RDNA4Accelerator`) |
| `IOAccelerationUserClient` | 1 | |
| published capability | — | `"IOAccelDisplayPipeCapabilities" = {"DisplayPipeSupported"=Yes,"TransactionsSupported"=Yes}` |
| **`IOAccelDisplayPipeGammaTable2`** | **0** | ← the gamma table object CoreDisplay needs |
| **`IOAccelDisplayPipePreCSCLinearizationVID`** | **0** | ← the linearization object (`SupportsLinearization`) |
| **`IOAccelDisplayPipePostCSCGammaVID`** | **0** | |
| `GammaTable`, `Linearization` | 0, 0 | |

`Unable to find display pipe for IOFB service / regID: 0x390b / 0x1000004b1` appears **once** in the
log, for a different registry id than the one that is queried afterwards — i.e. not fatal for the display
we care about. The pipe-level capability dictionary is exactly the two keys above, and it contains **no
gamma/linearization entry**, which is precisely the missing field.

So the answer to Q2 is "yes — accel plane", and the gap is narrow: the pipe exists, its *plane gamma/
linearization objects* do not.

## 8.5 Q3 — is gamma only one of several inputs? **Yes, several — but all from the same dictionary.**

`IOPCapabilities` is one object parsed from one dictionary: per-plane pixel formats, CSC, EOTF,
linearization, gamma and box support are separate *(count, entries)* pairs in the same 0xA8-byte plane
record (`+0x10`, `+0x78/0x80`, `+0x88/0x90`, …). They are **not** fed by `kIOFBTimingRange`,
`kConnectionColorDepthsSupported`, `' bpc'` or `'colr'`: those belong to the *other* gate, the
`InstallTiming` legacy path (§1.5), which logs `legacyRng_legacyEnc_legacyBit_legacyClr` and fails with
`kIOReturnUnsupported`. Fixing the timing range without the plane capabilities will not move
`SLSDisplaySupportsHDRMode`; fixing the plane capabilities may still leave `InstallTiming` legacy.

## 8.6 Smallest change on our side

Everything needed is already in IOAcceleratorFamily2 (in the KC); we only have to create and attach the
objects, in the accelerator's display-pipe bring-up:

1. **Create a gamma table object for the plane**
   `IOAccelDisplayPipeGammaTable2` (`__ZN31IOAccelDisplayPipeGammaTable2*`), initialised from an
   `IOAccelDisplayPipeGammaTableArgs` (`__ZN32IOAccelDisplayPipeGammaTableArgs`), and attach it with
   `IOAccelDisplayPipe::set_current_plane_gamma_table(uint32 plane, IOAccelDisplayPipeGammaTable2*)`
   (`__ZN18IOAccelDisplayPipe29set_current_plane_gamma_tableEjP29IOAccelDisplayPipeGammaTable2`).
   The args carry the entry count / width / channel count — this is the number that must become
   non-zero at plane `+0x88`.
2. **Create the pre-CSC linearization object** —
   `IOAccelDisplayPipePreCSCLinearizationVID::withArgs(IOAccelDisplayPipePreCSCLinearizationArgs*)`
   (`__ZN40IOAccelDisplayPipePreCSCLinearizationVID8withArgsEP41IOAccelDisplayPipePreCSCLinearizationArgs`)
   and attach with `set_current_pipe_precsclinearization_vid(...)`; this is what
   `SupportsLinearization`/`IsLinearizationTableBased` ask for, and EOTF support is gated behind it.
3. **Hook the transaction** `_fSymFBCLASSPerformPlaneGammaTransaction`
   (`PerformPlaneGammaTransaction`, cf. `IGAccelDisplayPipe::PerformGammaTransaction` /
   `PerformPQTransaction`) so the pipe can program what it advertises; the user-client selectors already
   exist (`IOAccelDisplayPipeUserClient2::transactionSetPlaneGammaTable(IOAccelDisplayPipeGammaTableArgs*)`,
   `…transactionSetPipePreCSCLinearizationVID(…)`).
4. If `IOAccelDisplayPipe::copyCapabilities()` (`__ZN18IOAccelDisplayPipe16copyCapabilitiesEv`) is what
   builds the dictionary, its override in `RDNA4DisplayPipe` must include the per-plane gamma/
   linearization entries — **[hyp]**: I could not disassemble it (it lives in the KC, and this box has no
   KC Mach-O symbolizer wired up), so whether the dictionary is built from the attached objects or from
   registry children of the pipe is the one remaining unknown. The *class inventory* (0 gamma objects
   vs 1 pipe) plus the CoreDisplay parse is what makes this the prime suspect, not a proof.

Necessary vs sufficient, again honestly: creating the objects is **necessary** (the count must be
non-zero); it is **not proven sufficient** — `SupportsEOTF` may additionally require the linearization
object (item 2), the CSC/colour-matrix capability, and the still-unexplained mode comparison in
`sub_51B7E` (§1.2 gate 4).

**Cheapest next measurement** (no code change, one boot): arm `rdna4-gamma=1` and repeat §8.1/§8.4:
`plane with no gamma` must go to **0** *only if* the accel path also changed; with the framebuffer-only
change it will stay at ~60, which confirms this section. The discriminating counters are
`plane with no gamma`, `device with no planes`, `hdr-policy` (appears ⇔ `word[IOPCapabilities+0xEA] != 0`),
and `logGammaTransferInfoForDevice`'s middle value (0.501961 = 8-bit/256 fallback, 0.500489 = 1023/2 for a
1024-entry 10-bit table — that tells us whether the *other* gamma path consumed the new table).

## 8.7 What this changes in §3

§3.2 item 4 ("give the framebuffer plane a real gamma table") was **wrong about the channel**, and the
§7 line "the plane capability comes from gamma/linearization tables on the framebuffer" is hereby
corrected: the tables are needed, but they must be published **on the accelerator's
`IOAccelDisplayPipe`**, not on the NDRV/framebuffer gamma path. §3.2 items 5–6 (modern timing range,
per-mode colour fields) are unaffected — they are the *other* gate. Everything else in §1–§5 stands; the
"gamma" hypothesis was right about the *missing input* and wrong about the *wire*.
