// gate.h -- el interruptor general. Si algo sale mal, gpuprobe se apaga y el
// juego sigue como si no existiera.
//
// ENTRA: cada hook, al principio.
// SALE: la fase, y el permiso (o no) de hacer cualquier cosa.
// DEPENDE DE: windows.h.
//
// La regla dura del proyecto: cualquier fallo cae a passthrough transparente.
// El juego nunca crashea por gpuprobe. Eso se sostiene con cuatro cosas y no
// con buenas intenciones:
//
//   1. Fases. Nada se engancha hasta que el device esta VERIFICADO, y verificar
//      es haber visto un evento propio llegar de punta a punta.
//   2. Un solo camino de degradado. gate_degrade() es definitivo: una vez
//      apagado no vuelve a prenderse en esa corrida. Reintentar algo que ya
//      fallo una vez adentro de un juego es como se hace un crash intermitente.
//   3. Guarda de reentrada por hilo. Si un hook nuestro termina llamando a algo
//      que vuelve a entrar, la segunda vuelta pasa derecho.
//   4. Un testigo de excepciones (VEH) que mira si la falla cayo DENTRO de
//      nuestro modulo. Si fue nuestra, lo dice en el log y apaga gpuprobe; si
//      no, no toca nada y deja que el juego haga lo suyo.
#pragma once

#include <d3d12.h>

namespace gp {

enum class Phase {
    Passthrough = 0,  // todavia no armamos nada
    Armed,            // hooks puestos, sin verificar
    Verified,         // vimos un evento propio de punta a punta
    Degraded,         // algo fallo: apagado para siempre en esta corrida
};

Phase gate_phase();
bool  gate_is(Phase p);

// Arma gpuprobe sobre este device. Devuelve false ante cualquier problema, y
// en ese caso queda en Passthrough: no es un error que haya que reportarle al
// juego.
bool gate_arm(ID3D12Device *device, const char *exe);

// El auto-test: se llama cuando el primer frame completo paso por el colector.
void gate_verify();

// Apaga gpuprobe. Definitivo.
void gate_degrade(const char *why);

// Log propio, en %LOCALAPPDATA%\gpuprobe\gpuprobe.log. Nunca en la carpeta del
// juego.
void gate_log(const char *fmt, ...);

// Guarda de reentrada por hilo. Mientras vive, cualquier otro hook de este
// hilo ve reentrant() == true y pasa derecho.
struct Reentry {
    Reentry();
    ~Reentry();
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

// true si ya estamos adentro de un hook en este hilo.
bool reentrant();

// Azucar para el principio de cada hook: si esto devuelve false, el hook tiene
// que llamar al original y no hacer nada mas.
inline bool gate_open() {
    return gate_phase() >= Phase::Armed && !reentrant();
}

}  // namespace gp
