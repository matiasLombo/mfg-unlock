// loader.h -- que set de Streamline y que snippet corren, y de donde: la
// deteccion de las copias del plugin, el veredicto del set, la cache de
// %LOCALAPPDATA%\mfg-unlock\sdk, la sustitucion en LdrLoadDll y el
// parcheo al mapear cada modulo.
//
// ENTRA: LdrRegisterDllNotification (on_dll_load, por cada dll que se mapea)
//   y LdrLoadDll (hk_ldrload, por cada dll que el juego pide); la copia que
//   ejecuta la identifica copia_que_ejecuta desde hk_slGetFeatureFunction
//   (proxy.cpp); la configuracion (g_inter_fuera, g_snippet_on, g_ota, ...).
// SALE: g_copias / g_copia_ejecuta / g_fase (la topologia y la fase de la
//   sesion), g_dlssg_base, el set objetivo (g_set, armar_set_objetivo), los
//   parches aplicados (via patches.h) y sus conteos en el log, el VEREDICTO
//   de evaluar_invariantes, y el veredicto guardado por juego
//   (ruta_de_estado).
// DEPENDE DE: patches.h, sites.h, diag.h, config.h, ntdll (Ldr*),
//   log_line/log_num, image_has (proxy.cpp, se usa antes) y los globales
//   compartidos que todavia viven en proxy.cpp.
//
// Movido de proxy.cpp en dos rangos (2026-09-11), en su orden; el bloque
// "forwarding" de las exportaciones de version.dll que estaba entre ambos se
// queda en proxy.cpp. Sin tocar una linea del cuerpo.
#pragma once

// Globales que solo usa este modulo (movidas de proxy.cpp).
static int g_cubins_done = 0;
static int g_gates = 0;
static bool g_native_pacer_found = false;  // sticky: one plugin with sites is enough
// Set once that build has actually been seen mapping, which is the only thing
// that makes another copy provably redundant.
static bool g_ota_mapped = false;
static int g_outputs_patched = 0;
static bool g_verdict_read = false;
static bool g_already_substituted = false;                         // M3

// The snippet does not always arrive under the name nvngx_dlssg.dll. NGX keeps
// OTA-updated snippets under ProgramData as <arch>_<appid>.bin, and that copy is
// usually the newest, so it is the one NGX actually uses -- matching on the file
// name alone patches the two copies that get discarded and misses the one that
// counts. Match on the whole path instead. "dlssg" appears in both
// nvngx_dlssg.dll and ...\NGX\models\dlssg\..., and not in sl.dlss_g.dll, which
// spells it with an underscore.
static bool path_has(const UNICODE_STRING *s, const wchar_t *needle) {
    if (s == nullptr || s->Buffer == nullptr) return false;
    const size_t n = s->Length / sizeof(wchar_t);
    size_t want = 0;
    while (needle[want] != 0) ++want;
    if (want == 0 || n < want) return false;
    for (size_t i = 0; i + want <= n; ++i) {
        size_t j = 0;
        while (j < want && lower(s->Buffer[i + j]) == lower(needle[j])) ++j;
        if (j == want) return true;
    }
    return false;
}

// Narrow a wide path for the log; these are all ASCII in practice.
static void log_wide(const char *label, const UNICODE_STRING *s) {
    char buf[512];
    int i = 0;
    while (label[i] != 0 && i < 120) { buf[i] = label[i]; ++i; }
    const size_t n = s ? s->Length / sizeof(wchar_t) : 0;
    for (size_t k = 0; k < n && i < 500; ++k, ++i) {
        wchar_t c = s->Buffer[k];
        buf[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    buf[i] = 0;
    log_line(buf);
}

// Sacar los sitios de un modulo que se descarga.
//
// Con un puntero unico esto no hacia falta: la siguiente copia parcheada lo
// pisaba. Con una lista, un modulo descargado deja punteros colgados y la
// escritura por frame se convierte en una violacion de acceso. Los juegos SI
// descargan sl.dlss_g -- se vio en los logs de Cyberpunk -- asi que sin esto la
// lista es mas peligrosa que el puntero que reemplazo.
// ---------------------------------------------------------------------------
// M1: observar que set de Streamline corre este juego, y decidir si sirve.
//
// El criterio esta escrito en docs/deteccion-del-set.md ANTES de este codigo.
// Lo esencial: CUAL copia queda viva es un hecho que solo se conoce al final,
// no al principio. Anoche la decision se tomaba dentro del hook de carga y eso
// rompio Cyberpunk dos veces, porque ahi todavia no se sabe cual copia va a
// sobrevivir. Asi que la primera corrida OBSERVA y escribe un veredicto; recien
// la siguiente puede actuar, que es lo que pidio el usuario.
//
// Nada de esto sustituye ni parchea: solo mira.
struct PluginCopy {
    const unsigned char *base;
    size_t size;
    unsigned minor;        // 2.<menor>; 0 si no se pudo leer
    int count_sites;     // del parche de la cuenta, por copia
    int pacer_sites;
    bool live;
};
static PluginCopy g_copies[8];
static int g_copies_n = 0;
static unsigned long long g_first_copy_ms = 0;
static bool g_verdict_written = false;

static void register_copy(const unsigned char *base, size_t size, unsigned minor,
                            int count_sites, int pacer_sites) {
    if (g_copies_n >= 8) return;
    PluginCopy &c = g_copies[g_copies_n++];
    c.base = base; c.size = size; c.minor = minor;
    c.count_sites = count_sites; c.pacer_sites = pacer_sites; c.live = true;
    if (g_first_copy_ms == 0) g_first_copy_ms = GetTickCount64();
}


static void executing_copy(const void *fn, const char *name) {
    static bool said = false;
    if (said || fn == nullptr) return;
    const unsigned char *p = (const unsigned char *)fn;
    // Dos pasadas: primero entre las que siguen mapeadas.
    //
    // Halo descarga y vuelve a cargar sl.dlss_g en la MISMA base, asi que dos
    // entradas del registro comparten rango y la vieja -- muerta -- ganaba el
    // match. El instrumento lo delato en su primera corrida: dijo "copia EJECUTA
    // indice 0, sigue mapeada 0" junto a "copia inactiva indice 1, sigue mapeada
    // 1", que es una contradiccion. Un puntero que el interposer acaba de
    // resolver no puede caer en un modulo descargado.
    int which = -1;
    for (int pass_n = 0; pass_n < 2 && which < 0; ++pass_n) {
        for (int i = 0; i < g_copies_n; ++i) {
            if (g_copies[i].base == nullptr) continue;
            if (pass_n == 0 && !g_copies[i].live) continue;
            if (p >= g_copies[i].base && p < g_copies[i].base + g_copies[i].size) {
                which = i;
                break;
            }
        }
    }
    // Solo se cierra el diagnostico si el puntero CAYO. Si no cayo se deja
    // constancia y se vuelve a intentar con la funcion siguiente: latchear un
    // fallo es quedarse con el peor dato de la corrida.
    if (which >= 0) {
        said = true;
        g_executing_copy = which;
        g_executing_resolved = true;
    }
    // La fase se decide con este dato, y no siempre llega antes del primer
    // frame token: en GTA V el token fue a los 34 s y esto a los 38. Asumir un
    // orden que el juego no garantiza es la misma clase de error que decidir la
    // copia viva por orden de mapeo. Se re-evalua ahora que el dato existe.
    if (g_executing_resolved) {
        g_phase = (LONG)Phase::ARMED;
        evaluate_invariants();
    }
    log_line("--- CAPA 0: que copia de sl.dlss_g ejecuta ---");
    log_line(name);
    if (which < 0) {
        static int warnings = 0;
        if (warnings < 4) {
            ++warnings;
            log_num("  el puntero NO cae en ninguna de las copias registradas; van ",
                    (unsigned)g_copies_n);
            log_line("  (se reintenta con la proxima funcion de DLSS-G)");
        }
        return;
    }
    for (int i = 0; i < g_copies_n; ++i) {
        log_num(i == which ? "  copia EJECUTA, indice " : "  copia inactiva, indice ",
                (unsigned)i);
        log_num("    version 2.", (unsigned)g_copies[i].minor);
        log_num("    sitios de cuenta ", (unsigned)g_copies[i].count_sites);
        log_num("    sitios de pacer ", (unsigned)g_copies[i].pacer_sites);
        log_num("    sigue mapeada (1 = si) ", (unsigned)(g_copies[i].live ? 1 : 0));
    }
    // Y lo que importa de verdad: si la que ejecuta no recibio los parches, todo
    // lo que midamos despues es sobre un binario que no tocamos.
    if (g_copies[which].count_sites <= 0 || g_copies[which].pacer_sites <= 0) {
        diag::Line l = diag::invariant(diag::Layer::IDENTITY, "copia-parcheada",
                                         "la copia que ejecuta no tiene todos los parches");
        l.pair("copia", which).pair("cuenta", g_copies[which].count_sites)
         .pair("pacer", g_copies[which].pacer_sites);
        log_line(l.b);
    } else
        log_line("  la copia que ejecuta tiene cuenta y pacer parcheados");
    // Comparacion contra el puntero que el resto del archivo viene usando.
    {
        const unsigned char *b = (const unsigned char *)g_dlssg_base;
        log_num("  g_dlssg_base apunta a la copia que ejecuta (1 = si) ",
                (unsigned)(b == g_copies[which].base ? 1 : 0));
    }
}

// Los invariantes de la topologia, evaluados UNA vez y tarde.
//
// Tarde a proposito: en el primer frame token ya cargo todo el grafo -- el
// interposer, sus plugins y el snippet -- asi que lo que se ve aca es lo que va
// a correr. Decidir al principio es lo que fallaba: g_dlssg_base se asignaba a
// la ultima copia mapeada y deteccion-del-set.md ya decia que "cual copia queda
// viva es un hecho observado al final, no deducible al principio".
//
// Esta version es DELIBERADAMENTE permisiva: solo cae en PASIVO si no hay
// ninguna copia viva identificada por ejecucion, que es la unica condicion bajo
// la cual todo lo que hagamos despues seria sobre un binario que no sabemos
// cual es. Los demas invariantes se reportan pero no bloquean, porque todavia
// no estan medidos en los tres juegos y apagar el mod por uno de ellos seria
// exactamente el tipo de regla no verificada que este trabajo viene a sacar.
static void evaluate_invariants(void) {
    if (g_phase != (LONG)Phase::ARMED) return;
    int live_n = 0, with_count = 0, with_pacer = 0;
    for (int i = 0; i < g_copies_n; ++i) {
        if (!g_copies[i].live) continue;
        ++live_n;
        if (g_copies[i].count_sites > 0) ++with_count;
        if (g_copies[i].pacer_sites > 0) ++with_pacer;
    }
    // Una linea con todos los numeros de la topologia, y el veredicto adelante.
    const bool passive = g_executing_resolved && g_executing_copy < 0;
    diag::Line l = diag::verdict(passive ? "PASIVO" : "ACTIVO",
                                    passive ? "no se identifico que copia ejecuta; no se parchea ni se reescriben opciones"
                                           : "topologia identificada");
    l.pair("copias", g_copies_n).pair("vivas", live_n).pair("con_cuenta", with_count)
     .pair("con_pacer", with_pacer).pair("ejecuta", g_executing_copy)
     .pair("ejecuta_cuenta", g_executing_copy >= 0 ? g_copies[g_executing_copy].count_sites : -1)
     .pair("ejecuta_pacer", g_executing_copy >= 0 ? g_copies[g_executing_copy].pacer_sites : -1)
     .pair("multiplicador", count_is_multiplier() ? 1 : 0)
     .pair("resuelto", g_executing_resolved ? 1 : 0);
    log_line(l.b);
    if (passive) {
        g_phase = (LONG)Phase::PASSIVE;
        return;
    }
    g_phase = (LONG)Phase::VERIFIED;
    g_phase = (LONG)Phase::ACTIVE;
}

static void copy_unloaded(const unsigned char *base, size_t size) {
    for (int i = 0; i < g_copies_n; ++i) {
        if (g_copies[i].base == base && g_copies[i].size == size) g_copies[i].live = false;
    }
}

// VERDE = 0, AMARILLO = 1, ROJO = 2. Ver docs/deteccion-del-set.md.
static int set_verdict(int *live_out, int *live_with_site_out) {
    int live_n = 0, with_site = 0;
    for (int i = 0; i < g_copies_n; ++i) {
        if (!g_copies[i].live) continue;
        ++live_n;
        if (g_copies[i].count_sites > 0) ++with_site;
    }
    if (live_out != nullptr) *live_out = live_n;
    if (live_with_site_out != nullptr) *live_with_site_out = with_site;
    // Dos estados, no tres.
    //
    // Hubo un AMARILLO para "anda pero es fragil" (mas de una copia mapeada, que
    // es el caso Cyberpunk). Se retiro porque nunca cambiaba ninguna decision:
    // verde no sustituye y amarillo tampoco. Una distincion que no decide nada
    // es ruido en un criterio, y ademas invitaba a tratar "raro" como "roto",
    // que es exactamente el error que rompio Cyberpunk dos veces.
    //
    // Se colapsa hacia VERDE, nunca hacia ROJO: si el caso de varias copias
    // quedara en rojo, pasaria a ser candidato a sustitucion y volveriamos al
    // mismo pozo.
    //
    // La cantidad de copias no se pierde -- se sigue loguendo -- pero deja de
    // ser un veredicto.
    // Tajante: VERDE solo si esta TODO limpio. Cualquier rareza, por minima que
    // sea, va a ROJO y se ofrece reemplazo.
    //
    // Antes VERDE era "hay alguien vivo que puede contar", y el caso de varias
    // copias caia ahi. Se cambio porque la objecion que sostenia lo anterior
    // resulto falsa: yo decia que sustituirle el set a un juego que anda lo
    // rompe, apoyado en un crash que NUNCA diagnostique -- sin dump, sin modulo,
    // sin offset, y con un mecanismo distinto del actual.
    //
    // Se midio. Cyberpunk forzado a ROJO, con la sustitucion activa, tres veces:
    //
    //     sustituciones 7, corrida completa, p90 4.00 / 3.93 / 3.97, max ~4.1
    //     linea base sin sustituir:          p90 3.97
    //
    // Indistinguible. Y de paso quedo probado que el cruce de trenes funciona:
    // corrio con el interposer 2.7 del juego y siete plugins 2.12.
    if (g_copies_n != 1) return 2;                // mas de una copia, o ninguna
    if (live_n != 1 || with_site != 1) return 2;   // se descargo, o no puede contar
    if (g_copies[0].pacer_sites <= 0) return 2;  // sin pacer
    // La version del interposer se lee aca y no de g_set_version, que solo se
    // llena cuando ya se decidio sustituir -- o sea nunca en la corrida que
    // observa, que es justo donde este chequeo tiene que valer.
    {
        HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
        if (interposer != nullptr && g_copies[0].minor != 0) {
            wchar_t ri[MAX_PATH];
            if (GetModuleFileNameW(interposer, ri, MAX_PATH) != 0) {
                unsigned mi = 0, ni = 0;
                version_supported(ri, &mi, &ni);
                if (ni != 0 && ni != g_copies[0].minor) return 2;   // set mezclado
            }
        }
    }
    return 0;                                     // VERDE: limpio
}

// El estado va a carpeta propia, NUNCA al lado del juego: [[ships-as-one-dll]].
static bool state_path(wchar_t *out, int max) {
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) return false;
    wchar_t dir[MAX_PATH];
    int k = 0;
    for (; base[k] != 0 && k < MAX_PATH - 32; ++k) dir[k] = base[k];
    const wchar_t *sub = L"\\mfg-unlock";
    for (int i = 0; sub[i] != 0; ++i) dir[k++] = sub[i];
    dir[k] = 0;
    CreateDirectoryW(dir, nullptr);
    // La clave es el NOMBRE del ejecutable mas su tamano, no la ruta completa.
    //
    // Con la ruta completa el veredicto no persiste en el banco: el worker se
    // copia a una carpeta nueva en cada corrida, asi que la clave cambiaba
    // siempre y la segunda corrida nunca encontraba lo que dejo la primera. Sin
    // eso, el mecanismo entero no se puede probar en la unica herramienta
    // disponible. Nombre + tamano es estable entre corridas y sigue separando
    // juegos distintos.
    wchar_t exe[MAX_PATH];
    const DWORD nn = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    int cut = (int)nn;
    while (cut > 0 && exe[cut-1] != L'\\' && exe[cut-1] != L'/') --cut;
    unsigned long long h = 1469598103934665603ULL;
    for (int i = cut; i < (int)nn; ++i) {
        wchar_t c = exe[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        h ^= (unsigned long long)c; h *= 1099511628211ULL;
    }
    {
        HANDLE hf = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(hf, &sz)) {
                h ^= (unsigned long long)sz.QuadPart; h *= 1099511628211ULL;
            }
            CloseHandle(hf);
        }
    }
    const DWORD n = nn;
    int p = k;
    const wchar_t *sep = L"\\set-";
    for (int i = 0; sep[i] != 0; ++i) dir[p++] = sep[i];
    for (int i = 15; i >= 0; --i) {
        const unsigned d = (unsigned)((h >> (i * 4)) & 0xF);
        dir[p++] = (wchar_t)(d < 10 ? (L'0' + d) : (L'a' + d - 10));
    }
    const wchar_t *ext = L".txt";
    for (int i = 0; ext[i] != 0; ++i) dir[p++] = ext[i];
    dir[p] = 0;
    if (p >= max) return false;
    for (int i = 0; i <= p; ++i) out[i] = dir[i];
    return true;
}

// Se emite UNA vez, y no antes de que el set haya terminado de acomodarse.
//
// Cuanto hay que esperar, medido y no elegido a ojo: en Cyberpunk la segunda
// copia aparece 60 ms despues de la primera; en el banco con --ota las TRES
// copias estan mapeadas a los 1281 ms; en Halo el ciclo entero tarda ~1.5 s.
//
// Estaba en 20 s "por holgura" y eso costaba corridas enteras: el banco falla
// seguido y una corrida que muere a los 5 s no llegaba a emitir nada, asi que el
// fixture no se podia medir. 8 s sigue siendo cinco veces el peor asentamiento
// visto y sobrevive a las corridas cortas.
static void emit_verdict_if_due(void) {
    if (g_verdict_written || g_copies_n == 0) return;
    if (GetTickCount64() - g_first_copy_ms < 8000ULL) return;
    g_verdict_written = true;

    int live_n = 0, with_site = 0;
    const int v = set_verdict(&live_n, &with_site);
    // El indice 1 ya no se produce; se deja el nombre para poder leer archivos
    // de estado viejos que digan AMARILLO (parsean a 1, que no habilita nada).
    static const char *kNombre[3] = { "VERDE", "AMARILLO", "ROJO" };
    log_line("--- veredicto del set de Streamline ---");
    log_num("  copias vistas ", (unsigned)g_copies_n);
    log_num("  vivas ", (unsigned)live_n);
    log_num("  vivas con el sitio de la cuenta ", (unsigned)with_site);
    for (int i = 0; i < g_copies_n; ++i) {
        log_num("  copia 2.", (unsigned)g_copies[i].minor);
        log_num("    sitios de cuenta ", (unsigned)g_copies[i].count_sites);
        log_num("    sitios de pacer ", (unsigned)g_copies[i].pacer_sites);
        log_num("    viva (1 = si) ", (unsigned)(g_copies[i].live ? 1 : 0));
    }
    char b[64] = "  VEREDICTO: ";
    int k = 13;
    for (const char *q = kNombre[v]; *q != 0; ++q) b[k++] = *q;
    b[k] = 0;
    log_line(b);
    if (v == 2) log_line("  (rojo: el set no esta limpio; candidato a sustitucion)");
    else        log_line("  (verde: una sola copia, viva, con sitios y de la version del interposer)");
    // Aca hubo una nota que decia que varias copias es "la configuracion mas
    // fragil". Se retiro por dos razones. Es redundante: dos lineas mas arriba
    // ya estan "copias vistas N" y el detalle por copia con cual sobrevivio, que
    // es el dato crudo. Y es un juicio, no un hecho -- se apoya en dos
    // incidentes en un solo juego, no en un experimento. Un veredicto con
    // matices es un tercer estado disfrazado, que es justo lo que se quiso sacar.

    // El diagnostico se guarda SOLO si no hubo sustitucion.
    //
    // Si se guardara siempre, el veredicto describiria el set YA arreglado --
    // que es sano, o sea VERDE -- y la corrida siguiente no sustituiria nada,
    // volviendo al set roto. Andaria una corrida si y una no.
    //
    // En el banco esto no se veia porque cada corrida lanza dos pases: el de
    // referencia no sustituye y reescribe ROJO, y el de objetivo sustituye. En
    // un juego real, con un solo pase por ejecucion, la oscilacion es real.
    //
    // Guardando solo el diagnostico sin sustituir, el ROJO queda pegado y la
    // sustitucion se sigue aplicando en cada arranque.
    if (g_already_substituted) {
        log_line("  (hubo sustitucion: no se pisa el diagnostico guardado)");
        return;
    }
    wchar_t path[MAX_PATH];
    if (!state_path(path, MAX_PATH)) return;
    // El consentimiento tiene que sobrevivir a esta reescritura.
    //
    // Se abre con CREATE_ALWAYS: trunca. Se escribian solo veredicto, vivas y
    // con_sitio, asi que la linea "consentimiento=" que M4 anexa se borraba y el
    // permiso duraba UNA corrida. Medido en Cyberpunk: 12:00 sustituyo 18
    // modulos con permiso; 12:01 el archivo ya no tenia la linea; la corrida
    // siguiente mapeo su 2.11 y la OTA 2.14 y no sustituyo nada. Es el mismo
    // defecto de "una corrida si y una no" que ya se corrigio para el veredicto,
    // ahora en el permiso.
    //
    // g_ya_sustituimos no lo tapa: Cyberpunk levanta DOS procesos con el dll
    // adentro y alcanza con que llegue aca el que no sustituyo.
    //
    // Se relee del disco en vez de confiar en g_consentimiento: quien lo llena
    // es el lazo de teclas, que no corre en todos los procesos ni bajo el banco.
    // leer_veredicto_previo es idempotente.
    read_previous_verdict();
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char txt[192];
    int t = 0;
    const char *header = "veredicto=";
    for (const char *q = header; *q != 0; ++q) txt[t++] = *q;
    for (const char *q = kNombre[v]; *q != 0; ++q) txt[t++] = *q;
    txt[t++] = '\n';
    const char *l2 = "vivas=";
    for (const char *q = l2; *q != 0; ++q) txt[t++] = *q;
    txt[t++] = (char)('0' + (live_n > 9 ? 9 : live_n));
    txt[t++] = '\n';
    const char *l3 = "con_sitio=";
    for (const char *q = l3; *q != 0; ++q) txt[t++] = *q;
    txt[t++] = (char)('0' + (with_site > 9 ? 9 : with_site));
    txt[t++] = '\n';
    if (g_consent >= 0) {
        const char *l4 = g_consent == 1 ? "consentimiento=si\n"
                                               : "consentimiento=no\n";
        for (const char *q = l4; *q != 0; ++q) txt[t++] = *q;
    }
    DWORD esc = 0;
    WriteFile(h, txt, (DWORD)t, &esc, nullptr);
    CloseHandle(h);
    if (g_consent >= 0)
        log_num("  consentimiento conservado (1 = si) ", (unsigned)g_consent);
    log_line("  estado guardado en LOCALAPPDATA\\mfg-unlock");
}
// ---------------------------------------------------------------------------

static void site_drop(volatile unsigned char **list, int *n,
                       const unsigned char *base, size_t size) {
    int w = 0;
    for (int i = 0; i < *n; ++i) {
        const unsigned char *q = (const unsigned char *)list[i];
        const bool inside = q >= base && q < base + size;
        if (!inside) list[w++] = list[i];
    }
    for (int i = w; i < *n; ++i) list[i] = nullptr;
    *n = w;
}

static VOID CALLBACK on_dll_load(ULONG reason, const DllNotifyData *d, PVOID) {
    if (reason == 2 && d != nullptr) {                       // 2 = UNLOADED
        const unsigned char *b = (const unsigned char *)d->DllBase;
        const size_t size = (size_t)d->SizeOfImage;
        const int before = g_wic_n + g_imm_n + g_imm2_n + g_imm3_n;
        site_drop(g_wic_sites,  &g_wic_n,  b, size);
        site_drop(g_imm_sites,  &g_imm_n,  b, size);
        site_drop(g_imm2_sites, &g_imm2_n, b, size);
        site_drop(g_imm3_sites, &g_imm3_n, b, size);
        copy_unloaded(b, size);
        // Los punteros a funciones del plugin/interposer quedan COLGANDO cuando
        // su modulo se descarga. site_drop limpia los sitios de datos, pero no
        // estos. Si no se anulan, una llamada de INICIATIVA PROPIA
        // (apply_override_now -> g_orig_setoptions) entra en memoria desmapeada
        // = 0xc0000005 en una direccion sin modulo. Es el crash del reload que
        // aparece con fracres, que dispara el override mas seguido (medido:
        // 0xc0000005 2.2 s despues de "modulo descargado"). Cyberpunk descarga y
        // recarga el plugin 2x por corrida (las dos copias). Anular deja que las
        // guardas "== nullptr" que ya existen salten, y no reproduce un override
        // pendiente sobre un plugin que ya no esta.
        {
            const ULONG_PTR lo = (ULONG_PTR)b, hi = lo + size;
            #define DROP_PFN_IF_IN(p) do { if ((ULONG_PTR)(p) >= lo && (ULONG_PTR)(p) < hi) (p) = nullptr; } while (0)
            DROP_PFN_IF_IN(g_orig_setoptions);
            DROP_PFN_IF_IN(g_orig_getfeaturefn);
            DROP_PFN_IF_IN(g_orig_pclmarker);
            DROP_PFN_IF_IN(g_orig_reflexmarker);
            DROP_PFN_IF_IN(g_orig_reflexstate);
            DROP_PFN_IF_IN(g_orig_slinit);
            #undef DROP_PFN_IF_IN
            if (g_orig_setoptions == nullptr) { g_opt_pending = 0; g_opt_have = 0; }
        }
        const int now_qpc = g_wic_n + g_imm_n + g_imm2_n + g_imm3_n;
        if (now_qpc != before) {
            log_line("modulo descargado: se retiran sus sitios parcheados");
            log_num("  sitios que quedan ", (unsigned)now_qpc);
        }
        return;
    }
    if (reason != 1 || d == nullptr) return;                 // 1 = LDR_DLL_NOTIFICATION_REASON_LOADED

    // Load order matters for reading the Streamline log afterwards: the plugin
    // caches the maximum at its startup, so it has to come after the snippet.
    if (name_is(d->BaseDllName, L"sl.interposer.dll")) {
        log_line("sl.interposer.dll mapped");
        arm_multiplier_override((unsigned char *)d->DllBase);
        return;
    }
    // Match the path, not the file name. Streamline prefers a newer plugin from
    // NVIDIA's OTA cache when it finds one -- Cyberpunk 2077 loads
    // ...\NGX\models\sl_dlss_g_0\versions\<id>\files\190_E658703.dll and leaves
    // the copy in its own folder unused. Matching "sl.dlss_g.dll" then patches the
    // file that never runs and reports zero sites on the one that does, which is
    // exactly the trap the snippet already taught us. "sl_dlss_g" catches the cache
    // layout and "sl.dlss_g" the game-folder one; patching both is harmless, since
    // a signature either matches an image or leaves it alone.
    if (path_has(d->FullDllName, L"sl.pcl") || path_has(d->FullDllName, L"sl_pcl")) {
        // El registro ETW doble de PCL abortaba GTA V al arrancar, a veces
        // (ver sites::kPclRegister). Se parchea cualquier copia con el sitio.
        const int n = patch_pcl_register(reinterpret_cast<unsigned char *>(d->DllBase));
        log_num("sl.pcl mapped; registro ETW doble tolerado, sitios: ", (unsigned)n);
    }
    if (path_has(d->FullDllName, L"sl.dlss_g") ||
        path_has(d->FullDllName, L"sl_dlss_g")) {
        if (!g_dynamic_known &&
            image_has((unsigned char *)d->DllBase, "sl::DLSSGMode::eDynamic")) {
            g_dynamic_known = true;
            log_line("  this copy knows DLSSGMode::eDynamic");
        }
        log_line("sl.dlss_g mapped");
        log_wide("  in ", d->FullDllName);
        // Solo se parchea lo que soportamos: 2.12 y 2.13.
        //
        // Hasta ahora se parcheaba cualquier copia que tuviera los sitios, y en
        // Halo eso incluia una 2.14 que Streamline carga por su cuenta -- el
        // juego enciende eAllowOTA y eLoadDownloadedPlugins por si mismo, no
        // nosotros. Esa copia falla en +0x3F2E9 leyendo un nulo a +0x40, con el
        // interposer 2.7.30 del juego en la misma lista de modulos, y lo hace
        // igual con nuestra redireccion entregando una 2.12 correcta. Dos dumps
        // distintos, el mismo modulo y el mismo offset.
        //
        // Que ese crash sea nuestro o del juego todavia no esta demostrado. Lo
        // que si es claro es que no tenemos por que escribirle a una version
        // que nunca validamos, y dejarla intacta separa las dos cosas: si sigue
        // crasheando sin que la toquemos, el crash es del par que arma el juego.
        {
            const UNICODE_STRING *u = d->FullDllName;
            if (u != nullptr && u->Buffer != nullptr) {
                const int nn = (int)(u->Length / sizeof(wchar_t));
                if (nn > 0 && nn < MAX_PATH) {
                    wchar_t path[MAX_PATH];
                    for (int i = 0; i < nn; ++i) path[i] = u->Buffer[i];
                    path[nn] = 0;
                    unsigned major = 0, minor_v = 0;
                    version_supported(path, &major, &minor_v);
                    // Se informa la version y NO se rechaza nada.
                    //
                    // Aca hubo un filtro que se negaba a parchear lo que no
                    // fuera 2.11/2.12/2.13, y el benchmark de Cyberpunk lo
                    // desarmo: ese juego mapea su 2.11 y despues una copia OTA
                    // 2.14; la 2.11 se descarga, y si la 2.14 quedo sin parchear
                    // se corre entera sin sitios. Medido: 622 ventanas, mediana
                    // 1.00 -- el 2.00 del final es el FG nativo del juego, no
                    // nuestro.
                    //
                    // O sea que en los dos juegos el plugin VIVO es la copia OTA
                    // 2.14, y parchearla es lo que hacia andar a Cyberpunk. El
                    // crash de Halo no es "la version 2.14": es el par 2.7.30 +
                    // 2.14 que arma Halo, porque Cyberpunk usa esa misma 2.14 y
                    // no crashea. Castigar a la version era castigar al testigo.
                    //
                    // Halo se resuelve donde corresponde, en la carga: alli la
                    // 2.14 ni llega a mapearse.
                    log_num("  version: 2.", (unsigned long long)minor_v);
                }
            }
        }
        // Marcar nada mas: esto corre bajo el loader lock y LoadLibrary desde
        // aca falla por diseno. La carga se hace en el camino de present.
        if (g_twocopies) g_twocopies_pending = 1;
        {
            g_dlssg_base = reinterpret_cast<const unsigned char *>(d->DllBase);
            if (g_six) {
                const int t6 = patch_cap_six(reinterpret_cast<unsigned char *>(d->DllBase));
                log_num("  tope del plugin subido a 6, sitios: ", (unsigned)t6);
                if (t6 < 2) log_line("  ! faltan sitios: uno solo no alcanza");
            }
            const int sc = patch_subframe_count(reinterpret_cast<unsigned char *>(d->DllBase));
            log_num("  sub-frame count made writable, sites: ", (unsigned)sc);
        }
        const int n = patch_enable_cpu_pacer(reinterpret_cast<unsigned char *>(d->DllBase));
        g_outputs_patched += n;
        log_num("  CPU pacer enabled, sites: ", (unsigned)n);
        // M1: se anota esta copia tal como quedo. No decide nada todavia.
        {
            unsigned major_c = 0, minor_c = 0;
            const UNICODE_STRING *u = d->FullDllName;
            if (u != nullptr && u->Buffer != nullptr) {
                const int nn = (int)(u->Length / sizeof(wchar_t));
                if (nn > 0 && nn < MAX_PATH) {
                    wchar_t rr[MAX_PATH];
                    for (int i = 0; i < nn; ++i) rr[i] = u->Buffer[i];
                    rr[nn] = 0;
                    version_supported(rr, &major_c, &minor_c);
                }
            }
            register_copy(reinterpret_cast<const unsigned char *>(d->DllBase),
                            (size_t)d->SizeOfImage, minor_c, g_wic_sites_last, n);
            g_wic_sites_last = -1;
        }
        if (g_meter_off) {
            const int mo = patch_metering_off(reinterpret_cast<unsigned char *>(d->DllBase));
            log_num("  driver flip metering switched off, sites: ", (unsigned)mo);
        }
        if (g_queue_mode >= 0) {
            const int q = patch_queue_mode(reinterpret_cast<unsigned char *>(d->DllBase), g_queue_mode);
            log_num("  queue parallelism mode forced to ", (unsigned)g_queue_mode);
            log_num("    sites: ", (unsigned)q);
        }
        // One verdict per module, not one per process: this callback runs for
        // every copy of the plugin that maps, and they do not all get the same
        // result -- the game-folder copy and NVIDIA's OTA copy have reported
        // different site counts in the same session. An aggregate would average
        // that into a number belonging to neither.
        // Nothing matched here, so this build has no pacer for us to enable --
        // in 2.8.0 and 2.9 there is not one to enable at all. Rather than leave
        // the generated frames to go out four-at-a-time, space them in the
        // present hook. One plugin finding sites is enough to call it off: the
        // native one is better than ours and they must not both run.
        // Sticky, and deliberately: this callback runs once per copy of the
        // plugin that maps, and a game can map two of different vintages. A
        // plain assignment let the last one win, so a patched OTA copy mapping
        // before the game's own stale one would leave our pacer switched on in
        // a process that already had NVIDIA's. One copy with sites is enough
        // to settle it for the process.
        if (n > 0) {
            g_native_pacer_found = true;
            log_line("  => pacer OK");
        } else if (!g_native_pacer_found) {
            // Said "spacing generated frames ourselves" until 2026-09-04,
            // when the fallback pacer that would have done so was removed: it
            // never changed anything a player could see, and once a form of
            // the pacer patch existed for every Streamline family on disk it
            // could no longer engage at all. What is left is a warning, and it
            // is worth keeping -- a build with no site here is one nobody has
            // looked at yet.
            log_line("  => no pacer site found in this build -- 4x may not be paced");
        } else {
            log_line("  => no pacer here, but another copy has one -- leaving it to that");
        }
        return;
    }
    if (name_is(d->BaseDllName, L"_nvngx.dll") || name_is(d->BaseDllName, L"nvngx.dll")) {
        log_line(name_is(d->BaseDllName, L"nvngx.dll") ? "nvngx.dll mapped"
                                                       : "_nvngx.dll mapped");
        return;
    }

    if (!path_has(d->FullDllName, L"dlssg")) return;
    // A build sitting in the OTA cache is not proof NGX will load it. Avatar:
    // Frontiers of Pandora has 20318464 cached and maps only its own copy --
    // the same way GTA V's sl.log says "OTA'd plugins will not be loaded!",
    // it is the game's decision, not ours to predict. Assuming the cache wins
    // made the verdict call the copy that actually ran a shadow and tell the
    // reader to ignore it: the precise failure the verdict exists to prevent,
    // in the opposite direction.
    //
    // So claim nothing until the OTA copy actually maps. Until then a copy is
    // treated as possibly-live and reports in full; once the OTA build has
    // been seen, anything else really is redundant and can say so.
    const bool is_ota = g_ota_newest[0] != 0 && path_has(d->FullDllName, g_ota_newest);
    if (is_ota) g_ota_mapped = true;
    const bool shadow = !is_ota && g_ota_mapped;
    const bool live = !shadow;
    detect_semantics(reinterpret_cast<unsigned char *>(d->DllBase));
    const int n = patch_gates(reinterpret_cast<unsigned char *>(d->DllBase));
    if (n > 0) ++g_gates;
    log_num("  gates rewritten: ", (unsigned)n);
    // Solo sobre NUESTRO snippet. detectar_semantica corrio dos lineas arriba
    // sobre este mismo modulo, asi que la respuesta es sobre el binario que
    // quedo mapeado y no sobre un flag ni sobre el nombre del juego.
    //
    // Sobre la build de julio esto seria el crash de Halo otra vez: topa en 3
    // por arquitectura, y pedirle mas entrega CERO frames, no menos.
    if (g_six && count_is_multiplier()) {
        const int sm = patch_snippet_max(reinterpret_cast<unsigned char *>(d->DllBase), 6);
        log_num("  tope del snippet subido a 6, sitios: ", (unsigned)sm);
        if (sm == 0) log_line("  ! no se encontro el sitio: el maximo sigue en 5");
    } else if (g_six) {
        log_line("  tope del snippet NO se toca: no es nuestra build");
    }
    if (g_mfcmax >= 2) {
        const int mm = patch_multiframe_max(reinterpret_cast<unsigned char *>(d->DllBase));
        log_num("  MultiFrameCountMax forzado, sitios: ", (unsigned)mm);
        log_num("    valor ", (unsigned)g_mfcmax);
    }
    if (g_preset_b) {
        const int pb = patch_preset_b(reinterpret_cast<unsigned char *>(d->DllBase));
        log_num("  interpolation preset B (UIR) forced, sites: ", (unsigned)pb);
    }
    // Only the copy NGX actually loads is worth rebuilding, and it is the one
    // whose gates took: the driver-store fallback reports 0 and is never used.
    if (g_cubins && n > 0) {
        const int cb = patch_cubins(reinterpret_cast<unsigned char *>(d->DllBase), live);
        g_cubins_done += cb;
        log_num("  kernels rebuilt from the Blackwell PTX: ", (unsigned)cb);
        if (shadow) {
            log_line("  => redundant: the OTA build already mapped, this copy is unused");
        } else if (cb == 0) {
            // Was `cb != 3`, from when the header held one build's three
            // kernels. It now covers several builds at once and a snippet
            // takes only its own subset -- Avatar's 310.3 has five where
            // 310.9 has three -- so a fixed count reported "NOT APPLIED"
            // directly under a line saying five had been rebuilt. Nothing had
            // gone wrong; the verdict was measuring the wrong thing. Zero is
            // the only count that means failure.
            //
            // The failure this exists for is silent in play: the unlock still
            // works, the frames still generate, they are just slower kernels.
            // Nothing errors, so say the fix out loud rather than leaving a
            // count to be recognised as wrong.
            log_line("  => CUBINS NOT APPLIED -- unlock works, but 4x will judder");
            log_line("     cubins.h was built for dlssg build:");
            log_line(kCubinsBuiltFor);
            log_line("     the snippet loaded here is the path below; if they differ,");
            log_line("     rerun: python tools/rebuild_cubins.py && sh build-proxy.sh");
        } else {
            log_line("  => gates and cubins OK");
        }
    } else if (n > 0) {
        log_line(shadow ? "  => redundant: the OTA build already mapped, this copy is unused"
                        : "  => gates OK");
    }
    log_wide("  in ", d->FullDllName);
}


// Cargar un plugin que SI se pueda parchear, elegido por lo que tiene adentro
// y no por el juego que lo pide.
//
// El parche de la cuenta esta atado a la version del plugin, no al titulo. En
// Halo eso quedo a la vista: carga un sl.dlss_g de Streamline 2.7.30, el sitio
// de la cuenta no existe en ese tren, y el log lo decia sin que nadie lo leyera:
//
//     ! work item count site not unique, sites: 0
//     PARCHE DE CUENTA: NO engancho en esta copia
//
// El gate si se reescribe, asi que MFG queda habilitado y la API acepta x3..x6,
// pero el bucle sigue generando UN frame. Medido contando presentaciones: x2 da
// 2.00, y x3 y x4 dan 2.00 tambien. No andaba mal, era el unico valor posible.
//
// Sitios encontrados en los builds de esta maquina:
//
//     2.7.10  2.7.30  2.8.11  2.8.12  2.10.0-3      0 sitios
//     2.11.0  2.12.129  2.13.0  2.14.0             1 sitio
//
// Por eso forzar OTA no alcanzaba: las banderas de slInit ya estaban puestas
// (banderas en +88 77, bits 3 y 6 en 1) y el unico build de la cache del mismo
// tren que Halo, el 2.7.10, tampoco tiene el sitio.
//
// La regla de aca no nombra ningun juego ni ninguna ruta de juego: si el plugin
// que se esta por cargar NO tiene el sitio, se busca en la cache de NGX uno que
// SI lo tenga y se carga ese en su lugar. Si el del juego ya sirve, no se toca
// nada -- de modo que esto no le puede cambiar el comportamiento a un juego que
// hoy funciona, solo puede ayudar a uno que hoy no.
static int count_sites_in(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 16 || sz.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return -1;
    }
    const DWORD n = (DWORD)sz.QuadPart;
    unsigned char *buf = (unsigned char *)VirtualAlloc(nullptr, n, MEM_COMMIT, PAGE_READWRITE);
    if (buf == nullptr) { CloseHandle(h); return -1; }
    DWORD read_n = 0;
    const BOOL ok = ReadFile(h, buf, n, &read_n, nullptr);
    CloseHandle(h);
    int sitios = 0;
    if (ok && read_n == n) {
        // La misma firma que usa patch_work_item_count, sobre el archivo en vez
        // de sobre el modulo mapeado: mov eax,[rdx+4] / mov r8d,0xC0 / mov [rcx+4],eax
        for (DWORD i = 0; i + 12 < n; ++i) {
            if (buf[i] != 0x8B || buf[i+1] != 0x42 || buf[i+2] != 0x04) continue;
            if (buf[i+3] != 0x41 || buf[i+4] != 0xB8 || buf[i+5] != 0xC0) continue;
            if (buf[i+9] != 0x89 || buf[i+10] != 0x41 || buf[i+11] != 0x04) continue;
            ++sitios;
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return sitios;
}

// Ademas del sitio, la VERSION tiene que ser una de las que soportamos: 2.12 o
// 2.13. Son contra las que se desarrollo todo esto -- los offsets estan
// anotados en este mismo archivo como "0x45da en 2.13, 0x45e2 en 2.12" -- y las
// unicas probadas en juego, GTA V y Cyberpunk.
//
// Este filtro se agrego despues de un crash, y el error que lo causo fue mio:
// se eligio el build mas nuevo de la cache (2.14.0) razonando que "es lo que
// hace la OTA de NVIDIA". Cargo, el parche de la cuenta engancho, x3..x6
// anduvieron sesenta segundos, y despues murio adentro del plugin:
//
//     0xC0000005 lectura en 0x0000000000000040
//     ...\sl_dlss_g_0\versions\134656\files\190_E658703.dll +0x3F2E9
//     con sl.interposer 2.7.30 del juego en la misma lista de modulos
//
// Un nulo desreferenciado a +0x40 es la forma de un desajuste de ABI entre
// interposer y plugin, no de un parche mal puesto. Que un build cargue y ande
// un rato NO es que sea compatible.
//
// Se miro tambien elegir por cercania al interposer, porque Cyberpunk reparte
// un interposer 2.7.1 con un plugin 2.11.1 y funciona. Pero eso valida ESE par,
// no valida nuestros parches en 2.11: de 2.11 se comprobo una sola firma, la de
// la cuenta, y los demas sitios no se verificaron nunca en ese tren. Un solo
// patron de bytes no alcanza para declarar soportada una version.
// La version se saca leyendo el archivo, NO con GetFileVersionInfo.
//
// Esta funcion se llama desde adentro de LdrLoadDll, con el loader lock tomado.
// La API de version vive en version.dll, que en este proceso somos nosotros y
// hay que resolver contra la real -- y resolverla implica un LoadLibrary. Pedir
// una carga desde adentro del loader es exactamente como se consigue un
// deadlock. Asi que se busca la cadena "FileVersion" del recurso VS_VERSION_INFO
// en el archivo y se parsea el valor que la sigue: sin APIs y sin loader.
static bool version_supported(const wchar_t *path, unsigned *major_out, unsigned *minor_out) {
    if (major_out != nullptr) *major_out = 0;
    if (minor_out != nullptr) *minor_out = 0;
    // FILE_SHARE_DELETE y no FILE_SHARE_WRITE, que es el modo correcto para un
    // archivo que el cargador puede tener mapeado.
    //
    // CORRECCION: este cambio se hizo creyendo que explicaba los "version: 2.0"
    // del log, y NO era eso. La causa real era otra y mas tonta: al sacar el
    // filtro de version, la edicion borro la llamada a version_soportada y dejo
    // el log_num, que imprimia una variable en cero. Se afirmo una causa sin
    // medirla y se escribio aca como si estuviera establecida. El modo de
    // apertura se deja porque es el correcto, no porque haya arreglado eso.
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 64 || sz.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return false;
    }
    const DWORD n = (DWORD)sz.QuadPart;
    unsigned char *buf = (unsigned char *)VirtualAlloc(nullptr, n, MEM_COMMIT, PAGE_READWRITE);
    if (buf == nullptr) { CloseHandle(h); return false; }
    DWORD read_n = 0;
    const BOOL ok_read = ReadFile(h, buf, n, &read_n, nullptr);
    CloseHandle(h);
    bool ok = false;
    if (ok_read && read_n == n) {
        static const wchar_t kClave[] = L"FileVersion";
        const int klen = 11;
        for (DWORD i = 0; i + (DWORD)(klen * 2) + 128 < n; i += 2) {
            const wchar_t *w = (const wchar_t *)(buf + i);
            bool similar = true;
            for (int k = 0; k < klen; ++k) {
                if (w[k] != kClave[k]) { similar = false; break; }
            }
            if (!similar) continue;
            // El valor viene despues, con relleno de ceros en el medio.
            unsigned major = 0, minor_v = 0, field = 0, acc = 0;
            bool as_number = false, ready = false;
            for (int j = klen; j < 80 && !ready; ++j) {
                const wchar_t c = w[j];
                if (c >= L'0' && c <= L'9') {
                    acc = acc * 10 + (unsigned)(c - L'0');
                    as_number = true;
                } else if (as_number && (c == L',' || c == L'.' || c == L' ' || c == 0)) {
                    if (field == 0) major = acc; else if (field == 1) { minor_v = acc; ready = true; }
                    ++field;
                    acc = 0;
                    as_number = false;
                } else if (as_number) {
                    break;      // basura: no era este
                }
            }
            if (field >= 2 || ready) {
                if (major_out != nullptr) *major_out = major;
                if (minor_out != nullptr) *minor_out = minor_v;
                // 2.11, 2.12 y 2.13. La lista sale de evidencia en juego, no de
            // haber visto una firma suelta: GTA V corre 2.13 y anda, Cyberpunk
            // corre 2.11 y anda, y 2.12 es la que este archivo tiene anotada con
            // sus offsets ("0x45da en 2.13, 0x45e2 en 2.12").
            //
            // Queda afuera 2.14, que es la unica sin ningun juego que la respalde
            // y la que aparece en los dos dumps, en +0x3F2E9 leyendo un nulo a +0x40.
            //
            // Se probo primero con {12,13} y habria dejado a Cyberpunk sin parchear:
            // su plugin es 2.11. Verificado sobre los archivos antes de instalar.
            ok = (major == 2 && (minor_v == 11 || minor_v == 12 || minor_v == 13));
                break;
            }
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return ok;
}


// log_wide toma un UNICODE_STRING; esto es para una ruta suelta.
static void log_path(const char *tag, const wchar_t *s) {
    char b[400];
    int k = 0;
    for (; tag[k] != 0 && k < 40; ++k) b[k] = tag[k];
    for (int i = 0; s[i] != 0 && k < 398; ++i, ++k)
        b[k] = (s[i] < 128) ? (char)s[i] : '?';
    b[k] = 0;
    log_line(b);
}

static bool equal_nocase(const wchar_t *a, const wchar_t *b) {
    for (int i = 0;; ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = (wchar_t)(x + 32);
        if (y >= L'A' && y <= L'Z') y = (wchar_t)(y + 32);
        if (x != y) return false;
        if (x == 0) return true;
    }
}

// ---------------------------------------------------------------------------
// M2: de donde sale un set de Streamline que si sirva.
//
// La regla, y su fundamento: se llevan los PLUGINS a la version del INTERPOSER,
// nunca al reves. El interposer es el borde con la aplicacion -- el juego se
// compilo y enlazo contra el -- asi que es lo unico que no se puede mover sin
// riesgo. Los plugins los carga el interposer y ese borde es interno.
//
// Medido en el caso ROJO del banco (ota:132874):
//     sl.interposer 2.12   <- ya soportada
//     sl.common 2.7, sl.dlss_g 2.7 (0 sitios), sl.pcl 2.7, sl.reflex 2.7
// y la cache de NGX tiene los 18 modulos en 2.12. O sea que ese caso se resuelve
// entero desde el disco, sin red.
//
// El interposer NO esta en la cache en ninguna version -- verificado con find
// sobre todo ProgramData\NVIDIA. Por eso, si la version del interposer no tiene
// un dlss_g parcheable, este camino no alcanza y hay que decirlo en vez de
// inventar una mezcla: es exactamente el caso de Halo (interposer 2.7.30, y el
// unico dlss_g 2.7 de la cache tiene 0 sitios).
struct ModuleSet {
    const wchar_t *name;      // sl.common.dll
    wchar_t path[MAX_PATH];     // reemplazo elegido, vacio si no hay
};
static ModuleSet g_set[9] = {
    { L"sl.common.dll",  {0} }, { L"sl.dlss_g.dll", {0} },
    { L"sl.pcl.dll",     {0} }, { L"sl.reflex.dll", {0} },
    { L"sl.nis.dll",     {0} }, { L"sl.dlss.dll",   {0} },
    { L"sl.dlss_d.dll",  {0} }, { L"sl.deepdvc.dll",{0} },
    { L"sl.nvperf.dll",  {0} },
};
static bool g_set_built = false;
static unsigned g_set_version = 0;

// sl.common.dll -> sl_common_0, que es como se llama la carpeta en la cache.
static void cache_folder(const wchar_t *module, wchar_t *out) {
    int k = 0;
    for (int i = 0; module[i] != 0; ++i) {
        if (module[i] == L'.') {
            // el ".dll" final no se copia
            if (module[i+1] == L'd' && module[i+2] == L'l' && module[i+3] == L'l') break;
            out[k++] = L'_';
        } else {
            out[k++] = module[i];
        }
    }
    out[k++] = L'_'; out[k++] = L'0'; out[k] = 0;
}

// La ruta de un modulo en NUESTRA carpeta: LOCALAPPDATA\mfg-unlock\sdk\2.<v>.
//
// Es la misma carpeta de donde ya salia el interposer. Que TODO salga de ahi es
// lo que da una base unica: hoy los nueve sl.* salen de la cache de NGX, que la
// maneja NVIDIA y cambia sola por OTA, y el snippet salia de la carpeta del
// juego -- distinto en cada uno. Eso fue exactamente la causa del crash de Halo:
// su nvngx_dlssg de julio clava el maximo en 3 y el de GTA V no.
//
// Verificado antes de conectarlo: el sl.dlss_g del SDK 2.12 tiene los cuatro
// sitios que parcheamos, en el MISMO RVA (0x47333) que el de la cache.
static bool path_in_our_sdk(const wchar_t *module, unsigned minor, wchar_t *out) {
    // Sin buffer intermedio: esto corre dentro de hk_ldrload, BAJO EL LOADER
    // LOCK, donde la pila es poca. Un wchar_t[MAX_PATH] local aca son 520 bytes
    // y desbordaron la pila de GTA V -- 0xC00000FD en ntdll, el juego ni abrio.
    // Ya habia pasado antes con el marco de 3288 bytes de hk_ldrload.
    // Se escribe directo sobre el buffer del llamador.
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, MAX_PATH - 80);
    if (n == 0 || n >= MAX_PATH - 80) return false;
    int k = (int)n;
    const wchar_t *sub = L"\\mfg-unlock\\sdk\\2.";
    for (int i = 0; sub[i] != 0; ++i) out[k++] = sub[i];
    if (minor >= 10) out[k++] = (wchar_t)(L'0' + (minor / 10));
    out[k++] = (wchar_t)(L'0' + (minor % 10));
    out[k++] = L'\\';
    for (int i = 0; module[i] != 0 && k < MAX_PATH - 1; ++i) out[k++] = module[i];
    out[k] = 0;
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// ---- el set embebido -------------------------------------------------------
//
// El dll no descarga nada y la cache de %LOCALAPPDATA%\mfg-unlock\sdk\2.12
// se armaba a mano: sin ella el mod no andaba, contra [[ships-as-one-dll]].
// Los seis archivos que se cargan de verdad van adentro del dll como un zip
// (tools/embed_sdk.py -> src/sdk_blob.S por .incbin, 4,97 MB con deflate;
// crudos son 10,35 MB), y al arrancar, si falta alguno o el tamano no
// coincide con src/sdk_manifest.h, se escriben. Se escribe a <nombre>.tmp y
// se renombra: un archivo a medias no puede quedar con el nombre bueno.
extern "C" const unsigned char sdk_zip[];
extern "C" const unsigned char sdk_zip_end[];

static bool sdk_cache_dir(wchar_t *out) {
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, MAX_PATH - 80);
    if (n == 0 || n >= MAX_PATH - 80) return false;
    int k = (int)n;
    const wchar_t *parts[3] = { L"\\mfg-unlock", L"\\sdk", L"\\2.12" };
    for (int p = 0; p < 3; ++p) {
        for (int i = 0; parts[p][i] != 0; ++i) out[k++] = parts[p][i];
        out[k] = 0;
        CreateDirectoryW(out, nullptr);
    }
    return true;
}
static bool sdk_file_size_is(const wchar_t *dir, const wchar_t *name, unsigned size) {
    wchar_t path[MAX_PATH];
    int k = 0;
    for (; dir[k] != 0; ++k) path[k] = dir[k];
    path[k++] = L'\\';
    for (int i = 0; name[i] != 0; ++i) path[k++] = name[i];
    path[k] = 0;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return false;
    return a.nFileSizeHigh == 0 && a.nFileSizeLow == size;
}
static bool sdk_write_file(const wchar_t *dir, const wchar_t *name, const unsigned char *data, unsigned size) {
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    int k = 0;
    for (; dir[k] != 0; ++k) path[k] = dir[k];
    path[k++] = L'\\';
    for (int i = 0; name[i] != 0; ++i) path[k++] = name[i];
    path[k] = 0;
    for (int i = 0; i <= k; ++i) tmp[i] = path[i];
    tmp[k] = L'.'; tmp[k + 1] = L't'; tmp[k + 2] = L'm'; tmp[k + 3] = L'p'; tmp[k + 4] = 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, data, size, &wrote, nullptr);
    CloseHandle(h);
    if (!ok || wrote != size) { DeleteFileW(tmp); return false; }
    return MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}
// Deja la cache completa. Devuelve cuantos archivos escribio (0 = ya estaba).
static int ensure_sdk_cache(void) {
    wchar_t dir[MAX_PATH];
    if (!sdk_cache_dir(dir)) { log_line("sdk: sin LOCALAPPDATA; la cache no se puede escribir"); return 0; }
    bool missing[16] = { false };
    int need = 0;
    for (int i = 0; i < kSdkFilesN && i < 16; ++i)
        if (!sdk_file_size_is(dir, kSdkFiles[i].name, kSdkFiles[i].size)) { missing[i] = true; ++need; }
    if (need == 0) { log_line("sdk: cache completa (los seis del set embebido)"); return 0; }
    const size_t zn = (size_t)(sdk_zip_end - sdk_zip);
    unsigned cap = 0;
    for (int i = 0; i < kSdkFilesN; ++i) if (kSdkFiles[i].size > cap) cap = kSdkFiles[i].size;
    unsigned char *buf = static_cast<unsigned char *>(VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buf == nullptr) { log_line("sdk: sin memoria para extraer el set"); return 0; }
    int written = 0;
    zipmini::walk(sdk_zip, zn, [&](const zipmini::Entry &e) {
        for (int i = 0; i < kSdkFilesN && i < 16; ++i) {
            if (!missing[i]) continue;
            // el nombre de la entrada es ASCII; el del manifiesto, wide
            int j = 0;
            while (j < e.name_len && kSdkFiles[i].name[j] != 0 && (wchar_t)e.name[j] == kSdkFiles[i].name[j]) ++j;
            if (j != e.name_len || kSdkFiles[i].name[j] != 0) continue;
            const long got = zipmini::extract(sdk_zip, zn, e, buf, cap);
            if (got != (long)kSdkFiles[i].size) { log_num("sdk: no se pudo extraer el archivo numero ", (unsigned)i); break; }
            if (sdk_write_file(dir, kSdkFiles[i].name, buf, (unsigned)got)) { ++written; missing[i] = false; }
            else log_num("sdk: no se pudo escribir el archivo numero ", (unsigned)i);
            break;
        }
    });
    VirtualFree(buf, 0, MEM_RELEASE);
    log_num("sdk: cache escrita desde el dll, archivos ", (unsigned)written);
    if (written != need) log_num("  ! faltaban y no se pudieron escribir ", (unsigned)(need - written));
    return written;
}

// Busca en la cache un archivo del modulo pedido cuya version sea 2.<menor>.
static bool find_in_cache(const wchar_t *module, unsigned minor, wchar_t *out) {
    // Nuestra carpeta manda. La cache de NGX queda de respaldo: si el archivo
    // no esta, nada cambia respecto de antes.
    if (path_in_our_sdk(module, minor, out)) return true;
    wchar_t folder[64];
    cache_folder(module, folder);
    wchar_t pattern[MAX_PATH];
    int k = 0;
    const wchar_t *base = L"C:\\ProgramData\\NVIDIA\\NGX\\models\\";
    for (; base[k] != 0; ++k) pattern[k] = base[k];
    for (int i = 0; folder[i] != 0; ++i) pattern[k++] = folder[i];
    const wchar_t *tail = L"\\versions\\*";
    for (int i = 0; tail[i] != 0; ++i) pattern[k++] = tail[i];
    pattern[k] = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        wchar_t glob[MAX_PATH];
        int q = k - 1;                       // sin el '*'
        for (int i = 0; i < q; ++i) glob[i] = pattern[i];
        int p = q;
        for (int i = 0; fd.cFileName[i] != 0; ++i) glob[p++] = fd.cFileName[i];
        const wchar_t *sub = L"\\files\\*.dll";
        for (int i = 0; sub[i] != 0; ++i) glob[p++] = sub[i];
        glob[p] = 0;
        WIN32_FIND_DATAW fa;
        HANDLE ha = FindFirstFileW(glob, &fa);
        if (ha == INVALID_HANDLE_VALUE) continue;
        do {
            if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t path[MAX_PATH];
            int r = 0;
            for (int i = 0; i < p - 5; ++i) path[r++] = glob[i];   // sin "*.dll"
            for (int i = 0; fa.cFileName[i] != 0 && r < MAX_PATH - 1; ++i) path[r++] = fa.cFileName[i];
            path[r] = 0;
            unsigned major = 0, minor_v = 0;
            version_supported(path, &major, &minor_v);
            if (major == 2 && minor_v == minor) {
                for (int i = 0; i <= r; ++i) out[i] = path[i];
                found = true;
                break;
            }
        } while (FindNextFileW(ha, &fa));
        FindClose(ha);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Reemplazo del interposer, que es la unica pieza que la cache de NGX no tiene
// en ninguna version -- verificado con find sobre todo ProgramData\NVIDIA.
//
// Dos fuentes, en orden:
//   1. la carpeta del propio ejecutable del juego. Cubre a Halo, que trae un set
//      2.13 completo junto a su exe (los ocho sl.*, byte por byte los de GTA V)
//      y sin embargo carga el 2.7.30 de Engine\Plugins.
//   2. nuestra carpeta, con lo bajado del release oficial de NVIDIA-RTX.
//
// Nunca se escribe al lado del juego.
static wchar_t g_set_interposer[MAX_PATH] = {0};

// El snippet de NGX, desde NUESTRA carpeta.
//
// Hasta ahora unificabamos una sola mitad. Los nueve `sl.*` se sustituyen y el
// interposer sale de LOCALAPPDATA\mfg-unlock\sdk\2.<v>, pero `nvngx_dlssg`
// -- el snippet que hace el trabajo -- lo elegia NGX, y cada juego trae el
// suyo. Estabamos parados sobre una base distinta en cada juego.
//
// Lo que eso costo, medido:
//
//   snippet                 tamano     version      tope
//   Halo (carpeta del juego) 7597104   --           max = 3
//   Cyberpunk                7607336   --           max = 3
//   GTA V                    7453808   --           sin tope
//   banco                    7519856   310.7.129    sin tope
//
// La build de julio clava el maximo por arquitectura:
//
//   nvngx_dlssg 0x26577  mov   r8d, 3
//               0x2657d  cmp   edi, 0x1b0        ; Blackwell
//               0x26583  cmovl r8d, ebx          ; Ada -> 1
//               0x26587  lea   rdx, 'DLSSG.MultiFrameCountMax'
//
// Por eso Halo topaba en 3.00x y el banco entregaba 5.00x en la MISMA GPU. No
// era un limite del hardware: era que juego decidia con que binario corriamos.
//
// Subir esa constante a mano ya se probo: la cuenta sube a 5.13x pero NGX falla
// 4346 veces con 0xbad00005 y la imagen parpadea en negro. La build de julio no
// puede hacerlo aunque se le diga que si.
//
// Asi que se hace lo mismo que con el interposer: una copia nuestra, en una
// carpeta nuestra, y todos los juegos cargan esa. Si el archivo no esta, no se
// toca nada y el juego usa el suyo -- o sea que un usuario sin la carpeta no
// nota ninguna diferencia.
static wchar_t g_snippet_base[MAX_PATH] = {0};

static bool find_snippet(wchar_t *out) {
    // El del SDK primero, que es la misma base que los sl.*. La carpeta
    // snippet\ queda de respaldo para copias puestas a mano.
    if (path_in_our_sdk(L"nvngx_dlssg.dll", 12, out)) return true;
    // Sin buffer intermedio, por la misma razon: loader lock, pila corta.
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, MAX_PATH - 64);
    if (n == 0 || n >= MAX_PATH - 64) return false;
    int k = (int)n;
    const wchar_t *tail = L"\\mfg-unlock\\snippet\\nvngx_dlssg.dll";
    for (int i = 0; tail[i] != 0; ++i) out[k++] = tail[i];
    out[k] = 0;
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

static bool version_is(const wchar_t *path, unsigned v) {
    unsigned major = 0, minor_v = 0;
    version_supported(path, &major, &minor_v);
    return (major == 2 && minor_v == v);
}

static bool find_interposer(unsigned v, wchar_t *out) {
    // 1) junto al ejecutable
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        int cut = (int)n;
        while (cut > 0 && exe[cut-1] != L'\\' && exe[cut-1] != L'/') --cut;
        wchar_t candidate[MAX_PATH];
        int k = 0;
        for (; k < cut; ++k) candidate[k] = exe[k];
        const wchar_t *name_w = L"sl.interposer.dll";
        for (int i = 0; name_w[i] != 0; ++i) candidate[k++] = name_w[i];
        candidate[k] = 0;
        if (version_is(candidate, v)) {
            for (int i = 0; i <= k; ++i) out[i] = candidate[i];
            return true;
        }
    }
    // 2) lo que bajamos, en LOCALAPPDATA\mfg-unlock\sdk\2.<v>\sl.interposer.dll
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) return false;
    wchar_t candidate[MAX_PATH];
    int k = 0;
    for (; base[k] != 0 && k < MAX_PATH - 64; ++k) candidate[k] = base[k];
    const wchar_t *sub = L"\\mfg-unlock\\sdk\\2.";
    for (int i = 0; sub[i] != 0; ++i) candidate[k++] = sub[i];
    if (v >= 10) { candidate[k++] = (wchar_t)(L'0' + (v / 10)); }
    candidate[k++] = (wchar_t)(L'0' + (v % 10));
    const wchar_t *tail = L"\\sl.interposer.dll";
    for (int i = 0; tail[i] != 0; ++i) candidate[k++] = tail[i];
    candidate[k] = 0;
    if (!version_is(candidate, v)) return false;
    for (int i = 0; i <= k; ++i) out[i] = candidate[i];
    return true;
}

// Arma el set apuntando a la version del interposer que ya esta cargado.
static void build_target_set(void) {
    if (g_set_built) return;
    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    if (interposer == nullptr) return;            // todavia no cargo; se reintenta
    g_set_built = true;

    wchar_t interposer_path[MAX_PATH];
    if (GetModuleFileNameW(interposer, interposer_path, MAX_PATH) == 0) return;
    unsigned major = 0, minor_v = 0;
    version_supported(interposer_path, &major, &minor_v);
    g_set_version = minor_v;
    log_num("set: el interposer del juego es 2.", (unsigned long long)minor_v);
    if (major != 2 || minor_v == 0) { log_line("  no se pudo leer su version; no se arma nada"); return; }

    int found_n = 0;
    for (int i = 0; i < 9; ++i) {
        if (find_in_cache(g_set[i].name, minor_v, g_set[i].path)) ++found_n;
        else g_set[i].path[0] = 0;
    }
    log_num("  modulos de esa version encontrados en la cache ", (unsigned long long)found_n);

    // El unico que decide si el set sirve: sin sitio en el dlss_g no hay nada
    // que hacer QUEDANDOSE en la version del interposer.
    bool usable = false;
    if (g_set[1].path[0] != 0) {
        const int s = count_sites_in(g_set[1].path);
        log_num("  sitios de cuenta en el dlss_g candidato ", (unsigned long long)(unsigned)s);
        usable = (s > 0);
    } else {
        log_line("  no hay dlss_g de esa version en la cache");
    }
    if (usable) return;

    // Segundo intento: mover TAMBIEN el interposer.
    //
    // Es el caso de Halo, y es el unico que queda cuando la version del
    // interposer no tiene un dlss_g parcheable: su 2.7.30 no lo tiene, y el
    // unico 2.7 de la cache tampoco (0 sitios; el sitio existe desde 2.11).
    //
    // Se puede porque Halo carga el interposer DINAMICAMENTE -- verificado
    // leyendo su tabla de importaciones: no aparece ahi, mientras que Cyberpunk
    // y el sample del banco si lo importan estaticamente y por eso en esos dos
    // no hay nada que interceptar.
    //
    // El interposer NO esta en la cache de NGX en ninguna version, asi que sale
    // de nuestra propia carpeta (descargado del release oficial) o del propio
    // juego si trae uno de esa version.
    log_line("  se prueba mover tambien el interposer");
    for (int i = 0; i < 9; ++i) g_set[i].path[0] = 0;
    static const unsigned kCandidatas[3] = { 13, 12, 11 };
    for (int c = 0; c < 3; ++c) {
        const unsigned v = kCandidatas[c];
        wchar_t dlssg[MAX_PATH];
        if (!find_in_cache(L"sl.dlss_g.dll", v, dlssg)) continue;
        if (count_sites_in(dlssg) <= 0) continue;
        wchar_t interposer[MAX_PATH];
        if (!find_interposer(v, interposer)) {
            log_num("  hay dlss_g parcheable en 2.", (unsigned long long)v);
            log_line("    pero no hay interposer de esa version ni en el juego ni bajado");
            continue;
        }
        int found_cnt = 0;
        for (int i = 0; i < 9; ++i) {
            if (find_in_cache(g_set[i].name, v, g_set[i].path)) ++found_cnt;
            else g_set[i].path[0] = 0;
        }
        for (int i = 0; interposer[i] != 0; ++i) g_set_interposer[i] = interposer[i];
        g_set_interposer[MAX_PATH-1] = 0;
        g_set_version = v;
        log_num("  set completo armado en 2.", (unsigned long long)v);
        log_num("    modulos de la cache ", (unsigned long long)found_cnt);
        log_path("    interposer: ", interposer);
        return;
    }
    log_line("  no hay ninguna version con dlss_g parcheable E interposer disponible");
    g_set_version = 0;
}
// ---------------------------------------------------------------------------

// Se engancha LdrLoadDll y no LoadLibraryExW.
//
// El primer intento hooked kernel32!LoadLibraryExW y NO disparo nunca: el log
// mostro "plugin: vigilando..." y despues el plugin del juego mapeandose igual,
// sin pasar por el hook. En Windows moderno kernel32!LoadLibraryExW es un salto
// a kernelbase, y quien llame a LoadLibraryW, a la copia de kernelbase o
// directo al loader no toca ese stub. LdrLoadDll es el embudo por donde pasan
// todos, sin excepcion.
typedef NTSTATUS(NTAPI *PFN_LDRLOAD)(PWSTR, PULONG, PUNICODE_STRING, PVOID *);
static PFN_LDRLOAD g_orig_ldrload = nullptr;
static wchar_t g_requested_path[MAX_PATH];
// Solo se vigila la carpeta de la cache si el plugin del juego ya resulto
// inservible y tuvimos que sustituirlo. Un juego cuyo plugin sirve -- GTA V con
// 2.13, Cyberpunk con 2.11 -- no cambia en nada: sus cargas de la cache siguen
// pasando de largo como hasta ahora.

// Veredicto que dejo la corrida ANTERIOR de este mismo ejecutable: -1 sin dato,
// 0 verde, 1 amarillo, 2 rojo. La sustitucion solo se permite con 2.
//
// Esto es lo que hace segura a la redireccion. Anoche decidia mirando el archivo
// que se estaba por cargar, y en Cyberpunk eso se cumplia sin que yo lo viera:
// una de sus copias no tiene el sitio, disparaba la sustitucion, y terminaba
// cambiandole la copia OTA que si usa. Con el veredicto persistido, un juego que
// funciona es VERDE o AMARILLO y jamas entra en esta rama.



// Agrega la respuesta al mismo archivo de estado, sin tocar el diagnostico.
static void save_consent(int si) {
    wchar_t path[MAX_PATH];
    if (!state_path(path, MAX_PATH)) return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const char *txt = si ? "consentimiento=si\n" : "consentimiento=no\n";
    DWORD esc = 0;
    int n = 0; while (txt[n] != 0) ++n;
    WriteFile(h, txt, (DWORD)n, &esc, nullptr);
    CloseHandle(h);
}

static void read_previous_verdict(void) {
    if (g_verdict_read) return;
    g_verdict_read = true;
    wchar_t path[MAX_PATH];
    if (!state_path(path, MAX_PATH)) return;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char b[192];
    DWORD read_n = 0;
    const BOOL ok = ReadFile(h, b, sizeof(b) - 1, &read_n, nullptr);
    CloseHandle(h);
    if (!ok || read_n == 0) return;
    b[read_n] = 0;
    for (DWORD i = 0; i + 4 < read_n; ++i) {
        if (b[i] == 'R' && b[i+1] == 'O' && b[i+2] == 'J' && b[i+3] == 'O') { g_previous_verdict = 2; break; }
        if (b[i] == 'V' && b[i+1] == 'E' && b[i+2] == 'R' && b[i+3] == 'D') { g_previous_verdict = 0; break; }
        if (b[i] == 'A' && b[i+1] == 'M' && b[i+2] == 'A' && b[i+3] == 'R') { g_previous_verdict = 1; break; }
    }
    for (DWORD i = 0; i + 16 < read_n; ++i) {
        if (b[i]=='c' && b[i+1]=='o' && b[i+2]=='n' && b[i+3]=='s' && b[i+4]=='e') {
            for (DWORD j = i; j + 2 < read_n; ++j) {
                if (b[j] == '=') {
                    g_consent = (b[j+1] == 's') ? 1 : 0;
                    break;
                }
            }
            break;
        }
    }
}
static UNICODE_STRING g_us_alt;
static LONG g_interposer_requests = 0;

// Cargar NOSOTROS el reemplazo y devolver el handle, en vez de reescribirle la
// ruta al cargador.
//
// Es EL cambio de fondo. Hasta ahora se le cambiaba el UNICODE_STRING a
// LdrLoadDll y se lo dejaba resolver: el cargador registra entonces el modulo
// bajo OTRA ruta, asi que el pedido siguiente del nombre original no matchea
// nada y mapea una SEGUNDA copia. De ahi salieron los dos defectos que rompieron
// juegos enteros hoy:
//
//   - Cyberpunk: dos sl.interposer.dll mapeados, sin independent flip, cero
//     ventanas de medicion
//   - Halo: el sl.dlss_g 2.7 del propio juego entrando detras del nuestro, con
//     'sites: 0', apagando el freno para toda la corrida
//
// Devolviendo un HMODULE ya cargado, el refcount y la identidad los lleva
// Windows: pedir dos veces el mismo archivo devuelve el mismo modulo y no hay
// segunda copia posible.
//
// La reentrada es obligatoria manejarla, no opcional: LoadLibraryW desde adentro
// de nuestro propio hook de LdrLoadDll vuelve a entrar por aca. El loader lock
// es del proceso y serializa esta funcion -- por eso g_ruta_pedida ya era global
// -- asi que una bandera simple alcanza y es mas barata que preguntar quien
// llamo. Sin ella, recursion infinita.
static LONG g_ldr_reentry = 0;

static NTSTATUS load_own(const wchar_t *own, PVOID *base) {
    if (base == nullptr) return (NTSTATUS)0xC0000001L;
    g_ldr_reentry = 1;
    HMODULE h = LoadLibraryW(own);
    g_ldr_reentry = 0;
    if (h == nullptr) return (NTSTATUS)0xC0000001L;
    *base = (PVOID)h;
    return (NTSTATUS)0L;
}

// El trabajo pesado NO se inline en el hook.
//
// hk_ldrload corre bajo el loader lock en CADA carga de modulo, en cualquier
// hilo del juego -- incluidos los que tienen poca pila. Con -O2 el compilador
// inlineaba la sustitucion entera y el marco quedaba en 3288 bytes, que se
// pagan tambien en el camino comun: un modulo que no nos interesa y se devuelve
// enseguida.
//
// Halo crasheo ahi: 0xc0000005 escribiendo a 0x30(%rsp) dentro de esta funcion,
// que es firma de pila agotada y no de puntero invalido. El marco era identico
// en el binario anterior, asi que el peligro no es nuevo -- pero 3288 bytes
// bajo el loader lock no tienen defensa.
static NTSTATUS NTAPI hk_ldrload(PWSTR path, PULONG chars, PUNICODE_STRING name,
                                 PVOID *base) {
    // Nuestra propia carga pasa de largo. Ver cargar_propio.
    if (g_ldr_reentry != 0) return g_orig_ldrload(path, chars, name, base);
    if (name == nullptr || name->Buffer == nullptr || name->Length == 0)
        return g_orig_ldrload(path, chars, name, base);
    const int n = (int)(name->Length / sizeof(wchar_t));
    if (n <= 0 || n >= MAX_PATH) return g_orig_ldrload(path, chars, name, base);
    // UNICODE_STRING no viene terminado en cero: se copia para poder mirarlo.
    for (int i = 0; i < n; ++i) g_requested_path[i] = name->Buffer[i];
    g_requested_path[n] = 0;
    int cut = n;
    while (cut > 0 && g_requested_path[cut-1] != L'\\' && g_requested_path[cut-1] != L'/')
        --cut;
    // Aca hubo un bloque de diagnostico que escribia al log DESDE ADENTRO de
    // LdrLoadDll, en cada carga de modulo del proceso y con el loader lock
    // tomado. Sirvio para lo que se puso -- probo que el gancho SI ve la copia
    // OTA, contra mi hipotesis de que NGX la mapeaba por fuera -- y se retira.
    // Escribir a un archivo bajo el loader lock, por cada DLL que carga el
    // juego, no es algo que deba quedar en un binario que se distribuye.

    // Se reconoce el modulo por su nombre de archivo, y ademas por la carpeta
    // de la cache de NGX: alli los archivos NO se llaman sl.<algo>.dll sino
    // 190_E658703.dll -- un nombre de contenido. Comparando solo el nombre, la
    // carga de la copia OTA pasaba de largo y entraba igual; el log lo mostro:
    // se sustituia una vez, Streamline se quedaba con la que no tocamos, y el
    // resultado era "sitios que quedan 0".
    int idx = -1;
    for (int i = 0; i < 9; ++i) {
        if (equal_nocase(g_requested_path + cut, g_set[i].name)) { idx = i; break; }
    }
    // -2 marca al interposer: se sustituye igual que los demas, pero su ruta
    // vive aparte porque no sale de la cache.
    const bool is_interposer = equal_nocase(g_requested_path + cut, L"sl.interposer.dll");
    if (is_interposer) idx = -2;
    // Diagnostico: CADA pedido del interposer, y si ya hay uno mapeado.
    //
    // El banco fallo 5 de 5 con el interposer sustituido y en loaded-modules
    // aparecian DOS: el del sample y el nuestro. Pero en el log habia una sola
    // linea de redireccion, asi que la segunda copia entro por un camino que
    // este gancho no vio. Esto dice cuantos pedidos hay y en que orden.
    if (is_interposer) {
        ++g_interposer_requests;
        log_num("interposer: pedido numero ", (unsigned)g_interposer_requests);
        log_path("  ruta pedida: ", g_requested_path);
    }
    // -3 marca al snippet de NGX. Sale de nuestra carpeta, igual que el
    // interposer. Ver buscar_snippet.
    if (g_snippet_on && equal_nocase(g_requested_path + cut, L"nvngx_dlssg.dll")) {
        if (g_snippet_base[0] == 0) find_snippet(g_snippet_base);
        if (g_snippet_base[0] != 0) idx = -3;
    }
    // Se normaliza antes de comparar: minusculas y todas las barras iguales.
    //
    // La version anterior no podia matchear NUNCA, por dos motivos a la vez, y
    // eso explica que la copia OTA se colara siempre:
    //   1. el patron tiene 15 caracteres y el bucle comparaba 16, asi que
    //      siempre metia el terminador en la comparacion;
    //   2. la ruta real llega con barras NORMALES en esa parte
    //      -- C:\ProgramData/NVIDIA/NGX/models/sl_dlss_g_0/... --
    //      contra un patron escrito con barras invertidas.
    //
    // Se vio con una linea de diagnostico: el gancho SI recibe esa ruta. La
    // hipotesis de que NGX la mapeaba fuera del cargador era falsa.
    bool in_cache = false;
    if (idx < 0) {
        // static, no local: 520 bytes menos de marco en un hook que corre bajo
        // el loader lock. Es seguro porque el loader lock serializa esta
        // funcion -- por la misma razon g_ruta_pedida ya era global.
        static wchar_t norm[MAX_PATH];
        int nn2 = 0;
        for (; nn2 < n && nn2 < MAX_PATH - 1; ++nn2) {
            wchar_t c = g_requested_path[nn2];
            if (c == L'/') c = L'\\';
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
            norm[nn2] = c;
        }
        norm[nn2] = 0;
        static const wchar_t kCache[] = L"\\ngx\\models\\sl_";
        int size = 0;
        while (kCache[size] != 0) ++size;
        for (int i = 0; i + size <= nn2 && !in_cache; ++i) {
            bool m = true;
            for (int k = 0; k < size; ++k) {
                if (norm[i + k] != kCache[k]) { m = false; break; }
            }
            in_cache = m;
        }
        // CUAL modulo de la cache, no "alguno".
        //
        // En la cache de NGX todos los modulos se llaman IGUAL --
        // 190_E658703.dll -- y lo que los distingue es la CARPETA:
        // sl_reflex_0, sl_pcl_0, sl_dlss_g_0. La version anterior detectaba
        // el prefijo de la cache y asumia dlss_g para cualquiera.
        //
        // Halo lo destapo: pedia el 190_E658703.dll de sl_reflex_0 y le
        // entregabamos el de sl_dlss_g_0. Streamline lo rechazaba y lo
        // descargaba en el acto -- tres "modulo descargado / sitios que
        // quedan 0" seguidos en 110 ms -- y como frame generation NECESITA
        // Reflex, el juego corrio 142 s con multiplicador 1.00 en las 123
        // ventanas.
        //
        // No se veia en GTA V, que mapea una sola copia y no toca la cache,
        // ni en Cyberpunk, cuyo Reflex propio ya estaba cargado antes.
        if (in_cache) {
            idx = -1;
            for (int i = 0; i < 9 && idx < 0; ++i) {
                wchar_t folder_w[64];
                cache_folder(g_set[i].name, folder_w);
                int lc = 0; while (folder_w[lc] != 0) ++lc;
                for (int j = 1; j + lc + 1 <= nn2; ++j) {
                    if (norm[j-1] != L'\\') continue;
                    bool m = true;
                    for (int k = 0; k < lc; ++k) {
                        wchar_t c = folder_w[k];
                        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
                        if (norm[j + k] != c) { m = false; break; }
                    }
                    if (m && norm[j + lc] == L'\\') { idx = i; break; }
                }
            }
        }
    }
    if (idx == -1) return g_orig_ldrload(path, chars, name, base);

    // El snippet va por su propio camino: no depende del veredicto del set, que
    // es sobre los sl.* del juego. Y no hace falta consentimiento porque no se
    // reemplaza nada del juego -- se carga un archivo nuestro en vez del suyo,
    // igual que el interposer.
    if (idx == -3) {
        log_path("snippet: pedido  ", g_requested_path);
        log_path("  se carga el nuestro: ", g_snippet_base);
        // La semantica NO se decide aca: la decide detectar_semantica mirando el
        // binario que quedo mapeado. Cargarlo desde nuestra carpeta y que sea de
        // una build u otra son cosas distintas.
        const NTSTATUS st3 = load_own(g_snippet_base, base);
        if (st3 < 0) {
            log_num("  no cargo, status ", (unsigned)st3);
            log_line("  se vuelve al del juego");
            return g_orig_ldrload(path, chars, name, base);
        }
        return st3;
    }

    // Base propia: si el modulo esta en NUESTRA carpeta, se usa ese y punto.
    //
    // El veredicto ROJO existe como salvaguarda para no cambiarle los binarios a
    // un juego que anda. Pero con base unificada el objetivo es el contrario:
    // que TODOS corran lo mismo, para que un arreglo valga en los tres y no haya
    // que descubrir por juego que binario le toco. La causa del crash de Halo
    // fue exactamente eso.
    //
    // Solo actua si el archivo existe: sin la carpeta poblada, nada cambia.
    // El interposer NO entra en la base por defecto.
    //
    // Es el modulo contra el que el juego enlaza y el que arma el swapchain.
    // Sustituirlo siempre rompio el banco: 3 de 3 corridas sin independent flip
    // ("SetMaximumFrameLatency changed from 0 to 1" y nada mas), cuando esta
    // misma manana enganchaba al primer intento. Los plugins y el snippet si
    // van; el interposer sigue el camino viejo, con veredicto y consentimiento.
    //
    // El interposer entra como todos, pero NUNCA se redirige un modulo que ya
    // esta mapeado.
    //
    // Ahi estaba el defecto, y no en el modulo. El banco fallaba 5 de 5 con el
    // interposer sustituido y en loaded-modules.json aparecian DOS -- el del
    // sample y el nuestro -- con una sola linea de redireccion en el log. La
    // pista que faltaba la dio este mismo gancho al preguntarlo:
    //
    //   interposer: pedido numero 1
    //     ya mapeado (GetModuleHandle) 1
    //
    // O sea que cuando vemos el pedido el modulo YA esta en el proceso: el
    // sample lo importa estaticamente y el cargador lo resolvio antes. Ese
    // pedido es una segunda carga del mismo nombre, que normalmente devuelve la
    // copia que ya esta. Cambiandole la ruta deja de ser el mismo modulo para el
    // cargador y se mapea una segunda copia.
    //
    // Con dos interposers el juego presenta por uno y los hooks de los plugins
    // viven en el otro: SetMaximumFrameLatency se queda en 1, no hay independent
    // flip y no hay una sola ventana de medicion -- aunque la interpolacion
    // "cambie de estado", porque los plugins estan vivos en la copia equivocada.
    // Es [[two-plugin-copies]] otra vez, un nivel mas arriba.
    //
    // Los dos archivos son byte a byte el mismo (mismo sha256, 647808 bytes),
    // asi que sustituirlo nunca fue el problema: la ruta lo era.
    //
    // La guarda vale para TODOS los modulos, no solo el interposer. Si ya esta
    // mapeado, la carga que sigue tiene que devolver esa copia y no una nueva.
    // En GTA V el interposer se mapea a los 16.7 s y no esta cargado antes, asi
    // que ahi si se sustituye y el juego queda con el set 2.12 completo.
    //
    // La guarda vale SOLO para el interposer. Aplicarla a todos fue el defecto.
    //
    // Existe porque el interposer llega por import estatico: ya esta mapeado
    // cuando vemos el pedido, y redirigirlo a otra ruta mapea una segunda copia.
    // Pero un plugin es otra cosa: que ya haya uno mapeado NO quiere decir que el
    // que estan pidiendo sea el mismo archivo.
    //
    // En Halo eso dejaba entrar el sl.dlss_g 2.7 PROPIO DEL JUEGO despues de
    // nuestro 2.12, con este log:
    //
    //   sl.dlss_g mapped  in ...\sdk\2.12\sl.dlss_g.dll   2.12   sites: 1
    //   base: ya mapeado, no se duplica: sl.dlss_g.dll
    //   sl.dlss_g mapped  in ...Halo...\ThirdParty\...      2.7    sites: 0
    //
    // Y esa copia sin sitios apagaba g_wic_ok para toda la corrida. De ahi salia
    // el freno, la cadencia sin byte y el congelamiento en CUSTOM y DYNAMIC.
    const bool already_there = idx == -2 &&
                         GetModuleHandleW(g_requested_path + cut) != nullptr;
    if (already_there) {
        static int said_already = 0;
        if (said_already < 12) {
            ++said_already;
            log_path("base: ya mapeado, no se duplica: ", g_requested_path + cut);
        }
        return g_orig_ldrload(path, chars, name, base);
    }
    if (g_snippet_on && (idx != -2 || !g_interposer_out)) {
        static wchar_t own[MAX_PATH];
        const wchar_t *name_w = (idx == -2) ? L"sl.interposer.dll" : g_set[idx].name;
        if (path_in_our_sdk(name_w, 12, own)) {
            log_path("base: pedido  ", g_requested_path);
            log_path("  se carga el nuestro: ", own);
            g_already_substituted = true;
            const NTSTATUS stb = load_own(own, base);
            if (stb >= 0) return stb;
            log_num("  no cargo, status ", (unsigned)stb);
            log_line("  se vuelve al del juego");
            return g_orig_ldrload(path, chars, name, base);
        }
    }

    read_previous_verdict();
    if (g_previous_verdict == 2 && g_consent != 1) {
        // ROJO pero sin un si explicito: no se toca nada. Cambiar que binarios
        // corre el juego de alguien no es una decision que tome el dll solo.
        static bool warned = false;
        if (!warned) {
            warned = true;
            log_line("set: este juego no puede usar MFG con su Streamline.");
            log_line("  hay un reemplazo disponible, pero falta autorizacion.");
            log_line("  el panel lo pregunta; hasta entonces no se sustituye nada.");
        }
        return g_orig_ldrload(path, chars, name, base);
    }
    if (g_previous_verdict != 2) {
        // Sin un ROJO de la corrida anterior no se sustituye NADA. Un juego que
        // funciona nunca llega a esta rama, que es la salvaguarda que faltaba.
        static bool said = false;
        if (!said) {
            said = true;
            log_num("set: sin veredicto ROJO previo, no se sustituye. veredicto=",
                    (unsigned long long)(unsigned)(g_previous_verdict + 1));
        }
        return g_orig_ldrload(path, chars, name, base);
    }

    build_target_set();
    if (g_set_version == 0) return g_orig_ldrload(path, chars, name, base);
    const wchar_t *replacement = (idx == -2) ? g_set_interposer : g_set[idx].path;
    if (replacement[0] == 0) return g_orig_ldrload(path, chars, name, base);

    // Si el que piden YA es de la version del interposer, no se toca: es el caso
    // normal y sustituirlo seria trabajo y riesgo por nada.
    unsigned major = 0, minor_v = 0;
    version_supported(g_requested_path, &major, &minor_v);
    if (major == 2 && minor_v == g_set_version && !in_cache)
        return g_orig_ldrload(path, chars, name, base);

    const wchar_t *alt = replacement;
    if (idx == -2) log_num("set: se sustituye el INTERPOSER, pedido 2.", (unsigned long long)minor_v);
    else           log_num("set: se sustituye un modulo, pedido 2.", (unsigned long long)minor_v);
    log_path("  pedido:  ", g_requested_path);
    log_path("  cargado: ", alt);
    g_already_substituted = true;
    const NTSTATUS st = load_own(alt, base);
    if (st < 0) {
        log_num("  no cargo, status ", (unsigned)st);
        log_line("  se vuelve al del juego");
        return g_orig_ldrload(path, chars, name, base);
    }
    return st;
}

static void arm_plugin_redirect(void) {
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt == nullptr) return;
    FARPROC p = GetProcAddress(nt, "LdrLoadDll");
    if (p == nullptr) { log_line("plugin: no hay LdrLoadDll"); return; }
    if (MH_CreateHook(reinterpret_cast<void *>(p), reinterpret_cast<void *>(&hk_ldrload),
                      reinterpret_cast<void **>(&g_orig_ldrload)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void *>(p)) == MH_OK)
        log_line("plugin: vigilando en LdrLoadDll que sl.dlss_g tenga el sitio de la cuenta");
    else
        g_orig_ldrload = nullptr;
}

// Highest-numbered directory under the OTA cache, which is the build NGX picks.
// Names are decimal ids, so "20318464" beats "20318081"; compared by length
// first so a shorter number never wins on lexical order alone.
static void find_newest_ota_build() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(
        L"C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssg\\versions\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        int n = 0;
        while (fd.cFileName[n] != 0 && n < 63) {
            if (fd.cFileName[n] < L'0' || fd.cFileName[n] > L'9') { n = -1; break; }
            ++n;
        }
        if (n <= 0) continue;
        int cur = 0;
        while (g_ota_newest[cur] != 0) ++cur;
        bool better = cur == 0 || n > cur;
        if (!better && n == cur) {
            for (int i = 0; i < n; ++i) {
                if (fd.cFileName[i] != g_ota_newest[i]) {
                    better = fd.cFileName[i] > g_ota_newest[i];
                    break;
                }
            }
        }
        if (better) {
            for (int i = 0; i < n; ++i) g_ota_newest[i] = fd.cFileName[i];
            g_ota_newest[n] = 0;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

