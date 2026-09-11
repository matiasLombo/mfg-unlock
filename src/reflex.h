// reflex.h -- lo que Reflex sabe y nosotros no: la latencia real por frame y
// la base (frames renderizados por segundo) contada por el propio driver.
//
// ENTRA: el informe de slReflexGetState (hk_slReflexGetState cuando el juego
//   lo llama; reflex_poll lo consulta cada kReflexEvery frames cuando no),
//   con los offsets verificados en [[lab-measures-latency]]; los marcadores
//   de PCL/Reflex (hk_slPCLSetMarker / hk_slReflexSetMarker) y el corte de
//   marcadores del banco (in_marker_gap, mfg-markergap).
// SALE: g_rfx_base (la base para measurement.h y writer.h), g_rfx_gpu /
//   g_rfx_drv / g_rfx_ft / g_rfx_min / g_rfx_max / g_rfx_n (la latencia que
//   measurement.h vuelca y muestra en el HUD), g_ctrl_fps / g_ctrl_n (el
//   estimador de base del controlador, ctrl_feed_dt), g_cap_mode / g_cap_cnt
//   / g_game_hwnd (lo que el juego pidio, para el volcado).
// DEPENDE DE: log_line/log_num, QueryPerformanceCounter, GetTickCount64, y los
//   globales compartidos que todavia viven en proxy.cpp.
//
// Movido de proxy.cpp tal cual (2026-09-11): un solo rango, mismo orden de
// declaracion. Sin tocar una linea del cuerpo.
#pragma once

// What the last capture was taken from. A game may call slDLSSGSetOptions
// once per frame, and re-capturing on every call meant two VirtualQuery
// syscalls per frame on the game's own render thread -- to copy bytes that had
// not changed since the frame before. The capture itself is still page-bounded
// every time it runs; what is skipped is running it when there is nothing new.
static LONG g_cap_mode = -1, g_cap_cnt = -1;
// Para clasificar los apagones sin pedirle al usuario que juegue distinto:
// si la ventana no tiene el foco, o el juego dejo de estar en primer plano,
// el apagon es del menu/pausa/alt-tab y no un defecto nuestro.
static HWND g_game_hwnd = nullptr;
// Latencia: sl.reflex la calcula solo. El sample llama a slReflexGetState
// todos los frames (StreamlineSample.cpp:901, sin condicion), asi que envolver
// esa llamada da el struct ya armado por el, con su GUID y su version -- no
// hay que adivinar nada. La cuenta que importa la escribe NVIDIA en su propio
// sample: totalGameToRenderLatency = gpuRenderEndTime - inputSampleTime.
// Paso 1, y por ahora lo unico: volcar los bytes para leer el layout. Nada de
// calcular latencias sobre offsets supuestos.
typedef unsigned (*PFN_slReflexGetState)(void *);
static PFN_slReflexGetState g_orig_reflexstate = nullptr;
static bool g_reflex_dumped = false;
// Sacado del volcado: GUID en +8 y version 2 en +24 de la ReflexState que
// el sample entrego ya armada. Hace falta porque GTA V llama a
// slReflexGetState apenas un punado de veces -- al abrir su menu, se ve --
// y no una vez por frame como el sample, asi que colgarse de sus llamadas
// no da ningun dato. Consultamos nosotros.
static const unsigned char kReflexStateGuid[16] = {
    0x85, 0x59, 0xbb, 0xf0,
    0xf9, 0xda,
    0x28, 0x47,
    0xb2, 0xfd, 0xae, 0x80, 0xa2, 0xbd, 0x79, 0x89
};
// 48 de cabecera mas 64 informes de 152 son 9776; se pide de mas y se
// pone en cero, que es como se pidio DLSSGState y nunca fallo.
static unsigned char g_reflex_buf[16384];
// Offsets confirmados con el volcado y con dos chequeos internos: el
// gpuActiveRenderTimeUs que escribe NVIDIA coincide con gpuRenderEnd-Start
// (3802 vs 3883 us) y su gpuFrameTimeUs de 16854 us es exactamente la base de
// 59 fps que medimos aparte. El cuerpo del informe 63, el mas reciente, cae en
// 72 + 152*63 = 9648.
static double g_rfx_gpu = 0.0;     // sim start -> fin de render en GPU
static double g_rfx_drv = 0.0;     // sim start -> fin de driver
static long long g_qpc_freq = 1;   // set in DllMain
static double g_rfx_ft = 0.0;          // gpuFrameTimeUs, para validar contra la base
static double g_ctrl_fps = 0.0;       // frames/tiempo, insesgada, para el controlador
// Cuantas muestras tiene el anillo del estimador ahora mismo.
//
// Existe porque el detector de escalon vacia el anillo y deja la base valiendo
// 1/dt de UN solo frame. Al armarse DYNAMIC los primeros frames son lentos --
// la reconfiguracion del swapchain esta medida en [[fractional-swapchain-churn]]
// -- asi que un frame de ~62 ms dejaba la base en 16 fps con la real en 156.
// Con eso el controlador calculaba 165/16 = 10.3 y topaba en 6.00, que es
// justo el ratio que revienta.
//
// Del log de Halo, sin interpretacion:
//
//     [37891ms] dynamic: refresh is 165            <- DYNAMIC recien armado
//     [38000ms] target needs more than 6x at this base, fps 165
//     [38000ms]   base is 16
//     [38172ms] measured: rendered fps 156         <- 172 ms despues
//     [38172ms]   base por Reflex 0                <- y Reflex no alimentaba
//
// Con la base real el ratio pedido es 165/33 = 5.0 exacto: alcanzable y sin
// tocar la ranura que el plugin no llena.
static int g_ctrl_n = 0;
static double g_rfx_base = 0.0;        // base en fps, del contador de frames de Reflex
static ULONGLONG g_ctrl_rfx_ms = 0;    // ultima vez que Reflex alimento el estimador

// El estimador de base del controlador. La logica es la misma de siempre y no
// se toca: lo unico que cambia es de donde viene el dt.
//
// Se calcula n/suma(dt) y no 1/promedio(dt): promediar intervalos para despues
// invertir NO da la tasa media -- por Jensen 1/E[dt] <= E[1/dt] -- y con tiempos
// de frame que varian la base salia sistematicamente baja, el controlador pedia
// de mas y las presentadas quedaban 2% arriba del objetivo. Ese era el sesgo
// residual que no se explicaba.
static void ctrl_feed_dt(double dt) {
    if (dt <= 0.0) return;
    static double ring[8] = { 0,0,0,0,0,0,0,0 };
    static int ri = 0, rn = 0;
    static double rsum = 0.0;
    if (rn == 8) rsum -= ring[ri];
    ring[ri] = dt;
    rsum += dt;
    ri = (ri + 1) & 7;
    if (rn < 8) ++rn;
    g_ctrl_fps = rsum > 0.0 ? (double)rn / rsum : 0.0;
    g_ctrl_n = rn;
    // Un escalon invalida la historia: se reempieza con el valor nuevo en vez
    // de arrastrar ocho frames de la carga anterior.
    static double prev2 = 0.0;
    if (prev2 > 0.0 && rn > 1) {
        const double mean = rsum / (double)rn;
        const bool lejos2 = dt < mean * 0.75 || dt > mean * 1.33;
        const bool igual2 = dt > prev2 * 0.88 && dt < prev2 * 1.12;
        if (lejos2 && igual2) {
            ring[0] = dt; ri = 1; rn = 1; rsum = dt;
            g_ctrl_fps = 1.0 / dt;
            g_ctrl_n = 1;         // y con una muestra no se decide nada
        }
    }
    prev2 = dt;
}

// Reflex manda mientras siga reportando. Medio segundo de silencio -- el menu de
// GTA V congela el marcador -- y el camino del token vuelve a alimentar.
static bool ctrl_fed_by_reflex(void) {
    return g_ctrl_rfx_ms != 0 && GetTickCount64() - g_ctrl_rfx_ms < 500;
}
static LONG g_rfx_n = 0;
// Minimo y maximo ademas del promedio: si la cadencia fraccionaria alterna
// entre comportarse como el entero de abajo y el de arriba, la latencia seria
// dos poblaciones y no un valor intermedio. El promedio de la ventana eso lo
// tapa, que es exactamente como este proyecto ya se equivoco una vez.
static unsigned g_rfx_min = 0xFFFFFFFFu;
static unsigned g_rfx_max = 0;
static unsigned long long g_rfx_lastid = 0;

// Dentro de la ventana de corte no reenvia el marcador: el id de frame de
// Reflex deja de avanzar y DLSS-G ve exactamente lo que ve en GTA V.
// Con dos numeros son segundos, como antes. Con cuatro son milisegundos:
// encendido, apagado, cada cuanto el apagon largo, y cuanto dura.
//
// Los cuatro existen porque la ventana de medicion son 45 frames -- unos 790 ms
// a base 57 -- y una rafaga encendida MAS LARGA que eso deja ventanas enteras
// generando, que leen 2.25. Con la rafaga mas corta que la ventana ninguna
// ventana llega a estar entera encendida: las que caen dentro del apagon leen
// 1.0 y las parciales llegan hasta 2.0, sin ninguna en 3.0. El apagon largo
// aparte da la racha de decenas de segundos.
static bool in_marker_gap(void) {
    if (g_marker_every <= 0.0) return false;
    static LARGE_INTEGER f = {}, s0 = {};
    if (f.QuadPart == 0) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&s0); }
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    const double t = (double)(n.QuadPart - s0.QuadPart) / (double)f.QuadPart;
    if (g_marker_long_every > 0.0) {
        const double u = t - (double)((long long)(t / g_marker_long_every)) *
                             g_marker_long_every;
        if (u < g_marker_long_for) return true;
    }
    const double cycle = g_marker_every + g_marker_for;
    return (t - (double)((long long)(t / cycle)) * cycle) >= g_marker_every;
}

static unsigned hk_slPCLSetMarker(unsigned marker, void *frame) {
    if (in_marker_gap()) { ++g_markers_dropped; return 0; }
    return g_orig_pclmarker ? g_orig_pclmarker(marker, frame) : 0;
}

static unsigned hk_slReflexSetMarker(unsigned marker, void *frame) {
    if (in_marker_gap()) { ++g_markers_dropped; return 0; }
    return g_orig_reflexmarker ? g_orig_reflexmarker(marker, frame) : 0;
}

// El informe 63 es el mas reciente. Offsets confirmados con el volcado y
// con dos chequeos internos del propio NVIDIA.
// Cuantos informes hacia atras se leen por consulta, y cada cuantos frames se
// consulta. El producto tiene que cubrir los frames entre consultas.
static const int kReflexBack = 6;
static const int kReflexEvery = 4;

// atras=0 es el informe 63, el mas reciente; atras=1 el 62, y asi.
static void reflex_take(const void *state, int back) {
    const unsigned char *q = (const unsigned char *)state + 72 + 152 * (63 - back);
    const unsigned long long id  = *(const unsigned long long *)(q + 0);
    const unsigned long long sim = *(const unsigned long long *)(q + 16);
    const unsigned long long drv = *(const unsigned long long *)(q + 72);
    const unsigned long long gpu = *(const unsigned long long *)(q + 104);
    const unsigned ft = *(const unsigned *)(q + 116);
    // Un informe por frame: consultas seguidas devuelven el mismo.
    if (id == 0 || id == g_rfx_lastid || sim == 0 || gpu <= sim || drv <= sim)
        return;
    // La base, del contador de frames del propio Reflex.
    //
    // La base se estimaba contando llamadas a slGetNewFrameToken. Eso depende
    // de como llame el juego, y los juegos no llaman igual: GTA V lo llama una
    // vez por frame y sale bien, Cyberpunk unas dieciseis y la base leyo
    // mediana 1789 fps con el monitor en 165. El deduplicador por indice que
    // habia para eso nunca sirvio -- en GTA V "frames past the gate" y "hook
    // calls total" son el mismo numero, 141064, o sea que jamas descarto una
    // llamada. Andaba por suerte del juego, no por el filtro.
    //
    // Este id lo lleva el driver y avanza exactamente una vez por frame
    // renderizado, lo llame el juego como lo llame. Y se usa la DIFERENCIA de
    // ids sobre el tiempo, no una cuenta propia, asi que perder muestras entre
    // consultas no sesga nada: si entre dos lecturas pasaron treinta frames, la
    // diferencia dice treinta.
    //
    // El limite de 1000 no es cosmetico: es lo que separa un numero fisico de
    // uno imposible. Si el layout del informe estuviera mal en alguna version
    // de Streamline, esto lo deja afuera en vez de alimentar al controlador con
    // basura, que es exactamente lo que paso.
    {
        // Sobre el MAXIMO id visto, no sobre el ultimo leido.
        //
        // reflex_poll lee seis informes hacia atras y el 63 es el mas nuevo, asi
        // que dentro de una misma consulta los ids llegan en orden DESCENDENTE.
        // Tomando el ultimo, la referencia se reiniciaba en cada consulta y la
        // diferencia se media sobre una secuencia desordenada: dio 75 fps en un
        // escenario con techo de 58.8, o sea un numero imposible. El maximo es
        // monotono y no le importa en que orden lleguen.
        static unsigned long long id_hi = 0, id_ref = 0;
        static LONGLONG t_ref = 0;
        if (id > id_hi) id_hi = id;
        LARGE_INTEGER tn;
        QueryPerformanceCounter(&tn);
        if (id_ref == 0 || id_hi < id_ref || t_ref == 0) {
            id_ref = id_hi;
            t_ref = tn.QuadPart;
        } else if (g_qpc_freq > 0) {
            const double secs = (double)(tn.QuadPart - t_ref) / (double)g_qpc_freq;
            if (secs >= 0.25) {
                const double fps = (double)(id_hi - id_ref) / secs;
                if (fps > 1.0 && fps < 1000.0) g_rfx_base = fps;
                id_ref = id_hi;
                t_ref = tn.QuadPart;
            }
        }
    }
    // El intervalo de frame del driver, al mismo estimador de siempre. Rango
    // fisico: 1 ms a 500 ms, o sea 2 a 1000 fps. Fuera de ahi el informe no
    // dice lo que creemos y se deja pasar al camino del token.
    if (ft >= 1000 && ft <= 500000) {
        ctrl_feed_dt((double)ft / 1e6);
        g_ctrl_rfx_ms = GetTickCount64();
    }
    g_rfx_lastid = id;
    g_rfx_gpu += (double)(gpu - sim);
    g_rfx_drv += (double)(drv - sim);
    g_rfx_ft  += (double)ft;
    ++g_rfx_n;
    const unsigned dv = (unsigned)(drv - sim);
    if (dv < g_rfx_min) g_rfx_min = dv;
    if (dv > g_rfx_max) g_rfx_max = dv;
}

// Se llama una vez por frame desde el hilo de present, que es donde ya
// corre el resto de la medicion.
static void reflex_poll(void) {
    if (g_orig_reflexstate == nullptr) return;
    // Una de cada kReflexEvery. La consulta llena 9776 bytes -- 64 informes de
    // 152 -- y se consumia una vez por ventana de 45 frames para el log y dos
    // veces por segundo para el HUD: llamarla por frame era unas treinta veces
    // mas de lo necesario, en el hilo de render.
    {
        static int n = 0;
        if (++n < kReflexEvery) return;
        n = 0;
    }
    for (int i = 0; i < 64; ++i) g_reflex_buf[i] = 0;
    for (int i = 0; i < 16; ++i) g_reflex_buf[8 + i] = kReflexStateGuid[i];
    *(unsigned long long *)(g_reflex_buf + 24) = 2;      // structVersion 2
    const unsigned r = g_orig_reflexstate(g_reflex_buf);
    if (r != 0) {
        static bool said = false;
        if (!said) { said = true; log_num("reflex: query refused, code ", r); }
        return;
    }
    // Se leen los ultimos informes, no solo el 63. El anillo tiene 64 y la
    // consulta se hace cada pocos frames, asi que leer hacia atras recupera los
    // frames que pasaron entre una consulta y la siguiente: mismas muestras con
    // una fraccion de las llamadas. reflex_take deduplica por frameID, asi que
    // releer uno ya visto no cuenta dos veces.
    for (int k = kReflexBack - 1; k >= 0; --k)
        reflex_take(g_reflex_buf, k);
}

static unsigned hk_slReflexGetState(void *state) {
    const unsigned r = g_orig_reflexstate ? g_orig_reflexstate(state) : 1;
    // El layout salio del volcado, no de suponer: GUID en +8, version 2 en +24,
    // banderas en +32, y frameReport[i] es a su vez una estructura de Streamline
    // de 152 bytes con cabecera propia de 32, asi que su cuerpo arranca en
    // 72 + 152*i. El [63] es el mas reciente: 72 + 152*63 = 9648.
    static int calls = 0;
    ++calls;
    if (r == 0 && state != nullptr) reflex_take(state, 0);
    if (r == 0 && state != nullptr && !g_reflex_dumped && calls > 300) {
        g_reflex_dumped = true;
        log_num("reflex: latencyReportAvailable ",
                (unsigned)((const unsigned char *)state)[33]);
        const unsigned char *p = (const unsigned char *)state + 9648;
        for (int row = 0; row < 8; ++row) {
            char line[80];
            int k = 0;
            const char *pre = "reflex dump +";
            while (pre[k] != 0) { line[k] = pre[k]; ++k; }
            const int off = row * 16;
            line[k++] = (char)('0' + (off / 100) % 10);
            line[k++] = (char)('0' + (off / 10) % 10);
            line[k++] = (char)('0' + off % 10);
            line[k++] = ':';
            for (int c = 0; c < 16; ++c) {
                const unsigned char v = p[off + c];
                line[k++] = ' ';
                line[k++] = "0123456789abcdef"[v >> 4];
                line[k++] = "0123456789abcdef"[v & 15];
            }
            line[k] = 0;
            log_line(line);
        }
    }
    return r;
}
