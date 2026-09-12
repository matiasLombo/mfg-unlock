// Configuracion: que archivos existen al lado de la dll y que dicen.
//
// EXTRACCION de DllMain y settings_load (proxy.cpp), sin cambiar nada de lo
// que hacen: los mismos archivos, la misma polaridad, los mismos rangos, el
// mismo parser de mfg-settings.txt caracter por caracter. Lo que cambia es
// que ahora esta en UN lugar, en una tabla, y se puede leer en el host sin
// Windows: la existencia de los archivos entra por un callback, el texto por
// un buffer.
//
// La polaridad se conserva tal cual, incluso la mezclada: hay archivos que
// APAGAN algo que va encendido por defecto (mfg-sinseis, mfg-nofrac,
// mfg-nowic...) y archivos que ENCIENDEN algo apagado (mfg-ota, mfg-x6...).
// Eso es una regla de este proyecto ([[flags-only-disable]]): lo que hace
// funcionar el mod va encendido por defecto y un flag solo lo apaga; los que
// encienden son experimentos. La tabla lo hace visible: `defecto` es el
// valor sin archivo, y el archivo lo invierte.
//
// Normalizar los nombres (una sola polaridad, un solo archivo de texto) es
// otra fase, y se hace contra tools/test_config.cpp.
#pragma once

namespace config {

struct Settings {
    // --- banderas: un archivo cada una, presente = invertir el defecto ---
    bool debug         = false;   // mfg-debug.txt
    bool watch         = false;   // mfg-watch.txt
    bool novsync       = false;   // mfg-novsync.txt
    bool pinlatency    = false;   // mfg-pinlatency.txt
    bool pacefollow    = false;   // mfg-pacefollow.txt
    bool frac          = true;    // mfg-nofrac.txt lo apaga
    bool sub2          = false;   // mfg-sub2.txt
    bool twocopies     = false;   // mfg-twocopies.txt
    bool ceilfirst     = false;   // mfg-ceilfirst.txt
    bool ota           = false;   // mfg-ota.txt
    bool slowalt       = true;    // mfg-noslowalt.txt lo apaga
    bool quiet         = false;   // mfg-quiet.txt
    bool nullalt       = false;   // mfg-nullalt.txt
    bool sat           = true;    // mfg-sinsat.txt lo apaga
    bool pathsplugins  = false;   // mfg-pathsplugins.txt
    bool peralt        = false;   // mfg-peralt.txt
    bool seis          = true;    // mfg-sinseis.txt lo apaga
    bool coninterposer = false;   // mfg-coninterposer.txt (g_inter_fuera = !coninterposer)
    bool sinbase       = false;   // mfg-sinbase.txt (g_snippet_on = !sinbase)
    bool mfcmax        = false;   // mfg-mfcmax.txt
    bool topefijo      = false;   // mfg-topefijo.txt
    bool x6            = false;   // mfg-x6.txt
    bool dyndiag       = false;   // mfg-dyndiag.txt
    bool latch         = true;    // mfg-nolatch.txt lo apaga
    bool deuda         = true;    // mfg-sin-deuda.txt lo apaga
    bool optsv3        = false;   // mfg-optsv3.txt
    bool blockalt      = false;   // mfg-blockalt.txt
    bool nowaitable    = false;   // mfg-nowaitable.txt
    bool panel         = true;    // mfg-nopanel.txt lo apaga
    bool wic           = true;    // mfg-nowic.txt lo apaga
    bool monoidx       = false;   // mfg-monoidx.txt
    bool cubins        = true;    // mfg-nocubins.txt lo apaga
    bool meter_off     = false;   // mfg-nometer.txt
    bool presetb       = false;   // mfg-presetb.txt
    bool sllog         = false;   // mfg-sllog.txt
    bool indicator     = false;   // mfg-indicator.txt
    bool host          = false;   // mfg-host.txt: Streamline en un juego sin Streamline (experimento)
    bool hosthudless   = false;   // modo host: taggear la salida de DLSS como color sin HUD (experimento; sin tag el plugin usa el backbuffer, que es lo que se ve bien)

    // --- numericos: un archivo con numeros adentro; 0 o -1 = ausente/invalido ---
    int blockms        = 0;       // mfg-blockms.txt: 1..99999
    int marker[4]      = {0, 0, 0, 0}; // mfg-markergap.txt
    int marker_n       = 0;       //   cuantos numeros traia
    int slowframe[3]   = {0, 0, 0};    // mfg-slowframe.txt
    int slowframe_n    = 0;
    int jitter         = 0;       // mfg-jitter.txt: 1..90
    int clamplatency   = 0;       // mfg-clamplatency.txt: un digito 1..9
    int queue          = -1;      // mfg-queue.txt: un digito 0..3
    int blocks         = 0;       // mfg-blocks.txt: 2..64
    int dynstep        = 0;       // dynstep: grilla del ratio de DYNAMIC en centesimas (0 = sin grilla, defecto)

    // --- mfg-settings.txt: -1 = la clave no estaba o no valia ---
    int mode           = -1;
    int target         = -1;
    int dynfps         = -1;
    int hud            = -1;
};

struct Flag {
    const wchar_t *file;   // el archivo viejo, presente = invertir el defecto
    const char *key;        // la clave en mfg-config.txt: `clave 0|1`, valor directo
    bool Settings::*field;
};

// Archivo -> campo. El valor con el archivo presente es !defecto, asi que la
// polaridad esta en el defecto del campo y no aca.
inline const Flag kFlags[] = {
    { L"mfg-debug.txt",          "debug", &Settings::debug },
    { L"mfg-watch.txt",          "watch", &Settings::watch },
    { L"mfg-novsync.txt",        "novsync", &Settings::novsync },
    { L"mfg-pinlatency.txt",     "pinlatency", &Settings::pinlatency },
    { L"mfg-pacefollow.txt",     "pacefollow", &Settings::pacefollow },
    { L"mfg-nofrac.txt",         "frac", &Settings::frac },
    { L"mfg-sub2.txt",           "sub2", &Settings::sub2 },
    { L"mfg-twocopies.txt",      "twocopies", &Settings::twocopies },
    { L"mfg-ceilfirst.txt",      "ceilfirst", &Settings::ceilfirst },
    { L"mfg-ota.txt",            "ota", &Settings::ota },
    { L"mfg-noslowalt.txt",      "slowalt", &Settings::slowalt },
    { L"mfg-quiet.txt",          "quiet", &Settings::quiet },
    { L"mfg-nullalt.txt",        "nullalt", &Settings::nullalt },
    { L"mfg-sinsat.txt",         "sat", &Settings::sat },
    { L"mfg-pathsplugins.txt",   "pathsplugins", &Settings::pathsplugins },
    { L"mfg-peralt.txt",         "peralt", &Settings::peralt },
    { L"mfg-sinseis.txt",        "seis", &Settings::seis },
    { L"mfg-coninterposer.txt",  "coninterposer", &Settings::coninterposer },
    { L"mfg-sinbase.txt",        "sinbase", &Settings::sinbase },
    { L"mfg-mfcmax.txt",         "mfcmax", &Settings::mfcmax },
    { L"mfg-topefijo.txt",       "topefijo", &Settings::topefijo },
    { L"mfg-x6.txt",             "x6", &Settings::x6 },
    { L"mfg-dyndiag.txt",        "dyndiag", &Settings::dyndiag },
    { L"mfg-nolatch.txt",        "latch", &Settings::latch },
    { L"mfg-sin-deuda.txt",      "deuda", &Settings::deuda },
    { L"mfg-optsv3.txt",         "optsv3", &Settings::optsv3 },
    { L"mfg-blockalt.txt",       "blockalt", &Settings::blockalt },
    { L"mfg-nowaitable.txt",     "nowaitable", &Settings::nowaitable },
    { L"mfg-nopanel.txt",        "panel", &Settings::panel },
    { L"mfg-nowic.txt",          "wic", &Settings::wic },
    { L"mfg-monoidx.txt",        "monoidx", &Settings::monoidx },
    { L"mfg-nocubins.txt",       "cubins", &Settings::cubins },
    { L"mfg-nometer.txt",        "meter_off", &Settings::meter_off },
    { L"mfg-presetb.txt",        "presetb", &Settings::presetb },
    { L"mfg-sllog.txt",          "sllog", &Settings::sllog },
    { L"mfg-indicator.txt",      "indicator", &Settings::indicator },
    { L"mfg-host.txt",           "host", &Settings::host },
    { L"mfg-hosthudless.txt",    "hosthudless", &Settings::hosthudless },
};
inline const int kFlagsN = (int)(sizeof(kFlags) / sizeof(kFlags[0]));

// `existe(archivo)` dice si el archivo esta al lado de la dll. En el dll es
// GetFileAttributesW; en el test, lo que el caso diga.
template <class Exists>
inline void read_flags(Settings &a, Exists exists) {
    for (int i = 0; i < kFlagsN; ++i)
        if (exists(kFlags[i].file)) a.*(kFlags[i].field) = !(a.*(kFlags[i].field));
}

// Los tres parsers de numeros que DllMain tenia repetidos, tal cual:

// Digitos desde el primer byte, se corta en el primer no-digito.
// (mfg-blockms, mfg-jitter, mfg-blocks)
inline int leading_int(const char *b, unsigned n) {
    int v = 0;
    for (unsigned i = 0; i < n && b[i] >= '0' && b[i] <= '9'; ++i)
        v = v * 10 + (b[i] - '0');
    return v;
}

// Hasta `max` enteros separados por cualquier cosa que no sea digito.
// (mfg-markergap, mfg-slowframe). Devuelve cuantos encontro.
inline int loose_ints(const char *b, unsigned n, int *v, int max) {
    int k = 0; unsigned i = 0;
    while (i < n && k < max) {
        while (i < n && (b[i] < '0' || b[i] > '9')) ++i;
        if (i >= n) break;
        int x = 0;
        while (i < n && b[i] >= '0' && b[i] <= '9') x = x * 10 + (b[i++] - '0');
        v[k++] = x;
    }
    return k;
}

// Un solo digito en el primer byte, dentro de [lo, hi]; -1 si no.
// (mfg-clamplatency 1..9, mfg-queue 0..3)
inline int first_digit(const char *b, unsigned n, char lo, char hi) {
    if (n == 0 || b[0] < lo || b[0] > hi) return -1;
    return b[0] - '0';
}

// Cada numerico con el rango que DllMain le aplicaba. `leer(archivo, buf,
// cap)` devuelve cuantos bytes trajo, 0 si no existe.
template <class Reader>
inline void read_numerics(Settings &a, Reader read) {
    char b[48]; unsigned n;
    if ((n = read(L"mfg-blockms.txt", b, 15)) > 0) {
        const int v = leading_int(b, n);
        if (v > 0 && v < 100000) a.blockms = v;
    }
    if ((n = read(L"mfg-markergap.txt", b, 31)) > 0)
        a.marker_n = loose_ints(b, n, a.marker, 4);
    if ((n = read(L"mfg-slowframe.txt", b, 47)) > 0)
        a.slowframe_n = loose_ints(b, n, a.slowframe, 3);
    if ((n = read(L"mfg-jitter.txt", b, 15)) > 0) {
        const int v = leading_int(b, n);
        if (v > 0 && v <= 90) a.jitter = v;
    }
    if ((n = read(L"mfg-clamplatency.txt", b, 15)) > 0) {
        const int v = first_digit(b, n, '1', '9');
        if (v >= 0) a.clamplatency = v;
    }
    if ((n = read(L"mfg-queue.txt", b, 15)) > 0) {
        const int v = first_digit(b, n, '0', '3');
        if (v >= 0) a.queue = v;
    }
    if ((n = read(L"mfg-blocks.txt", b, 31)) > 0) {
        const int v = leading_int(b, n);
        if (v >= 2 && v <= 64) a.blocks = v;
    }
}

// mfg-settings.txt, caracter por caracter como settings_load lo hacia.
//
// Cuatro claves seguidas de un numero. Lo que no se reconoce se ignora en vez
// de valer cero: una escritura truncada no puede convertirse en "AUTO,
// objetivo nada". `filas` es kPanRows y `max_target` es kMaxCustom, que viven
// en overlay.h; el piso del target NO se aplica aca (depende de otro flag) y
// tampoco la semilla de la cuenta: eso es politica y queda en quien llama.
inline void parse_settings(const char *buf, unsigned n, int rows, int max_target,
                             Settings &a) {
    for (unsigned i = 0; i < n; ++i) {
        const bool is_mode = (i + 5 < n) && buf[i] == 'm' && buf[i+1] == 'o' &&
                             buf[i+2] == 'd' && buf[i+3] == 'e';
        const bool is_dfps = (i + 7 < n) && buf[i] == 'd' && buf[i+1] == 'y' &&
                             buf[i+2] == 'n' && buf[i+3] == 'f' &&
                             buf[i+4] == 'p' && buf[i+5] == 's' && buf[i+6] == ' ';
        const bool is_hud  = (i + 4 < n) && buf[i] == 'h' &&
                             buf[i+1] == 'u' && buf[i+2] == 'd' &&
                             buf[i+3] == ' ';
        const bool is_tgt  = (i + 7 < n) && buf[i] == 't' && buf[i+1] == 'a' &&
                             buf[i+2] == 'r' && buf[i+3] == 'g' && buf[i+4] == 'e' &&
                             buf[i+5] == 't';
        if (is_dfps) {
            int v = 0; unsigned j = i + 7;
            while (j < n && buf[j] >= '0' && buf[j] <= '9')
                v = v * 10 + (buf[j++] - '0');
            if (v >= 0 && v <= 1000) a.dynfps = v;
            i = j;
            continue;
        }
        if (is_hud) {
            a.hud = buf[i+4] == '1' ? 1 : 0;
            i += 4;
            continue;
        }
        if (!is_mode && !is_tgt) continue;
        unsigned j = i + (is_mode ? 4 : 6);
        while (j < n && (buf[j] == ' ' || buf[j] == '=')) ++j;
        if (j >= n || buf[j] < '0' || buf[j] > '9') { i = j; continue; }
        int v = 0;
        while (j < n && buf[j] >= '0' && buf[j] <= '9' && v < 100000)
            v = v * 10 + (buf[j++] - '0');
        if (is_mode) {
            if (v >= 0 && v < rows) a.mode = v;
        } else if (is_tgt && v >= 0 && v <= max_target) {
            a.target = v;
        }
        i = j;
    }
}


// --- mfg-config.txt: el archivo unico ---
//
// Una clave por linea, `clave valor`. Las banderas por su nombre de campo con
// 0 o 1 (polaridad DIRECTA: `seis 0` apaga el 6X, `ota 1` enciende OTA); los
// numericos por su nombre con el mismo rango que su archivo; markergap y
// slowframe con sus 2-4 y 1-3 numeros. Se lee DESPUES de los archivos, asi
// que una clave explicita le gana a un archivo presente. Lo que no se
// reconoce se ignora. El panel no toca este archivo: escribe mfg-settings.txt
// entero cada vez, y por eso las banderas no pueden vivir ahi.

struct Numeric {
    const char *key;
    int Settings::*field;
    int lo, hi;
};
inline const Numeric kNumerics[] = {
    { "blockms",      &Settings::blockms,      1, 99999 },
    { "jitter",       &Settings::jitter,       1, 90 },
    { "clamplatency", &Settings::clamplatency, 1, 9 },
    { "queue",        &Settings::queue,        0, 3 },
    { "blocks",       &Settings::blocks,       2, 64 },
    { "dynstep",      &Settings::dynstep,      0, 100 },
};
inline const int kNumericsN = (int)(sizeof(kNumerics) / sizeof(kNumerics[0]));

inline bool key_is(const char *line, unsigned n, const char *key, unsigned *fin) {
    unsigned i = 0;
    while (key[i] != 0) {
        if (i >= n || line[i] != key[i]) return false;
        ++i;
    }
    if (i >= n || (line[i] != ' ' && line[i] != '=' && line[i] != '\t')) return false;
    *fin = i;
    return true;
}

// Devuelve cuantas claves reconocio.
inline int parse_config(const char *buf, unsigned n, Settings &a) {
    int seen = 0;
    unsigned i = 0;
    while (i < n) {
        unsigned fin = i;
        while (fin < n && buf[fin] != '\n' && buf[fin] != '\r') ++fin;
        const char *l = buf + i;
        const unsigned len = fin - i;
        unsigned k = 0;
        bool done = false;
        for (int b = 0; b < kFlagsN && !done; ++b)
            if (key_is(l, len, kFlags[b].key, &k)) {
                int v[1] = {0};
                if (loose_ints(l + k, len - k, v, 1) == 1 && (v[0] == 0 || v[0] == 1)) {
                    a.*(kFlags[b].field) = v[0] == 1;
                    ++seen;
                }
                done = true;
            }
        for (int m = 0; m < kNumericsN && !done; ++m)
            if (key_is(l, len, kNumerics[m].key, &k)) {
                int v[1] = {0};
                if (loose_ints(l + k, len - k, v, 1) == 1 &&
                    v[0] >= kNumerics[m].lo && v[0] <= kNumerics[m].hi) {
                    a.*(kNumerics[m].field) = v[0];
                    ++seen;
                }
                done = true;
            }
        if (!done && key_is(l, len, "markergap", &k)) {
            const int c = loose_ints(l + k, len - k, a.marker, 4);
            if (c >= 2) { a.marker_n = c; ++seen; }
            done = true;
        }
        if (!done && key_is(l, len, "slowframe", &k)) {
            const int c = loose_ints(l + k, len - k, a.slowframe, 3);
            if (c >= 1) { a.slowframe_n = c; ++seen; }
            done = true;
        }
        i = fin;
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) ++i;
    }
    return seen;
}

}  // namespace cfg
