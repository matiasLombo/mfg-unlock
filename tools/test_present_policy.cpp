// La decision sobre un swapchain nuevo (src/present_policy.h), una fila por
// incidente MEDIDO. Cada fila cita el log que la produjo: no es lo que
// deberia pasar, es lo que paso y lo que tuvo que salir para que no vuelva.
//
//   g++ -std=c++20 -O2 -I src tools/test_present_policy.cpp -o /tmp/tpp.exe && /tmp/tpp.exe
#include <cstdio>
#include "present_policy.h"
using namespace present_policy;

static int failures = 0;
static void check(const char *label, long got, long expected) {
    const bool ok = got == expected;
    if (!ok) ++failures;
    printf("  %-66s %s (dio %ld, esperado %ld)\n", label, ok ? "ok  " : "FALLA", got, expected);
}
static void fila(const char *caso, Input e, bool reg, bool slot, bool adopt, Reason r) {
    const Output o = decide_hook(e);
    printf("%s\n", caso);
    check("  registra la vtable", o.register_vtable, reg);
    check("  escribe el slot", o.write_slot, slot);
    check("  adopta la instancia", o.adopt, adopt);
    check("  motivo", (long)o.reason, (long)r);
}

int main(void) {
    printf("decide_hook: los incidentes\n\n");

    // Cyberpunk desde Steam, 2026-09-11: "present: RECURSION, profundidad 4244 /
    // EXCEPCION 0xC00000FD en gameoverlayrenderer64.dll". El overlay hace
    // byte-detour de Present; el slot no se escribe. Y la primera guarda hacia
    // return: "cero lineas de PresentCount y el HUD en cero" -> adopta igual.
    fila("overlay de Steam (Cyberpunk, profundidad 4244): registra y adopta, sin slot",
         { false, false, SlotOwner::Dxgi, true, false }, true, false, true, Reason::Overlay);

    // GTA V 2026-09-11 15:xx: "present: el slot YA lo engancho otro; no nos
    // apilamos / ReShade.asi". La rama hacia return antes de adoptar: ventanas
    // sin "runtime PresentCount", "el contador no andaba".
    fila("ReShade dueno del slot (GTA V, contador vacio): registra y adopta, sin slot",
         { false, false, SlotOwner::Other, true, false }, true, false, true, Reason::Overlay);
    fila("  y lo mismo sin overlay en el proceso",
         { false, false, SlotOwner::Other, false, false }, true, false, true, Reason::OtherOwner);

    // Metro (modo host) 2026-09-11: el swapchain del juego es el PROXY de
    // Streamline, su Present vive en sl.interposer.dll y el overlay no lo
    // alcanza. Sin el slot no salen los marcadores de present: 1.00 con el
    // plugin diciendo "enabled". Con el slot, 2.00-4.00.
    fila("proxy de Streamline en modo host (Metro, 2.00): todo, slot incluido",
         { false, false, SlotOwner::Interposer, true, true }, true, true, true, Reason::HostProxy);
    fila("  el proxy sin modo host (Cyberpunk normal): es otro dueno",
         { false, false, SlotOwner::Interposer, true, false }, true, false, true, Reason::Overlay);

    // Cyberpunk 2026-09-11: "el descartable a los 6,5 s, el del juego a los
    // 10,6 s, misma vtable". Releer vt[8] guardaria nuestro hook como original
    // (recursion garantizada); no adoptar dejaba el HUD en cero.
    fila("vtable ya registrada (Cyberpunk, descartable vs juego): solo adopta",
         { true, false, SlotOwner::Ours, true, false }, false, false, true, Reason::AlreadyRegistered);

    // Los dos casos de "no se toca" que ya existian.
    fila("nuestro hook en el slot sin original registrado: nada",
         { false, false, SlotOwner::Ours, false, false }, false, false, false, Reason::OurHookNoOrig);
    fila("tabla de vtables llena: nada",
         { false, true, SlotOwner::Dxgi, false, false }, false, false, false, Reason::TableFull);

    // Sin overlay ni otro dueno (el exe lanzado directo): el slot se escribe.
    fila("limpio (Cyberpunk lanzado directo, 170 s sin excepcion): todo",
         { false, false, SlotOwner::Dxgi, false, false }, true, true, true, Reason::Clean);
    fila("  dueno desconocido cuenta como limpio",
         { false, false, SlotOwner::Unknown, false, false }, true, true, true, Reason::Clean);

    printf("\nslot_writable: la guarda de write_slot, para cualquier slot\n");
    check("dxgi sin overlay", slot_writable(SlotOwner::Dxgi, false, false), 1);
    check("dxgi con overlay: no", slot_writable(SlotOwner::Dxgi, true, false), 0);
    check("otro dueno (ReShade): no", slot_writable(SlotOwner::Other, false, false), 0);
    check("proxy de SL en modo host, con overlay: si", slot_writable(SlotOwner::Interposer, true, true), 1);
    check("proxy de SL sin modo host: no", slot_writable(SlotOwner::Interposer, false, false), 0);
    check("ya es nuestro: no", slot_writable(SlotOwner::Ours, false, false), 0);

    printf("\n%s\n", failures == 0 ? "todos los casos en verde" : "HAY CASOS EN ROJO");
    return failures ? 1 : 0;
}
