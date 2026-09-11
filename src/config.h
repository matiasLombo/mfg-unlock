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

namespace cfg {

struct Ajustes {
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

    // --- mfg-settings.txt: -1 = la clave no estaba o no valia ---
    int mode           = -1;
    int target         = -1;
    int dynfps         = -1;
    int hud            = -1;
};

struct Bandera {
    const wchar_t *archivo;
    bool Ajustes::*campo;
};

// Archivo -> campo. El valor con el archivo presente es !defecto, asi que la
// polaridad esta en el defecto del campo y no aca.
inline const Bandera kBanderas[] = {
    { L"mfg-debug.txt",         &Ajustes::debug },
    { L"mfg-watch.txt",         &Ajustes::watch },
    { L"mfg-novsync.txt",       &Ajustes::novsync },
    { L"mfg-pinlatency.txt",    &Ajustes::pinlatency },
    { L"mfg-pacefollow.txt",    &Ajustes::pacefollow },
    { L"mfg-nofrac.txt",        &Ajustes::frac },
    { L"mfg-sub2.txt",          &Ajustes::sub2 },
    { L"mfg-twocopies.txt",     &Ajustes::twocopies },
    { L"mfg-ceilfirst.txt",     &Ajustes::ceilfirst },
    { L"mfg-ota.txt",           &Ajustes::ota },
    { L"mfg-noslowalt.txt",     &Ajustes::slowalt },
    { L"mfg-quiet.txt",         &Ajustes::quiet },
    { L"mfg-nullalt.txt",       &Ajustes::nullalt },
    { L"mfg-sinsat.txt",        &Ajustes::sat },
    { L"mfg-pathsplugins.txt",  &Ajustes::pathsplugins },
    { L"mfg-peralt.txt",        &Ajustes::peralt },
    { L"mfg-sinseis.txt",       &Ajustes::seis },
    { L"mfg-coninterposer.txt", &Ajustes::coninterposer },
    { L"mfg-sinbase.txt",       &Ajustes::sinbase },
    { L"mfg-mfcmax.txt",        &Ajustes::mfcmax },
    { L"mfg-topefijo.txt",      &Ajustes::topefijo },
    { L"mfg-x6.txt",            &Ajustes::x6 },
    { L"mfg-dyndiag.txt",       &Ajustes::dyndiag },
    { L"mfg-nolatch.txt",       &Ajustes::latch },
    { L"mfg-sin-deuda.txt",     &Ajustes::deuda },
    { L"mfg-optsv3.txt",        &Ajustes::optsv3 },
    { L"mfg-blockalt.txt",      &Ajustes::blockalt },
    { L"mfg-nowaitable.txt",    &Ajustes::nowaitable },
    { L"mfg-nopanel.txt",       &Ajustes::panel },
    { L"mfg-nowic.txt",         &Ajustes::wic },
    { L"mfg-monoidx.txt",       &Ajustes::monoidx },
    { L"mfg-nocubins.txt",      &Ajustes::cubins },
    { L"mfg-nometer.txt",       &Ajustes::meter_off },
    { L"mfg-presetb.txt",       &Ajustes::presetb },
    { L"mfg-sllog.txt",         &Ajustes::sllog },
    { L"mfg-indicator.txt",     &Ajustes::indicator },
};
inline const int kBanderasN = (int)(sizeof(kBanderas) / sizeof(kBanderas[0]));

// `existe(archivo)` dice si el archivo esta al lado de la dll. En el dll es
// GetFileAttributesW; en el test, lo que el caso diga.
template <class Existe>
inline void leer_banderas(Ajustes &a, Existe existe) {
    for (int i = 0; i < kBanderasN; ++i)
        if (existe(kBanderas[i].archivo)) a.*(kBanderas[i].campo) = !(a.*(kBanderas[i].campo));
}

// Los tres parsers de numeros que DllMain tenia repetidos, tal cual:

// Digitos desde el primer byte, se corta en el primer no-digito.
// (mfg-blockms, mfg-jitter, mfg-blocks)
inline int entero_prefijo(const char *b, unsigned n) {
    int v = 0;
    for (unsigned i = 0; i < n && b[i] >= '0' && b[i] <= '9'; ++i)
        v = v * 10 + (b[i] - '0');
    return v;
}

// Hasta `max` enteros separados por cualquier cosa que no sea digito.
// (mfg-markergap, mfg-slowframe). Devuelve cuantos encontro.
inline int enteros_sueltos(const char *b, unsigned n, int *v, int max) {
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
inline int digito_primero(const char *b, unsigned n, char lo, char hi) {
    if (n == 0 || b[0] < lo || b[0] > hi) return -1;
    return b[0] - '0';
}

// Cada numerico con el rango que DllMain le aplicaba. `leer(archivo, buf,
// cap)` devuelve cuantos bytes trajo, 0 si no existe.
template <class Leer>
inline void leer_numericos(Ajustes &a, Leer leer) {
    char b[48]; unsigned n;
    if ((n = leer(L"mfg-blockms.txt", b, 15)) > 0) {
        const int v = entero_prefijo(b, n);
        if (v > 0 && v < 100000) a.blockms = v;
    }
    if ((n = leer(L"mfg-markergap.txt", b, 31)) > 0)
        a.marker_n = enteros_sueltos(b, n, a.marker, 4);
    if ((n = leer(L"mfg-slowframe.txt", b, 47)) > 0)
        a.slowframe_n = enteros_sueltos(b, n, a.slowframe, 3);
    if ((n = leer(L"mfg-jitter.txt", b, 15)) > 0) {
        const int v = entero_prefijo(b, n);
        if (v > 0 && v <= 90) a.jitter = v;
    }
    if ((n = leer(L"mfg-clamplatency.txt", b, 15)) > 0) {
        const int v = digito_primero(b, n, '1', '9');
        if (v >= 0) a.clamplatency = v;
    }
    if ((n = leer(L"mfg-queue.txt", b, 15)) > 0) {
        const int v = digito_primero(b, n, '0', '3');
        if (v >= 0) a.queue = v;
    }
    if ((n = leer(L"mfg-blocks.txt", b, 31)) > 0) {
        const int v = entero_prefijo(b, n);
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
inline void parsear_settings(const char *buf, unsigned n, int filas, int max_target,
                             Ajustes &a) {
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
            if (v >= 0 && v < filas) a.mode = v;
        } else if (is_tgt && v >= 0 && v <= max_target) {
            a.target = v;
        }
        i = j;
    }
}

}  // namespace cfg
