# mfg-unlock

DLSS Multi Frame Generation at 3x and 4x on Ada, which NVIDIA restricts to
Blackwell. A `version.dll` proxy that rewrites two comparisons in the DLSS-G
snippet while the loader maps it, then fixes the frame pacing that makes the
extra frames worth having.

Confirmed on an RTX 4070 Ti in two games, one Vulkan and one D3D12, with the same
binary:

| | DOOM The Dark Ages | GTA V Enhanced |
| --- | --- | --- |
| API | Vulkan | D3D12 |
| Streamline | 2.12.129 | 2.13.0 |
| snippet | 310.7.129 | 310.8.0.0 |

The snippet's own diagnostic overlay reports `4x`, so the unlock is confirmed by
NVIDIA's code rather than by inference.

---

## Read this first

**Anti-cheat.** This injects a DLL into the game process. GTA V ships BattlEye,
and other titles ship EAC or their own. Loading this into a game with anti-cheat
active — GTA Online especially — can get an account banned. Single-player only,
and check what your game runs before you install anything.

**Tested on one machine.** One RTX 4070 Ti, driver 616.56, two games, one person.
Other Ada cards should work — the architecture gate and every signature are the
same across the family — but "should" is not "does". Nothing here has been seen
on a 4060, 4080 or 4090.

**Nothing is written to disk.** All five patches are applied to the mapped image
in memory. Delete `version.dll` and the machine is exactly as it was.

---

## Why the unlock alone is not enough

Removing the gate makes 3x and 4x selectable. It does not make them good: with
only the gate removed, 15% of generated frames arrive stacked on top of another,
and entering a game directly at 4x looks visibly wrong until you toggle the mode.

Measured in DOOM at 4x, frames delivered on time versus stacked:

| | base fps | on time | stacked |
| --- | --- | --- | --- |
| gate only | 48.7 | 72.5% | 15.4% |
| + pacer | 41.3 | 88.8% | 0.4% |
| + pacer + metering off | 40.6 | **94.3%** | 0.1% |

## What each patch does

**The gate** — two `cmp <reg>, 0x1B0` against the NGX architecture id, one
setting `DLSSG.MultiFrameCountMax` to 1 or 5, the other permitting a frame count
above 1. Both immediates become 0, so the comparison reads "arch >= 0".

**The pacer** (`mfg-unpin.txt`) — neutralises the `cmovae` that computes
`meteringFlag = (feedbackCounter >= 30) && (byte[field] == 0)`. That counter
rises once per batch and crosses 30 about three quarters of a second in, so the
flag flips *during gameplay*, and a flip costs a `flushAll` plus a switch of the
command context DLFG runs on. That transition is the stutter you see when you
enter a game already set to 4x — and why changing the multiplier and changing it
back appears to fix it. Pinning the flag removes the transition.

**Flip metering off** (`mfg-nometer.txt`) — stops the same flag being cleared, so
the CPU pacer keeps the spacing rather than handing it to hardware metering.

**The kernel rebuild** (`mfg-cubins.txt`) — see below.

**The overlay** (`mfg-indicator.txt`) — sets `__NGX_SHOW_INDICATOR=1024`, which
turns on the diagnostic overlay NVIDIA already ships inside the snippet, drawn by
its own `cuda_font_kernel`. It must be exactly 1024; every other value is
silently zeroed. It reports the multiplier in effect, so you can confirm 4x
without trusting your eyes.

Each is opt-in: create the named empty file next to `version.dll`. Confirm what
actually applied in `mfg-unlock.log` — a flag file existing is not evidence a
patch matched.

## Rebuilding three kernels from the Blackwell PTX

The snippet carries, for each of its 31 framework kernels, PTX compiled for
`compute_120` **and** for `compute_89` — and they are not the same source. Ada's
runs the boolean work through a couple of hundred 16-bit predicates that
Blackwell's does without.

None of the Blackwell PTX is Blackwell-only: all 31 assemble for `sm_89`
unchanged. Ada was simply given the worse of the two. Three of them produce a
cubin that still fits the space the original occupies, which is what makes an
in-memory swap possible:

| kernel | | |
| --- | --- | --- |
| motion-vector estimate | 2312 -> 2160 instr | -7% |
| inpaint | 904 -> 648 | -28% |
| inpaint decision | 752 -> 576 | -23% |

Same algorithm either way — identical shared-memory layout, identical access
offsets — so this is a speed change, not an image change.

`src/cubins.h` is **not** in this repository: it is derived from NVIDIA's own
PTX, so it is generated on your machine from the snippet you already have.

```
pip install nvidia-cuda-nvcc nvidia-cuda-nvdisasm nvidia-cuda-cuobjdump
python tools/rebuild_cubins.py
```

Without it the proxy still builds and everything else still works; the swap is
compiled out.

## Installing

```
./build-proxy.sh
cp version.dll "<game folder>"
```

The game must import `version.dll` — DOOM and GTA V both do. Check the import
table of anything else first, and forward that DLL instead if it does not.

Then create the flag files you want:

```
cd "<game folder>"
type nul > mfg-unpin.txt
type nul > mfg-nometer.txt
type nul > mfg-cubins.txt
```

DOOM exposes the multiplier through `r_streamlineDLSSGMode` ("0:Off, 1-3:On and
number of frames to generate") in `DOOMTheDarkAgesConfig.local`, so no add-on is
needed. Games that only ever ask for one generated frame need
`slDLSSGSetOptions` intercepted instead; `src/addon.cpp` does that through
ReShade and is **untested**.

### Checking it worked

`mfg-unlock.log`, next to the dll:

```
gates rewritten: 2
CPU pacer enabled, sites: 1
  metering field at ctx+17648
driver flip metering switched off, sites: 1
kernels rebuilt from the Blackwell PTX: 3
```

A patch reporting `sites: 0` matched nothing and did nothing. And in `sl.log`:

```
Multi-frame supported, max generated frames 5 (SL Plugin supports 5, NGX feature supports 5)
```

## Why a proxy and not a patched file

NGX verifies the snippet's Authenticode signature as it loads it. A modified file
is refused and NGX falls back to the driver-store copy, which has no multi-frame
support at all — frame generation disappears rather than staying at 2x. The
signature is checked at load and never again, so the bytes are rewritten in the
mapped image instead, before anything executes them.

Timing is the whole trick: `PopulateParameters`, which decides what
`DLSSG.MultiFrameCountMax` reports, runs inside `NVSDK_NGX_*_Init_Ext`. Patching
after that is too late. `LdrRegisterDllNotification` fires while the loader is
still bringing the image in, and a statically imported proxy is loaded before the
game's first instruction.

**Patch the copy NGX actually loads.** It is usually not the one in the game
folder — NGX prefers the newest OTA copy under
`C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\<id>\files\`. This proxy matches
by path substring so it catches whichever is used. `sl.log` names it outright.

## Matching by shape, not by bytes

Every signature here matches the *shape* of an instruction sequence and reads
registers and offsets out of the modrm and displacement. This is not stylistic.
The metering field has moved on every single release — `0x44A0` in 2.11.1,
`0x44F8` in 2.12.129, `0x44F0` in 2.13.0 — and the pacer sequence was built with
`esi` in 2.11.1 and `edi` in 2.12.129. A literal signature stops matching on a
Streamline update, reports zero sites, and keeps looking installed. That happened
here and cost hours.

If you add a patch, match the shape, derive offsets from an anchor, and log the
site count.

## Repository

```
src/proxy.cpp             the proxy: export forwarding, the five patches
src/addon.cpp             ReShade add-on for games that ask for one frame (untested)
tools/rebuild_cubins.py   rebuilds the three kernels from your snippet
tools/mfg_unlock.py       offline file patcher (kept for reference; see above)
tools/snippet_probe.cpp   drives the snippet directly, no game needed
ANALYSIS.md               how the snippet and sl.dlss_g fit together
TESTING.md                how to verify a change
```

## Licence

MIT, for the code in this repository. It contains nothing of NVIDIA's: the
kernel rebuild reads the snippet installed on your own machine.
