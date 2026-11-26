# The accel plane gamma objects: what they are, and one trial option that makes them

Status: **reversed, implemented, built; not yet run on the card** (2026-11-26, after
`docs/hdr-userspace-gating.md` §8 diagnosed the gate). This file is written first and appended as the work
proceeds, so a killed session does not lose the findings.

Starting point, as §8 left it: CoreDisplay's `IOPCapabilities::GammaChannelTableEntryCountAndType()`
(`CoreDisplay+0x117D2`, image base `0x7FF80535B000`) reads `sPipelineDictionary -> planelist -> plane[0]+0x88`,
a **count**; it is zero on this machine (`Attempting to get capabilities from plane with no gamma`, 60× a
boot), and the dictionary is parsed from **IOAccelerator**. Our accelerator publishes `IOAccelDisplayPipe` = 1
and the two-key capability dictionary, but no `IOAccelDisplayPipeGammaTable2` /
`IOAccelDisplayPipePreCSCLinearizationVID` object exists.

All addresses are unslid vmaddrs in `/home/Celia/kc/SystemKernelExtensions.kc` (x86_64, macOS 26.6.2 / 25G83),
`com.apple.iokit.IOAcceleratorFamily2`. The parser is a copy of the earlier sessions'
`/tmp/trng-work2/{kc2,dis2}.py` + `/tmp/gamma-work/dis3.py` in `/tmp/accelg-work/` (`pip install --user
capstone`; the copies are unmodified). Vtable/fixup entries decode with the rule `hdr-gamma.md` §10 states
("low 36 bits, minus `0x40000000` when that bit is set") — verified against three entries whose targets are
known from the symbol table (`copyCapabilities`, `free`, `init`).

---

## 1. The reversed API

### 1.1 The two objects

Both are **`OSObject` subclasses, not `IOService`s**. Their vtables (0x146002e0 / 0x14600508, 0x130 bytes) are
identical to each other apart from four entries — slots 0/1 (the destructors), slot 7 (`getMetaClass`) and
**slot 18 (`free`)** — i.e. the classes add **no virtual at all except `free`**, and everything that matters
here (the factories, `init`, the getters) is a plain non-virtual member reachable only by name. That is also
why `ioreg` can never list them (§2.1).

| what | symbol | VA |
|---|---|---|
| `IOAccelDisplayPipeGammaTable2::withGammaTableData(IOAccelDisplayPipeGammaTableData *)` | `__ZN29IOAccelDisplayPipeGammaTable218withGammaTableDataEP32IOAccelDisplayPipeGammaTableData` | `0x145d11ee` |
| `IOAccelDisplayPipeGammaTable2::init(IOAccelDisplayPipeGammaTableData *)` | `__ZN29IOAccelDisplayPipeGammaTable24initEP32IOAccelDisplayPipeGammaTableData` | `0x145d1540` |
| `IOAccelDisplayPipeGammaTable2::free()` | `__ZN29IOAccelDisplayPipeGammaTable24freeEv` | `0x145d15a4` |
| `getTableMinValue` / `getTableMaxValue` / `getTableCount` / `getTableEntry(u,f*,f*,f*)` | `__ZNK29IOAccelDisplayPipeGammaTable2…` | `0x145d15de` / `0x145d15ee` / `0x145d15fe` / `0x145d160a` |
| `IOAccelDisplayPipePreCSCLinearizationVID::withArgs(IOAccelDisplayPipePreCSCLinearizationArgs *)` | `__ZN40IOAccelDisplayPipePreCSCLinearizationVID8withArgsEP41IOAccelDisplayPipePreCSCLinearizationArgs` | `0x145d1236` |
| `IOAccelDisplayPipePreCSCLinearizationVID::init(IOAccelDisplayPipePreCSCLinearizationArgs *)` | `__ZN40IOAccelDisplayPipePreCSCLinearizationVID4initEP41IOAccelDisplayPipePreCSCLinearizationArgs` | `0x145d1806` |
| `IOAccelDisplayPipePreCSCLinearizationVID::free()` | `__ZN40IOAccelDisplayPipePreCSCLinearizationVID4freeEv` | `0x145d189c` |
| `getPlane` / `getType` / `getFormat` / `getConfigurationID` | `__ZNK40IOAccelDisplayPipePreCSCLinearizationVID…` | `0x145d18d8` / `18e2` / `18ec` / `18f6` |
| `getMinimumValue` / `getMaximumValue` / `getSize` / `getData` | same | `0x145d1900` / `190c` / `1918` / `1922` |
| `IOAccelDisplayPipePostCSCGammaVID::withArgs` / `init` (not used) | `__ZN33IOAccelDisplayPipePostCSCGammaVID…` | `0x145d127e` / `0x145d1af8` |
| vtables | `__ZTV29IOAccelDisplayPipeGammaTable2` / `__ZTV40IOAccelDisplayPipePreCSCLinearizationVID` / `__ZTV33IOAccelDisplayPipePostCSCGammaVID` | `0x146002e0` / `0x14600508` / `0x14600730` |

`withGammaTableData` and `withArgs` are the family's factories, and their shape is identical
(`0x145d11ee`/`0x145d1236`):

```
rax = [gMetaClass + 0x88]        ; OSMetaClass::alloc (slot 17)
obj = alloc()
if (obj) { ok = obj->init(args); if (!ok) { obj->release(); obj = null } }
return obj
```

so a factory call ends with a reference of ours to release. Their argument is the *whole blob* — header plus
data — which `init` copies in one `memcpy`; nothing needs to stay alive after the call.

### 1.2 The argument structures

`IOAccelDisplayPipeGammaTableData` — what a gamma table object is made of (`init` at `0x145d1540`:
`size = 0x10 + count*0x10`, `count = data[0]`, `count > 0x20000` refused, then a copy of `size` bytes):

| offset | field | evidence |
|---|---|---|
| `+0x00` | `u32 count` | `mov r14d,[rsi]`; `shl r14,4; add r14,0x10`; `getTableCount` = `[blob+0]` |
| `+0x04` | **unknown** | never read by `init` (copied verbatim); not read by any getter |
| `+0x08` | `f32 minimumValue` | `getTableMinValue` = `[blob+8]` |
| `+0x0c` | `f32 maximumValue` | `getTableMaxValue` = `[blob+0xc]` |
| `+0x10 + i*0x10` | entry `i`: `f32 r, f32 g, f32 b, f32 unused` | `getTableEntry(u,f*,f*,f*)`: `[blob+u*16+0x10]`, `+0x14`, `+0x18`; `+0x1c` not read |

`IOAccelDisplayPipePreCSCLinearizationArgs` — what a linearization object is made of (`init` at
`0x145d1806`; the getters read the object's fields, the object copies the arg's):

| offset | field | object field | getter |
|---|---|---|---|
| `+0x00` | **unknown** | — | never read by `init` |
| `+0x04` | `u32 plane` | `+0x0c` | `getPlane` |
| `+0x08` | `u64 type` | `+0x10` | `getType` |
| `+0x10` | `u64 format` | `+0x18` | `getFormat` |
| `+0x18` | `u64 configurationID` | `+0x20` | `getConfigurationID` |
| `+0x20` | `f32 minimumValue` | `+0x28` | `getMinimumValue` |
| `+0x24` | `f32 maximumValue` | `+0x2c` | `getMaximumValue` |
| `+0x28` | `u64 size` | `+0x30` | `getSize` |
| `+0x30` | the data (`size` bytes) | `+0x38` (a copy) | `getData` |

(`init` does `[args+0x28]` bytes-allocation, `if (!p) return false`, then `memcpy(p, args+0x30, size)`; the
three `u64` reads are 8-byte moves, so the *arg* fields may be 32-bit with padding — the distinction cannot
change behaviour here and is left open.)

`IOAccelDisplayPipeGammaTableArgs` — what the **user client** takes (selector 5). `IOAccelDisplayPipe::
transaction_set_plane_gamma_table` at `0x145cd23a` walks the pipe's framebuffer resources comparing
`[resource+0x54]` with `args[0]`, then tail-calls `Transaction2::set_plane_gamma_table(args[1], args+8)`:

| offset | field |
|---|---|
| `+0x00` | `u32 framebuffer` (matched against the pipe's framebuffer resource `+0x54`) |
| `+0x04` | `u32 plane` |
| `+0x08` | the `IOAccelDisplayPipeGammaTableData` (header + entries) |

and `s_transaction_set_plane_gamma_table` (`0x145d1e9c`) enforces `inputLength >= 0x18` and
`count = input[8] <= 0x20000` and `inputLength >= 0x18 + count*0x10`. `Transaction2::set_plane_gamma_table`
(`0x145cd26e`) makes the object, refuses `plane > 1` (`kIOReturnBadArgument` `0xe00002c2`), stores it in the
transaction's own state at `[txn+0x38] + plane*0x70 + 0x40` (**taking the factory's reference, no retain**) and
sets dirty bit `4 << plane` in `[txn+0x48]`.

### 1.3 Where `IOAccelDisplayPipe` keeps them

`IOAccelDisplayPipe::init(accel, machine, fb, index)` (`0x145cb2ac`) stores: accel at `+0x88`, display machine
at `+0x90`, framebuffer at `+0x98`, framebuffer index at `+0x284`, and allocates the **per-plane state array at
`+0x240`** (null ⇒ `init` fails, so a pipe that exists always has one).

| what | where | setter (all non-virtual, all retain what they store, release what they replace) |
|---|---|---|
| plane *p*'s gamma table | `[pipe+0x240]` + `p*0x70` + `0x40` | `set_current_plane_gamma_table(u32 p, IOAccelDisplayPipeGammaTable2 *)` — `0x145cbf76` |
| pipe pre-gamma table | `[pipe+0x240] + 0x000` | `set_current_pipe_pregamma_table` — `0x145cbe1c` |
| pipe post-gamma table | `[pipe+0x240] + 0x008` | `set_current_pipe_postgamma_table` — `0x145cbe6c` |
| pipe pre-CSC linearization | `[pipe+0x240] + 0x158` | `set_current_pipe_precsclinearization_vid` — `0x145cbebe` |
| pipe post-CSC gamma | `[pipe+0x240] + 0x160` | (no symbol; `0x145cbf1a`) |
| getters | `getCurrentPlaneGammaTable(u)` `0x145d00e0`, `getCurrentPipePreCSCLinearizationVID()` `0x145d011a` | |

`set_current_plane_gamma_table` computes the slot with `imul plane, 0x70` and **does not bound-check the plane
index** — only plane 0 is safe without knowing the array's capacity. `IOAccelDisplayPipe::free()`
(`0x145cb9e8`) releases the four pipe-wide slots and then loops the plane records releasing `+0x40` (two
planes), so the pipe is the right owner of the last reference: attach (retain) then release ours.

### 1.4 The user client, and the dictionary the userspace parses

`IOAccelDisplayPipeUserClient2`'s selector table `sDisplayMethodDescs` (`0x14600950`, 14 entries of 24 bytes
`{function, scalarIn, structIn, scalarOut, structOut}`; `kIOUCVariableStructSize` = `0xffffffff`):

| sel | method | in | out |
|---|---|---|---|
| 0 | `setPipeIndex` | 1 scalar | 1 scalar |
| 1 | `getDisplayModeScaler` | — | 24 struct |
| 2 | `getCapabilitiesData` | — | variable |
| 3 | `requestNotify` | 24 struct | — |
| 4 | `transactionBegin` | — | 1 scalar |
| **5** | **`transactionSetPlaneGammaTable`** | variable (`>= 0x18 + count*0x10`) | — |
| 6 | `transactionSetPipePreGammaTable` | variable | — |
| 7 | `transactionSetPipePostGammaTable` | variable | — |
| 8 | `transactionEnd` | 280 struct | — |
| 9 | `transactionWait` | 12 struct | — |
| **10** | **`transactionSetPipePreCSCLinearizationVID`** | variable | — |
| 11 | `transactionSetPipePostCSCGammaVID` | variable | — |
| 12 | `copySurface` | 2 scalars | — |
| 13 | `triage` | — | variable |

`getCapabilitiesData(char *out, size_t *len)` (`0x145d2ab6`) is the *only* read path to userspace: it takes the
pipe's busy lock, `dict = pipe->copyCapabilities()` (vtable **slot 282**, `[vt+0x8d0]`), `OSSerialize`s it,
checks `*len`, copies and sets `*len`. So the userspace sees exactly `copyCapabilities()`'s dictionary.

And `IOAccelDisplayPipe::copyCapabilities()` (`0x145cc2cc`) is four instructions:

```
mov rdi, [rdi+0x88]                     ; ★ the *accelerator* ([pipe+0x88], from init)
mov rax, [rdi]
mov rax, [rax+0x2e8]                    ; IORegistryEntry::copyProperty(const char *, plane) — a virtual, slot 93
lea rsi, "IOAccelDisplayPipeCapabilities"
jmp rax
```

**It builds nothing: it returns the accelerator's `IOAccelDisplayPipeCapabilities` property verbatim** — the
same property `src/rdna4accel.cpp:986` already sets (`{"DisplayPipeSupported"=Yes,
"TransactionsSupported"=Yes}`).

---

## 2. Three findings that change §8's plan

### 2.1 The objects cannot appear in `ioreg` — the census row was not evidence of absence

`IOAccelDisplayPipeGammaTable2` and `IOAccelDisplayPipePreCSCLinearizationVID` are `OSObject`s (§1.1). Only
`IOService`s get a registry entry, so `ioreg -c IOAccelDisplayPipeGammaTable2` returns **0 with the objects
created**, and the `0` in `hdr-userspace-gating.md` §8.4's table does not distinguish "not created" from
"created". The acceptance criterion "`ioreg -c IOAccelDisplayPipeGammaTable2` goes 0 → 1" from the task
statement **cannot be met by any code**; §5 replaces it with two channels that do work (a ring-log line and a
registry property on the pipe, which *is* an `IOService`).

### 2.2 Apple publishes the same two-key dictionary — so the count is not in the dictionary

`AMDRadeonX6000_AMDGraphicsAccelerator::populateAccelConfig` (`0xbdbfa5c`, the accelerator for Apple's own
AMD GCN/RDNA parts) ends its capability block with exactly:

```
OSDictionary::withCapacity(2)
  "DisplayPipeSupported"   = kOSBooleanTrue
  "TransactionsSupported"  = kOSBooleanTrue
accel->setProperty("IOAccelDisplayPipeCapabilities", dict)
```

and **no driver in this kernel collection overrides `copyCapabilities()`**: `__ZN18IOAccelDisplayPipe16copyCapabilitiesEv`
is an **undefined import** (`n_type = 0x01` = `N_UNDF|N_EXT`, `n_value = 0`) in `AMDRadeonX4000`,
`AMDRadeonX5000`, `AMDRadeonX6000`, `AppleIntelKBLGraphics` and `AppleIntelICLGraphics`, and
`AppleParavirtDisplayPipe::copyCapabilities` (`0x142c1376`) is a defined stub that does nothing but jump to the
base's slot 282 (`mov rax, [__ZTV18IOAccelDisplayPipe + …]; jmp [rax+0x8e0]`). There is **no per-plane
capability key string anywhere in the kernel collection** (`Gamma`/`Linear`/`PreCSC`/`PlaneCapab`/`Supports*`
sweep over the whole file: the only dictionary-shaped capability strings are the two above).

So the base-class dictionary is byte-identical to ours on Apple hardware, and the per-plane gamma entry count
CoreDisplay reads must come from something else. Anything we add to that dictionary would be *invented* — and
an invented dictionary is parsed in **userspace**, where a wrong shape is a WindowServer failure, not a
kernel panic. That is why this change adds no dictionary keys (§3.3).

### 2.3 What is left, honestly

The remaining candidates for the count's wire, in the order the next boot will discriminate them:

1. **The objects themselves** (what this change builds): necessary, but note that inside this kernel collection they
   are **inert**: a call-site scan of IOAcceleratorFamily2's `__text` finds exactly one caller each for
   `set_current_plane_gamma_table` and `set_current_pipe_precsclinearization_vid` —
   `IOAccelDisplayPipeTransaction2::set_transaction_args` (`+0x5a9`, `+0x9d5`), i.e. the transaction path copying
   what the *userspace* supplied into the pipe's current state — and **zero callers** for
   `getCurrentPlaneGammaTable` / `getCurrentPipePreCSCLinearizationVID`. Nothing in the kernel reads a plane's gamma
   table back out. What this change produces is therefore the same pipe state a userspace
   `transactionSetPlaneGammaTable` would, and whether that is enough depends on code outside the kernel (2, 3).
2. **IOAccelerator.framework's parser** (userspace, `IOAccelerator.framework/IOAccelerator`): it is in the dyld
   shared cache on this machine (the on-disk framework is a skeleton, no binary), so it could not be
   disassembled offline here; it may query the pipe through a selector we consider a write
   (`transactionSetPlaneGammaTable`, i.e. CoreDisplay *offering* a table is what makes the plane "have gamma")
   or through presentment/AGDC data. The dlsym'd name `IOAccelDisplayPlaneGammaTable` (§8.2's 14) is the
   userspace function CoreDisplay uses for this query, and its implementation is the thing to read next.
3. **AGDC** (`AppleGraphicsDeviceControl`, our `RDNA4DeviceControl`): the Intel graphics kext in this same KC
   carries `AGDCLinearizationType_t` and `AGDCFBLinearizationSegmentEntry_t`, i.e. plane linearization is an
   AGDC-visible concept on some Apple drivers.

§6 says how each is settled.

---

## 3. What was implemented

### 3.1 Files

| file | change |
|---|---|
| `src/rdna4accel.cpp` | four new solved symbols (the two factories, the two setters); the option read once in `accelTo`; `RDNA4DisplayPipe::gammaAttach()` called from `RDNA4DisplayPipe::init()` after the family's |
| `src/ioaccel.hpp` | a hand-added block at the end: `IOAccelDisplayPipeGammaTableData`, `IOAccelGammaTableEntry`, `IOAccelDisplayPipeGammaTableArgs`, `IOAccelDisplayPipePreCSCLinearizationArgs` + `static_assert`s on their sizes. **`tools/gen/accelgen.py` rewrites this file wholesale** — the block must be kept (or moved into the generator) the next time it is run |
| `docs/hdr-accel-gamma.md` | this file |

`src/accelcensus.cpp` and `src/kext.mk` were **not** touched: no new source file, no new census hook.

Nothing is virtual-indexed by hand: `setProperty`/`release` are ordinary SDK virtual calls, and the four
family functions are called through solved addresses with the family's own register convention
(`this` in `rdi`, so `void (*)(IOAccelDisplayPipe *, uint32_t, OSObject *)`).

### 3.2 The option

`rdna4-accelgamma`, `Trial::option` semantics (`src/trial.cpp`: a boot-arg of that name, or `armed
rdna4-accelgamma=1` in the `rdna4-trial` NVRAM variable, which is written back as `used …` before it is acted
on, so a boot that dies loses it).

* **Default off**: it is not in `trial.cpp`'s `builtIn` table, so an unarmed boot reads `gAccelGammaKind = 0`,
  `gammaAttach()` returns before allocating or logging anything, and no property is set — the boot is
  byte-identical to today's.
* `=1`: **1024 entries of 10 bits**. Chosen as the default because the framebuffer's own gamma description on
  this machine is `IOFBGammaWidth=10, IOFBGammaCount=1024` (`hdr-gamma.md`), so both gamma wires then agree.
* `=2`: **256 entries of 12 bits** — the *wider* type, which is the one CoreDisplay's
  `GammaChannelTableEntryCountAndType()` walk prefers when it has several (`cmp rdx,0x10/0xc/0xa/8` in
  `hdr-userspace-gating.md` §8.1) and what Apple's IGAccel pipe publishes for its PQ/2084 LUTs
  (`Pipe12bit_2084_LinearizationGammaLUT` in the KC). **Caveat, stated because it undercuts the choice**: the
  object stores *floats*, so the bit width is not carried by the object at all — it can only ever be a hint in
  the log. Both kinds therefore give the same count, which is the value the gate tests; the two exist so the
  hardware boot can show whether anything downstream cares.

Both ramps are the identity (`i/(count-1)` for R=G=B, min 0, max 1); the linearization is a 256-entry 16-bit
identity ramp with `plane = 0`, `type = format = configurationID = 0` (**placeholders**: nothing in the kernel
interprets them, and we have no ground truth for them — §6).

### 3.3 The order of operations (why a failure cannot leave a half state)

1. `state = [pipe+0x240]`; if null, or any of the four symbols was not found → log, **return** (nothing made).
2. Make the gamma table object (factory; the blob is freed after the factory copied it) — no attach yet.
3. Make the linearization object the same way.
4. If **either** factory failed → log which, release what exists, **return** (nothing attached).
5. Attach both (`set_current_plane_gamma_table(this, 0, gamma)`, then
   `set_current_pipe_precsclinearization_vid(this, lin)`), then release our two references (the setters
   retained; the pipe's `free()` releases them — §1.3).
6. Read the two slots back out of the pipe's state and log **one line**: counts, both object pointers, both
   slot pointers, and a verdict string. The slots are set as a registry property `RDNA4AccelGamma` on the pipe
   (an `IOService`, so `ioreg` can see it).

The read-back at step 6 is also the self-check on the state offsets: if `[state+0x40]`/`[state+0x158]` were
not where the setters write, the verdict would say `NOT attached` instead of claiming success.

### 3.4 The log line (the verdict)

```
AMDRadeonRDNA4: accel: pipe 0x… (framebuffer 0): rdna4-accelgamma=1: gamma table 0x… (1024x10, 16400 bytes),
linearization 0x… (560 bytes): plane 0 slot 0x…, linearization slot 0x…: 1024x10 gamma table on plane 0: attached;
pre-CSC linearization: attached
```

and on a failure one of

```
… rdna4-accelgamma=1: no plane state: nothing attached
… rdna4-accelgamma=1: a function the family does not have here: nothing attached
… rdna4-accelgamma=1: gamma table NOT made, pre-CSC linearization made: nothing attached
```

No `%s` is ever given a number (`rdna4Log` is `__attribute__((format(printf,1,2)))`, so the compiler refuses
it) — the rule the 2026-11-26 00:30 panic cycle cost us.

### 3.5 Build and test provenance

| what | result |
|---|---|
| kext, clean build on the Mac (`~/rdna4/a4-accelg`, `make -j2 kext`) | **exit 0**, `AMDRadeonRDNA4.kext for x86_64 (min macOS 11.0)`, UUID `52947B00-AF15-363C-88C7-3E2FFFA16853` |
| new warnings | **none**: 5 warning *lines* in the incremental log with this change, 5 without it (same two in `rdna4accel.cpp`, `-Wsign-conversion` at 310/336, pre-existing); the one new warning the first build produced (`kPipePlaneStride` unused) was fixed |
| the new code is in the binary | `strings` of the built kext contains the four mangled symbols, both verdict formats and the `RDNA4AccelGamma` property name |
| host tests (`make -j2 CXX=g++ CC=gcc test` here) | one failure, `navi44: gfx images: 0 built (mes header)` — the pre-existing one `hdr-gamma.md` §11 recorded as failing from the pristine tree (nothing in `test/` exercises `rdna4accel.cpp`; this box has no `clang++`, so `CXX` is overridden) |

---

## 4. Nothing was run on the card by this change

The option is off by default, and this session did not reboot the machine (the tree also carries another
agent's in-flight work: `docs/hdr-userspace-gating.md`, `src/dsc.hpp`).

---

## 5. Acceptance criteria for the hardware boot

One boot, in this order. **The criterion from the task statement is corrected**: `ioreg -c
IOAccelDisplayPipeGammaTable2` stays 0 by design (§2.1) and must not be used as the pass/fail signal.

```bash
# 0 (arm it; a boot-arg does the same)
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trial="armed rdna4-accelgamma=1"
#   … reboot, wait for the desktop …

# 1 the objects exist and the pipe holds them: one line per pipe (4 on this machine)
sudo sysctl -n debug.rdna4_log | grep 'rdna4-accelgamma'
#   expect: 4 lines, each ending "plane 0 slot 0x…, linearization slot 0x…: 1024x10 gamma table on plane 0: attached;
#   pre-CSC linearization: attached"; a failure line names which half failed and says "nothing attached"

# 2 the same verdict as a registry property (the pipe is an IOService; the objects are not)
ioreg -c RDNA4DisplayPipe -r -l | grep RDNA4AccelGamma
#   expect: "RDNA4AccelGamma" = "1024x10 gamma table on plane 0: attached; pre-CSC linearization: attached"

# 3 ★ the gate itself
sudo log show --last 10m --info --debug --predicate 'process == "WindowServer"' > /tmp/ws.log
grep -c 'plane with no gamma' /tmp/ws.log     # baseline 60 per boot
grep -c 'hdr-policy'          /tmp/ws.log     # baseline 0
```

Read the result like this:

| 1, 2 | 3 (`plane with no gamma`) | conclusion |
|---|---|---|
| attached | **0** | the objects are the wire; then `hdr-policy` (and `radna4-gamma`'s `logGammaTransferInfoForDevice` middle value, `0.500489` = a 1024-entry table vs `0.501961` = the 8-bit/256 fallback) say whether HDR opens |
| attached | still ~60 | the objects are necessary but **not** the wire: the count comes from the userspace parser or from AGDC (§2.3, §6) — do not stack more changes into this boot; the next measurement is #2 or #3 there |
| a failure line | — | the family is not the one this code was reversed against: keep the log line, do not retry the same boot |

Do not reboot with `rdna4-accelgamma=1` still armed and no evidence of a *reason* — the trial variable is
consumed on use, so the boot after a bad one is already clean.

## 6. What is not verified (and how it would be settled)

| claim | status | how to settle it |
|---|---|---|
| the four mangled names exist in the **running** IOAcceleratorFamily2 | **[inf]** — they are in this machine's KC symtab | boot with the option; the "a function the family does not have here" line appears if not |
| the state offsets `+0x240 / 0x70 / 0x40 / 0x158` are the running ones | **[inf]** twice from the KC (`set_current_*`, `free()`), and self-checked at run time by the read-back. The slot written is also one the family itself writes: its own `set_transaction_args` calls `set_current_plane_gamma_table` with planes 0 and 1 on the transaction path, so plane 0's record always exists | acceptance 1/2 |
| the factories accept our blobs (16 400 B / 560 B) and return objects | **[inf]** from `init`; never run | acceptance 1 |
| a gamma table object with a nonzero count makes CoreDisplay count it | **the open question** — §2.2 says the *dictionary* cannot carry it, and §2.3 lists the alternatives | acceptance 3 |
| `IOAccelDisplayPipeGammaTableData+0x04` and `PreCSCLinearizationArgs+0x00` | **unknown** — never read by any kernel code we found | a driver that sets them (IGAccel) or a later measurement; nothing here depends on them (zero) |
| `type/format/configurationID` of the linearization object | **placeholder 0** — no ground truth | disassemble how a real pipe fills them (`IGAccelDisplayPipe::PerformGammaTransaction` / `AGDCFBLinearizationSegmentEntry_t` in AppleIntelKBL/ICLGraphics) |
| whether an override of `copyCapabilities()` (slot 282) with per-plane entries would be read by IOAccelerator.framework — and with which keys | **unknown**: the framework's binary is in the dyld shared cache, so it was not reversed | extract the cache image on the Mac (`dyld_shared_cache_util`/`jtool`-class tool) and disassemble the parser; or boot with a *guessed* dictionary only if a WindowServer crash is acceptable that boot |
| the DCN is unaffected | **[inf]** — the objects are data the family copies; our `performTransaction` does not read them and the plane gamma is not programmed by this change | the desktop still scans out in acceptance 1 |

## 7. Rollback

There is no sysctl: the option is read once, at `accelTo`, and it is a **trial**, so

* **the next boot without the trial is already clean** (the variable wrote itself back as `used …` when it was
  read), and
* to stop using it from a boot-arg, drop `rdna4-accelgamma=1` from `boot-args` and reboot.

Nothing else in the driver changed behaviour: with the option off the four symbols are still solved (harmless
lookups), `gammaAttach()` returns on its first line, and no property, object or log line is added — verified by
the "no new warnings / byte-identical boot path" check in §3.5 rather than by a boot.

If a boot with the option armed hangs or panics, read the panic report first
(`ls -t /Library/Logs/DiagnosticReports/Kernel-*.panic`, `macOSPanicString`): the ring log dies with the
panic, the properties do not (AGENTS.md §一). A `rdna4Log`/`_vsnprintf` frame in the backtrace would mean a
format-string mistake, and the two verdict formats above are the only new ones (no `%s` takes a number).

## 8. How to re-check the kernel side

```bash
pip install --user capstone
mkdir -p /tmp/accelg-work && cp /tmp/trng-work2/{kc2,dis2}.py /tmp/gamma-work/dis3.py /tmp/accelg-work/
cd /tmp/accelg-work && python3 - <<'PY'
import kc2, dis3
im = kc2.by_eid('IOAcceleratorFamily')[0]
dis3.show(im, 0x145d1540, 240, 'IOAccelDisplayPipeGammaTable2::init')          # the gamma table's data layout
dis3.show(im, 0x145d1806, 260, 'PreCSCLinearizationVID::init')                 # the linearization's arg layout
dis3.show(im, 0x145cbf76, 200, 'set_current_plane_gamma_table')                # [pipe+0x240]+plane*0x70+0x40, retain/release
dis3.show(im, 0x145cbebe, 200, 'set_current_pipe_precsclinearization_vid')      # [pipe+0x240]+0x158
dis3.show(im, 0x145cc2cc, 64,  'copyCapabilities')                             # [pipe+0x88]->copyProperty("IOAccelDisplayPipeCapabilities")
dis3.show(im, 0x145d2ab6, 400, 'UserClient2::getCapabilitiesData')             # the only read path to userspace
PY
```

The selector table is `__ZN29IOAccelDisplayPipeUserClient219sDisplayMethodDescsE` at `0x14600950`, 14 entries of
24 bytes; decode entries with `v = raw & 0xFFFFFFFFF; if (v & 0x40000000) v -= 0x40000000`. The pipe's own
vtable is `__ZTV18IOAccelDisplayPipe` at `0x145ff5d8` (slot 282 = `[+0x8d0]` = `copyCapabilities` at
`0x145cc2cc`).
