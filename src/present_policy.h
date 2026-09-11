// present_policy.h -- que se hace con un swapchain nuevo: pura, sin Windows.
//
// hook_swapchain_present tenia cuatro ramas con la misma decision (registrar
// la vtable, escribir el slot de Present, adoptar la instancia) y cada rama
// repitio el mismo error de forma: un return que se llevaba la adopcion, un
// hook sin la guarda del overlay. Cinco de los ocho defectos del 11/09 fueron
// ahi. Aca la decision es una funcion de cinco hechos, y cada incidente
// medido es una fila de tools/test_present_policy.cpp.
//
// ENTRA: Input, lo que present.h averigua del swapchain (vtable ya
//   registrada, tabla llena, quien es dueno del slot 8, overlay de Steam en
//   el proceso, modo host).
// SALE: Output, tres booleanos y el motivo; present.h ejecuta y loguea.
// DEPENDE DE: nada.
//
// Las reglas, cada una con su medicion:
//   - vtable ya registrada: solo adoptar. Cyberpunk: el descartable a los 6,5 s
//     y el del juego a los 10,6 s, misma vtable; releer vt[8] guardaria nuestro
//     hook como original (recursion), y no adoptar dejaba el HUD en cero.
//   - overlay de Steam: registrar y adoptar, NO escribir el slot. Byte-detour
//     de Present; llamar al original vuelve a nosotros: profundidad 4244,
//     0xC00000FD en gameoverlayrenderer64.dll.
//   - otro dueno del slot (ReShade.asi en GTA V): igual que el overlay.
//     Apilarse forma un lazo; y no adoptar dejaba el contador vacio (11/09).
//   - proxy de Streamline en modo host: el slot SI se escribe. Su Present vive
//     en sl.interposer.dll, el overlay no lo alcanza (Metro, 2.00-4.00).
//   - limpio: todo.
#pragma once

namespace present_policy {

enum class SlotOwner { Dxgi, Other, Interposer, Ours, Unknown };

enum class Reason { AlreadyRegistered, OurHookNoOrig, TableFull, HostProxy, Overlay, OtherOwner, Clean };

struct Input {
    bool vtable_registered;   // la vtable ya esta en g_vt_present
    bool table_full;
    SlotOwner owner;          // modulo al que apunta vt[8]
    bool overlay_present;     // gameoverlayrenderer64.dll mapeado
    bool host_on;
};

struct Output {
    bool register_vtable;
    bool write_slot;
    bool adopt;
    Reason reason;
};

inline Output decide_hook(const Input &e) {
    if (e.vtable_registered) return { false, false, true, Reason::AlreadyRegistered };
    if (e.owner == SlotOwner::Ours) return { false, false, false, Reason::OurHookNoOrig };
    if (e.table_full) return { false, false, false, Reason::TableFull };
    if (e.host_on && e.owner == SlotOwner::Interposer) return { true, true, true, Reason::HostProxy };
    if (e.overlay_present) return { true, false, true, Reason::Overlay };
    if (e.owner == SlotOwner::Other || e.owner == SlotOwner::Interposer) return { true, false, true, Reason::OtherOwner };
    return { true, true, true, Reason::Clean };
}

// La guarda de write_slot, la misma para cualquier slot de una vtable
// compartida: no se escribe sobre el slot de otro, ni con el overlay en el
// proceso, salvo que el slot sea del proxy de Streamline (modo host).
inline bool slot_writable(SlotOwner owner, bool overlay_present, bool host_on) {
    if (owner == SlotOwner::Ours) return false;
    if (host_on && owner == SlotOwner::Interposer) return true;
    if (overlay_present) return false;
    return owner == SlotOwner::Dxgi || owner == SlotOwner::Unknown;
}

}  // namespace present_policy
