"""El modelo de una sesion, ya agregado sobre todos los frames.

Lo que entra son eventos sueltos; lo que sale de aca es "esta pasada costo
2.4 ms de mediana en 880 frames y escribe sobre ESTE recurso". La mediana y no
el promedio, por lo mismo que en core/stats.h: un hitch de 40 ms mueve el
promedio y no dice nada de la pasada.
"""

from dataclasses import dataclass, field
from statistics import median
from typing import Dict, List, Optional


@dataclass
class Resource:
    key: int
    dkey: int
    cat: str
    fmt: str
    fmt_id: int
    w: int
    h: int
    d: int
    mips: int
    samples: int
    flags: int
    heap: str
    bytes: int
    site: int
    first_frame: int
    # Lo que se observo sobre el recurso a lo largo de la sesion. Es lo que
    # despues decide si una accion es segura de aplicar.
    states_written: set = field(default_factory=set)
    states_read: set = field(default_factory=set)
    was_copy_src: bool = False
    was_copy_dst: bool = False

    @property
    def megabytes(self) -> float:
        return self.bytes / (1024.0 * 1024.0)

    @property
    def square(self) -> bool:
        return self.w == self.h

    def never_read(self) -> bool:
        """Escrito alguna vez y nunca transicionado a un estado de lectura.

        En D3D12 para leer una textura en un shader hay que transicionarla a
        *_SHADER_RESOURCE si o si, asi que la ausencia de esa transicion en
        toda la sesion es evidencia fuerte, no una heuristica.
        """
        return bool(self.states_written) and not self.states_read and not self.was_copy_src


@dataclass
class PassAgg:
    key: int
    rt_key: int
    rt_dkey: int
    ordinal: int
    queue: int
    rt_w: int
    rt_h: int
    rt_bytes: int
    draws: int = 0
    samples: List[float] = field(default_factory=list)       # ms, frames light
    deep_samples: List[float] = field(default_factory=list)  # ms, frames deep
    exclusive: List[float] = field(default_factory=list)     # ms sin tapar por otra queue
    frames_seen: int = 0

    @property
    def ms(self) -> float:
        if self.samples:
            return median(self.samples)
        if self.deep_samples:
            return median(self.deep_samples)
        return 0.0

    @property
    def exclusive_ms(self) -> float:
        """Los ms de la pasada que NO estan tapados por otra queue.

        Es el techo real de lo que se puede ganar optimizandola: si una pasada
        de compute de 0.8 ms corre entera adentro de la de sombras, bajarla a
        cero no acorta el frame ni un microsegundo. La ganancia estimada de
        cualquier candidato se limita con este numero.
        """
        if self.exclusive:
            return median(self.exclusive)
        return self.ms

    @property
    def overlapped(self) -> bool:
        """Mas de la mitad de la pasada corre tapada por otra queue."""
        return self.ms > 0 and self.exclusive_ms < self.ms * 0.5


@dataclass
class BarrierAgg:
    res_key: int
    before: int
    after: int
    count: int = 0
    redundant_same_state: int = 0
    common_roundtrip: int = 0


@dataclass
class PsoCompile:
    key: int
    frame: int
    compile_ms: float
    render_thread: bool
    bytes: int


@dataclass
class FrameAgg:
    index: int
    cpu_ms: float
    present_ms: float
    dropped: int
    deep: bool


@dataclass
class VramSample:
    frame: int
    budget: int
    usage: int
    committed: int

    @property
    def over_budget(self) -> bool:
        return self.budget > 0 and self.usage > self.budget

    @property
    def pressure(self) -> float:
        return self.usage / self.budget if self.budget else 0.0


@dataclass
class Session:
    exe: str = ""
    adapter: str = ""
    out_w: int = 0
    out_h: int = 0
    vram_budget: int = 0
    schema: int = 0
    version: str = ""
    path: str = ""
    resources: Dict[int, Resource] = field(default_factory=dict)      # por key
    by_dkey: Dict[int, List[Resource]] = field(default_factory=dict)
    passes: Dict[int, PassAgg] = field(default_factory=dict)
    barriers: Dict[tuple, BarrierAgg] = field(default_factory=dict)
    psos: List[PsoCompile] = field(default_factory=list)
    frames: List[FrameAgg] = field(default_factory=list)
    vram: List[VramSample] = field(default_factory=list)
    # Conmutaciones del harness A/B: id de accion -> [(frame, on)].
    toggles: Dict[int, List[tuple]] = field(default_factory=dict)
    bad_lines: int = 0
    warmup_frames: int = 0

    # --- resumenes ------------------------------------------------------
    @property
    def frame_ms(self) -> float:
        vals = [f.cpu_ms for f in self.frames if not f.deep]
        if not vals:
            vals = [f.cpu_ms for f in self.frames]
        return median(vals) if vals else 0.0

    @property
    def gpu_ms(self) -> float:
        """Lo que la GPU estuvo ocupada en la queue principal, por frame."""
        return sum(p.ms for p in self.passes.values() if p.queue == 0)

    def pass_list(self) -> List[PassAgg]:
        return sorted(self.passes.values(), key=lambda p: p.ms, reverse=True)

    def resource_for_pass(self, p: PassAgg) -> Optional[Resource]:
        """El recurso sobre el que escribe la pasada, por INSTANCIA.

        Por dkey no alcanza: dos recursos distintos con el mismo descriptor
        comparten clave de clase, y atribuirle a uno el costo del otro es como
        empieza un reporte que propone apagar la pasada equivocada.
        """
        r = self.resources.get(p.rt_key)
        if r is not None:
            return r
        lst = self.by_dkey.get(p.rt_dkey)
        return lst[0] if lst else None

    def siblings(self, r: Resource) -> List[Resource]:
        """Los recursos que comparten descriptor con este.

        Si hay mas de uno, NINGUN matcher por descriptor puede apuntarle a uno
        solo, y toda accion sobre el toca a todos. El reporte tiene que decirlo.
        """
        return [x for x in self.by_dkey.get(r.dkey, []) if x.key != r.key]

    def vram_peak(self) -> Optional[VramSample]:
        return max(self.vram, key=lambda v: v.usage) if self.vram else None

    def thrashing(self) -> bool:
        """El juego pide mas VRAM de la que el adaptador le presupuesta.

        Cuando esto pasa, el driver empieza a paginar por PCIe y TODOS los ms
        por pasada miden paginacion. Optimizar una pasada en ese estado es
        perder el tiempo: el techo esta en otro lado.
        """
        return any(v.over_budget for v in self.vram)
