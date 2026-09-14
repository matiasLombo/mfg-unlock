// ring.h -- el ring SPSC por el que viaja cada Event del hilo de render al de
// gpuprobe.
//
// ENTRA: push() desde UN solo hilo productor.
// SALE: drain() desde UN solo hilo consumidor.
// DEPENDE DE: events.h, <atomic>.
//
// Un ring por hilo que graba command lists. Los juegos graban en 8 o 16 hilos
// y un ring compartido con lock seria exactamente el tipo de cosa que hace que
// un profiler cambie lo que mide -- o que deadlockee contra un hilo del juego
// que ya tiene un lock del runtime tomado. SPSC sin locks: el productor solo
// escribe head, el consumidor solo escribe tail.
//
// Cuando el ring se llena, push() DESCARTA y cuenta. Nunca bloquea al hilo de
// render. Los descartes salen en el JSONL (dropped) porque un frame con
// eventos perdidos no se puede analizar como si estuviera completo.
#pragma once

#include "events.h"

#include <atomic>
#include <cstddef>

namespace gp {

template <size_t N>
class RingT {
    static_assert((N & (N - 1)) == 0, "N tiene que ser potencia de 2");

public:
    // Devuelve false si no entro. El productor no reintenta: perder un evento
    // es preferible a frenar el frame.
    bool push(const Event &e) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[head] = e;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Copia hasta max eventos a out. Devuelve cuantos.
    size_t drain(Event *out, size_t max) {
        size_t n = 0;
        size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        while (tail != head && n < max) {
            out[n++] = slots_[tail];
            tail = (tail + 1) & (N - 1);
        }
        tail_.store(tail, std::memory_order_release);
        return n;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    u64 dropped() const { return dropped_.load(std::memory_order_relaxed); }
    void reset_dropped() { dropped_.store(0, std::memory_order_relaxed); }

    static constexpr size_t capacity() { return N - 1; }

private:
    // head y tail en lineas de cache distintas: si comparten linea, cada push
    // invalida la linea que el consumidor esta leyendo y el ring "sin locks"
    // termina costando mas que un mutex.
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<u64>    dropped_{0};
    Event slots_[N];
};

// 16384 eventos = ~1.6 MB por hilo. Un frame pesado con instrumentacion deep
// anda por los 3000 eventos, asi que el consumidor puede dormirse cinco frames
// sin que se pierda nada.
using Ring = RingT<16384>;

}  // namespace gp
