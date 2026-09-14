"""Verificacion del contrato. Ver contract.sh."""

import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "analyzer"))

from gpuprobe_analyzer import ab, ingest, report, rules  # noqa: E402

fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print(f"  FALLO {msg}")


def main(outdir, ab_exe=None):
    # --- 1. la sesion normal: los cinco defectos deliberados aparecen -----
    s = ingest.load(os.path.join(outdir, "sesion.jsonl"))
    check(s.schema == 1, "el analizador no leyo el esquema del writer de C++")
    check(s.bad_lines == 0, f"{s.bad_lines} lineas que el writer emitio y el "
                            f"lector no entiende")
    check(len(s.frames) == 900, f"frames leidos: {len(s.frames)}")
    check(len(s.resources) == 13, f"recursos leidos: {len(s.resources)}")
    check(len(s.passes) == 10, f"pasadas leidas: {len(s.passes)}")

    cands = rules.analyze(s)
    by = {c.cid.split("_")[0] for c in cands}
    check("shadow" in by, "no encontro el shadow map de 4096")
    check("post" in by, "no encontro los RTs full-res de post")
    check("orphan" in by, "no encontro el recurso que se escribe y nunca se lee")
    check("barriers" in by, "no encontro los barriers redundantes")
    check(any(c.cid == "pso_stutter" for c in cands),
          "no encontro los PSOs compilados en gameplay")

    shadow = [c for c in cands if c.cid.startswith("shadow_")][0]
    check(shadow.gain_ms > 0.9, f"ganancia del shadow demasiado baja: "
                                f"{shadow.gain_ms:.2f}")
    check('category = "shadowmap"' in shadow.action_toml,
          "el TOML del shadow no matchea por categoria")

    # La pasada de compute corre tapada: tiene que salir como NO-candidato.
    hidden = rules.hidden_passes(s)
    check(len(hidden) == 1, f"pasadas tapadas detectadas: {len(hidden)}")

    # El reporte y el perfil se generan sin explotar y el perfil es TOML valido
    # para el parser de C++ (eso lo prueba test_profile; aca solo que exista).
    md = report.render(s, cands)
    check("## Candidatos" in md, "el reporte no tiene seccion de candidatos")
    check("estimada" in md, "el reporte no aclara que la ganancia es estimada")
    prof = report.profile_toml(s, cands)
    check("[[action]]" in prof, "el perfil generado no tiene acciones")
    check("enabled = false" in prof, "el perfil generado trae algo prendido")

    # --- 2. thrashing: manda sobre todo lo demas -------------------------
    st = ingest.load(os.path.join(outdir, "thrash.jsonl"))
    check(st.thrashing(), "no detecto el thrashing de VRAM")
    ct = rules.analyze(st)
    check(ct[0].cid == "vram_thrashing", "el techo de VRAM no quedo primero")
    check(all(c.blocked for c in ct if c.cid != "vram_thrashing"),
          "hay candidatos no marcados bajo una sesion en thrashing")

    # --- 3. A/B: la ganancia inyectada se recupera -----------------------
    sab = ingest.load(os.path.join(outdir, "ab.jsonl"))
    res = ab.measure(sab)
    check(len(res) == 1, f"acciones medidas: {len(res)}")
    if res:
        r = res[0]
        check(r.significant, "no detecto la ganancia inyectada de 0.9 ms")
        check(abs(r.delta_ms - 0.9) < 0.1,
              f"ganancia medida {r.delta_ms:.3f} ms, esperada 0.9")

    # --- 4. C++ y Python tienen que dar el MISMO veredicto ---------------
    exe = ab_exe or os.path.join(outdir, "ab_check")
    if not os.path.exists(exe):
        # En Windows el binario lo compila CMake con otro nombre y con .exe.
        for cand in (os.path.join(outdir, "ab_check.exe"),
                     os.path.join(outdir, "gpuprobe-ab-check.exe"),
                     os.path.join(outdir, "Release", "gpuprobe-ab-check.exe"),
                     os.path.join("build", "Release", "gpuprobe-ab-check.exe")):
            if os.path.exists(cand):
                exe = cand
                break
    if not os.path.exists(exe):
        check(False, f"no encontre el binario ab_check (probe {exe})")
        print(f"contrato: {len(fails)} FALLOS")
        return 1
    for args in (("7.0", "8.0", "400", "3", "4"),
                 ("8.0", "8.0", "400", "11", "12"),
                 ("9.0", "8.0", "300", "5", "6")):
        out = subprocess.run([exe, *args], capture_output=True, text=True,
                             check=True).stdout
        cpp = dict(line.split() for line in out.strip().splitlines())
        base_on, base_off, n, s_on, s_off = args
        py = ab.compare(_synth(float(base_on), int(n), int(s_on)),
                        _synth(float(base_off), int(n), int(s_off)))
        for key in ("median_on", "median_off", "delta_ms", "ci_lo", "ci_hi"):
            check(abs(float(cpp[key]) - py[key]) < 1e-9,
                  f"{key}: C++ {cpp[key]} vs Python {py[key]} ({args})")
        check((cpp["significant"] == "1") == py["significant"],
              f"veredicto distinto entre C++ y Python ({args})")
        check(abs(float(cpp["p_value"]) - py["p_value"]) < 1e-9,
              f"p distinto: C++ {cpp['p_value']} vs Python {py['p_value']}")

    if fails:
        print(f"contrato: {len(fails)} FALLOS")
        return 1
    print("contrato: writer C++ -> JSONL -> analizador, y stats.h == ab.py")
    return 0


def _synth(base, n, seed, jitter=0.35):
    rng = ab._Rng(seed)
    out = []
    for _ in range(n):
        u = (rng.next() % 10000) / 10000.0
        out.append(base + (u - 0.5) * 2.0 * jitter)
    return out


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp",
                  sys.argv[2] if len(sys.argv) > 2 else None))
