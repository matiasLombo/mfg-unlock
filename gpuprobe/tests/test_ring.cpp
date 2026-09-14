// test_ring.cpp -- el ring bajo dos hilos de verdad, no simulados.
//
// Lo que importa probar: que no se pierde ni se duplica nada mientras entra y
// sale a la vez, y que cuando se llena DESCARTA en vez de bloquear. Un ring
// que bloquea al productor es un stutter en el juego.
#include "../core/ring.h"

#include "harness.h"

#include <thread>
#include <vector>

using namespace gp;

int main() {
    // --- secuencial: orden y capacidad ---
    {
        RingT<8> r;
        Event e;
        for (u32 i = 0; i < 7; ++i) {
            e.frame = i;
            CHECK(r.push(e));
        }
        // El octavo no entra: la capacidad util es N-1.
        CHECK(!r.push(e));
        CHECK_EQ_U64(r.dropped(), 1);

        Event out[16];
        CHECK_EQ_U64(r.drain(out, 16), 7);
        for (u32 i = 0; i < 7; ++i) CHECK_EQ_U64(out[i].frame, i);
        CHECK(r.empty());
    }

    // --- dos hilos: nada se pierde, nada se duplica, el orden se mantiene ---
    {
        static Ring r;
        constexpr u64 kTotal = 200000;
        std::vector<u64> got;
        got.reserve(kTotal);

        std::thread consumer([&] {
            Event buf[512];
            while (got.size() < kTotal) {
                const size_t n = r.drain(buf, 512);
                for (size_t i = 0; i < n; ++i) got.push_back(buf[i].frame);
                if (n == 0) std::this_thread::yield();
            }
        });

        Event e;
        for (u64 i = 0; i < kTotal; ++i) {
            e.frame = i;
            // El productor reintenta SOLO en el test, para poder verificar que
            // no hay perdida ni duplicacion. En el DLL no reintenta nunca.
            while (!r.push(e)) std::this_thread::yield();
        }
        consumer.join();

        CHECK_EQ_U64(got.size(), kTotal);
        bool in_order = true;
        for (u64 i = 0; i < kTotal && in_order; ++i)
            if (got[i] != i) in_order = false;
        CHECK(in_order);
    }

    return gp_test::report("test_ring");
}
