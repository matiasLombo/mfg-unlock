"""Las reglas: de la sesion a candidatos accionables.

Cada candidato lleva cuatro cosas, y las cuatro importan tanto como la primera:

- ganancia ESTIMADA en ms, con la cuenta que la produjo escrita al lado
  (gain_basis). Una ganancia sin su cuenta no se puede discutir.
- riesgo visual: bajo / medio / alto.
- estabilidad frente a parches del juego: alta / media / baja. Un matcher por
  descriptor de un recurso de tamano fijo sobrevive updates; uno por ordinal de
  pasada se rompe con el primer parche que agregue una pasada antes.
- el TOML listo para pegar en el perfil.

Dos limites duros que se aplican ANTES que cualquier regla:

1. Si la sesion esta en thrashing de VRAM, todo lo demas es ruido. Los ms por
   pasada estan midiendo paginacion por PCIe, no el costo real de la pasada.
2. La ganancia de una pasada nunca puede superar su tiempo EXCLUSIVO: lo que
   no esta tapado por otra queue. Una pasada de compute que corre entera
   adentro de la de sombras puede costar 0.8 ms y valer cero.
"""

from dataclasses import dataclass, field
from typing import List, Optional

from .model import PassAgg, Resource, Session

RISK_LOW, RISK_MED, RISK_HIGH = "bajo", "medio", "alto"
STAB_HIGH, STAB_MED, STAB_LOW = "alta", "media", "baja"


@dataclass
class Thresholds:
    min_gain_ms: float = 0.15      # menos que esto no se distingue del ruido
    min_share: float = 0.01        # 1% del frame
    shadow_min_dim: int = 2048     # el enunciado del proyecto
    post_max_draws: int = 8        # una pasada de post es un quad, no geometria
    top_n: int = 12
    barrier_ms_each: float = 0.01  # regla de pulgar, marcada como tal
    warmup: int = 120


@dataclass
class Candidate:
    cid: str
    title: str
    kind: str                    # el kind de accion del perfil, o "informe"
    gain_ms: float
    gain_basis: str
    visual_risk: str
    stability: str
    evidence: List[str] = field(default_factory=list)
    action_toml: str = ""
    hitch_ms: float = 0.0        # para lo que saca un hitch en vez de bajar ms
    blocked: bool = False        # la sesion esta en thrashing: primero eso
    priority: int = 0            # 1 = va primero pase lo que pase (el techo)
    dkey: int = 0
    pass_key: int = 0

    @property
    def sort_key(self):
        # El hitch pesa menos que los ms de frametime pero no cero: sacar un
        # hitch de 14 ms importa mas que 0.02 ms de barriers.
        return (self.priority, self.gain_ms + self.hitch_ms * 0.1, self.gain_ms)


def _share(ms: float, frame_ms: float) -> float:
    return ms / frame_ms if frame_ms > 0 else 0.0


def _fmt_mb(b: int) -> str:
    return f"{b / (1024.0 * 1024.0):.1f} MB"


def _ambiguity(s: Session, r) -> List[str]:
    """Aviso cuando el matcher por descriptor no puede apuntar a uno solo.

    Es la contracara de matchear por descriptor: sobrevive parches del juego,
    pero identifica una CLASE. Si el juego tiene tres RTs RGBA16F full-res, la
    regla los toca a los tres. Decirlo es la diferencia entre una accion y una
    sorpresa.
    """
    sibs = s.siblings(r)
    if not sibs:
        return []
    return [f"OJO: {len(sibs)} recurso(s) mas comparten este descriptor "
            f"({r.fmt} {r.w}x{r.h}); la accion los toca a todos. Revisar en la "
            f"tabla de pasadas cuales son antes de prenderla"]


def _toml(kind: str, name: str, match: str, extra: str = "",
          verify: str = "full") -> str:
    lines = ["[[action]]", f'name = "{name}"', f'kind = "{kind}"',
             f"match = {{ {match} }}"]
    if extra:
        lines.append(extra)
    lines.append(f'verify = "{verify}"')
    lines.append("enabled = false   # prendela con el overlay o con ab = true")
    lines.append("ab = true")
    return "\n".join(lines)


# --- regla 1: sombras y cubemaps sobredimensionados ----------------------

def rule_oversized_shadows(s: Session, th: Thresholds) -> List[Candidate]:
    out = []
    for p in s.pass_list():
        r = s.resource_for_pass(p)
        if r is None or r.cat not in ("shadowmap", "shadowcube", "cubemap"):
            continue
        if max(r.w, r.h) <= th.shadow_min_dim:
            continue
        excl = p.exclusive_ms
        if excl < th.min_gain_ms or _share(excl, s.frame_ms) < th.min_share:
            continue
        # Bajar a la mitad deja 1/4 de los pixeles. Una pasada de sombras es
        # casi toda rasterizacion y test de profundidad, pero el costo de
        # vertices y de setup por draw NO baja con la resolucion: 0.85 es el
        # descuento por esa parte que no escala.
        fill = 0.85
        gain = excl * 0.75 * fill
        half = max(r.w // 2, 512)
        out.append(Candidate(
            cid=f"shadow_{r.dkey:x}",
            title=f"{r.cat} {r.w}x{r.h}" + (f" x{r.d}" if r.d > 1 else "") +
                  f" a {half}x{half}",
            kind="resource_scale",
            gain_ms=gain,
            gain_basis=(f"{excl:.2f} ms exclusivos x 0.75 (1/4 de los pixeles) "
                        f"x 0.85 (lo que no escala: vertices y setup)"),
            visual_risk=RISK_MED,
            stability=STAB_HIGH,
            evidence=[
                f"{p.draws} draws por frame sobre un {r.fmt} de {r.w}x{r.h}"
                + (f" en array de {r.d}" if r.d > 1 else ""),
                f"{p.ms:.2f} ms de mediana, {_share(p.ms, s.frame_ms) * 100:.1f}% del frame",
                f"{_fmt_mb(r.bytes)} de VRAM" +
                (f"; {excl:.2f} ms no tapados por otra queue" if p.overlapped else ""),
                "el matcher es por descriptor de tamano fijo: sobrevive parches del juego",
            ] + _ambiguity(s, r),
            action_toml=_toml(
                "resource_scale",
                f"{r.cat} {r.w} a la mitad",
                f'category = "{r.cat}", min_w = {r.w}, format = "{r.fmt}"',
                "scale = 0.5"),
            dkey=r.dkey, pass_key=p.key))
    return out


# --- regla 2: RTs full-res en pasadas de post ---------------------------

def rule_fullres_post(s: Session, th: Thresholds) -> List[Candidate]:
    out = []
    passes = s.pass_list()
    max_ord = max((p.ordinal for p in passes), default=0)
    for p in passes:
        r = s.resource_for_pass(p)
        if r is None or r.cat not in ("rt_full", "volume"):
            continue
        if p.draws > th.post_max_draws:
            continue  # geometria, no post: bajar su RT cambia la imagen entera
        excl = p.exclusive_ms
        if excl < th.min_gain_ms or _share(excl, s.frame_ms) < th.min_share:
            continue
        # Una pasada de post es fill puro: 4 vertices y un shader por pixel.
        gain = excl * 0.75 * 0.95
        # Las ultimas pasadas del frame son las que el usuario MIRA (tonemap,
        # UI, composicion final). Bajarles la resolucion se ve siempre.
        late = max_ord > 0 and p.ordinal >= max_ord * 0.8
        ambiguous = bool(s.siblings(r))
        risk = RISK_HIGH if late else (
            RISK_LOW if r.fmt.startswith(("R8_", "R16_")) and not ambiguous
            else RISK_MED)
        out.append(Candidate(
            cid=f"post_{p.key:x}",
            title=f"pasada de post en {r.fmt} {r.w}x{r.h} a media resolucion",
            kind="resource_scale",
            gain_ms=gain,
            gain_basis=(f"{excl:.2f} ms exclusivos x 0.75 (1/4 de los pixeles) "
                        f"x 0.95 (fill puro: 4 vertices y un shader por pixel)"),
            visual_risk=risk,
            stability=STAB_HIGH if not ambiguous else STAB_MED,
            evidence=[
                f"{p.draws} draw(s) sobre un RT full-res: es post, no geometria",
                f"{p.ms:.2f} ms de mediana, {_share(p.ms, s.frame_ms) * 100:.1f}% del frame",
                f"{_fmt_mb(r.bytes)} de VRAM en el RT",
                ("pasada tardia del frame: puede ser la imagen final o la UI"
                 if late else "el matcher va por clase de escala: vale a 1440p y a 2160p"),
            ] + _ambiguity(s, r),
            action_toml=_toml(
                "resource_scale",
                f"post {r.fmt} a media",
                f'category = "{r.cat}", format = "{r.fmt}"',
                "scale = 0.5",
                verify="full"),
            dkey=r.dkey, pass_key=p.key))
    return out


# --- regla 3: recursos creados y nunca leidos ---------------------------

def rule_never_read(s: Session, th: Thresholds) -> List[Candidate]:
    out = []
    for r in s.resources.values():
        if not r.never_read():
            continue
        # Las pasadas que escriben sobre EL, por instancia. Por descriptor
        # sumaria el costo de sus hermanos y la ganancia saldria inflada.
        writers = [p for p in s.passes.values() if p.rt_key == r.key]
        gain = sum(p.exclusive_ms for p in writers)
        # Si comparte descriptor con otro recurso, el matcher del perfil no
        # puede apuntarle a el solo: la accion tocaria a los dos.
        sibs = s.siblings(r)
        out.append(Candidate(
            cid=f"orphan_{r.dkey:x}",
            title=f"{r.fmt} {r.w}x{r.h} se escribe y nunca se lee",
            kind="skip_pass",
            gain_ms=gain,
            gain_basis=(f"{gain:.2f} ms de las {len(writers)} pasada(s) que "
                        f"escriben sobre un recurso que nadie lee"),
            visual_risk=RISK_LOW if not sibs else RISK_HIGH,
            stability=STAB_MED if not sibs else STAB_LOW,
            evidence=([
                f"OJO: hay {len(sibs)} recurso(s) mas con el MISMO descriptor "
                f"({r.fmt} {r.w}x{r.h}); un matcher por descriptor los toca a "
                f"todos, no solo a este. Sin una forma de distinguirlos, esta "
                f"accion NO se puede aplicar tal cual"] if sibs else []) + [
                "en toda la sesion nunca se transiciono a un estado de lectura",
                "en D3D12 leer una textura en un shader EXIGE esa transicion, "
                "asi que esto es evidencia, no heuristica",
                f"{_fmt_mb(r.bytes)} de VRAM que se liberan",
                "revisar igual que no lo lea un frame que no grabamos (menus, "
                "cinematicas): la sesion cubre lo que se jugo",
            ],
            action_toml=_toml(
                "skip_pass", f"pasada huerfana sobre {r.fmt} {r.w}x{r.h}",
                f'category = "{r.cat}", format = "{r.fmt}", '
                f"min_w = {r.w}, max_w = {r.w}",
                verify="none"),
            dkey=r.dkey))
    return out


# --- regla 4: barriers redundantes --------------------------------------

def rule_redundant_barriers(s: Session, th: Thresholds) -> List[Candidate]:
    frames = max(1, len([f for f in s.frames if f.index > th.warmup]))
    same_state = 0
    roundtrips = 0
    detail = []
    for b in s.barriers.values():
        if b.redundant_same_state:
            same_state += b.redundant_same_state
            detail.append(f"transicion al estado en el que ya estaba "
                          f"(0x{b.before:x}) x{b.redundant_same_state}")
        if b.common_roundtrip:
            roundtrips += b.common_roundtrip
            detail.append(f"ida y vuelta por COMMON desde 0x{b.before:x} "
                          f"x{b.common_roundtrip}")
    total = same_state + roundtrips
    if total == 0:
        return []
    per_frame = total / frames
    gain = per_frame * th.barrier_ms_each
    out = [Candidate(
        cid="barriers",
        title=f"{per_frame:.1f} barriers redundantes por frame",
        kind="barrier_filter",
        gain_ms=gain,
        gain_basis=(f"{per_frame:.1f} barriers/frame x {th.barrier_ms_each} ms "
                    f"cada uno -- REGLA DE PULGAR, no medido: el costo real de "
                    f"un barrier depende de si fuerza descompresion. Confirmar "
                    f"con el harness A/B antes de creerle"),
        visual_risk=RISK_LOW,
        stability=STAB_MED,
        evidence=detail[:6] + [
            f"{total} en {frames} frames",
            "una transicion a COMMON puede forzar descompresion del render "
            "target; volver a entrar cuesta de nuevo",
        ],
        action_toml=_toml(
            "barrier_filter", "sacar la vuelta por COMMON",
            'from = "PIXEL_SHADER_RESOURCE", to = "COMMON"',
            verify="none"))]
    return out


# --- regla 5: PSOs compilados en gameplay -------------------------------

def rule_pso_stutter(s: Session, th: Thresholds) -> List[Candidate]:
    late = [p for p in s.psos if p.frame > th.warmup]
    if not late:
        return []
    on_render = [p for p in late if p.render_thread]
    worst = max(late, key=lambda p: p.compile_ms)
    total = sum(p.compile_ms for p in late)
    return [Candidate(
        cid="pso_stutter",
        title=f"{len(late)} PSO(s) compilados durante el gameplay",
        kind="informe",
        gain_ms=0.0,   # no baja el frametime medio, y decirlo asi es el punto
        hitch_ms=worst.compile_ms,
        gain_basis=(f"no baja el frametime medio: saca un hitch de "
                    f"{worst.compile_ms:.1f} ms (peor caso) y {total:.1f} ms "
                    f"repartidos en {len(late)} compilaciones"),
        visual_risk=RISK_LOW,
        stability=STAB_LOW,
        evidence=[
            f"{len(on_render)} de {len(late)} compilaron EN EL HILO DE RENDER: "
            f"el frame se frena hasta que termina el driver",
            f"peor caso {worst.compile_ms:.1f} ms en el frame {worst.frame}",
            "gpuprobe no puede precompilar por el juego: lo que puede es "
            "decir exactamente cuales y en que frame, para que se fuerce el "
            "shader cache o se toquen las opciones que los crean antes",
        ],
        action_toml="")]


# --- regla 6: el techo de VRAM ------------------------------------------

def rule_vram_ceiling(s: Session, th: Thresholds) -> List[Candidate]:
    peak = s.vram_peak()
    if peak is None:
        return []
    if not s.thrashing():
        # Cerca del techo tambien vale decirlo: a 95% cualquier accion que
        # agregue memoria lo cruza.
        if peak.pressure < 0.95:
            return []
        return [Candidate(
            cid="vram_near",
            title=f"VRAM al {peak.pressure * 100:.0f}% del budget",
            kind="informe",
            gain_ms=0.0,
            gain_basis="no es una ganancia: es un margen que se esta agotando",
            visual_risk=RISK_LOW,
            stability=STAB_HIGH,
            evidence=[
                f"pico de {_fmt_mb(peak.usage)} sobre un budget de "
                f"{_fmt_mb(peak.budget)} (frame {peak.frame})",
                "cualquier accion que agregue VRAM cruza el techo desde aca",
            ])]

    over = peak.usage - peak.budget
    return [Candidate(
        cid="vram_thrashing",
        title=f"falso GPU-bound: {_fmt_mb(over)} por encima del budget",
        kind="mip_bias",
        priority=1,
        gain_ms=0.0,
        gain_basis=("no estimable desde la sesion: mientras el driver pagina "
                    "por PCIe, TODOS los ms por pasada miden paginacion y no "
                    "el costo real de la pasada"),
        visual_risk=RISK_MED,
        stability=STAB_HIGH,
        evidence=[
            f"pico de {_fmt_mb(peak.usage)} contra un budget de "
            f"{_fmt_mb(peak.budget)} en el frame {peak.frame}",
            "bajar la calidad de texturas del juego un escalon suele ser mejor "
            "que cualquier mip_bias que podamos aplicar desde afuera",
            "hasta que esto se arregle, el resto de los candidatos de esta "
            "sesion no son confiables: estan medidos bajo thrashing",
        ],
        action_toml=_toml("mip_bias", "sesgar mips de los atlas grandes",
                          'category = "texture", min_w = 2048',
                          "mip_bias = 1", verify="none"))]


# --- el motor -----------------------------------------------------------

ALL_RULES = [
    rule_vram_ceiling,
    rule_oversized_shadows,
    rule_fullres_post,
    rule_never_read,
    rule_redundant_barriers,
    rule_pso_stutter,
]


def analyze(s: Session, th: Optional[Thresholds] = None) -> List[Candidate]:
    th = th or Thresholds()
    cands: List[Candidate] = []
    for rule in ALL_RULES:
        cands.extend(rule(s, th))

    # El techo de VRAM manda sobre todo lo demas. No se ocultan los otros
    # candidatos -- se marcan, y el reporte dice por que no hay que creerles.
    if s.thrashing():
        for c in cands:
            if c.cid != "vram_thrashing":
                c.blocked = True

    cands.sort(key=lambda c: c.sort_key, reverse=True)
    return cands


def hidden_passes(s: Session, th: Optional[Thresholds] = None) -> List[PassAgg]:
    """Pasadas que corren tapadas por otra queue: optimizarlas no gana nada.

    Van al reporte como NO-candidatos, con su costo al lado. Es la informacion
    que evita que alguien las 'optimice' y despues no entienda por que el
    frametime no se movio.
    """
    th = th or Thresholds()
    return [p for p in s.pass_list()
            if p.overlapped and p.ms >= th.min_gain_ms]
