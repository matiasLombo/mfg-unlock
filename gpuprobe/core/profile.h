// profile.h -- el perfil por juego: acciones declarativas en TOML, matcheadas
// por DESCRIPTOR.
//
// ENTRA: el texto de %LOCALAPPDATA%\gpuprobe\profiles\<exe>.toml.
// SALE: un Profile validado, o una lista de errores con numero de linea.
// DEPENDE DE: keys.h. Nada de Windows: el parser se testea en el host.
//
// Tres reglas que estan en el codigo y no solo en el README:
//
// 1. Si el archivo tiene UN error, no se aplica NADA. El hot-reload cambia el
//    perfil entero o no cambia nada; un perfil a medio aplicar es la clase de
//    estado que despues nadie puede reproducir.
// 2. Toda accion nace apagada salvo que diga enabled = true, y toda accion es
//    reversible en el limite de frame.
// 3. resource_scale lleva un gate de verificacion (verify). El ejecutor NO
//    aplica un scale a un recurso que no paso el gate, aunque el matcher
//    matchee: ver executor para la lista de razones por las que un recurso
//    puede ser inescalable (lo copian con extents fijos, vive en un heap
//    compartido, sus viewports no pasan por RSSetViewports).
#pragma once

#include "keys.h"

#include <string>
#include <vector>

namespace gp {

enum class ActionKind : u32 {
    None = 0,
    ResourceScale,   // crear el recurso mas chico
    MipBias,         // sesgar el mip que se muestrea
    SkipPass,        // no ejecutar los draws de una pasada
    BarrierFilter,   // descartar una transicion redundante
    DrsSettings,     // tocar el driver por NVAPI
};

const char *action_kind_name(ActionKind);

// El gate de seguridad de una accion. Cada valor es una condicion que el
// ejecutor tiene que haber OBSERVADO antes de aplicar; si no la observo, la
// accion queda pendiente y se dice en el log.
enum class Verify : u32 {
    None = 0,            // el usuario se hace cargo
    NoCopyObserved,      // el recurso nunca fue origen ni destino de una copia
    NotPlaced,           // no comparte heap con otro recurso
    ViewportsTracked,    // todos sus viewports pasaron por RSSetViewports
    Full,                // las tres de arriba
};

const char *verify_name(Verify);

// Un matcher vacio matchea todo, y eso es deliberado para poder escribir una
// regla global. Cada campo seteado agrega una condicion (AND).
struct Matcher {
    bool       has_category = false;
    Category   category = Category::Unknown;
    bool       has_format = false;
    Format     format = Format::Unknown;
    bool       has_scale = false;
    ScaleClass scale = ScaleClass::Fixed;
    u32        min_w = 0, max_w = 0;   // 0 = sin limite
    u32        min_h = 0, max_h = 0;
    u32        min_array = 0;
    bool       has_square = false;
    bool       square = false;
    u32        require_flags = 0;
    u32        deny_flags = 0;
    DescKey    dkey{0};                // match exacto por clave, opcional
    // Para barrier_filter: los estados de la transicion a descartar.
    bool       has_from = false;
    u32        from_state = 0;
    bool       has_to = false;
    u32        to_state = 0;
};

struct Action {
    u64        id = 0;
    ActionKind kind = ActionKind::None;
    Matcher    match;
    double     scale = 1.0;      // resource_scale
    i32        mip_bias = 0;     // mip_bias
    u32        drs_id = 0;       // drs_settings
    u32        drs_value = 0;
    Verify     verify = Verify::Full;
    bool       enabled = false;
    bool       ab = false;        // entra al harness A/B
    std::string name;
};

struct Profile {
    std::string exe;
    std::string note;
    // Instrumentacion: cada cuantos frames uno deep, y cuantas queries como
    // maximo por frame en modo light. El default es conservador a proposito.
    u32  deep_every = 120;
    u32  query_budget = 64;
    // Harness A/B: cada cuantos frames conmuta y cuantos descarta despues de
    // conmutar (los primeros frames tras un toggle miden caches frias).
    u32  ab_period = 60;
    u32  ab_warmup = 5;
    bool observe_only = true;    // ACT hay que pedirlo explicitamente
    std::vector<Action> actions;

    const Action *find(u64 id) const {
        for (const Action &a : actions) if (a.id == id) return &a;
        return nullptr;
    }
};

struct ParseError {
    int         line = 0;
    std::string msg;
};

struct ParseResult {
    bool                    ok = false;
    Profile                 profile;
    std::vector<ParseError> errors;
};

// Parsea el subconjunto de TOML que usa el perfil: tablas, arrays de tablas,
// pares clave = valor, y tablas inline de un nivel. No hay anidamiento mas
// profundo y no lo va a haber.
ParseResult parse_profile(const char *text, size_t len);

// Serializa de vuelta, para que el overlay pueda guardar lo que el usuario
// toco en vivo sin que nadie tenga que escribir TOML a mano.
std::string write_profile(const Profile &);

// El matcheo en si. Puro, y el nucleo de la promesa de estabilidad frente a
// updates: nada de esto mira un hash de shader.
bool matches(const Matcher &, const ResourceDesc &, const OutputInfo &);

}  // namespace gp
