"""El harness A/B: que mida la ganancia real y que NO invente ninguna."""

import unittest

from gpuprobe_analyzer import ab, ingest

from fixtures import Builder


def synth(base, n, seed, jitter=0.35, hitch=0.0):
    rng = ab._Rng(seed)
    out = []
    for _ in range(n):
        u = (rng.next() % 10000) / 10000.0
        x = base + (u - 0.5) * 2.0 * jitter
        if hitch and rng.next() % 100 == 0:
            x += hitch
        out.append(x)
    return out


class TestAB(unittest.TestCase):

    def test_sin_diferencia_no_hay_ganancia(self):
        a = synth(8.0, 600, 1, hitch=30.0)
        b = synth(8.0, 600, 2, hitch=30.0)
        r = ab.compare(a, b)
        self.assertFalse(r["significant"])
        self.assertLessEqual(r["ci_lo"], 0.0)
        self.assertGreaterEqual(r["ci_hi"], 0.0)

    def test_una_ganancia_de_1ms_se_detecta(self):
        on = synth(7.0, 600, 3, hitch=30.0)
        off = synth(8.0, 600, 4, hitch=30.0)
        r = ab.compare(on, off)
        self.assertTrue(r["significant"])
        self.assertAlmostEqual(r["delta_ms"], 1.0, delta=0.2)
        self.assertGreater(r["ci_lo"], 0.0)

    def test_una_optimizacion_que_empeora_no_pasa(self):
        on = synth(9.0, 400, 5)
        off = synth(8.0, 400, 6)
        r = ab.compare(on, off)
        self.assertFalse(r["significant"])
        self.assertLess(r["delta_ms"], 0.0)

    def test_muestras_chicas_no_deciden(self):
        r = ab.compare([7.0, 7.1, 7.0, 6.9], [9.0, 9.1, 9.0, 8.9])
        self.assertFalse(r["significant"])
        self.assertEqual(r["p_value"], 1.0)

    def test_determinismo(self):
        on, off = synth(7.0, 200, 9), synth(8.0, 200, 10)
        r1, r2 = ab.compare(on, off), ab.compare(on, off)
        self.assertEqual(r1, r2)

    def test_medicion_de_una_sesion_con_toggles(self):
        # La accion 3 se prende y apaga cada 60 frames; cuando esta prendida,
        # el frame cuesta 1 ms menos.
        b = Builder()
        for f in range(1, 901):
            on = (f // 60) % 2 == 1
            if f % 60 == 0:
                b.action(f, 3, (f // 60) % 2 == 1)
            b.frame_end(f, 8.0 - (1.0 if on else 0.0) + (f % 7) * 0.01)
        s = ingest.load_lines(b.build(), warmup=100)
        res = ab.measure(s)
        self.assertEqual(len(res), 1)
        r = res[0]
        self.assertTrue(r.significant)
        self.assertAlmostEqual(r.delta_ms, 1.0, delta=0.1)
        self.assertEqual(r.verdict, "GANA")

    def test_los_frames_pegados_al_toggle_se_descartan(self):
        # Los primeros frames despues de conmutar miden caches frias. Si
        # entraran, un cambio sin efecto parecerìa costar.
        b = Builder()
        for f in range(1, 901):
            on = (f // 60) % 2 == 1
            if f % 60 == 0:
                b.action(f, 3, on)
            # Los 5 frames siguientes al toggle cuestan 5 ms de mas, siempre.
            penalty = 5.0 if (f % 60) < 5 else 0.0
            b.frame_end(f, 8.0 + penalty)
        s = ingest.load_lines(b.build(), warmup=100)
        r = ab.measure(s)[0]
        # Sin el settle, las medianas saldrian distintas por el arranque.
        self.assertFalse(r.significant)
        self.assertAlmostEqual(r.delta_ms, 0.0, places=6)

    def test_dos_acciones_prendidas_a_la_vez_no_se_miden(self):
        # Si hay dos prendidas no se sabe cual gano: esos frames no entran.
        b = Builder()
        b.action(1, 1, True)
        b.action(1, 2, True)
        for f in range(1, 400):
            b.frame_end(f, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        for r in ab.measure(s):
            self.assertEqual(r.n_on, 0)
            self.assertFalse(r.significant)

    def test_frames_deep_o_con_perdidas_no_entran(self):
        b = Builder()
        for f in range(1, 901):
            on = (f // 60) % 2 == 1
            if f % 60 == 0:
                b.action(f, 3, on)
            deep = 1 if f % 120 == 0 else 0
            dropped = 3 if f % 200 == 0 else 0
            b.frame_end(f, 8.0 - (1.0 if on else 0.0), dropped=dropped, deep=deep)
        s = ingest.load_lines(b.build(), warmup=100)
        r = ab.measure(s)[0]
        self.assertTrue(r.significant)
        # De 900 frames: se van los 100 de warmup, los 5 pegados a cada uno de
        # los 15 toggles, los 7 deep y los 4 con eventos perdidos.
        total = r.n_on + r.n_off
        self.assertLessEqual(total, 800 - 7 - 4)
        self.assertGreater(total, 600)


if __name__ == "__main__":
    unittest.main()
