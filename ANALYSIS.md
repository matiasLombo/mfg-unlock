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

`DLSSGOptions` (SL 2.13, 0x78 bytes):

```
0x00  StructType  structType      (16 bytes)
0x10  uint64      structVersion
0x18  BaseStructure* next
0x20  DLSSGMode   mode
0x24  uint32      numFramesToGenerate     <-- the field to rewrite
0x28  DLSSGFlags  flags
...
```


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

The first gate cannot be reached this way. `m_multiFrameSupported` is only read
by the count validation, which runs at *evaluate* time and needs a full set of
tagged D3D12 resources.
