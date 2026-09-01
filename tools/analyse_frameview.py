#!/usr/bin/env python3
"""Compare FrameView captures to tell frame pacing apart from image artefacts.

Streamline reports its own pipeline as healthy at 3x and 4x -- no pacer skips,
no timeouts, no errors -- while the picture still looks wrong. That leaves two
possibilities, and they need different evidence:

  pacing        the frames are made correctly but reach the panel unevenly
  image         the frames arrive on time and the interpolation itself is bad

FrameView measures the first directly, because it records both sides of the
handoff:

  MsBetweenPresents        what the game and Streamline believe they did
  MsBetweenDisplayChange   when the screen actually changed
  Dropped                  the frame was presented and never shown
  Frame Gen Multiplier     which mode was live, per frame

If display intervals are as even at 4x as at 2x and nothing is dropped, the
pacing is fine and the problem is the generated images. If they are not, it is
pacing -- and the drop count says whether frames are being thrown away, which
is the one failure Streamline cannot log, since from its side the present
succeeded.

usage: analyse_frameview.py <folder or csv...>
"""
import sys, os, csv, glob, statistics as st


def load(path):
    rows = []
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def num(r, key):
    v = (r.get(key) or '').strip()
    if v in ('', 'NA', 'N/A'):
        return None
    try:
        return float(v)
    except ValueError:
        return None


def col(rows, key):
    return [v for v in (num(r, key) for r in rows) if v is not None]


def pct(xs, p):
    if not xs:
        return float('nan')
    s = sorted(xs)
    return s[min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))]


def describe(path):
    rows = load(path)
    if not rows:
        return None
    disp = [v for v in col(rows, 'MsBetweenDisplayChange') if v > 0]
    pres = [v for v in col(rows, 'MsBetweenPresents') if v > 0]
    drop = col(rows, 'Dropped')
    mult = {}
    for r in rows:
        m = (r.get('Frame Gen Multiplier') or '').strip()
        if m:
            mult[m] = mult.get(m, 0) + 1
    dropped = sum(1 for d in drop if d >= 0.5)
    # Spacing consistency: the spread of display intervals relative to their
    # median. Judder shows up here even when the average framerate looks fine.
    med = st.median(disp) if disp else float('nan')
    jitter = [abs(d - med) for d in disp] if disp else []
    return {
        'name': os.path.basename(path),
        'frames': len(rows),
        'mult': ', '.join(f"{k} x{v}" for k, v in sorted(mult.items(), key=lambda x: -x[1])[:3]),
        'disp_avg_fps': 1000.0 / med if disp and med else float('nan'),
        'pres_avg_fps': 1000.0 / st.median(pres) if pres else float('nan'),
        'disp_med': med,
        'disp_p99': pct(disp, 99),
        'disp_p999': pct(disp, 99.9),
        'disp_sd': st.pstdev(disp) if len(disp) > 1 else float('nan'),
        'jitter_med': st.median(jitter) if jitter else float('nan'),
        'jitter_p99': pct(jitter, 99),
        'dropped': dropped,
        'drop_pct': 100.0 * dropped / max(len(drop), 1),
        'n_disp': len(disp),
    }


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__.strip().splitlines()[-1])
    files = []
    for a in args:
        if os.path.isdir(a):
            files += sorted(glob.glob(os.path.join(a, '*Log.csv')))
        else:
            files.append(a)
    if not files:
        sys.exit("no *Log.csv found")

    out = [d for d in (describe(f) for f in files) if d]
    if not out:
        sys.exit("nothing readable")

    print(f"{'capture':<46}{'mult':<12}{'disp fps':>9}{'pres fps':>9}"
          f"{'median':>9}{'sd':>8}{'p99':>8}{'p99.9':>9}{'dropped':>10}")
    print('-' * 120)
    for d in out:
        print(f"{d['name'][:45]:<46}{d['mult'][:11]:<12}"
              f"{d['disp_avg_fps']:>9.1f}{d['pres_avg_fps']:>9.1f}"
              f"{d['disp_med']:>9.2f}{d['disp_sd']:>8.2f}"
              f"{d['disp_p99']:>8.2f}{d['disp_p999']:>9.2f}"
              f"{d['dropped']:>7d} {d['drop_pct']:>5.1f}%")

    print("\nspacing consistency -- how far each displayed frame lands from the median interval")
    print(f"{'capture':<46}{'median off':>12}{'p99 off':>10}{'sd/median':>12}")
    print('-' * 120)
    for d in out:
        ratio = d['disp_sd'] / d['disp_med'] if d['disp_med'] else float('nan')
        print(f"{d['name'][:45]:<46}{d['jitter_med']:>12.2f}{d['jitter_p99']:>10.2f}{ratio:>12.3f}")

    print("\nreading it:")
    print("  sd/median flat across multipliers + no drops  -> pacing is fine, look at the images")
    print("  sd/median grows with the multiplier           -> pacing")
    print("  dropped > 0 and growing                       -> frames are made and thrown away")
    print("  disp fps well below pres fps                  -> the panel is not keeping up")


if __name__ == '__main__':
    main()
