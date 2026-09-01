# How to test this

Nothing here has been installed. Every step below is something you run
deliberately, and every one of them is reversible.

## Already proven, on your 4070 Ti, without a game

`tools/nvngx.dll.probe.exe` creates a D3D12 device, loads the snippet, and calls
its `PopulateParameters` the way the NGX runtime would, then prints every
parameter it sets:

```
$ tools/nvngx.dll.probe.exe <original nvngx_dlssg.dll>
DLSSG.MultiFrameCountMax     uint     1 (0x1)      ==> maximum multiplier 2x

$ tools/nvngx.dll.probe.exe <patched nvngx_dlssg.dll>
DLSSG.MultiFrameCountMax     uint     5 (0x5)      ==> maximum multiplier 6x
```

That is the capability the Streamline plugin clamps against, measured on the
real card. What it does *not* prove is that generating three frames works once
the pipeline is running — that needs a game.

It goes further than that, too:

```
Init_Ext            ok
PopulateParameters  ok      DLSSG.MultiFrameCountMax = 5
CreateFeature       ok      feature 10, 2560x1440 from 1280x720
ReleaseFeature      ok
```

The patched snippet builds a whole frame-generation feature on the card and
tears it down cleanly. Re-run any of it with `tools/run_probe.sh`.

### Making the snippet talk, inside a game

Set these before launching and the NGX snippet logs what it is doing, both to
stdout and to `nvngx_dlssg_<version>.log`:

```
__NGX_LOG_LEVEL=2
__NGX_LOG_PATH_OVERRIDE=C:\some\folder
```

That log is where `m_gpuArch = 0x190` and any "Multi frame is not supported on
this device" would show up. In Steam, put `__NGX_LOG_LEVEL=2 %command%` in the
launch options.

## The three candidates you have installed

| game | snippet | Streamline | route |
| --- | --- | --- | --- |
| DOOM The Dark Ages | 310.6.0.0 | 2.11.1 | console variable, **no add-on needed** |
| Halo Campaign Evolved | 310.8.0.0 | 2.13.0 | add-on (ReShade already installed there) |
| GTA V Enhanced | 310.8.0.0 | 2.13.0 | add-on, but see the warning below |

### DOOM The Dark Ages — start here

DOOM has this console variable built in:

```
r_streamlineDLSSGMode    "DLSS-FG Mode: 0:Off, 1-3:On and number of frames to generate"
```

It already understands 1, 2 and 3 generated frames. So the whole test is one
patch and one config line — no add-on, no hooking, nothing of ours running in
the process.

1. Patch the snippet:
   ```
   python tools/mfg_unlock.py "C:\Program Files (x86)\Steam\steamapps\common\DOOMTheDarkAges\streamline\production\nvngx_dlssg.dll"
   ```
   It writes `nvngx_dlssg.dll.orig` next to it first.

2. Add to `C:\Users\matia\Saved Games\id Software\DOOMTheDarkAges\base\DOOMTheDarkAgesConfig.local`:
   ```
   r_streamlineLogLevel "2"
   r_streamlineEnableDebugLogging "1"
   r_streamlineDLSSGMode "3"
   ```

3. Launch, enable DLSS + Frame Generation in the menu, and look for this line in
   the Streamline log:
   ```
   Multi-frame <enabled>, max generated frames 5 (SL Plugin supports 5, NGX feature supports 5)
   ```
   `max generated frames 5` is the patch working. `1` means it did not.

`r_streamlineFrameGenShowInterpolatedFrames` tints generated frames, which is
the quickest way to see with your own eyes whether three are being made instead
of one.

To undo: delete the patched dll and rename `.orig` back.

#### If 3x and 4x do not appear in the menu

Do not guess — one log line says exactly where it stops. Streamline takes its
logging from the environment, so put this in the Steam launch options:

```
SL_LOG_LEVEL=2 SL_LOG_PATH=C:\tmp\sl SL_ENABLE_CONSOLE_LOGGING=1 %command%
```

Then find this in `sl.log`:

```
Multi-frame <state>, max generated frames A (SL Plugin supports B, NGX feature supports C)
```

| what it says | what it means | what to do |
| --- | --- | --- |
| `NGX feature supports 1` | the game is not using the dll we patched | check which `nvngx_dlssg.dll` it loaded; that folder also holds `.dlsss` copies from a DLSS swapper, and the driver can supply its own |
| `NGX feature supports 5`, `max generated frames 1` | a driver profile is capping it | DRS key `0x104D6667`, set by the NVIDIA App's frame-generation override; clear it there |
| all three say `5`, menu still 2x | the runtime is unlocked and the game's UI is the limit | skip the menu, set the console variable below |

The console variable takes the count directly — `"DLSS-FG Mode: 0:Off, 1-3:On
and number of frames to generate"` — so it does not care what the menu offers.
In `DOOMTheDarkAgesConfig.local`:

```
r_streamlineDLSSGMode "3"
```

Enable frame generation in the menu first, quit, then set it, so the menu cannot
write over the value on its way out.

If even that gets overwritten, `mfg-multiplier.addon64` is the last resort. It
hooks `slSetData`, which is API-agnostic, so it works under Vulkan as well —
provided ReShade loads in DOOM.

Why the menu is expected to work: DOOM has no NVAPI in its executable at all, so
it cannot ask the driver what GPU this is. It reads the limit from Streamline,
and in the 2.11.1 plugin DOOM ships, `slDLSSGGetState` forwards through
`slGetData`, which at `0x52343` copies `[plugin+0x45A4]` — the value our patch
feeds — into `DLSSGState+0x34`. The one condition is that the game passes a
`structVersion` of 2 or more; below that the field is never filled in.

#### It also has the right benchmark built in

DOOM ships a benchmark mode in the menus that writes `benchmark.json`, and what
it records happens to be exactly what this question needs:

```
avg_fps  median_fps  1_percent_low_fps  sd_fps  avg_fps_excluding_outliers
s_percentile_frame_time / _cpu_frame_time / _gpu_frame_time   percentiles_ms
```

plus a stutter-frame count controlled by `benchmark_stutterFrameThreshold`,
`benchmark_stutterFrameThresholdPerc` and `benchmark_stutterFrameWindow`, and
on-screen frametime graphs via `benchmark_graph_frame`, `_cpu`, `_gpu`.

Average FPS will rise with the multiplier no matter how bad the result looks, so
it is close to useless here. **`sd_fps`, the frametime percentiles and the
stutter count are the measurement** — they are where a CPU pacer failing to
space three generated frames will show up.

Run the same benchmark three times, changing one line between runs:

| run | `r_streamlineDLSSGMode` | what it is |
| --- | --- | --- |
| 1 | `1` | 2x, the baseline that works today |
| 2 | `2` | 3x |
| 3 | `3` | 4x |

If `sd_fps` and the stutter count stay flat while `avg_fps` climbs, it works. If
they climb with it, the pacer is the limit and that is the honest result.

### Not the Bright Memory benchmark

Worth writing down so it does not get tried twice: the benchmark we used for the
neural-rendering work cannot test this. Its executable contains no reference to
Streamline, `sl.dlss_g`, `DLSSG` or `numFramesToGenerate` — only
`NVSDK_NGX_D3D12_Init` for the upscaler — and the folder has no `sl.interposer.dll`.
It has DLSS upscaling and ray reconstruction and no frame generation at all.
Frame generation needs the game to load Streamline, hand over its swapchain and
tag depth and motion vectors; dropping DLLs in cannot add code that calls them.

### Halo Campaign Evolved — the add-on route

ReShade is already installed there. Patch the snippet the same way, then:

```
cp mfg-multiplier.addon64 "C:\Program Files (x86)\Steam\steamapps\common\Halo Campaign Evolved\Meteorite\Binaries\Win64\"
```

Open the ReShade overlay, find "MFG Multiplier", pick 3x or 4x. The panel shows
what the game asked for, what was applied, and whether the plugin refused. If it
refuses, the add-on immediately re-sends the game's own value, so frame
generation keeps working instead of switching off.

### GTA V Enhanced — not a good test bed

`dlss-enabler.log` there says:

```
[INIT] Disabling native DLSSG implementation
[DLSSG] Loading built-in frame generation backend: FSR 3.1B
```

DLSS Enabler turns off NVIDIA frame generation and routes to FSR3, so there is
no DLSS-G to multiply. Test the other two first; if you want GTA V, that has to
be undone.

## What to look for

- **Does it run at all** — three generated frames per rendered frame.
- **Pacing.** Blackwell has hardware flip metering for spacing the extra
  presents; Ada does not, and `sl.dlss_g` falls back to its CPU pacer
  (`pacer.cpp`, "CPU pacer is skipping the frame"). Judge frame *timing*, not
  average FPS — the average will look great either way. Stutter or a sawtooth
  frametime graph is the failure mode to expect.
- **Latency.** More generated frames means the real frame is held longer.
- **Artefacts** at 3x/4x versus 2x. The network is the same, run at
  `t = 1/4, 2/4, 3/4` instead of `1/2`, so these should be *fewer* per unit of
  time than 2x at half the framerate, not more.
