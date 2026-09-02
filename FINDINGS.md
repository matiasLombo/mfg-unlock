# What we learned taking DLSS-G apart

Notes from unlocking Multi Frame Generation on Ada, working out why 4x looked
wrong, and rebuilding three of its kernels. Written down because the dead ends
cost far more than the answers did.

Hardware: RTX 4070 Ti (Ada, `sm_89`), driver 616.56. Titles: DOOM The Dark Ages
(Vulkan), GTA V Enhanced (D3D12).

---

## 1. Ada is not running a worse algorithm

The obvious theory — that 4x is tuned for Blackwell and Ada gets a lesser path —
is wrong, and it took a full teardown to say so with confidence.

- **There are exactly two architecture comparisons in the entire snippet.** Both
  are `cmp <reg>, 0x1B0`. One sets `DLSSG.MultiFrameCountMax` to 1 or 5; the
  other stores a bool that only permits a frame count above 1. Neither selects an
  algorithm.
- **The network kernels ship no Blackwell build at all.** 39 fatbins carry only
  `compute_89` PTX. Ada runs precompiled `sm_89` cubins; Blackwell JITs the same
  PTX. This is the *inverse* of DLSS Neural Rendering, where the DLL shipped
  `sm_120` only and Ada had to be built from scratch.
- **`sm_86` and `sm_89` cubins are identical** — same instruction counts, same
  opcodes, same registers, same shared memory, kernel for kernel. The only
  difference is how `nvdisasm` spells `F2FP` per architecture.
- **No FP8.** The one tensor feature Ada has and Ampere does not is unused; the
  convolutions are FP16 `HMMA`. Our earlier neural-rendering work had already
  measured FP8 and FP16 as equivalent on Ada anyway.

**Nothing about the generated image can be worse on Ada.** Same weights, same
kernels, same `timeFactor = index / (count + 1)`.

## 2. What was actually wrong

Frame *timing*, and one specific transition.

`presentCommon` computes, every frame:

    meteringFlag = (feedbackCounter >= 30) && (byte[ctx+field] == 0)

The counter rises once per batch and crosses 30 about three quarters of a second
after frame generation starts. When the flag changes value, the plugin calls
`flushAll` and switches the command context DLFG runs on — mid-gameplay.

That is the whole artefact. Enter a game already set to 4x and it stutters; change
the multiplier and change it back and it is fine, because that path re-runs setup
cleanly; alt-tab and it returns. Pinning the flag removes the transition.

The user's own observation — "switch to 3x and back and 4x works" — located this
after hours of disassembly had not. **A reproducible state-dependent symptom is
worth more than any amount of static analysis.**

## 3. Where the Blackwell code actually is

Not in the network. In the 31 *framework* kernels — the geometry around it:
motion vector estimation, forward warp, inpainting, the pull/push pyramid,
candidate blending.

Each ships PTX for `compute_120` **and** `compute_89`, and they are different
source. Ada's version does its boolean work with a couple of hundred 16-bit
predicates that Blackwell's does without:

    sm_89 :  setp.ne.s16 x81,  setp.u16 x74,  and.b16 x45
    sm_120:  setp.ne.s16 x 9,  setp.u16 x 2,  and.b16 x 1

All 31 assemble for `sm_89` unchanged — none of it is Blackwell-only. Ada was
given the worse of the two.

Three produce a cubin that fits the original's slot, so they can be swapped in
memory: motion-vector estimate (-7%), inpaint (-28%), inpaint decision (-23%).
The shared-memory layout and access offsets are identical on both sides, so it is
a speed change and not an image change. The other 28 need relocation into fresh
memory to fit.

## 4. Method

### Analyse the binary that actually loads

Hours went into `streamline/production/nvngx_dlssg.dll` before noticing NGX had
loaded something else entirely: the newest OTA copy under
`C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\<id>\files\`. Different version,
different hash, different struct offsets. `sl.log` names the file it opened —
read that line before anything else.

### Match shapes, not bytes

Three patches here silently stopped matching across Streamline versions:

- the metering field moved on every release: `0x44A0`, `0x44F8`, `0x44F0`
- the pacer sequence was built with `esi` in 2.11.1 and `edi` in 2.12.129
- the store that clears the field is an immediate zero in 2.12.x and a *zeroed
  register* in 2.13.0

Each failure was silent: zero sites, no error, the flag file still sitting there
looking installed. One of them stayed dead for hours while its measured effect
was still being quoted as if it were live. **Print the site count, and treat zero
as a failure.**

### A flag file is not evidence

`g_meter_off` was declared and read but never assigned, so its patch never ran at
all. Nothing in the system noticed. Wire the check to the log, not to the file.

### Verify before you ship code to the GPU

`cuModuleLoadData` on the rebuilt cubins catches an invalid or ISA-incompatible
build in a second, without launching anything. Fingerprint the original by
`(.text size, .nv.shared size, register count)` — unique across all 31 — so a
snippet update stops matching instead of being patched wrongly.

### NVIDIA ships its own instrumentation

`__NGX_SHOW_INDICATOR` must be exactly **1024**; every other value is silently
zeroed. It draws an overlay with the snippet's own `cuda_font_kernel` reporting
build, API, resolutions and the multiplier in effect. `__NGX_LOG_LEVEL=2` plus
`__NGX_LOG_PATH_OVERRIDE` get the snippet's log. That overlay confirmed `4x` from
NVIDIA's code rather than from our inference.

## 5. Dead ends, so nobody repeats them

| | |
| --- | --- |
| `k_central_block_original` | second copy of a kernel in the binary, never launched; same FP16 op multiset, only a different shared-memory strategy |
| Optical Flow Accelerator | not referenced anywhere in the snippet; the AI flow model runs on every architecture |
| Scene change detector | fires above **45 degrees of camera rotation in one frame**; it is for cuts, not for play |
| `EndpointPassthrough` | selected by a `resourcesCreated` flag set once at init, not a per-frame decision |
| Dynamic MFG | requires flip config **V2**; the Vulkan path hardcodes V1, so it is impossible there on *any* GPU |
| `rsync.cpp` pacing | dead under Vulkan — "only supported on DX12" — so its target-time computation never runs |
| `numFramesPerBatch` | metering is programmed with N, not N+1; our own frame-interval histogram is unimodal, which rules out the unmetered-frame theory |
| Recompiling the network with a newer `ptxas` | 39/39 assemble, but the result is larger in every case and does not fit the original slot |
