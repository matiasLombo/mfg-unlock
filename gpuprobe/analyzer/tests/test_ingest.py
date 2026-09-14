"""Lectura del JSONL: lo que tiene que sobrevivir y lo que tiene que contar."""

import unittest

from gpuprobe_analyzer import ingest
from gpuprobe_analyzer.ingest import (STATE_COMMON, STATE_PIXEL_SHADER,
                                      STATE_RENDER_TARGET)

from fixtures import Builder


class TestIngest(unittest.TestCase):

    def test_header_y_resumen(self):
        b = Builder()
        b.res(0x10, 0xAA, "rt_full", "R8G8B8A8_UNORM", 2560, 1440,
              flags=4)
        b.pass_(200, 0x100, 0x10, 0xAA, 2.0, draws=50).frame_end(200, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertEqual(s.schema, 1)
        self.assertEqual(s.exe, "test.exe")
        self.assertEqual((s.out_w, s.out_h), (2560, 1440))
        self.assertEqual(len(s.passes), 1)
        self.assertAlmostEqual(s.frame_ms, 8.0)

    def test_una_linea_rota_no_tira_el_archivo(self):
        # Un crash del juego corta la ultima linea a la mitad. El resto de la
        # sesion -- que puede ser media hora de gameplay -- tiene que servir.
        b = Builder()
        b.pass_(200, 0x100, 0x10, 0xAA, 1.0).frame_end(200, 8.0)
        b.raw('{"t":"pass","f":201,"gpu_ms":1.0')     # cortada
        b.raw('no soy json')
        b.pass_(202, 0x100, 0x10, 0xAA, 1.0).frame_end(202, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertEqual(s.bad_lines, 2)
        self.assertEqual(len(s.frames), 2)

    def test_warmup_se_descarta(self):
        # Los primeros frames son carga de nivel: si entran, el top de pasadas
        # queda dominado por la primera compilacion de todo.
        b = Builder()
        b.pass_(5, 0x100, 0x10, 0xAA, 50.0).frame_end(5, 60.0)
        for f in range(200, 210):
            b.pass_(f, 0x100, 0x10, 0xAA, 2.0).frame_end(f, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        p = list(s.passes.values())[0]
        self.assertAlmostEqual(p.ms, 2.0)
        self.assertEqual(len(p.samples), 10)

    def test_frames_deep_no_se_mezclan(self):
        b = Builder()
        for f in range(200, 210):
            b.pass_(f, 0x100, 0x10, 0xAA, 2.0).frame_end(f, 8.0)
        b.pass_(210, 0x100, 0x10, 0xAA, 3.5, deep=1).frame_end(210, 12.0, deep=1)
        s = ingest.load_lines(b.build(), warmup=100)
        p = list(s.passes.values())[0]
        self.assertEqual(len(p.samples), 10)
        self.assertEqual(len(p.deep_samples), 1)
        self.assertAlmostEqual(p.ms, 2.0)   # el deep no contamina la mediana

    def test_tiempo_exclusivo_con_async_compute(self):
        # La de compute corre entera adentro de la de graficos: su tiempo
        # exclusivo es cero y optimizarla no puede ganar nada. La de graficos
        # pierde solo la parte tapada.
        b = Builder()
        for f in range(200, 220):
            b.pass_(f, 0x100, 0x10, 0xAA, 4.0, queue=0, begin_ms=0.0)
            b.pass_(f, 0x200, 0x20, 0xBB, 1.0, queue=1, begin_ms=0.5)
            b.frame_end(f, 9.0)
        s = ingest.load_lines(b.build(), warmup=100)
        gfx = s.passes[0x100]
        cmp_ = s.passes[0x200]
        self.assertAlmostEqual(gfx.exclusive_ms, 3.0, places=3)
        self.assertAlmostEqual(cmp_.exclusive_ms, 0.0, places=3)
        self.assertFalse(gfx.overlapped)
        self.assertTrue(cmp_.overlapped)

    def test_pasada_mayormente_tapada_se_marca(self):
        # Aunque sea la mas larga: si la mayor parte corre en paralelo con
        # otra queue, el techo de lo que se puede ganar es lo que queda
        # afuera, y el reporte tiene que decirlo antes de que alguien la
        # "optimice" y no vea moverse el frametime.
        b = Builder()
        for f in range(200, 220):
            b.pass_(f, 0x100, 0x10, 0xAA, 4.0, queue=0, begin_ms=0.0)
            b.pass_(f, 0x200, 0x20, 0xBB, 3.0, queue=1, begin_ms=0.5)
            b.frame_end(f, 9.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertAlmostEqual(s.passes[0x100].exclusive_ms, 1.0, places=3)
        self.assertTrue(s.passes[0x100].overlapped)

    def test_nunca_leido(self):
        b = Builder()
        b.res(0x10, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        b.res(0x20, 0xBB, "rt_half", "R8G8B8A8_UNORM", 1280, 720, flags=4)
        for f in range(200, 205):
            b.barrier(f, 0x10, STATE_COMMON, STATE_RENDER_TARGET)
            b.barrier(f, 0x20, STATE_RENDER_TARGET, STATE_PIXEL_SHADER)
            b.frame_end(f, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertTrue(s.resources[0x10].never_read())
        self.assertFalse(s.resources[0x20].never_read())

    def test_barriers_redundantes(self):
        b = Builder()
        for f in range(200, 210):
            # al estado en el que ya esta
            b.barrier(f, 0x10, STATE_PIXEL_SHADER, STATE_PIXEL_SHADER)
            # ida y vuelta por COMMON
            b.barrier(f, 0x20, STATE_PIXEL_SHADER, STATE_COMMON)
            b.barrier(f, 0x20, STATE_COMMON, STATE_RENDER_TARGET)
            b.frame_end(f, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        same = sum(x.redundant_same_state for x in s.barriers.values())
        rt = sum(x.common_roundtrip for x in s.barriers.values())
        self.assertEqual(same, 10)
        self.assertEqual(rt, 10)

    def test_thrashing(self):
        b = Builder()
        b.vram(200, b.budget - 1000).frame_end(200, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertFalse(s.thrashing())

        b2 = Builder()
        b2.vram(200, b2.budget + 1000).frame_end(200, 8.0)
        s2 = ingest.load_lines(b2.build(), warmup=100)
        self.assertTrue(s2.thrashing())

    def test_pasada_por_instancia_no_por_descriptor(self):
        # Dos recursos con el MISMO descriptor: el costo de cada pasada tiene
        # que ir a su recurso, no repartirse.
        b = Builder()
        b.res(0x10, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        b.res(0x11, 0xAA, "rt_full", "R16G16B16A16_FLOAT", 2560, 1440, flags=4)
        for f in range(200, 205):
            b.pass_(f, 0x100, 0x10, 0xAA, 2.0)
            b.pass_(f, 0x200, 0x11, 0xAA, 1.0, begin_ms=2.0)
            b.frame_end(f, 8.0)
        s = ingest.load_lines(b.build(), warmup=100)
        self.assertEqual(s.resource_for_pass(s.passes[0x100]).key, 0x10)
        self.assertEqual(s.resource_for_pass(s.passes[0x200]).key, 0x11)
        self.assertEqual(len(s.siblings(s.resources[0x10])), 1)


if __name__ == "__main__":
    unittest.main()
