// slinit.h -- el hook de slInit: leer las Preferences que el juego le da a
// Streamline y, si se pidio, cambiarlas (OTA, pathsToPlugins).
//
// ENTRA: la llamada slInit(pref, sdk) del juego (hk_slInit), con la
//   disposicion de Preferences verificada contra include/sl_core_types.h
//   del SDK 2.12 (+40 pathsToPlugins, +48 numPathsToPlugins, +88 flags);
//   la configuracion (g_ota, g_pathsplugins).
// SALE: el volcado de las banderas en el log, las banderas reescritas
//   cuando g_ota (eAllowOTA | eLoadDownloadedPlugins) o g_pathsplugins
//   (nuestra carpeta), y la llamada original.
// DEPENDE DE: log_line/log_num, GetEnvironmentVariableW, y los globales
//   compartidos que todavia viven en proxy.cpp. Lo instala
//   arm_slinit_temprano (proxy.cpp) o el armado general.
//
// Movido de proxy.cpp tal cual (2026-09-11): un rango, mismo orden. Sin
// tocar una linea del cuerpo. Reglas: [[forzar-ota-para-parchear]].
#pragma once

static const int kPrefFlags = 88;

// Engancha SOLO slInit, para poder correrlo temprano.
//
// El armado general vive en el hilo del panel y llega tarde: en el sample del
// banco slInit ya se llamo cuando ese hilo despierta, asi que el volcado nunca
// aparecio. En Cyberpunk si llega porque ahi el interposer carga mas tarde.
//
// Corre en DllMain solo si existe mfg-ota.txt. Inicializar MinHook bajo el
// bloqueo del cargador es un riesgo conocido, asi que se paga unicamente
// cuando alguien pidio el forzado; sin la bandera, nada de esto pasa y los
// juegos que ya andan no cambian.
static void arm_slinit_early(void);


static unsigned hk_slInit(void *pref, unsigned long long sdk) {
    if (pref != nullptr) {
        unsigned char *p = (unsigned char *)pref;
        log_num("slInit: structVersion ", (unsigned)*(unsigned long long *)(p + 24));
        const unsigned long long f = *(unsigned long long *)(p + kPrefFlags);
        log_num("  banderas en +88 ", (unsigned)f);
        // EXPERIMENTO (mfg-pathsplugins.txt): apagar OTA y nombrar NUESTRA
        // carpeta, para que el plugin manager del interposer DEL JUEGO enumere
        // ahi y no mire la cache de NGX.
        //
        // La disposicion no es una suposicion: sale del header oficial del SDK
        // 2.12, include/sl_core_types.h, contando desde BaseStructure
        // (next +0, GUID +8, structVersion +24 = 32):
        //
        //   +32 bool showConsole          +40 const wchar_t** pathsToPlugins
        //   +36 LogLevel logLevel         +48 uint32_t numPathsToPlugins
        //   +56 pathToLogsAndData         +88 PreferenceFlags flags
        //
        // Los dos ceros que se veian en el volcado eran justamente
        // pathsToPlugins nulo y numPathsToPlugins en 0. Y el +88 de las
        // banderas, verificado hace tiempo por otro camino, cae en el mismo
        // lugar: el conteo cierra entero.
        //
        // Detras de un flag porque cambia la TOPOLOGIA de carga, que es la
        // unica parte del diseno que puede romper un juego que hoy anda. Y
        // porque falta medir algo que el header no dice: el plugin manager,
        // ante el mismo plugin en dos rutas, se queda con el mas nuevo ("A
        // duplicate was found, but a newer plugin version was available"). Si
        // el juego trae 2.13 y nosotros 2.12, puede preferir el suyo aunque
        // apuntemos aca.
        if (g_pathsplugins) {
            static wchar_t buf[MAX_PATH];
            static const wchar_t *list[1];
            static bool built = false;
            if (!built) {
                const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH - 40);
                if (n > 0 && n < MAX_PATH - 40) {
                    int k = (int)n;
                    const wchar_t *tail = L"\\mfg-unlock\\sdk\\2.12";
                    for (int i = 0; tail[i] != 0; ++i) buf[k++] = tail[i];
                    buf[k] = 0;
                    list[0] = buf;
                    built = true;
                }
            }
            if (built) {
                *(const wchar_t ***)(p + 40) = list;
                *(unsigned *)(p + 48) = 1;
                *(unsigned long long *)(p + kPrefFlags) =
                    f & ~((1ull << 3) | (1ull << 6));   // eAllowOTA, eLoadDownloadedPlugins
                log_line("  pathsToPlugins apuntado a nuestra carpeta (mfg-pathsplugins.txt)");
                log_num("    banderas ahora ",
                        (unsigned)*(unsigned long long *)(p + kPrefFlags));
            }
        }
        // logLevel, a verbose, para que el runtime diga por que corta.
        //
        // beginCommandList (sl.common 0x77AF0) devuelve false por una de dos
        // razones y las dos escriben al log de Streamline:
        //
        //     'Command list not closed'
        //     "Couldn't reset the allocator %S for %.2f seconds."
        //
        // Ninguna aparece en el sl.log de Halo, pero las dos estan gateadas por
        // una bandera global:
        //
        //     0x77b08  cmp byte ptr [rip+0x59e2a], 0
        //     0x77b0f  je  0x77b98        ; sin log, igual devuelve false
        //
        // SL_LOG_LEVEL=2 ya se pone por entorno y no alcanzo: lo que la
        // aplicacion pasa en Preferences gana. Aca se pisa el campo.
        //
        // Offset 36: BaseStructure son 32 (next 8 + GUID 16 + structVersion 8),
        // showConsole ocupa 4 con relleno, y logLevel viene despues. El mismo
        // conteo que da 88 para las banderas, que ya esta verificado.
        //
        // Solo si el valor que hay tiene forma de enum chico -- si no, el
        // offset no es el que creemos y escribir seria corromper.
        {
            LONG *nivel = (LONG *)(p + 36);
            const LONG before = *nivel;
            log_num("  logLevel en +36 ", (unsigned)before);
            if (before >= 0 && before <= 3 && g_sllog_on) {
                DWORD old = 0;
                if (VirtualProtect(nivel, 4, PAGE_READWRITE, &old)) {
                    *nivel = 2;                 // eVerbose
                    VirtualProtect(nivel, 4, old, &old);
                    log_line("  logLevel forzado a verbose (mfg-sllog.txt)");
                }
            }
        }
        log_num("    eAllowOTA (bit 3) ", (unsigned)((f >> 3) & 1));
        log_num("    eLoadDownloadedPlugins (bit 6) ", (unsigned)((f >> 6) & 1));
        // Volcado crudo para poder ubicar el campo si el offset no fuera 88.
        for (int fila = 0; fila < 9; ++fila) {
            char buf[64];
            const unsigned long long v = *(unsigned long long *)(p + fila * 8);
            wsprintfA(buf, "    +%d = %08X%08X", fila * 8,
                      (unsigned)(v >> 32), (unsigned)v);
            log_line(buf);
        }
        if (g_ota) {
            DWORD old = 0;
            if (VirtualProtect(p + kPrefFlags, 8, PAGE_READWRITE, &old)) {
                *(unsigned long long *)(p + kPrefFlags) = f | (1ull << 3) | (1ull << 6);
                VirtualProtect(p + kPrefFlags, 8, old, &old);
                log_num("  OTA forzado, banderas ahora ",
                        (unsigned)*(unsigned long long *)(p + kPrefFlags));
            } else {
                log_line("  ! no se pudo escribir las banderas");
            }
        }
    }
    return g_orig_slinit(pref, sdk);
}
