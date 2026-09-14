"""Las reglas: que encuentren lo que hay y que NO encuentren lo que no hay.

El test negativo (una sesion sana no produce candidatos) vale tanto como los
positivos: un analizador que siempre propone algo entrena a que se lo ignore.
"""

import unittest

from gpuprobe_analyzer import ingest, rules
from gpuprobe_analyzer.ingest import (STATE_COMMON, STATE_DEPTH_WRITE,
                                      STATE_PIXEL_SHADER, STATE_RENDER_TARGET)

from fixtures import Builder


def ids(cands):
    return {c.cid.split("_")[0] for c in cands}


class TestRules(unittest.TestCase):

    def test_shadow_sobredimensionado(self):
        b = Builder()
        b.res(0x10, 0xAA, "shadowmap", "D32_FLOAT", 4096, 4096, d=4, flags=2,
              nbytes=4096 * 4096 * 4 * 4)
        for f in range(200, 260):
            b.pass_(f, 0x100, 0x10, 0xAA, 2.4, draws=900, rt_w=4096, rt_h=4096)
            b.frame_end(f, 10.0)
        s = ingest.load_lines(b.build(), warmup=100)
        cands = rules.analyze(s)
        shadow = [c for c in cands if c.cid.startswith("shadow_")]
        self.assertEqual(len(shadow), 1)
        c = shadow[0]
        self.assertEqual(c.kind, "resource_scale")
        # 2.4 ms x 0.75 x 0.85
        self.assertAlmostEqual(c.gain_ms, 2.4 * 0.75 * 0.85, places=3)
        self.assertEqual(c.stability, rules.STAB_HIGH)
        self.assertIn("shadowmap", c.action_toml)
        self.assertIn("scale = 0.5", c.action_toml)

    def test_shadow_de_2048_no_se_toca(self):
        # El enunciado dice "por encima de 2048": 2048 justo NO entra.
        b = Builder()
        b.res(0x10, 0xAA, "shadowmap", "D32_FLOAT", 2048, 2048, flags=2)
        for f in range(200, 260):
            b.pass_(f, 0x100, 0x10, 0xAA, 2.4, draws=900, rt_w=2048, rt_h=2048)
            b.frame_end(f, 10.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertEqual([c for c in rules.analyze(s) if c.cid.startswith("shadow_")], [])

    def test_shadow_barato_no_se_toca(self):
        # 4096 pero cuesta 0.05 ms: no vale el riesgo visual.
        b = Builder()
        b.res(0x10, 0xAA, "shadowmap", "D32_FLOAT", 4096, 4096, flags=2)
        for f in range(200, 260):
            b.pass_(f, 0x100, 0x10, 0xAA, 0.05, draws=10, rt_w=4096, rt_h=4096)
            b.frame_end(f, 10.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertEqual([c for c in rules.analyze(s) if c.cid.startswith("shadow_")], [])

    def test_post_fullres_si_y_geometria_no(self):
        b = Builder()
        b.res(0x10, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        b.res(0x20, 0xBB, "rt_full", "R8G8B8A8_UNORM", 2560, 1440, flags=4)
        for f in range(200, 260):
            # post: un draw, full-res, caro
            b.pass_(f, 0x100, 0x10, 0xAA, 2.2, draws=1, ordinal=1)
            # geometria: 1400 draws. Bajarle el RT cambia la imagen entera y
            # ademas no es una pasada "mal dimensionada".
            b.pass_(f, 0x200, 0x20, 0xBB, 3.0, draws=1400, ordinal=2,
                    begin_ms=2.2)
            b.frame_end(f, 10.0)
        s = ingest.load_lines(b.build(), warmup=100)
        post = [c for c in rules.analyze(s) if c.cid.startswith("post_")]
        self.assertEqual(len(post), 1)
        self.assertEqual(post[0].pass_key, 0x100)

    def test_ganancia_limitada_por_el_tiempo_exclusivo(self):
        # Una pasada de post cara pero tapada por otra queue: la ganancia
        # estimada tiene que salir de lo exclusivo, no de su costo.
        b = Builder()
        b.res(0x10, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        b.res(0x20, 0xBB, "rt_full", "R8G8B8A8_UNORM", 2560, 1440, flags=4)
        for f in range(200, 260):
            b.pass_(f, 0x100, 0x10, 0xAA, 2.0, draws=1, queue=1, begin_ms=0.0)
            b.pass_(f, 0x200, 0x20, 0xBB, 4.0, draws=1400, queue=0, begin_ms=0.0)
            b.frame_end(f, 10.0)
        s = ingest.load_lines(b.build(), warmup=100)
        post = [c for c in rules.analyze(s) if c.cid.startswith("post_")]
        self.assertEqual(post, [])   # 0 ms exclusivos: no hay nada que ganar

    def test_huerfano_con_hermanos_baja_la_estabilidad(self):
        b = Builder()
        # Dos recursos con el mismo descriptor: uno se lee, el otro no.
        b.res(0x10, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        b.res(0x11, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        for f in range(200, 260):
            b.barrier(f, 0x10, STATE_COMMON, STATE_RENDER_TARGET)
            b.barrier(f, 0x11, STATE_RENDER_TARGET, STATE_PIXEL_SHADER)
            b.pass_(f, 0x100, 0x10, 0xAA, 0.5, draws=1)
            b.frame_end(f, 10.0)
        s = ingest.load_lines(b.build(), warmup=100)
        orph = [c for c in rules.analyze(s) if c.cid.startswith("orphan_")]
        self.assertEqual(len(orph), 1)
        self.assertEqual(orph[0].stability, rules.STAB_LOW)
        self.assertEqual(orph[0].visual_risk, rules.RISK_HIGH)
        self.assertTrue(any("MISMO descriptor" in e for e in orph[0].evidence))
        # Y la ganancia es la de SU pasada, no la de los dos recursos juntos.
        self.assertAlmostEqual(orph[0].gain_ms, 0.5, places=3)

    def test_pso_en_gameplay(self):
        b = Builder()
        for f in range(200, 260):
            b.frame_end(f, 10.0)
        b.pso(230, 0xF1, 14.0, render_thread=1)
        b.pso(240, 0xF2, 9.0, render_thread=1)
        s = ingest.load_lines(b.build(), warmup=100)
        pso = [c for c in rules.analyze(s) if c.cid == "pso_stutter"]
        self.assertEqual(len(pso), 1)
        # No baja el frametime medio, y el reporte tiene que decirlo asi.
        self.assertEqual(pso[0].gain_ms, 0.0)
        self.assertAlmostEqual(pso[0].hitch_ms, 14.0)
        self.assertIn("hilo de render", " ".join(pso[0].evidence).lower())

    def test_thrashing_bloquea_todo_lo_demas(self):
        b = Builder()
        b.res(0x10, 0xAA, "shadowmap", "D32_FLOAT", 4096, 4096, flags=2)
        for f in range(200, 260):
            b.pass_(f, 0x100, 0x10, 0xAA, 2.4, draws=900, rt_w=4096, rt_h=4096)
            if f % 30 == 0:
                b.vram(f, b.budget + (1 << 30))
            b.frame_end(f, 20.0)
        s = ingest.load_lines(b.build(), warmup=100)
        cands = rules.analyze(s)
        self.assertEqual(cands[0].cid, "vram_thrashing")
        self.assertTrue(all(c.blocked for c in cands if c.cid != "vram_thrashing"))

    def test_sesion_sana_no_propone_nada(self):
        # Todo a resolucion razonable, barato, sin huerfanos ni barriers de
        # mas: el analizador tiene que callarse.
        b = Builder()
        b.res(0x10, 0xAA, "rt_half", "R8G8B8A8_UNORM", 1280, 720, flags=4)
        b.res(0x20, 0xBB, "shadowmap", "D32_FLOAT", 2048, 2048, flags=2)
        for f in range(200, 260):
            b.barrier(f, 0x10, STATE_RENDER_TARGET, STATE_PIXEL_SHADER)
            b.barrier(f, 0x20, STATE_DEPTH_WRITE, STATE_PIXEL_SHADER)
            b.pass_(f, 0x100, 0x10, 0xAA, 0.4, draws=1)
            b.pass_(f, 0x200, 0x20, 0xBB, 0.9, draws=300, begin_ms=0.4,
                    rt_w=2048, rt_h=2048)
            b.vram(f, b.budget // 2)
            b.frame_end(f, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        cands = [c for c in rules.analyze(s) if c.gain_ms >= 0.15 or c.hitch_ms]
        self.assertEqual(cands, [], f"propuso de mas: {[c.title for c in cands]}")


if __name__ == "__main__":
    unittest.main()


class TestRegresiones(unittest.TestCase):
    """Casos que aparecieron corriendo el testbed de verdad sobre WARP.

    Los tres estaban en verde en los unitarios y en rojo en la corrida real:
    por eso el job de Windows existe.
    """

    def test_un_rt_escrito_sin_barriers_igual_cuenta_como_escrito(self):
        # El testbed crea el huerfano ya en RENDER_TARGET y nunca lo
        # transiciona: por barriers no se veia escrito, y sin escrituras la
        # regla del huerfano no disparaba. Que una pasada lo tenga bindeado
        # como RT es evidencia directa.
        b = Builder()
        b.res(0x10, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        for f in range(200, 260):
            b.pass_(f, 0x100, 0x10, 0xAA, 0.5, draws=1)
            b.frame_end(f, 10.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertTrue(s.resources[0x10].never_read())
        self.assertTrue(any(c.cid.startswith("orphan_") for c in rules.analyze(s)))

    def test_el_warmup_de_la_sesion_manda_sobre_el_default(self):
        # Un PSO compilado en el frame 120 con warmup de sesion 30 tiene que
        # aparecer; antes lo filtraba el warmup por defecto de los umbrales.
        b = Builder()
        for f in range(31, 240):
            b.frame_end(f, 10.0)
        b.pso(120, 0xF1, 12.0, render_thread=1)
        s = ingest.load_lines(b.build(), warmup=30)
        self.assertTrue(any(c.cid == "pso_stutter" for c in rules.analyze(s)))
