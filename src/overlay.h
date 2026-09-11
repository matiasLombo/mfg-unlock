// The panel: a window of our own, drawn beside the game rather than inside it.
//
// Three earlier attempts drew into the game's own swap chain from a hook on
// IDXGISwapChain::Present, and every one of them eventually killed a game.
// The last and clearest was GTA V: all IDXGISwapChain instances of a class
// share one vtable, so replacing slot 8 replaced it for every swap chain in
// the process. When the game built its second one, Steam's overlay reinstalled
// its own hook, read our pointer believing it to be the original, and the two
// called each other until the stack ran out -- 0xC00000FD, thirty-two
// milliseconds after our log said `tracking 2`, with twenty-two frames of
// gameoverlayrenderer64.dll on it. Byte-detouring instead had the same failure
// by a different road, which is why it came and went between runs: whoever
// hooks first wins, and that is a race.
//
// So this does not hook rendering at all. A layered top-level window sits over
// the game and we paint it ourselves, into a plain 32-bit bitmap, on our own
// thread. There is no device, no queue, no fence, no command list and no
// shader -- the panel is solid rectangles and a 5x7 font, and the CPU draws
// those faster than it takes to hand them to a GPU. Nothing we do here can
// reach the game's frame: the worst failure available is that our own window
// does not appear.
//
// The cost is exclusive fullscreen, where a layered window is not composited.
// Every game this runs on defaults to borderless.

#pragma once
#include <windows.h>

// ---- the shared state, read and written by proxy.cpp ---------------------
//
// 0 AUTO -- leave whatever the game asked for
// 1 OFF  -- DLSSGMode::eOff
// 2..4   -- eOn with 1, 2 or 3 generated frames, i.e. 2x, 3x, 4x
// 5 DYNAMIC -- eDynamic, with a frame-rate target
extern volatile LONG g_force_sel;
// M4: 1 mientras el panel tiene que preguntar si se sustituye el Streamline del
// juego. Lo pone proxy.cpp cuando el veredicto guardado es ROJO y todavia no hay
// respuesta. Cambiar que binarios corre el juego de alguien no es una decision
// que el dll pueda tomar solo.
extern volatile LONG g_pide_permiso;
extern volatile LONG g_force_generated;
extern volatile LONG g_last_seen_generated;
extern volatile LONG g_dyn_target;
extern bool g_dynamic_known;
// What the plugin said it accepts, in generated frames. Zero means it has not
// answered yet -- which is not the same as "nothing", so nothing is greyed on
// the strength of it.
extern volatile LONG g_frames_max;
extern bool g_ov_enabled;
static void log_line(const char *text);
static void log_num(const char *label, unsigned long long v);


// ---- font ---------------------------------------------------------------

struct Glyph { char c; unsigned char col[5]; };
static const Glyph kFont[] = {
    {' ',{0x00,0x00,0x00,0x00,0x00}}, {'0',{0x3E,0x51,0x49,0x45,0x3E}},
    // Flechas para el HUD: en DYNAMIC dicen si el multiplicador subio o
    // bajo desde el refresco anterior. Columnas de izquierda a derecha,
    // bit 0 arriba, igual que el resto de la fuente.
    {'^',{0x10,0x08,0x04,0x08,0x10}}, {'v',{0x04,0x08,0x10,0x08,0x04}},
    {'/',{0x20,0x10,0x08,0x04,0x02}}, {'>',{0x00,0x41,0x22,0x14,0x08}},
    {'1',{0x00,0x42,0x7F,0x40,0x00}}, {'2',{0x42,0x61,0x51,0x49,0x46}},
    {'3',{0x21,0x41,0x45,0x4B,0x31}}, {'4',{0x18,0x14,0x12,0x7F,0x10}},
    {'5',{0x27,0x45,0x45,0x45,0x39}}, {'6',{0x3C,0x4A,0x49,0x49,0x30}},
    {'7',{0x01,0x71,0x09,0x05,0x03}}, {'8',{0x36,0x49,0x49,0x49,0x36}},
    {'9',{0x06,0x49,0x49,0x29,0x1E}}, {'A',{0x7E,0x11,0x11,0x11,0x7E}},
    {'B',{0x7F,0x49,0x49,0x49,0x36}}, {'C',{0x3E,0x41,0x41,0x41,0x22}},
    {'D',{0x7F,0x41,0x41,0x22,0x1C}}, {'E',{0x7F,0x49,0x49,0x49,0x41}},
    {'F',{0x7F,0x09,0x09,0x09,0x01}}, {'G',{0x3E,0x41,0x49,0x49,0x7A}},
    {'H',{0x7F,0x08,0x08,0x08,0x7F}}, {'I',{0x00,0x41,0x7F,0x41,0x00}},
    {'K',{0x7F,0x08,0x14,0x22,0x41}}, {'L',{0x7F,0x40,0x40,0x40,0x40}},
    {'M',{0x7F,0x02,0x0C,0x02,0x7F}}, {'N',{0x7F,0x04,0x08,0x10,0x7F}},
    {'O',{0x3E,0x41,0x41,0x41,0x3E}}, {'P',{0x7F,0x09,0x09,0x09,0x06}},
    {'R',{0x7F,0x09,0x19,0x29,0x46}}, {'S',{0x46,0x49,0x49,0x49,0x31}},
    {'T',{0x01,0x01,0x7F,0x01,0x01}}, {'U',{0x3F,0x40,0x40,0x40,0x3F}},
    {'V',{0x1F,0x20,0x40,0x20,0x1F}}, {'X',{0x63,0x14,0x08,0x14,0x63}},
    {'Y',{0x07,0x08,0x70,0x08,0x07}}, {'-',{0x08,0x08,0x08,0x08,0x08}},
    {':',{0x00,0x36,0x36,0x00,0x00}}, {'.',{0x00,0x60,0x60,0x00,0x00}},
};

// ---- geometry -----------------------------------------------------------
//
// The window is the panel, so panel coordinates and window coordinates are the
// same thing and the pointer cannot wander off into dead space.

static const int kPanW = 340;
static const int kPad = 14;
static const int kHdrH = 42;
static const int kBoxH = 26, kBoxGap = 6;
// Nueve filas internas, ocho visibles. AUTO sigue existiendo con su indice 0
// para no cambiarle el numero a ninguna de las otras -- siete lugares del
// proxy comparan contra kSelDynamic y kSelMaxFixed por valor -- pero no se
// dibuja ni se puede elegir. La fila 8 es DYNAMIC, grisada, reservada para la
// reimplementacion.
static const int kPanRows = 9;
static const int kFirstRow = 1;                    // 0 es AUTO, oculta
static const int kVisRows = kPanRows - kFirstRow;
static const int kRowH = 22, kListPad = 6;
static const int kListH = kVisRows * kRowH + 2 * kListPad;
static const int kGap = 8;
static const int kTgtH = 80;   // el maximo: DYNAMIC lleva slider, CUSTOM no
static const int kFootH = 38;
static const int kListTop = kHdrH + 2 * (kBoxH + kBoxGap) + 2;
static const int kTgtTop = kListTop + kListH + kGap;
static const int kPanHMax = kTgtTop + kTgtH + kGap + kFootH;

static const int kHotSlider = 100, kHotValue = 101;
static const int kHotHud = 102;          // el casillero del pie

// El HUD es una segunda ventana, no un dibujo dentro del swapchain del juego.
// Misma receta que el panel, que es la unica que en este proyecto nunca rompio
// el render: capa propia, sin foco, transparente a los clicks.
// 460 y no 340: en DYNAMIC la linea lleva el multiplicador vivo, la flecha y
// el rango recorrido, y con 340 se montaba sobre la latencia de la derecha.
static const int kHudW = 460, kHudH = 30;
static unsigned *g_hud_px = nullptr;
static HWND g_hud_hwnd = nullptr;
static HDC  g_hud_dc = nullptr;
static HBITMAP g_hud_bmp = nullptr;
static bool g_hud_open = false;          // si la ventana esta creada y mostrada

// The multiplier, times a hundred: 150 is 1.5x. A multiplier and not a target
// frame rate, which is what this asked for at first and what made it behave
// badly -- with a base swinging between 66 and 114 fps, a target of 100 sits
// in the middle of that, so generation was unnecessary half the time and
// insufficient the other half. Multiplying whatever the base happens to be has
// no such middle: 1.5x is 1.5x at 66 and at 114.
// From 2.0x up. Below that the cadence needs batches that generate nothing,
// and a batch generating nothing breaks presentation: the render loop spins
// free -- 128 tokens a second against a base of 70 -- while what reaches the
// screen collapses to 30. The frames are made and thrown away. Patching the
// count, the loop bound and the zero-count validation all worked and none of
// them touch that, because it is downstream of every one of them.
//
// Offering 1.5x would be offering 30 fps, so it is not offered.
// Quarter steps through the range that works, coarser above it.
//
// The old list jumped 2.00 to 2.50 to 3.00, so the whole point of this -- the
// values between the integers -- was three positions on a slider. 2.10x and
// 2.90x both measured within 1% of target, so the resolution is real and the
// panel was hiding it.
//
// Below 2.00 is still offered because it is what a 40 fps base wants, but it is
// the range where the cadence has to switch generation off and on, and that is
// the part still not measured properly.
static const int kStops[] = { 100, 125, 150, 175,
                              200, 225, 250, 275, 300, 325, 350, 375, 400,
                              450, 500, 550, 600 };
static const int kNStops = (int)(sizeof(kStops) / sizeof(kStops[0]));
// El piso baja de 200 a 150. Medido en el banco, base fijada en 59 por el slow
// frame, 45-47 ventanas por punto, distribucion por ventana con PresentCount:
//
//   pedido  ratio  ventanas en el valor  cambios  reservas  perdidas  latencia media  swing
//   1.25    1.22        45 de 47          148       223        16       10.2 ms      8.3 ms
//   1.50    1.47        36 de 47          147       223         0       12.6 ms      4.5 ms
//   1.75    1.73        46 de 47          146       223         0       14.1 ms      4.0 ms
//   2.00    2.00        47 de 47            1       223         0       13.9 ms      3.6 ms
//
// Lo que se creia y era falso: que abajo de 2.0x prender y apagar la generacion
// costaria mas que cambiar de cuenta. Cuesta lo mismo -- los ~147 cambios salen
// enteros del ciclo de 16 ms, y las reservas se quedan en 223, el piso del
// entero fijo. Alternar 0/1 reserva MENOS que alternar 1/2 (272 a 2.50x): la
// reserva sigue el tamano de la cuenta, no los cambios.
//
// Por que 150 y no 110: 1.25x es el unico punto que tira presentaciones (16, y
// sl.log no habia registrado ninguna en ninguna otra configuracion) y su swing
// es el doble que el de cualquier otro. Ahi el estado bajo ocupa el 75% del
// ciclo y la mayoria de los frames no generan nada, que es de donde sale su
// latencia baja -- no es un frame mas rapido, son dos poblaciones.
//
// Y lo que NO justifica esto: la latencia. 1.50x y 1.75x caen dentro del ruido
// entre corridas (siete corridas de 2.50x dan medias de 12.4 a 15.0). El unico
// punto claramente por debajo es 1.25x, que es el que no se ofrece. Lo que
// sub-2.0x da de verdad es un escalon de fps entre apagado y 2x, y menos
// interpolacion para quien vea artefactos en 2x. Eso ultimo no lo mide el banco.
// El piso vuelve a 200. Se bajo a 150 con la tabla de arriba, que sigue siendo
// cierta -- 1.50x entrega 1.47 sin perder una sola presentacion, sin costar
// reservas ni reconfiguraciones extra -- y aun asi en GTA V no se vio bien ni
// fluido. El banco mide el ratio, la latencia y las perdidas; no mide como se
// ve, y esa era la unica razon que le quedaba a sub-2.0x. Sin esa razon no hay
// opcion que ofrecer.
//
// Para retomarlo, si se retoma: la sospecha es que con cuenta 0 la mitad de los
// frames no llevan interpolacion, asi que la cadencia mezcla dos texturas de
// movimiento distintas. Eso se atacaria con la distribucion, no con el ratio, y
// el banco no lo puede juzgar.
static const int kMinCustom = 200, kMaxCustom = 600;

// Objetivos de DYNAMIC, en fps presentados. El cero es AUTO y significa el
// refresh del monitor, que es lo que el controlador usa cuando no se le da un
// numero. Paradas y no un continuo: el objetivo se elige entre valores que
// significan algo -- 60, 120, el refresh -- y no en 137.
static const int kFpsStops[] = { 0, 60, 72, 90, 100, 120, 144, 165, 180, 200, 240 };
static const int kNFps = (int)(sizeof(kFpsStops) / sizeof(kFpsStops[0]));

static int ov_fps_index(int fps) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < kNFps; ++i) {
        const int d = kFpsStops[i] > fps ? kFpsStops[i] - fps : fps - kFpsStops[i];
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static unsigned g_ov_frame = 0;

static char g_ov_edit[5] = { 0, 0, 0, 0, 0 };
static int  g_ov_editlen = 0;

// Index into this is what g_force_sel holds. kSelDynamic replaces a literal 5
// that had been written out in seven places; adding two rows in the middle
// would have needed every one of them found and changed by hand, and the ones
// missed would each have been a silent wrong branch.
// Fijados por valor a proposito: derivarlos de kPanRows los movia al agregar
// la fila nueva, y eso le habria cambiado el significado a cada comparacion
// del proxy sin un solo error de compilacion.
static const int kSelDynamic = 7;                  // la fila CUSTOM
static const int kSelDynFuture = 8;                // DYNAMIC, seleccionable
// Decia "grisada" y dejo de ser cierto cuando DYNAMIC paso a ser nuestro:
// en ov_build_panel su fila es la unica con avail fijo en true.
static const int kSelMaxFixed = 6;                 // la ultima de 2X..6X
static const char *kRowName[kPanRows] = { "AUTO", "OFF", "2X", "3X", "4X",
                                          "5X", "6X", "CUSTOM", "DYNAMIC" };

// ---- the pixel buffer ---------------------------------------------------

static HWND g_ov_hwnd = nullptr;          // ours, not the game's
static HWND g_ov_game = nullptr;          // the game's, for positioning
static HDC  g_ov_dc = nullptr;
static HBITMAP g_ov_bmp = nullptr;
static unsigned *g_ov_px = nullptr;       // top-down BGRA, premultiplied
static int g_ov_h_cur = kPanHMax;

// UpdateLayeredWindow wants premultiplied alpha: a half-transparent red is
// (0x80,0,0) at alpha 0x80, not (0xFF,0,0). Getting this wrong shows as a
// bright halo around everything rather than as an error.
static inline unsigned ov_argb(float r, float g, float b, float a) {
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    const unsigned A = (unsigned)(a * 255.0f + 0.5f);
    const unsigned R = (unsigned)(r * a * 255.0f + 0.5f);
    const unsigned G = (unsigned)(g * a * 255.0f + 0.5f);
    const unsigned B = (unsigned)(b * a * 255.0f + 0.5f);
    return (A << 24) | (R << 16) | (G << 8) | B;
}

// El destino es elegible para que el HUD reuse estas primitivas tal cual. Sin
// esto habria que duplicar ov_rect, ov_text y la fuente entera, y dos copias de
// un dibujado divergen calladas.
static unsigned *g_dst_px = nullptr;
static int g_dst_w = 0, g_dst_h = 0;

static void ov_target(unsigned *px, int w, int h) {
    g_dst_px = px; g_dst_w = w; g_dst_h = h;
}

static void ov_rect(float fx, float fy, float fw, float fh,
                    float r, float g, float b, float a) {
    if (g_dst_px == nullptr) return;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = (int)(fx + fw), y1 = (int)(fy + fh);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_dst_w) x1 = g_dst_w;
    if (y1 > g_dst_h) y1 = g_dst_h;
    const unsigned c = ov_argb(r, g, b, a);
    for (int y = y0; y < y1; ++y) {
        unsigned *row = g_dst_px + (size_t)y * g_dst_w;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

static void ov_text(const char *s, float x, float y, float px,
                    float r, float g, float b) {
    float cx = x;
    for (const char *p = s; *p != 0; ++p) {
        for (const Glyph &gl : kFont) {
            if (gl.c != *p) continue;
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (gl.col[col] & (1 << row))
                        ov_rect(cx + col * px, y + row * px, px, px, r, g, b, 1.0f);
            break;
        }
        cx += 6.0f * px;
    }
}

static void ov_text_right(const char *s, float right, float y, float px,
                          float r, float g, float b) {
    int n = 0;
    for (const char *q = s; *q != 0; ++q) ++n;
    ov_text(s, right - n * 6.0f * px, y, px, r, g, b);
}

// A one-pixel border, four rects.
static void ov_frame(float x, float y, float w, float h,
                     float r, float g, float b, float a) {
    ov_rect(x, y, w, 1.0f, r, g, b, a);
    ov_rect(x, y + h - 1.0f, w, 1.0f, r, g, b, a);
    ov_rect(x, y, 1.0f, h, r, g, b, a);
    ov_rect(x + w - 1.0f, y, 1.0f, h, r, g, b, a);
}

// ---- layout, shared by the drawing and the hit test ---------------------

static bool ov_target_shown(void) {
    return g_force_sel == kSelDynamic || g_force_sel == kSelDynFuture;
}

// CUSTOM solo necesita la caja del numero; DYNAMIC lleva ademas el slider del
// objetivo, asi que el panel crece solo cuando esa fila esta elegida.
static int ov_tgt_h(void) {
    return g_force_sel == kSelDynFuture ? kTgtH : 46;
}

static const int kPermisoH = 40;

static int ov_panel_h(void) {
    // La franja de la pregunta se SUMA abajo: asi no se mueve nada de lo que ya
    // estaba, que es lo unico prudente cuando no se puede ver el resultado.
    return kTgtTop + (ov_target_shown() ? ov_tgt_h() + kGap : 0) + kFootH
         + (g_pide_permiso ? kPermisoH : 0);
}

static void ov_row_rect(int i, float *rx, float *ry, float *rw, float *rh) {
    *rx = (float)(kPad + 4);
    *ry = (float)(kListTop + kListPad + (i - kFirstRow) * kRowH);
    *rw = (float)(kPanW - 2 * kPad - 8);
    *rh = (float)(kRowH - 2);
}

static void ov_track_rect(float *x0, float *x1, float *y) {
    *x0 = (float)(kPad + 18);
    *x1 = (float)(kPanW - kPad - 18);
    *y  = (float)(kTgtTop + 48);
}

static void ov_hud_box(float *x, float *y, float *w, float *h) {
    *w = 16.0f;
    *h = 16.0f;
    *x = (float)(kPanW - kPad - 60);
    *y = (float)(ov_panel_h() - kFootH) + 11.0f;
}

static void ov_value_rect(float *x, float *y, float *w, float *h) {
    *w = 96.0f;
    *h = 26.0f;
    *x = (float)(kPanW - kPad - 12) - *w;
    *y = (float)(kTgtTop + 10);
}

static int ov_stop_index(int fps) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < kNStops; ++i) {
        const int d = kStops[i] > fps ? kStops[i] - fps : fps - kStops[i];
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// The stop nearest a pointer x, so dragging lands on a real value rather than
// on whatever pixel the hand stopped at.
// Que parada de fps cae bajo el puntero.
static int ov_fps_at(float mx) {
    float x0, x1, ty;
    ov_track_rect(&x0, &x1, &ty);
    float t = (mx - x0) / (x1 - x0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int i = (int)(t * (float)(kNFps - 1) + 0.5f);
    if (i < 0) i = 0;
    if (i >= kNFps) i = kNFps - 1;
    return i;
}

static int ov_stop_at(float mx) {
    float x0, x1, ty;
    ov_track_rect(&x0, &x1, &ty);
    float t = (mx - x0) / (x1 - x0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int i = (int)(t * (float)(kNStops - 1) + 0.5f);
    if (i < 0) i = 0;
    if (i >= kNStops) i = kNStops - 1;
    return i;
}

static void ov_update_pointer(void) {
    g_ov_hot = -1;
    if (ov_target_shown()) {
        float vx, vy, vw, vh;
        ov_value_rect(&vx, &vy, &vw, &vh);
        if (g_ov_mx >= vx && g_ov_mx < vx + vw && g_ov_my >= vy && g_ov_my < vy + vh) {
            g_ov_hot = kHotValue;
            return;
        }
    }
    if (g_force_sel == kSelDynFuture) {
        float x0, x1, ty;
        ov_track_rect(&x0, &x1, &ty);
        // Banda generosa: la pista tiene cuatro pixeles de alto y nadie le
        // acierta a eso moviendo el mouse con la mano.
        if (g_ov_mx >= x0 - 10.0f && g_ov_mx <= x1 + 10.0f &&
            g_ov_my >= ty - 12.0f && g_ov_my <= ty + 14.0f) {
            g_ov_hot = kHotSlider;
            return;
        }
    }
    {
        float bx, by, bw, bh;
        ov_hud_box(&bx, &by, &bw, &bh);
        // Banda generosa: el casillero mide 16 px y se apunta con el mouse.
        if (g_ov_mx >= bx - 6.0f && g_ov_mx <= bx + bw + 46.0f &&
            g_ov_my >= by - 6.0f && g_ov_my <= by + bh + 6.0f) {
            g_ov_hot = kHotHud;
            return;
        }
    }
    for (int i = kFirstRow; i < kPanRows; ++i) {
        float rx, ry, rw, rh;
        ov_row_rect(i, &rx, &ry, &rw, &rh);
        if (g_ov_mx >= rx && g_ov_mx < rx + rw && g_ov_my >= ry && g_ov_my < ry + rh) {
            g_ov_hot = i;
            break;
        }
    }
}

// ---- typing -------------------------------------------------------------

static void ov_edit_begin(void) {
    g_ov_editing = true;
    g_ov_editlen = 0;
    g_ov_edit[0] = 0;
}

// Acepta digitos y un separador. El buffer llega a cuatro para que entre
// "2.75" completo; el quinto byte es el terminador.
static void ov_edit_digit(char c) {
    if (!g_ov_editing || g_ov_editlen >= 4) return;
    if (c == ',') c = '.';
    if (c == '.') {
        // Un solo separador, y no como primer caracter.
        if (g_ov_editlen == 0) return;
        for (int i = 0; i < g_ov_editlen; ++i)
            if (g_ov_edit[i] == '.') return;
    }
    g_ov_edit[g_ov_editlen++] = c;
    g_ov_edit[g_ov_editlen] = 0;
}

static void ov_edit_back(void) {
    if (!g_ov_editing || g_ov_editlen == 0) return;
    g_ov_edit[--g_ov_editlen] = 0;
}

// The value to apply, or -1 if nothing should change. Clamps rather than
// refusing: below the lowest stop means AUTO, above the highest is the highest.
static int ov_edit_commit(void) {
    if (!g_ov_editing) return -1;
    g_ov_editing = false;
    if (g_ov_editlen == 0) return -1;
    // Se lee como se escribe: "3" es 3.00 y "2.75" es 2.75. Antes se
    // interpretaba como centesimas, asi que habia que teclear 275 para
    // pedir 2.75 y cualquier entrada corta caia al piso: escribir 3 daba
    // 2.00, que es el bug que se reporto.
    int whole = 0, frac = 0, fdig = 0;
    bool after = false;
    for (int i = 0; i < g_ov_editlen; ++i) {
        const char ch = g_ov_edit[i];
        if (ch == '.') { after = true; continue; }
        if (ch < '0' || ch > '9') continue;
        if (!after) whole = whole * 10 + (ch - '0');
        else if (fdig < 2) { frac = frac * 10 + (ch - '0'); ++fdig; }
    }
    while (fdig < 2) { frac *= 10; ++fdig; }   // "2.5" es 2.50, no 2.05
    int v = whole * 100 + frac;
    // CUSTOM va de 2.00 a 6.00. Bajo a 1.50 un rato y volvio: los
    // fraccionarios de abajo se midieron: 1.50x entrega 1.47 sin perder ni una
    // presentacion, y 1.25x pierde 16, que es por que el piso es 1.50 y no 1.10.
    // La tabla completa esta al lado de kMinCustom. El techo es el limite
    // estructural del plugin, no una preferencia.
    if (v < kMinCustom) v = kMinCustom;
    if (v > kMaxCustom) v = kMaxCustom;
    return v;
}

static void ov_edit_cancel(void) {
    g_ov_editing = false;
    g_ov_editlen = 0;
    g_ov_edit[0] = 0;
}

// ---- drawing ------------------------------------------------------------

static void ov_num(char *out, int v) {
    int k = 0, n = 0, d[5];
    if (v <= 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0 && n < 5) { d[n++] = v % 10; v /= 10; }
    while (n > 0) out[k++] = (char)('0' + d[--n]);
    out[k] = 0;
}

static void ov_build_panel(void) {
    ++g_ov_frame;
    ov_target(g_ov_px, kPanW, kPanHMax);
    const float px = 2.0f;
    const int h = ov_panel_h();
    const float in = (float)kPad, inw = (float)(kPanW - 2 * kPad);

    // Clear to fully transparent first: everything outside the panel must have
    // alpha zero or the window shows as a black slab over the game.
    for (int y = 0; y < kPanHMax; ++y) {
        unsigned *row = g_ov_px + (size_t)y * kPanW;
        for (int x = 0; x < kPanW; ++x) row[x] = 0;
    }

    ov_rect(0, 0, (float)kPanW, (float)h, 0.05f, 0.06f, 0.07f, 0.94f);
    ov_frame(0, 0, (float)kPanW, (float)h, 0.20f, 0.55f, 0.25f, 0.55f);
    ov_rect(0, 0, (float)kPanW, 3.0f, 0.35f, 0.88f, 0.38f, 1.0f);

    // M4: la pregunta va en una franja propia al pie, agregada al alto. Se
    // dibuja aca, temprano, para que nada de lo de abajo la pise.
    if (g_pide_permiso) {
        const float fy = (float)(h - kPermisoH);
        ov_rect(1.0f, fy, (float)(kPanW - 2), (float)(kPermisoH - 1),
                0.30f, 0.18f, 0.04f, 0.96f);
        ov_rect(1.0f, fy, (float)(kPanW - 2), 2.0f, 0.95f, 0.62f, 0.15f, 1.0f);
        ov_text("ESTE JUEGO NO PUEDE USAR MFG ASI",
                (float)kPad, fy + 7.0f, 1.5f, 0.98f, 0.80f, 0.35f);
        ov_text("F7 USAR REEMPLAZO    F8 DEJARLO COMO ESTA",
                (float)kPad, fy + 22.0f, 1.4f, 0.85f, 0.70f, 0.45f);
    }

    ov_text("MFG UNLOCK", in, 16.0f, 2.4f, 0.42f, 0.95f, 0.45f);
    ov_text("///", (float)(kPanW - kPad - 24), 18.0f, 1.6f, 0.25f, 0.60f, 0.30f);

    const int asked = (int)g_last_seen_generated;
    const int sel   = (int)g_force_sel;

    {
        char v[8];
        v[0] = (char)('0' + (asked + 1 > 9 ? 9 : asked + 1)); v[1] = 'X'; v[2] = 0;
        const float by = (float)kHdrH;
        ov_frame(in, by, inw, (float)kBoxH, 0.22f, 0.26f, 0.30f, 1.0f);
        ov_text("GAME ASKS", in + 12.0f, by + 9.0f, px, 0.60f, 0.62f, 0.66f);
        ov_text_right(v, in + inw - 12.0f, by + 9.0f, px, 0.80f, 0.82f, 0.86f);

        const float by2 = by + kBoxH + kBoxGap;
        ov_frame(in, by2, inw, (float)kBoxH, 0.22f, 0.26f, 0.30f, 1.0f);
        ov_text("OVERRIDE", in + 12.0f, by2 + 9.0f, px, 0.60f, 0.62f, 0.66f);
        ov_text_right(kRowName[sel], in + inw - 12.0f, by2 + 9.0f, px,
                      sel > 0 ? 0.42f : 0.60f,
                      sel > 0 ? 0.95f : 0.62f,
                      sel > 0 ? 0.45f : 0.66f);
    }

    {
        ov_frame(in, (float)kListTop, inw, (float)kListH, 0.22f, 0.26f, 0.30f, 1.0f);
        for (int i = kFirstRow; i < kPanRows; ++i) {
            float rx, ry, rw, rh;
            ov_row_rect(i, &rx, &ry, &rw, &rh);
            // A row above what the plugin accepts is not a worse setting, it
            // is a rejected one: every Streamline back to 2.7.32 refuses a
            // frame count over its maximum outright, and only 2.11.1 and newer
            // soften that into a clamp. Offering 6X where it will be refused
            // would stop frame generation with nothing to explain it.
            const bool avail = (i == kSelDynFuture) ? true
                             : (i == kSelDynamic) ? true
                             : (i < 2 || g_frames_max == 0 ||
                                (LONG)(i - 1) <= g_frames_max);
            const bool on  = (sel == i) && avail;
            const bool hot = (g_ov_hot == i) && avail;
            if (on) {
                // Filled solid, dark text on it. A selection you have to
                // compare against its neighbours to find is not a selection.
                ov_rect(rx, ry, rw, rh, 0.36f, 0.92f, 0.40f, 1.0f);
                ov_rect(rx - 6.0f, ry, 4.0f, rh, 0.42f, 1.00f, 0.45f, 1.0f);
                ov_text(kRowName[i], rx + 12.0f, ry + 7.0f, px, 0.04f, 0.10f, 0.05f);
                ov_text(">", rx + rw - 16.0f, ry + 7.0f, px, 0.04f, 0.10f, 0.05f);
            } else {
                if (hot) ov_rect(rx, ry, rw, rh, 0.13f, 0.16f, 0.19f, 1.0f);
                const float d = avail ? 1.0f : 0.34f;
                const float c = hot ? 0.95f : 0.72f;
                ov_text(kRowName[i], rx + 12.0f, ry + 7.0f, px,
                        c * d, c * d, (c + 0.03f) * d);
            }
        }
    }

    // Only while DYNAMIC is the selection: the panel is shorter without it
    // rather than carrying a control that does nothing.
    if (ov_target_shown()) {
        const float ty = (float)kTgtTop;
        const bool dyn = g_force_sel == kSelDynFuture;
        ov_frame(in, ty, inw, (float)ov_tgt_h(), 0.22f, 0.26f, 0.30f, 1.0f);
        ov_text(dyn ? "TARGET FPS" : "MULTIPLIER", in + 12.0f, ty + 17.0f, px,
                0.60f, 0.62f, 0.66f);

        char t[8];
        if (dyn) {
            // AUTO cuando es cero: el controlador toma el refresh.
            const int v = (int)g_dyn_fps;
            int k = 0;
            if (v <= 0) { t[k++] = 'A'; t[k++] = 'U'; t[k++] = 'T'; t[k++] = 'O'; }
            else {
                if (v >= 100) t[k++] = (char)('0' + (v / 100) % 10);
                if (v >= 10)  t[k++] = (char)('0' + (v / 10) % 10);
                t[k++] = (char)('0' + v % 10);
            }
            t[k] = 0;
        } else if (g_ov_editing) {
            int k = 0;
            for (; k < g_ov_editlen; ++k) t[k] = g_ov_edit[k];
            t[k] = 0;
        } else {
            // Shown the way it is meant: 150 held internally reads as 1.50X.
            const int v = (int)(g_dyn_target < kMinCustom ? kMinCustom
                                                          : g_dyn_target);
            int k = 0;
            t[k++] = (char)('0' + (v / 100) % 10);
            t[k++] = '.';
            t[k++] = (char)('0' + (v / 10) % 10);
            t[k++] = (char)('0' + v % 10);
            t[k++] = 'X';
            t[k] = 0;
        }

        float vx, vy, vw, vh;
        ov_value_rect(&vx, &vy, &vw, &vh);
        const bool vhot = g_ov_hot == kHotValue;
        ov_rect(vx, vy, vw, vh, 0.03f, 0.05f, 0.04f, 1.0f);
        ov_frame(vx, vy, vw, vh,
                 g_ov_editing ? 0.42f : (vhot ? 0.35f : 0.24f),
                 g_ov_editing ? 1.00f : (vhot ? 0.62f : 0.30f),
                 g_ov_editing ? 0.45f : (vhot ? 0.38f : 0.34f), 1.0f);
        ov_text(t, vx + 10.0f, vy + 9.0f, px, 0.55f, 0.98f, 0.58f);
        if (g_ov_editing && ((g_ov_frame / 15) & 1) == 0) {
            int n = 0;
            while (t[n] != 0) ++n;
            ov_rect(vx + 10.0f + n * 6.0f * px, vy + 7.0f, 2.0f, 12.0f,
                    0.55f, 0.98f, 0.58f, 1.0f);
        }

        // CUSTOM se escribe: un slider de doce paradas no podia expresar 2.35.
        // DYNAMIC si lleva slider, porque su objetivo son fps y los valores que
        // importan son pocos y conocidos.
        if (dyn) {
            float x0, x1, sy;
            ov_track_rect(&x0, &x1, &sy);
            const int si = ov_fps_index((int)g_dyn_fps);
            const float fr = (float)si / (float)(kNFps - 1);
            const float hx = x0 + (x1 - x0) * fr;
            ov_rect(x0, sy, x1 - x0, 4.0f, 0.16f, 0.20f, 0.18f, 1.0f);
            ov_rect(x0, sy, hx - x0, 4.0f, 0.36f, 0.92f, 0.40f, 1.0f);
            const bool shot = g_ov_hot == kHotSlider;
            ov_rect(hx - 7.0f, sy - 7.0f, 14.0f, 18.0f,
                    shot ? 0.50f : 0.36f, shot ? 1.00f : 0.92f,
                    shot ? 0.52f : 0.40f, 1.0f);
            for (int q = 0; q < kNFps; ++q) {
                const float tx = x0 + (x1 - x0) * ((float)q / (float)(kNFps - 1));
                ov_rect(tx, sy + 9.0f, 1.0f, q == si ? 6.0f : 3.0f,
                        q == si ? 0.45f : 0.24f, q == si ? 0.92f : 0.28f,
                        q == si ? 0.48f : 0.30f, 1.0f);
            }
            ov_text("AUTO", x0 - 2.0f, sy + 19.0f, 1.5f, 0.44f, 0.46f, 0.50f);
            ov_text("240", x1 - 26.0f, sy + 19.0f, 1.5f, 0.44f, 0.46f, 0.50f);
        }
    }

    {
        const float fy = (float)(h - kFootH);
        ov_rect(in, fy, inw, 1.0f, 0.20f, 0.22f, 0.26f, 1.0f);
        ov_frame(in, fy + 10.0f, 34.0f, 18.0f, 0.35f, 0.85f, 0.38f, 1.0f);
        ov_text("ESC", in + 6.0f, fy + 15.0f, 1.6f, 0.45f, 0.95f, 0.48f);
        ov_text(g_ov_editing ? "CANCELS" : "CLOSES", in + 44.0f, fy + 15.0f, 1.6f,
                0.50f, 0.52f, 0.56f);

        // El casillero del HUD. Vive en el pie y no en la lista de modos porque
        // no es un modo: no cambia nada de lo que la dll hace, solo si se ve.
        float bx, by, bw, bh;
        ov_hud_box(&bx, &by, &bw, &bh);
        const bool bhot = g_ov_hot == kHotHud;
        ov_frame(bx, by, bw, bh,
                 bhot ? 0.45f : 0.30f, bhot ? 0.95f : 0.60f,
                 bhot ? 0.48f : 0.34f, 1.0f);
        if (g_hud_on)
            ov_rect(bx + 4.0f, by + 4.0f, bw - 8.0f, bh - 8.0f,
                    0.36f, 0.92f, 0.40f, 1.0f);
        ov_text("HUD", bx + bw + 8.0f, fy + 15.0f, 1.6f,
                g_hud_on ? 0.45f : 0.40f, g_hud_on ? 0.95f : 0.42f,
                g_hud_on ? 0.48f : 0.46f);
    }

    // Pointer last, so it is never painted over.
    ov_rect(g_ov_mx, g_ov_my, 2.0f, 14.0f, 0.0f, 0.0f, 0.0f, 0.9f);
    ov_rect(g_ov_mx, g_ov_my, 14.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.9f);
    ov_rect(g_ov_mx + 1, g_ov_my + 1, 1.0f, 11.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    ov_rect(g_ov_mx + 1, g_ov_my + 1, 11.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);

    g_ov_h_cur = h;
}

// ---- the pointer, from the mouse device ---------------------------------
//
// Raw input reports device deltas, so the cursor clipping every one of these
// games does -- which is what defeated four earlier attempts to read the
// cursor position -- cannot touch it. The registration is saved and handed
// back on close, since a process holds only one per device; while the panel is
// open, the game losing the mouse is the input blocking we wanted anyway.

static bool g_ov_riactive = false;
static RAWINPUTDEVICE g_ov_riprev = { 0, 0, 0, nullptr };
static bool g_ov_rihadprev = false;

static void ov_raw_mouse(const RAWMOUSE *m) {
    if (!g_ov_visible) return;
    if ((m->usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
        g_ov_mx += (float)m->lLastX;
        g_ov_my += (float)m->lLastY;
    }
    if (g_ov_mx < 0) g_ov_mx = 0;
    if (g_ov_my < 0) g_ov_my = 0;
    if (g_ov_mx > (float)(kPanW - 2)) g_ov_mx = (float)(kPanW - 2);
    if (g_ov_my > (float)(ov_panel_h() - 2)) g_ov_my = (float)(ov_panel_h() - 2);
}

static void ov_raw_claim(void) {
    if (g_ov_hwnd == nullptr) return;
    if (!g_ov_riactive) {
        UINT n = 0;
        g_ov_rihadprev = false;
        // Documented to return -1 and set n on the sizing call, so an ordinary
        // "did it fail" test reads backwards here.
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        if (n != 0 && n <= 64) {
            RAWINPUTDEVICE cur[64];
            const UINT got = GetRegisteredRawInputDevices(cur, &n, sizeof(RAWINPUTDEVICE));
            if (got != (UINT)-1) {
                for (UINT i = 0; i < got; ++i) {
                    if (cur[i].usUsagePage == 0x01 && cur[i].usUsage == 0x02) {
                        g_ov_riprev = cur[i];
                        g_ov_rihadprev = true;
                        break;
                    }
                }
            }
        }
    }
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;            // generic desktop
    rid.usUsage     = 0x02;            // mouse
    rid.dwFlags     = RIDEV_INPUTSINK; // delivered without focus
    rid.hwndTarget  = g_ov_hwnd;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)) == FALSE && !g_ov_riactive)
        log_line("panel: raw mouse claim FAILED");
    g_ov_riactive = true;
}

static void ov_raw_release(void) {
    if (!g_ov_riactive) return;
    if (g_ov_rihadprev) {
        RegisterRawInputDevices(&g_ov_riprev, 1, sizeof(RAWINPUTDEVICE));
    } else {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage     = 0x02;
        rid.dwFlags     = RIDEV_REMOVE;
        rid.hwndTarget  = nullptr;     // must be null when removing
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }
    g_ov_riactive = false;
}

// ---- the window ---------------------------------------------------------

static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        UINT sz = 0;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &sz,
                            sizeof(RAWINPUTHEADER)) == 0 &&
            sz != 0 && sz <= sizeof(RAWINPUT)) {
            RAWINPUT ri;
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz,
                                sizeof(RAWINPUTHEADER)) == sz &&
                ri.header.dwType == RIM_TYPEMOUSE)
                ov_raw_mouse(&ri.data.mouse);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// The game's main window, found by enumeration rather than taken from a hook
// on swap-chain creation. Nothing here needs DXGI any more, and a window we
// only position ourselves against does not justify a hook.
static BOOL CALLBACK ov_find_game(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    const long area = (r.right - r.left) * (long)(r.bottom - r.top);
    if (area < 40000) return TRUE;                 // not the main window
    HWND *out = (HWND *)lp;
    *out = h;
    return FALSE;
}

static bool ov_make_window(void) {
    if (g_ov_hwnd != nullptr) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ov_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MfgUnlockPanel";
    RegisterClassExW(&wc);

    // NOACTIVATE so opening it never pulls focus off the game -- a borderless
    // game that loses focus can minimise. TRANSPARENT so clicks fall through
    // to the game: we read the buttons with GetAsyncKeyState, and a window
    // that swallowed them would leave the game unable to tell we had gone.
    g_ov_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"MfgUnlockPanel", L"", WS_POPUP,
        0, 0, kPanW, kPanHMax, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_ov_hwnd == nullptr) { log_line("panel: window FAILED"); return false; }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kPanW;
    bi.bmiHeader.biHeight = -kPanHMax;             // negative: top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_ov_dc = CreateCompatibleDC(screen);
    g_ov_bmp = CreateDIBSection(g_ov_dc, &bi, DIB_RGB_COLORS,
                                (void **)&g_ov_px, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (g_ov_bmp == nullptr || g_ov_px == nullptr) {
        log_line("panel: bitmap FAILED");
        DestroyWindow(g_ov_hwnd);
        g_ov_hwnd = nullptr;
        return false;
    }
    SelectObject(g_ov_dc, g_ov_bmp);
    log_line("panel: window ready (drawn beside the game, not inside it)");
    return true;
}

static void ov_present_window(void) {
    if (g_ov_hwnd == nullptr || g_ov_px == nullptr) return;

    // Follow the game's client area. Alt-tabbing, a resolution change or a
    // move all just work, because the position is read every time rather than
    // cached at creation.
    if (g_ov_game == nullptr || !IsWindow(g_ov_game))
        EnumWindows(&ov_find_game, (LPARAM)&g_ov_game);
    POINT origin = { 0, 0 };
    if (g_ov_game != nullptr) ClientToScreen(g_ov_game, &origin);

    POINT dst = { origin.x + 40, origin.y + 40 };
    SIZE  size = { kPanW, g_ov_h_cur };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_ov_hwnd, nullptr, &dst, &size, g_ov_dc, &src,
                        0, &bf, ULW_ALPHA);
}

// ---- el HUD ---------------------------------------------------------------

static bool ov_game_in_front(void);   // definida mas abajo, junto al panel


static bool hud_make_window(void) {
    if (g_hud_hwnd != nullptr) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ov_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MfgUnlockHud";
    RegisterClassExW(&wc);
    g_hud_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"MfgUnlockHud", L"", WS_POPUP,
        0, 0, kHudW, kHudH, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hud_hwnd == nullptr) { log_line("hud: window FAILED"); return false; }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kHudW;
    bi.bmiHeader.biHeight = -kHudH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_hud_dc = CreateCompatibleDC(screen);
    g_hud_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                                 (void **)&g_hud_px, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (g_hud_dc == nullptr || g_hud_bmp == nullptr || g_hud_px == nullptr) {
        log_line("hud: bitmap FAILED");
        DestroyWindow(g_hud_hwnd);
        g_hud_hwnd = nullptr;
        return false;
    }
    SelectObject(g_hud_dc, g_hud_bmp);
    return true;
}

static void hud_build(void) {
    if (g_hud_px == nullptr) return;
    ov_target(g_hud_px, kHudW, kHudH);
    for (int i = 0; i < kHudW * kHudH; ++i) g_hud_px[i] = 0;
    ov_rect(0.0f, 0.0f, (float)kHudW, (float)kHudH, 0.02f, 0.03f, 0.03f, 0.72f);
    ov_rect(0.0f, 0.0f, (float)kHudW, 1.0f, 0.36f, 0.92f, 0.40f, 0.9f);

    char line[64];
    int k = 0;
    // "57/342 FPS": la base y lo que sale a pantalla.
    //
    // La barra se dibuja solo si hay base medida. Reflex tarda unas ventanas en
    // dar una, y "0/342" se leeria como que la generacion no esta andando
    // justo cuando si lo esta.
    const int base = (int)((g_hud_base_x10 + 5) / 10);
    if (base > 0) {
        if (base >= 100) line[k++] = (char)(48 + (base / 100) % 10);
        if (base >= 10)  line[k++] = (char)(48 + (base / 10) % 10);
        line[k++] = (char)(48 + base % 10);
        line[k++] = 47;                                              // '/'
    }
    const int fps = (int)((g_hud_fps_x10 + 5) / 10);
    if (fps >= 100) line[k++] = (char)(48 + (fps / 100) % 10);
    if (fps >= 10)  line[k++] = (char)(48 + (fps / 10) % 10);
    line[k++] = (char)(48 + fps % 10);
    line[k++] = 32; line[k++] = 70; line[k++] = 80; line[k++] = 83;  // " FPS"
    line[k++] = 32; line[k++] = 45; line[k++] = 32;                  // " - "

    // El modo es el nombre de la fila, y si es CUSTOM tambien el numero escrito.
    const int sel = (int)g_force_sel;
    const char *nm = (sel >= 0 && sel < kPanRows) ? kRowName[sel] : "?";
    for (const char *q = nm; *q != 0 && k < 40; ++q) line[k++] = *q;
    // El multiplicador vivo. En CUSTOM es el que se tecleo y no se mueve; en
    // DYNAMIC lo escribe el controlador cuadro a cuadro, y ahi es el dato que
    // faltaba: la fila decia "DYNAMIC" y nunca en cuanto estaba.
    if (sel == kSelDynamic || sel == kSelDynFuture) {
        const int v = (int)g_dyn_target;
        line[k++] = 32;
        line[k++] = (char)(48 + (v / 100) % 10);
        line[k++] = 46;
        line[k++] = (char)(48 + (v / 10) % 10);
        line[k++] = (char)(48 + v % 10);

        // Y los cambios, que en DYNAMIC son la mitad de la informacion: un
        // numero solo no dice si esta quieto en 2.47 o rebotando entre 2.1 y
        // 2.8. La flecha es contra el refresco anterior y el rango es lo
        // recorrido en los ultimos seis refrescos, o sea unos tres segundos.
        //
        // Se muestra solo en DYNAMIC: en CUSTOM el valor no se mueve nunca y
        // una flecha ahi seria ruido que finge actividad.
        if (sel == kSelDynFuture) {
            static int prev = 0;
            static int ring[6] = { 0, 0, 0, 0, 0, 0 };
            static int rpos = 0, rn = 0;
            ring[rpos] = v;
            rpos = (rpos + 1) % 6;
            if (rn < 6) ++rn;
            int lo = v, hi = v;
            for (int i = 0; i < rn; ++i) {
                if (ring[i] < lo) lo = ring[i];
                if (ring[i] > hi) hi = ring[i];
            }
            // Banda muerta de 0.02 para la flecha: sin ella parpadea con el
            // ruido de un digito y deja de leerse como direccion.
            if (v > prev + 2)      line[k++] = '^';
            else if (v < prev - 2) line[k++] = 'v';
            else                   line[k++] = 32;
            prev = v;
            // El rango solo si hay algo que contar. Cuando esta clavado,
            // "2.47 2.47-2.47" es peor que nada.
            if (hi - lo > 2 && k < 46) {
                line[k++] = 32;
                line[k++] = (char)(48 + (lo / 100) % 10);
                line[k++] = 46;
                line[k++] = (char)(48 + (lo / 10) % 10);
                line[k++] = 45;                                  // '-'
                line[k++] = (char)(48 + (hi / 100) % 10);
                line[k++] = 46;
                line[k++] = (char)(48 + (hi / 10) % 10);
            }
        }
    }
    line[k] = 0;
    ov_text(line, 10.0f, 11.0f, 1.7f, 0.72f, 0.96f, 0.74f);

    // La latencia va a la derecha, y solo si hay dato: mostrar un cero
    // inventado seria peor que no mostrar nada.
    const LONG us = g_hud_lat_us;
    char lat[16];
    int j = 0;
    if (us > 0) {
        const int ms10 = (int)((us + 50) / 100);
        if (ms10 >= 1000) lat[j++] = (char)(48 + (ms10 / 1000) % 10);
        if (ms10 >= 100)  lat[j++] = (char)(48 + (ms10 / 100) % 10);
        lat[j++] = (char)(48 + (ms10 / 10) % 10);
        lat[j++] = 46;
        lat[j++] = (char)(48 + ms10 % 10);
        lat[j++] = 32; lat[j++] = 109; lat[j++] = 115;   // " ms"
    } else {
        lat[j++] = 45; lat[j++] = 45; lat[j++] = 32;
        lat[j++] = 109; lat[j++] = 115;                  // "-- ms"
    }
    lat[j] = 0;
    ov_text_right(lat, (float)(kHudW - 10), 11.0f, 1.7f, 0.60f, 0.86f, 0.62f);
}

static void hud_present(void) {
    if (g_hud_hwnd == nullptr || g_hud_px == nullptr) return;
    if (g_ov_game == nullptr || !IsWindow(g_ov_game))
        EnumWindows(&ov_find_game, (LPARAM)&g_ov_game);
    POINT origin = { 0, 0 };
    int cw = 1920;
    if (g_ov_game != nullptr) {
        ClientToScreen(g_ov_game, &origin);
        RECT rc;
        if (GetClientRect(g_ov_game, &rc)) cw = rc.right - rc.left;
    }
    POINT dst = { origin.x + (cw - kHudW) / 2, origin.y + 12 };
    SIZE  size = { kHudW, kHudH };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hud_hwnd, nullptr, &dst, &size, g_hud_dc, &src,
                        0, &bf, ULW_ALPHA);
}

// Corre en cada present, este el panel abierto o no: el HUD es independiente y
// tiene que sobrevivir a cerrar el panel con la tilde.
static void hud_tick(void) {
    const bool want = g_hud_on && ov_game_in_front();
    if (want && !g_hud_open) {
        if (!hud_make_window()) return;
        hud_build();
        hud_present();
        ShowWindow(g_hud_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hud_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_hud_open = true;
        log_line("hud: shown");
        return;
    }
    if (!want && g_hud_open) {
        if (g_hud_hwnd != nullptr) ShowWindow(g_hud_hwnd, SW_HIDE);
        g_hud_open = false;
        return;
    }
    if (!g_hud_open) return;
    // Dos refrescos por segundo: alcanza para leerlo y no compite con el render.
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 500) return;
    last = now;
    hud_build();
    hud_present();
}

static void ov_show(bool on) {
    if (on) {
        if (!ov_make_window()) return;
        // Start on the panel rather than in a corner, so the first movement is
        // visible where the player is already looking.
        g_ov_mx = kPanW * 0.5f;
        g_ov_my = (float)(kListTop + kRowH / 2);
        ov_build_panel();
        ov_present_window();
        ShowWindow(g_ov_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_ov_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ov_raw_claim();
        log_line("panel: shown");
        return;
    }
    ov_raw_release();
    ov_edit_cancel();
    if (g_ov_hwnd != nullptr) ShowWindow(g_ov_hwnd, SW_HIDE);
    log_line("panel: hidden");
}

// Called from the panel thread every tick while open.
static bool ov_game_in_front(void) {
    if (g_ov_game == nullptr) return true;      // not found yet; do not hide
    const HWND fg = GetForegroundWindow();
    if (fg == g_ov_game || fg == g_ov_hwnd) return true;
    // A game may put its own dialogs or a splash in front; anything belonging
    // to this process still counts as the game being in front.
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void ov_tick(void) {
    if (!g_ov_visible || g_ov_hwnd == nullptr) return;
    const bool front = ov_game_in_front();
    static bool shown_now = true;
    if (front != shown_now) {
        shown_now = front;
        ShowWindow(g_ov_hwnd, front ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    if (!front) return;                         // nothing to paint or to read
    ov_update_pointer();
    ov_build_panel();
    ov_present_window();
    // Re-asserted, but not every tick: re-registering hundreds of times a
    // second is a syscall for every few milliseconds of a claim nothing is
    // contesting.
    static int n = 0;
    if (--n <= 0) { n = 8; ov_raw_claim(); }
}
