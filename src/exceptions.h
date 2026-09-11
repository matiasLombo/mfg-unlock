// exceptions.h -- el testigo: quien mata el proceso, dicho por el proceso
// mismo, y la autopsia del sub-frame en el instante del crash.
//
// ENTRA: cada excepcion del proceso, por el vectored exception handler
//   (testigo_excepcion, registrado en DllMain con AddVectoredExceptionHandler)
//   ANTES que cualquier __try del juego o del driver.
// SALE: el bloque "EXCEPCION ----" en el log (codigo, modulo, offset,
//   operacion, la seleccion en curso, la cuenta pedida y aplicada, el byte
//   vivo) y, si cae dentro de sl.dlss_g, la autopsia de los sub-frames
//   (autopsia_subframes, con leer_seguro para no morir leyendo).
//   Solo registra y devuelve EXCEPTION_CONTINUE_SEARCH: no cambia nada.
// DEPENDE DE: log_line/log_num, GetModuleHandleExW, y los globales
//   compartidos que todavia viven en proxy.cpp (g_force_sel,
//   g_force_generated, g_api_applied, g_count_live, g_dlssg_base).
//
// Movido de proxy.cpp tal cual (2026-09-11): un rango, mismo orden. Sin
// tocar una linea del cuerpo.
#pragma once

// Quien mata el proceso, dicho por el proceso mismo.
//
// Halo muere al entrar a 6X sin dejar dump ni evento WER: algo se traga la
// excepcion antes de que Windows la vea, asi que siete intentos no dieron ni
// una direccion. Un vectored exception handler corre ANTES que cualquier
// __try del juego o del driver, asi que lo ve igual.
//
// Solo registra y devuelve CONTINUE_SEARCH: no cambia el comportamiento, no
// traga nada, no intenta recuperarse. Es un testigo.
//
// Se filtran las excepciones de control de flujo que son normales y ruidosas
// (breakpoints de depurador, C++ EH, y las de "primera oportunidad" que los
// motores usan a proposito): sin ese filtro el log se llena y el evento que
// importa se pierde.
// Lee memoria ajena sin poder faultear a su vez.
//
// Corre DENTRO del manejador de excepciones, donde una segunda violacion de
// acceso no da un segundo aviso: mata el proceso sin log. VirtualQuery contesta
// por pagina y sin tocar el contenido, asi que se pregunta primero.
static bool leer_seguro(const void *src, void *dst, unsigned n) {
    if (src == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(src, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD leible = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & leible) == 0) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    // Que el rango entero entre en esta region; si cruza pagina, no se lee.
    const ULONG_PTR fin = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    if ((ULONG_PTR)src + n > fin) return false;
    memcpy(dst, src, n);
    return true;
}

// La autopsia del array de sub-frames, en el instante del fallo.
//
// El fallo es siempre el mismo: sl.dlss_g +0x3ED6F, LECTURA de [nulo+0x40].
//
//     0x3ecc9  mov  r10d, [rbp + 0xf8]        ; indice, 6to argumento
//     0x3ed62  lea  rcx, [r10 + r10*2]
//     0x3ed66  shl  rcx, 6                    ; r10 * 192
//     0x3ed6a  mov  rax, [rcx + r13 + 0x48]   ; array[r10]
//     0x3ed6f  mov  ecx, [rax + 0x40]         ; <- aca
//
// Con el CONTEXT en la mano se leen r10 (que indice pidio) y r13 (la base del
// contexto), y de ahi las 6 ranuras y los dos contadores en +4 y +8. Eso separa
// de una vez las hipotesis que llevan tres intentos sin distinguirse:
//
//   * si el nulo esta en r10 y las de abajo estan llenas -> la presentacion
//     pidio un indice que la generacion nunca lleno
//   * si TODAS estan nulas -> el contexto no es el que creiamos, o se destruyo
//   * si [r13+4] no es nuestro byte -> el parche no llega a este contexto
//
// Solo lee. No cambia nada y devuelve CONTINUE_SEARCH igual que el resto.
static void autopsia_subframes(const CONTEXT *ctx, const void *modbase) {
    if (ctx == nullptr) return;
    const ULONG_PTR base = (ULONG_PTR)ctx->R13;
    const unsigned idx = (unsigned)(ctx->R10 & 0xFFFFFFFFu);
    log_line("  --- autopsia del array de sub-frames ---");
    log_num("  indice pedido (r10) ", (unsigned long long)idx);
    log_num("  base del contexto (r13) 0x", (unsigned long long)base);
    LONG total = 0, hechos = 0;
    if (leer_seguro((const void *)(base + 4), &total, 4))
        log_num("  [ctx+4] (nuestro byte) ", (unsigned)total);
    else
        log_line("  [ctx+4] ilegible");
    if (leer_seguro((const void *)(base + 8), &hechos, 4))
        log_num("  [ctx+8] (el otro contador) ", (unsigned)hechos);
    else
        log_line("  [ctx+8] ilegible");
    // Las 6 ranuras inline de 192 bytes. Cual esta llena y cual no es el dato.
    for (unsigned i = 0; i < 6; ++i) {
        const ULONG_PTR ranura = base + 0x48 + (ULONG_PTR)i * 192;
        void *ptr = nullptr;
        if (!leer_seguro((const void *)ranura, &ptr, sizeof(ptr))) {
            log_num("  ranura ilegible ", (unsigned long long)i);
            continue;
        }
        log_num(ptr == nullptr ? "  ranura NULA " : "  ranura llena ",
                (unsigned long long)i);
    }
    // La cuenta que usa el lado de PRESENTACION, si es que es esta.
    //
    // En 0x52c80 -- la funcion que termina llamando a dlfgPresent con el indice
    // que falla -- el objeto sale de un puntero global en el RVA 0x8f1e8. Y en
    // 0x52e31 hay una funcion hermana que valida su indice contra +0x4168:
    //
    //     0x52e31  cmp  edx, dword ptr [rax + 0x4168]
    //     0x52e37  jb   ...                             ; si entra, sigue
    //
    // La ruta que revienta NO hace esa comprobacion. Si +0x4168 resulta valer
    // lo mismo que el indice pedido, entonces ese campo es de donde sale, y es
    // el que hay que alinear con lo realmente generado. Si no coincide, la
    // deduccion es mia y esta mal, y hay que buscar en otro lado.
    //
    // El RVA es de esta build (sl_dlss_g 134273). En otra no significa nada, y
    // por eso se imprime crudo y sin interpretar.
    if (modbase != nullptr) {
        const void *pp = nullptr;
        if (leer_seguro((const unsigned char *)modbase + 0x8f1e8, &pp, sizeof(pp))
            && pp != nullptr) {
            LONG cuenta = 0;
            if (leer_seguro((const unsigned char *)pp + 0x4168, &cuenta, 4))
                log_num("  [global+0x4168] (cuenta de presentacion?) ",
                        (unsigned)cuenta);
            else
                log_line("  [global+0x4168] ilegible");
        } else {
            log_line("  el global de 0x8f1e8 no se pudo leer");
        }
    }
    // La cadena de llamadas, sacada de la pila.
    //
    // Hace falta porque la lectura estatica se acabo: 0x3ec80 y 0x3e6f0 no
    // tienen NINGUN llamador localizable -- ni salto relativo, ni puntero en
    // datos, ni export. Se invocan por puntero armado en runtime.
    //
    // No es un desenrollado formal: se barre la pila y se anota todo valor que
    // caiga dentro del modulo del plugin. Entre esos estan las direcciones de
    // retorno, y con sus offsets se ve de donde vino la llamada. Sobra ruido
    // -- punteros a codigo que quedaron en la pila sin ser retornos -- asi que
    // los offsets hay que contrastarlos con el desensamblado, no creerlos.
    if (modbase != nullptr) {
        const ULONG_PTR mb = (ULONG_PTR)modbase;
        // El tamano del modulo, para saber que es "adentro".
        ULONG_PTR mfin = mb + 0x97000;          // sl.dlss_g 2.12 mide 0x97000
        log_line("  --- posibles retornos en la pila ---");
        const ULONG_PTR sp = (ULONG_PTR)ctx->Rsp;
        int puestos = 0;
        for (int i = 0; i < 160 && puestos < 14; ++i) {
            ULONG_PTR v = 0;
            if (!leer_seguro((const void *)(sp + (ULONG_PTR)i * 8), &v, 8)) continue;
            if (v <= mb || v >= mfin) continue;
            log_num("  +0x", (unsigned long long)(v - mb));
            ++puestos;
        }
        if (puestos == 0) log_line("  ninguno");
    }
    // Los registros crudos. r10 es el indice y r13 la base; el resto es para
    // identificar el objeto desde memoria viva, que es lo que el binario solo
    // no alcanza a decir.
    log_num("  rbx 0x", (unsigned long long)ctx->Rbx);
    log_num("  rcx 0x", (unsigned long long)ctx->Rcx);
    log_num("  rdx 0x", (unsigned long long)ctx->Rdx);
    log_num("  rsi 0x", (unsigned long long)ctx->Rsi);
    log_num("  rdi 0x", (unsigned long long)ctx->Rdi);
    log_num("  r12 0x", (unsigned long long)ctx->R12);
    log_num("  r14 0x", (unsigned long long)ctx->R14);
    log_num("  r15 0x", (unsigned long long)ctx->R15);
    log_line("  --- fin de la autopsia ---");
}

static LONG CALLBACK testigo_excepcion(EXCEPTION_POINTERS *info) {
    if (info == nullptr || info->ExceptionRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD c = info->ExceptionRecord->ExceptionCode;
    // Se registra SOLO lo que puede matar el proceso.
    //
    // La primera version filtraba por una mascara y dejaba pasar 0x40010006
    // (DBG_PRINTEXCEPTION_C, o sea OutputDebugString). Halo emite doce de esas
    // en los primeros catorce segundos, el tope se lleno con ruido y si hubo un
    // fallo real no quedo registrado.
    //
    // Ahora la lista es blanca, no negra: solo las excepciones que terminan un
    // proceso. Cualquier cosa que no este aca se ignora.
    const bool mortal =
        c == EXCEPTION_ACCESS_VIOLATION            ||   // 0xC0000005
        c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED       ||
        c == EXCEPTION_DATATYPE_MISALIGNMENT       ||
        c == EXCEPTION_ILLEGAL_INSTRUCTION         ||
        c == EXCEPTION_IN_PAGE_ERROR               ||
        c == EXCEPTION_INT_DIVIDE_BY_ZERO          ||
        c == EXCEPTION_PRIV_INSTRUCTION            ||
        c == EXCEPTION_STACK_OVERFLOW              ||   // 0xC00000FD
        c == 0xC0000409u                           ||   // fail-fast / stack cookie
        c == 0xC0000374u                           ||   // heap corrompido
        c == 0xC000041Du;                               // excepcion en un callback
    if (!mortal) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG dichas = 0;
    if (InterlockedIncrement(&dichas) > 40) return EXCEPTION_CONTINUE_SEARCH;
    const void *dir = info->ExceptionRecord->ExceptionAddress;
    log_line("EXCEPCION ------------------------------------------");
    log_num("  codigo 0x", (unsigned long long)c);
    log_num("  direccion 0x", (unsigned long long)(ULONG_PTR)dir);
    // De que modulo es esa direccion, que es lo que hace util al numero.
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)dir, &m) && m != nullptr) {
            wchar_t name_w[MAX_PATH];
            const DWORD n = GetModuleFileNameW(m, name_w, MAX_PATH);
            if (n > 0) {
                int cut = (int)n;
                while (cut > 0 && name_w[cut-1] != L'\\') --cut;
                char a[128];
                int k = 0;
                for (int q = cut; name_w[q] != 0 && k < 120; ++q)
                    a[k++] = (char)(name_w[q] < 128 ? name_w[q] : '?');
                a[k] = 0;
                log_line("  modulo:");
                log_line(a);
                log_num("  offset en el modulo 0x",
                        (unsigned long long)((ULONG_PTR)dir - (ULONG_PTR)m));
            }
        } else {
            log_line("  modulo: NINGUNO (memoria sin modulo: puntero corrupto)");
        }
    }
    if (c == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        log_num("  operacion (0 lee, 1 escribe, 8 ejecuta) ",
                (unsigned long long)info->ExceptionRecord->ExceptionInformation[0]);
        log_num("  sobre la direccion 0x",
                (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    }
    log_num("  seleccion en curso ", (unsigned)g_force_sel);
    log_num("  cuenta pedida ", (unsigned)g_force_generated);
    log_num("  cuenta aplicada en la API ", (unsigned)g_api_applied);
    log_num("  byte vivo en el sitio ", (unsigned)g_count_live);
    // Solo para la lectura de [nulo+0x40], que es el fallo que se repite. En
    // cualquier otro los registros no significan lo mismo y el volcado seria
    // ruido con forma de dato.
    if (c == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2 &&
        info->ExceptionRecord->ExceptionInformation[0] == 0 &&
        info->ExceptionRecord->ExceptionInformation[1] == 0x40)
    {
        HMODULE mm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)dir, &mm);
        autopsia_subframes(info->ContextRecord, (const void *)mm);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
