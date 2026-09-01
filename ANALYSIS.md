# Where NVIDIA gates Multi Frame Generation

Reverse engineering notes on `nvngx_dlssg.dll` (DLSS-G "MFGLW") and the
Streamline plugin that drives it, aimed at running 3x/4x frame generation on
Ada (RTX 40) instead of only 2x.

Binaries examined:

| file | version | notes |
| --- | --- | --- |
| `nvngx_dlssg.dll` | 310.7.129 | standalone download |
| `nvngx_dlssg.dll` | 310.8.0.0 | shipped in GTA V Enhanced |
| `sl.dlss_g.dll` | 2.13.0-beta10 | Streamline plugin |
| `sl.common.dll` | 2.13.0 | Streamline core |

---

## 1. The chain

```
game  --slDLSSGSetOptions(numFramesToGenerate)-->  sl.dlss_g.dll
sl.dlss_g  --DLSSG.MultiFrameCount / MultiFrameIndex-->  nvngx_dlssg.dll (NGX snippet)
nvngx_dlssg  --> CUDA kernels
```

Each generated frame is a separate `EvaluateFeature` call carrying
`MultiFrameIndex = 1..N`.  `sl.dlss_g` owns the swapchain and presents them.

## 2. Every architecture check lives in the snippet

`sl.dlss_g.dll` has **no** architecture test for multi-frame.  It asks the
snippet what it can do and takes the smaller of that and its own cap of 5:

```
0x58285  Get("DLSSG.MultiFrameCountMax") -> ecx
0x582b0  edx = 5
0x582b5  cmp ecx, edx
0x582b7  cmovb edx, ecx          ; min(ngx_max, 5)
0x582c1  [plugin+0x45dc] = edx   ; numFramesToGenerateMax
```

That is not the last word on it. Immediately afterwards a driver-profile value
is folded in with a second minimum, logged as "DRS limits max generated frames
to %u":

```
0x5841c  cmp byte [plugin+0x44b4], 0     ; is the DRS value present
0x58439  edx = [plugin+0x45dc]
0x5843b  cmp [rax], edx
0x5843d  cmovb r8, rax                   ; min(current, DRS)
0x58444  [plugin+0x45dc] = ...
```

and if the NGX parameter is missing or zero, `0x58416` writes 1 outright. So the
effective maximum is `min(snippet, 5, DRS key 0x104D6667)`. On a machine with no
NVIDIA App override that key reads 0 and drops out — the snippet log confirms
`drsReadKey SUCCESS: id 104d6667, value 00000000` — but a profile that sets it
will cap the result no matter what the snippet says.

and later rejects the app's request against it:

```
0x56f74  r8d = options->numFramesToGenerate     (DLSSGOptions + 0x24)
0x56f78  cmp r8d, 1 ; jb   -> "must be greater than 0"
0x5702b  r9d = numFramesToGenerateMax
0x57032  cmp r8d, r9d ; ja -> "greater than DLSS-G supported numFramesToGenerateMax"
```

So the entire policy is in `nvngx_dlssg.dll`, in exactly two places.

### Gate 1 — the capability it advertises

`DLSSGInstanceManager::PopulateParameters`:

```
cmp  ebp, 0x1B0          ; ebp = NGX GPU arch enum, 0x1B0 = Blackwell GB20x
jl   .not_blackwell
mov  edi, 5              ; up to 5 generated frames (6x)
  ... DRS MULTI_FRAME_COUNT_LIMIT may clamp to 1..4 ...
.not_blackwell:
mov  edi, 1
.set:
Set("DLSSG.MultiFrameCountMax", edi)
```

### Gate 2 — the runtime check

`EndpointCore::Create`:

```
call [vtable+0x38]       ; GetGPUArchitecture()
cmp  eax, 0x1B0
setae al
mov  byte [this+0x28], al        ; m_multiFrameSupported
```

That byte has exactly one consumer, `EndpointCoreInputs::ComputeAndValidateTimeFactor`:

```
if (!m_multiFrameSupported) {
    if (count != 1) fail "Multi frame is not supported on this device. Found count (%d) but expected (1)";
    if (index != 1) fail "Multi frame is not supported on this device. Found index (%d) but expected (1)";
} else {
    max = 5; if (DRS limit in 1..4) max = limit;
    if (count > max) fail "input MultiFrameCount %d is greater than the maximum supported count (%d)";
}
timeFactor = (float)index / ((float)count + 1.0f);
```

The arch enum comes from a normaliser at `0x1a680` mapping NVAPI arch ids:
Volta `0x140`, Turing `0x160`, Ampere `0x170`, Hopper `0x180`, Ada `0x190`,
GB100 `0x1A0`, GB20x `0x1B0`.

## 3. Why this is a policy switch and not a hardware limit

Three independent pieces of evidence:

1. **The interpolation is parameterised by time, not by count.** The network is
   evaluated once per generated frame at `t = index / (count + 1)`. Generating
   three frames is the same kernel run three times at t = 1/4, 2/4, 3/4. There
   is no separate "MFG model".

2. **The snippet ships native Ada code.** Of the 70 fatbins in `.data`, 31 carry
   a compiled **`sm_89` cubin**; the sm_120 entries are PTX only. Ada is the
   architecture this build was compiled *for*.

   ```
   CUBIN/sm_89 x31   PTX/sm_120 x31   PTX/sm_89 x70
   ```

3. **The version resource says so:** `NGXGpuArchitecture = NVSDK_NGX_GPU_Arch_Ada`.

What Blackwell genuinely adds is hardware flip metering for pacing the extra
presents. That affects frame-time smoothness, not whether frames can be made.
Expect pacing to be the weak point, not image quality.

## 4. The patch

Both gates are `cmp <r32>, 0x1B0`. Rewriting the immediate to 0 makes the
comparison read "arch >= 0", true for every real architecture id, so whichever
way the compiler phrased the predicate the Blackwell branch is the one taken.

Four bytes of payload:

| build | rva | before | after |
| --- | --- | --- | --- |
| 310.6.0.0 | `0x1A026` | `cmp esi,0x1B0 ; cmovl` | `cmp esi,0 ; cmovl` |
| 310.6.0.0 | `0x33EFF` | `cmp eax,0x1B0 ; setae` | `cmp eax,0 ; setae` |
| 310.7.129 | `0x1A99C` | `cmp ebp,0x1B0 ; jl` | `cmp ebp,0 ; jl` |
| 310.7.129 | `0x39ECF` | `cmp eax,0x1B0 ; setae` | `cmp eax,0 ; setae` |
| 310.8.0.0 | `0x166F2` | `cmp ebp,0x1B0 ; jl` | `cmp ebp,0 ; jl` |
| 310.8.0.0 | `0x35BEF` | `cmp eax,0x1B0 ; setae` | `cmp eax,0 ; setae` |

Note 310.6.0.0: the same gate, compiled to `cmovl` rather than a branch. Which
form appears is a compiler decision, not a version one, so a patcher that keys
on a byte string finds one site and silently misses the other. `tools/mfg_unlock.py`
walks `.pdata` function ranges, disassembles, and takes every `cmp r32, 0x1B0`
whose first flag consumer is an *ordering* test — `jl`, `jb`, `jge`, `setae`,
`cmovl` and the rest. Equality tests are refused rather than patched, since
"arch >= 0" would not preserve their meaning, and it says so loudly when it does
not find exactly two sites.

### Does anything assume a count of 1?

This was the claim least supported by reasoning alone, so here is every read of
the two fields, found by scanning the whole disassembly for the offsets
`EndpointCoreInputs+0x4EC` (index) and `+0x4F0` (count):

| rva | what it does |
| --- | --- |
| `0x65732`, `0x65847`, `0x6589C`, `0x658DE` | the validation itself |
| `0x660D4`, `0x66108` | reading them out of the NGX parameters |
| `0x65921` | `timeFactor = index / (count + 1)` -> `+0x4F4` |
| `0x658FC` | `isMultiFrame = count > 1` -> `+0x4F8` |
| `0x658EF` | `isLastGenerated = index == count` -> `+0x4F9` |
| `0x3EF2A` | `count + 1` formatted into the on-screen indicator, `"- %dx"` |
| `0x3B63B` | copied into a telemetry struct |
| `0x5682D`-`0x56849` | index, count and timeFactor read together for a state dump |

`+0x4F4` is then consumed by three render paths (`0x3D895`, `0x41B32`,
`0x4495C`). That is the complete accounting: the count produces a float, two
booleans, a log line and a string. **Nothing is sized by it.**

Which follows from where it lives. The count arrives as a per-evaluate NGX
parameter, while every allocation happens in `CreateFeature`, whose parameter
parse reads only `Width`, `Height`, `InternalWidth`, `InternalHeight`,
`BackbufferFormat`, `DynamicResolution`, `UseReflexMatrices` and the node masks.
The one count-shaped thing it does read,
`DLSSG.NvAppOvrAppliedVal.MultiFrameCount`, is stored as an optional and only
echoed back for telemetry.

The `m_multiFrameSupported` byte at `EndpointCore+0x28` likewise has exactly one
reader, `0x3AF18`, which passes it to the validation. Other `[reg+0x28]` byte
tests in the binary belong to unrelated classes -- `0x79C62` loads its object
from `[rbx+8]` first.

### And the capability query?

`GetFeatureRequirements` is the other place that could plausibly hide a gate: it
queries device ID, architecture, OS build and driver version, and it runs before
any of the above. It does not gate multi-frame. The whole implementation,
`0x37280`-`0x37A97`, contains no architecture constant of the `0x1?0` form and no
multi-frame reference of any kind — it decides whether DLSS-G runs at all, which
on Ada it already does at 2x.

A note on method, since this kind of claim is only as good as the search behind
it: everything here is disassembled per function from `.pdata` ranges, never by
linear sweep over `.text`. A linear sweep desynchronises on the first jump table
and will miss real instructions.

## 5. What is still missing for old games

The patch makes the runtime *capable*. It does not make a game *ask*. A title
that shipped before DLSS 4 calls `slDLSSGSetOptions` with
`numFramesToGenerate = 1`, and Streamline will faithfully generate one frame.

`sl.dlss_g` reads DRS keys from the driver profile (`dlfgDrs.cpp`):

```
0x10C7D835  deny multi-frame count override
0x104D6667  max generated frames        -> min() against the app's request, a LIMIT not a force
0x10308298  DLSSG mode (0..4)
0x10CF4125  dynamic target frame rate
0x10A89C8E, 0x00A879CF, 0x10562D0F, 0x104399DC, 0x10ACFBC9
```

`0x104D6667` only clamps downwards, so the NVIDIA App override cannot raise a
game above what it requests either. The request has to be changed at the source.

**Interception point.** `sl.dlss_g.dll` exports exactly two symbols: `DllMain`
and `slGetPluginFunction`. The host resolves `slDLSSGSetOptions` through the
latter. Hooking `slGetPluginFunction` and wrapping the returned pointer gives a
single, version-stable place to rewrite `numFramesToGenerate`.

`DLSSGOptions`, read off `slSetData` — it walks the chain through `[node+0x00]`
and compares the two type qwords at `[node+0x08]` and `[node+0x10]`:

```
0x00  BaseStructure* next
0x08  StructType     structType          (16 bytes)
0x18  uint64         structVersion
0x20  DLSSGMode      mode                0 = off
0x24  uint32         numFramesToGenerate <-- the field to rewrite
```

`{cb f1 c5 fa fd 2d 36 4f a1 e6 3a 9e 86 52 56 c5}` is the DLSSGOptions type;
`{35 64 1b 17 3c 9b c8 4f 99 94 fb e5 25 69 aa a4}` is the viewport handle.

`slDLSSGSetOptions(rcx = options, rdx = viewport)` — note the order, the public
header has it the other way round — copies the first `0x28` bytes of the options
onto its stack, overwrites the copy's `next` with the viewport, and calls
`slSetData` on that copy. So `slSetData` is the real choke point, and on that
path it is handed a private copy rather than the game's own struct.


## 6. Measuring it without a game

`tools/snippet_probe.cpp` settles the second gate empirically. It creates a
D3D12 device, loads the snippet, and drives it the way the NGX runtime does:

```
NVSDK_NGX_D3D12_Init_Ext(appId, dataPath, device, sdkVersion, featureInfo)
NVSDK_NGX_D3D12_PopulateParameters_Impl(param)
```

`param` is a hand-built vtable rather than a C++ class, because the snippet is
MSVC-built and MSVC emits same-name virtual overloads in *reverse* declaration
order:

```
Set  0 void*   1 ID3D12Resource*  2 ID3D11Resource*  3 int
     4 uint    5 double           6 float            7 uint64
Get  8 void** 9 ID3D12Resource** 10 ID3D11Resource** 11 int*
    12 uint* 13 double*          14 float*          15 uint64*
    16 Reset
```

Two things the public header will not tell you:

- `Init_Ext` takes `(rcx appId, rdx dataPath, r8 device, r9d sdkVersion,
  stack featureInfo)`. The header has the version last.
- It refuses to start unless the *calling module* has `nvngx.dll` in its path:
  `GetModuleHandleExW(FROM_ADDRESS, <return address>)`, `GetModuleFileNameW`,
  then `wcsstr` against `L"nvngx.dll"`. Failing that you get `0xBAD00002` and
  "Error: Not called from NGX runtime". Naming the harness
  `nvngx.dll.probe.exe` is enough.

Result on an RTX 4070 Ti, all snippet builds tested:

```
as shipped   DLSSG.MultiFrameCountMax = 1     max multiplier 2x
patched      DLSSG.MultiFrameCountMax = 5     max multiplier 6x
```

The harness goes further than the capability query, because
`EndpointCoreCreateParameters::ParseNGXParameters` needs only scalars — a
command list and a handful of numbers are enough to build the real feature:

```
Init_Ext                          0x00000001  ok
PopulateParameters                0x00000001  ok
  DLSSG.ModelVersion              512   (FG v2)
  DLSSG.MultiFrameCountMax        5     <-- 1 before the patch
CreateFeature (feature 10)        0x00000001  ok    2560x1440 from 1280x720
ReleaseFeature                                ok
```

So on Ada the patched snippet initialises, advertises five generated frames,
builds a complete frame-generation feature and tears it down cleanly. What is
still untested offline is the evaluate path with a count above 1: that runs
`ComputeAndValidateTimeFactor`, and it needs tagged colour, depth and
motion-vector resources plus real GPU work.

### Getting the snippet to talk

Two environment variables, no registry writes:

```
__NGX_LOG_LEVEL=2
__NGX_LOG_PATH_OVERRIDE=<directory>
```

The snippet then logs to stdout *and* to `nvngx_dlssg_<version>.log`. That is
where `m_gpuArch = 0x190`, the DRS key reads, and any "Multi frame is not
supported on this device" would appear. It works inside a game as well as in the
harness.

The `ShowDlssIndicator` value under the NGXCore registry key turns on the
on-screen DLSS indicator, which renders the multiplier through the `"- %dx"` at
`0x3EF2A` — the fastest visual confirmation that three frames are being
generated rather than one.
