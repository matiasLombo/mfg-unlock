# Multi Frame Generation on pre-Blackwell GPUs

NVIDIA ships DLSS Frame Generation with a single generated frame (2x) on
everything below Blackwell. The restriction is two `cmp reg, 0x1B0` instructions
in the NGX snippet. Nothing else in the stack is architecture dependent, and the
snippet already carries native `sm_89` code for every kernel it runs.

See [ANALYSIS.md](ANALYSIS.md) for how that was established.

## Two parts

**1. Remove the gate** — `tools/mfg_unlock.py` rewrites both comparisons so the
snippet reports `DLSSG.MultiFrameCountMax = 5` and accepts counts above 1:

```
python tools/mfg_unlock.py "<game>/nvngx_dlssg.dll"
```

It keeps a `.orig` backup, locates the sites by pattern rather than by offset,
and refuses to touch a file it does not recognise.

**2. Ask for more frames** — `mfg-multiplier.addon64` is a ReShade add-on that
rewrites `numFramesToGenerate` on its way into `sl.dlss_g.dll`. Games written
before DLSS 4 always ask for one; the unlocked runtime will still only give what
it is asked for.

Drop it next to the game executable alongside ReShade and pick a multiplier in
the overlay. If the plugin rejects the request the add-on immediately re-sends
the game's own value, so frame generation keeps working rather than switching off.

## What to expect

Blackwell has hardware flip metering for spacing the extra presents. Ada does
not, and `sl.dlss_g` falls back to its CPU pacer. Frame *pacing* is where this
should show its limits — not image quality, which comes from the same network
run at `t = index / (count + 1)`.

## Requirements

- NVIDIA GPU, Turing or newer, with a driver that has DLSS-G
- A game using Streamline DLSS Frame Generation
- ReShade with add-on support, for part 2
