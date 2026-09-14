"""El harness A/B del lado del analizador: de una sesion con toggles a una
ganancia MEDIDA.

El DLL prende y apaga cada candidato cada N frames y deja constancia en el
JSONL (eventos `action` con on = 0/1). Aca se parte el frametime en las dos
condiciones y se decide si la diferencia supera el ruido.

Es la misma estadistica que core/stats.h -- mediana, Mann-Whitney con
correccion por empates, intervalo por bootstrap -- reimplementada en Python
porque el reporte se genera offline y no queremos un binario en el medio. Los
tests comparan las dos implementaciones sobre las mismas muestras.

Por que alternar cada pocos frames en vez de correr dos sesiones: los clocks
de la GPU se mueven con temperatura y potencia, y en diez minutos ese drift es
mas grande que muchas de las optimizaciones que buscamos. Alternando, las dos
condiciones ven el mismo drift.
"""

import math
import os
from dataclasses import dataclass, field
from statistics import median
from typing import Dict, List, Optional

from .model import Session

# Frames a descartar despues de cada conmutacion: los primeros miden caches
# frias y PSOs que se rearman, no la optimizacion.
DEFAULT_SETTLE = 5


@dataclass
class ABResult:
    action_id: int
    title: str
    n_on: int
    n_off: int
    median_on: float
    median_off: float
    delta_ms: float
    ci_lo: float
    ci_hi: float
    p_value: float
    noise_ms: float
    significant: bool
    captures: List[str] = field(default_factory=list)

    @property
    def verdict(self) -> str:
        if self.significant:
            return "GANA"
        if self.delta_ms < 0 and self.p_value < 0.01:
            return "EMPEORA"
        return "no se distingue del ruido"


class _Rng:
    """xorshift64: determinista y sin dependencias, igual que en stats.h."""

    def __init__(self, seed: int = 12345):
        self.s = seed or 1

    def next(self) -> int:
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFFFFFFFFFF
        s ^= s >> 7
        s ^= (s << 17) & 0xFFFFFFFFFFFFFFFF
        self.s = s
        return s

    def below(self, n: int) -> int:
        return self.next() % n


def mann_whitney_p(a: List[float], b: List[float]) -> float:
    na, nb = len(a), len(b)
    if na < 8 or nb < 8:
        return 1.0
    allv = [(x, 0) for x in a] + [(x, 1) for x in b]
    allv.sort(key=lambda t: t[0])
    ranks = [0.0] * len(allv)
    tie_sum = 0.0
    i = 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        r = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[k] = r
        t = j - i + 1
        if t > 1:
            tie_sum += t ** 3 - t
        i = j + 1
    ra = sum(ranks[i] for i in range(len(allv)) if allv[i][1] == 0)
    ua = ra - na * (na + 1) / 2.0
    ub = na * nb - ua
    u = min(ua, ub)
    mu = na * nb / 2.0
    n = na + nb
    sigma2 = (na * nb / 12.0) * ((n + 1.0) - tie_sum / (n * (n - 1.0)))
    if sigma2 <= 0:
        return 1.0
    z = (abs(u - mu) - 0.5) / math.sqrt(sigma2)
    if z <= 0:
        return 1.0
    return math.erfc(z / math.sqrt(2.0))


def bootstrap_ci(on: List[float], off: List[float], iters: int = 2000,
                 seed: int = 12345):
    if len(on) < 4 or len(off) < 4:
        return 0.0, 0.0
    rng = _Rng(seed)
    deltas = []
    for _ in range(iters):
        sa = [on[rng.below(len(on))] for _ in range(len(on))]
        sb = [off[rng.below(len(off))] for _ in range(len(off))]
        deltas.append(median(sb) - median(sa))
    deltas.sort()
    lo = deltas[int(0.025 * (len(deltas) - 1))]
    hi = deltas[int(0.975 * (len(deltas) - 1))]
    return lo, hi


def frame_noise(v: List[float]) -> float:
    if len(v) < 3:
        return 0.0
    return median([abs(v[i] - v[i - 1]) for i in range(1, len(v))])


def compare(on: List[float], off: List[float], alpha: float = 0.01) -> dict:
    if not on or not off:
        return dict(median_on=0.0, median_off=0.0, delta_ms=0.0, ci_lo=0.0,
                    ci_hi=0.0, p_value=1.0, noise_ms=0.0, significant=False)
    mon, moff = median(on), median(off)
    delta = moff - mon
    p = mann_whitney_p(on, off)
    lo, hi = bootstrap_ci(on, off)
    return dict(median_on=mon, median_off=moff, delta_ms=delta, ci_lo=lo,
                ci_hi=hi, p_value=p, noise_ms=max(frame_noise(on), frame_noise(off)),
                significant=(p < alpha and lo > 0.0 and delta > 0.0))


def measure(s: Session, settle: int = DEFAULT_SETTLE,
            toggles: Optional[Dict[int, List[tuple]]] = None) -> List[ABResult]:
    """Parte el frametime por condicion y mide cada accion.

    Solo se usan los frames en los que la accion medida es la UNICA prendida:
    con dos acciones prendidas a la vez no se sabe cual gano, y un reporte que
    no lo sabe no sirve para decidir.
    """
    toggles = toggles if toggles is not None else s.toggles
    if not toggles:
        return []

    # Estado por frame de cada accion.
    frames = sorted(s.frames, key=lambda f: f.index)
    ids = sorted(toggles.keys())
    state = {aid: 0 for aid in ids}
    last_change = {aid: -10 ** 9 for aid in ids}
    events = {}
    for aid, evs in toggles.items():
        for frame, on in evs:
            events.setdefault(frame, []).append((aid, on))

    on_samples = {aid: [] for aid in ids}
    off_samples = {aid: [] for aid in ids}

    for f in frames:
        for aid, on in events.get(f.index, []):
            if state[aid] != on:
                state[aid] = on
                last_change[aid] = f.index
        if f.index <= s.warmup_frames:
            # Carga de nivel: no es gameplay y no se compara con nada.
            continue
        if f.deep or f.dropped:
            # Un frame deep mide con toda la instrumentacion encima, y uno con
            # eventos perdidos no es comparable. Ninguno de los dos entra.
            continue
        for aid in ids:
            # La accion medida tiene que ser la unica prendida.
            if any(state[o] and o != aid for o in ids):
                continue
            if f.index - last_change[aid] < settle:
                continue
            (on_samples if state[aid] else off_samples)[aid].append(f.cpu_ms)

    out = []
    for aid in ids:
        st = compare(on_samples[aid], off_samples[aid])
        out.append(ABResult(
            action_id=aid, title=f"accion #{aid}",
            n_on=len(on_samples[aid]), n_off=len(off_samples[aid]),
            captures=_captures(s, aid), **st))
    out.sort(key=lambda r: r.delta_ms, reverse=True)
    return out


def _captures(s: Session, aid: int) -> List[str]:
    """Las capturas PNG que el DLL dejo al lado de la sesion, si estan.

    El harness saca la misma escena con y sin la optimizacion; el reporte las
    linkea para que el costo visual se evalue mirando, que es la unica forma.
    """
    if not s.path:
        return []
    base = os.path.splitext(s.path)[0]
    found = []
    for suffix in ("on", "off"):
        p = f"{base}-a{aid}-{suffix}.png"
        if os.path.exists(p):
            found.append(p)
    return found


def render(s: Session, results: List[ABResult]) -> str:
    L = [f"# gpuprobe A/B -- {s.exe or 'sesion'}", "",
         f"Frametime base: {s.frame_ms:.2f} ms de mediana sobre "
         f"{len(s.frames)} frames.", "",
         "Cada candidato se prendio y apago alternando durante la misma "
         "corrida, asi que las dos condiciones vieron el mismo drift de clocks "
         "y la misma escena. Estas ganancias son **medidas**.", "",
         "| candidato | ON | OFF | ganancia | IC 95% | p | n | veredicto |",
         "| --- | ---: | ---: | ---: | --- | ---: | ---: | --- |"]
    for r in results:
        L.append(f"| {r.title} | {r.median_on:.2f} ms | {r.median_off:.2f} ms | "
                 f"{r.delta_ms:+.2f} ms | [{r.ci_lo:+.2f}, {r.ci_hi:+.2f}] | "
                 f"{r.p_value:.2g} | {r.n_on}/{r.n_off} | **{r.verdict}** |")
    L.append("")
    for r in results:
        if r.captures:
            L.append(f"### {r.title} -- costo visual")
            L.append("")
            for c in r.captures:
                L.append(f"- `{c}`")
            L.append("")
    descartados = [r for r in results if not r.significant]
    if descartados:
        L.append("## Descartados")
        L.append("")
        for r in descartados:
            L.append(f"- {r.title}: {r.delta_ms:+.2f} ms con el intervalo en "
                     f"[{r.ci_lo:+.2f}, {r.ci_hi:+.2f}] y ruido frame a frame "
                     f"de {r.noise_ms:.2f} ms -- no se distingue")
        L.append("")
    return "\n".join(L)
