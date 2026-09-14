"""El reporte en markdown y el perfil TOML que sale de una sesion.

Regla de escritura del reporte: cada numero dice de donde salio y si es medido
o estimado. Un reporte que mezcla las dos cosas es como termina alguien
creyendo que gano 2 ms porque una cuenta sobre pixeles decia que podia.
"""

from typing import List, Optional

from .model import Session
from .rules import Candidate, Thresholds, hidden_passes


def _mb(b: int) -> str:
    return f"{b / (1024.0 * 1024.0):.0f} MB"


def render(s: Session, cands: List[Candidate],
           th: Optional[Thresholds] = None) -> str:
    th = th or Thresholds()
    L: List[str] = []
    a = L.append

    a(f"# gpuprobe -- {s.exe or 'sesion'}")
    a("")
    a(f"- adaptador: {s.adapter or '?'}")
    a(f"- salida: {s.out_w}x{s.out_h}")
    a(f"- frames analizados: {len(s.frames)} "
      f"(los primeros {s.warmup_frames} se descartan como carga)")
    a(f"- frametime: {s.frame_ms:.2f} ms de mediana")
    a(f"- GPU en la queue principal: {s.gpu_ms:.2f} ms por frame")
    if s.bad_lines:
        a(f"- lineas ilegibles en el JSONL: {s.bad_lines} "
          f"(sesion cortada o eventos descartados por el ring)")
    peak = s.vram_peak()
    if peak:
        a(f"- VRAM: pico de {_mb(peak.usage)} sobre un budget de "
          f"{_mb(peak.budget)} ({peak.pressure * 100:.0f}%)")
    a("")

    if s.thrashing():
        a("## El techo esta en la VRAM, no en las pasadas")
        a("")
        a("El juego pidio mas memoria de la que el adaptador le presupuesta. "
          "Mientras eso pase, el driver pagina por PCIe y **todos los ms por "
          "pasada de esta sesion miden paginacion**, no el costo real de cada "
          "pasada. Los candidatos de mas abajo quedan marcados como no "
          "confiables: primero hay que bajar del techo y volver a medir.")
        a("")

    # --- el costo del frame ---------------------------------------------
    a("## A donde se va el frame")
    a("")
    a("| pasada | ms | exclusivos | % del frame | draws | RT |")
    a("| --- | ---: | ---: | ---: | ---: | --- |")
    passes = s.pass_list()
    shown = passes[:th.top_n]
    for p in shown:
        r = s.resource_for_pass(p)
        share = (p.ms / s.frame_ms * 100.0) if s.frame_ms else 0.0
        rt = f"{r.cat} {r.w}x{r.h} {r.fmt}" if r else "?"
        excl = f"{p.exclusive_ms:.2f}"
        if p.overlapped:
            excl += " (tapada)"
        a(f"| `{p.key:016x}` | {p.ms:.2f} | {excl} | {share:.1f}% | "
          f"{p.draws} | {rt} |")
    rest = passes[th.top_n:]
    if rest:
        rest_ms = sum(p.ms for p in rest)
        a(f"| _{len(rest)} pasadas mas_ | {rest_ms:.2f} | | "
          f"{rest_ms / s.frame_ms * 100.0 if s.frame_ms else 0:.1f}% | | "
          f"_ruido: ninguna llega al umbral_ |")
    a("")

    hidden = hidden_passes(s, th)
    if hidden:
        a("### Pasadas tapadas por otra queue (optimizarlas no gana nada)")
        a("")
        for p in hidden:
            r = s.resource_for_pass(p)
            a(f"- `{p.key:016x}` cuesta {p.ms:.2f} ms pero corre entera "
              f"adentro del trabajo de otra queue "
              f"({p.exclusive_ms:.2f} ms exclusivos)"
              + (f" -- {r.cat} {r.w}x{r.h}" if r else ""))
        a("")

    # --- candidatos -------------------------------------------------------
    a("## Candidatos")
    a("")
    real = [c for c in cands if c.gain_ms >= th.min_gain_ms or c.hitch_ms > 0
            or c.priority > 0]
    if not real:
        a("Ninguno supera el umbral. La sesion no tiene pasadas mal "
          "dimensionadas que valga la pena tocar.")
        a("")
    total = sum(c.gain_ms for c in real if not c.blocked)
    if total > 0:
        a(f"Suma de las ganancias **estimadas**: {total:.2f} ms sobre un frame "
          f"de {s.frame_ms:.2f} ms. Esto todavia no es una medicion -- para "
          f"eso esta el harness A/B, que prende y apaga cada candidato cada "
          f"pocos frames y descarta lo que no supere el ruido.")
        a("")

    for c in real:
        mark = " (no confiable: sesion en thrashing)" if c.blocked else ""
        head = f"### {c.title}{mark}"
        a(head)
        a("")
        if c.hitch_ms > 0:
            a(f"- **saca un hitch de {c.hitch_ms:.1f} ms** (no baja el "
              f"frametime medio)")
        else:
            a(f"- ganancia estimada: **{c.gain_ms:.2f} ms** "
              f"({c.gain_ms / s.frame_ms * 100.0 if s.frame_ms else 0:.1f}% "
              f"del frame)")
        a(f"- riesgo visual: {c.visual_risk}")
        a(f"- estabilidad frente a parches: {c.stability}")
        a(f"- cuenta: {c.gain_basis}")
        for e in c.evidence:
            a(f"- {e}")
        if c.action_toml:
            a("")
            a("```toml")
            a(c.action_toml)
            a("```")
        a("")

    a("---")
    a("")
    a("Las ganancias de este reporte son **estimadas**. El numero que vale "
      "sale de `gpuprobe ab`, que mide el juego con cada candidato prendido y "
      "apagado alternando cada pocos frames y aplica un test de significancia.")
    return "\n".join(L)


def profile_toml(s: Session, cands: List[Candidate]) -> str:
    """El perfil con todos los candidatos, apagados y marcados para A/B."""
    L = [f"# Perfil generado por gpuprobe para {s.exe or 'este juego'}.",
         "# Todo nace APAGADO. Prendelo desde el overlay, o corre el harness",
         "# A/B (ab = true) y dejalo prendido solo si la ganancia se mide.",
         "",
         "[gpuprobe]"]
    if s.exe:
        L.append(f'exe = "{s.exe}"')
    L.append("observe_only = true   # pasar a false para que el ejecutor actue")
    L.append("deep_every = 120")
    L.append("query_budget = 64")
    L.append("ab_period = 60")
    L.append("ab_warmup = 5")
    L.append("")
    for c in cands:
        if not c.action_toml:
            continue
        L.append(f"# {c.title}")
        L.append(f"#   ganancia estimada {c.gain_ms:.2f} ms | riesgo visual "
                 f"{c.visual_risk} | estabilidad {c.stability}")
        if c.blocked:
            L.append("#   OJO: medido con la sesion en thrashing de VRAM")
        L.append(c.action_toml)
        L.append("")
    return "\n".join(L)
