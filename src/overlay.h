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

static volatile bool g_ov_visible = false;

// ---- font ---------------------------------------------------------------

struct Glyph { char c; unsigned char col[5]; };
static const Glyph kFont[] = {
    {' ',{0x00,0x00,0x00,0x00,0x00}}, {'0',{0x3E,0x51,0x49,0x45,0x3E}},
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
static const int kPanRows = 8;
static const int kRowH = 22, kListPad = 6;
static const int kListH = kPanRows * kRowH + 2 * kListPad;
static const int kGap = 8;
static const int kTgtH = 80;
static const int kFootH = 38;
static const int kListTop = kHdrH + 2 * (kBoxH + kBoxGap) + 2;
static const int kTgtTop = kListTop + kListH + kGap;
static const int kPanHMax = kTgtTop + kTgtH + kGap + kFootH;

static const int kHotSlider = 100, kHotValue = 101;

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

static float g_ov_mx = 0.0f, g_ov_my = 0.0f;
static int   g_ov_hot = -1;
static unsigned g_ov_frame = 0;

// Typing into the value box, polled like every other key here so a game that
// never delivers WM_CHAR cannot stop it.
static bool g_ov_editing = false;
static char g_ov_edit[5] = { 0, 0, 0, 0, 0 };
static int  g_ov_editlen = 0;

// Index into this is what g_force_sel holds. kSelDynamic replaces a literal 5
// that had been written out in seven places; adding two rows in the middle
// would have needed every one of them found and changed by hand, and the ones
// missed would each have been a silent wrong branch.
static const int kSelDynamic = kPanRows - 1;
static const int kSelMaxFixed = kSelDynamic - 1;    // the last of the 2X..6X rows
static const char *kRowName[kPanRows] = { "AUTO", "OFF", "2X", "3X", "4X",
                                          "5X", "6X", "DYNAMIC" };

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

static void ov_rect(float fx, float fy, float fw, float fh,
                    float r, float g, float b, float a) {
    if (g_ov_px == nullptr) return;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = (int)(fx + fw), y1 = (int)(fy + fh);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kPanW) x1 = kPanW;
    if (y1 > kPanHMax) y1 = kPanHMax;
    const unsigned c = ov_argb(r, g, b, a);
    for (int y = y0; y < y1; ++y) {
        unsigned *row = g_ov_px + (size_t)y * kPanW;
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
    return g_force_sel == kSelDynamic;
}

static int ov_panel_h(void) {
    return kTgtTop + (ov_target_shown() ? kTgtH + kGap : 0) + kFootH;
}

static void ov_row_rect(int i, float *rx, float *ry, float *rw, float *rh) {
    *rx = (float)(kPad + 4);
    *ry = (float)(kListTop + kListPad + i * kRowH);
    *rw = (float)(kPanW - 2 * kPad - 8);
    *rh = (float)(kRowH - 2);
}

static void ov_track_rect(float *x0, float *x1, float *y) {
    *x0 = (float)(kPad + 18);
    *x1 = (float)(kPanW - kPad - 18);
    *y  = (float)(kTgtTop + 48);
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
        float x0, x1, ty;
        ov_track_rect(&x0, &x1, &ty);
        // A generous band: the track is four pixels tall and nobody hits that
        // with a pointer they are moving by hand.
        if (g_ov_mx >= x0 - 10.0f && g_ov_mx <= x1 + 10.0f &&
            g_ov_my >= ty - 12.0f && g_ov_my <= ty + 14.0f) {
            g_ov_hot = kHotSlider;
            return;
        }
    }
    for (int i = 0; i < kPanRows; ++i) {
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

static void ov_edit_digit(char c) {
    if (!g_ov_editing || g_ov_editlen >= 3) return;
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
    int v = 0;
    for (int i = 0; i < g_ov_editlen; ++i) v = v * 10 + (g_ov_edit[i] - '0');
    if (v < kStops[0]) v = kStops[0];
    if (v > kStops[kNStops - 1]) v = kStops[kNStops - 1];
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
        for (int i = 0; i < kPanRows; ++i) {
            float rx, ry, rw, rh;
            ov_row_rect(i, &rx, &ry, &rw, &rh);
            // A row above what the plugin accepts is not a worse setting, it
            // is a rejected one: every Streamline back to 2.7.32 refuses a
            // frame count over its maximum outright, and only 2.11.1 and newer
            // soften that into a clamp. Offering 6X where it will be refused
            // would stop frame generation with nothing to explain it.
            const bool avail = (i == kSelDynamic) ? true
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
        ov_frame(in, ty, inw, (float)kTgtH, 0.22f, 0.26f, 0.30f, 1.0f);
        ov_text("MULTIPLIER", in + 12.0f, ty + 17.0f, px, 0.60f, 0.62f, 0.66f);

        char t[8];
        if (g_ov_editing) {
            int k = 0;
            for (; k < g_ov_editlen; ++k) t[k] = g_ov_edit[k];
            t[k] = 0;
        } else {
            // Shown the way it is meant: 150 held internally reads as 1.50X.
            const int v = (int)(g_dyn_target < 100 ? 100 : g_dyn_target);
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

        float x0, x1, sy;
        ov_track_rect(&x0, &x1, &sy);
        // The handle follows the value in force, not the half-typed digits:
        // "1" on the way to "120" would otherwise throw it to the far left.
        const int shown = ov_stop_index((int)g_dyn_target);
        const float f = (float)shown / (float)(kNStops - 1);
        const float hx = x0 + (x1 - x0) * f;
        ov_rect(x0, sy, x1 - x0, 4.0f, 0.16f, 0.20f, 0.18f, 1.0f);
        ov_rect(x0, sy, hx - x0, 4.0f, 0.36f, 0.92f, 0.40f, 1.0f);
        const bool shot = g_ov_hot == kHotSlider;
        ov_rect(hx - 7.0f, sy - 7.0f, 14.0f, 18.0f,
                shot ? 0.50f : 0.36f, shot ? 1.00f : 0.92f, shot ? 0.52f : 0.40f, 1.0f);

        // Ticks at the stops worth naming. Their positions are their real
        // ones: the spacing is even because the stops are, not because the
        // labels were placed where they looked good.
        // A faint tick at every stop, so the track reads as a ruler and the
        // handle visibly lands on one rather than anywhere it likes.
        for (int si = 0; si < kNStops; ++si) {
            const float tx = x0 + (x1 - x0) * ((float)si / (float)(kNStops - 1));
            ov_rect(tx, sy + 9.0f, 1.0f, 3.0f, 0.24f, 0.28f, 0.30f, 1.0f);
        }
        // 1.00, 2.00, 3.00, 4.00 and the top, found by value rather than by
        // index, so the list can change length without the labels lying.
        static const int kLabelValues[] = { 100, 200, 300, 400, 600 };
        int kLabelled[5];
        for (int li = 0; li < 5; ++li) {
            kLabelled[li] = kNStops - 1;
            for (int sj = 0; sj < kNStops; ++sj)
                if (kStops[sj] == kLabelValues[li]) { kLabelled[li] = sj; break; }
        }
        const float lpx = 1.5f;
        for (int i = 0; i < 5; ++i) {
            const int si = kLabelled[i];
            const float tx = x0 + (x1 - x0) * ((float)si / (float)(kNStops - 1));
            const bool cur = si == shown;
            ov_rect(tx, sy + 9.0f, 1.0f, 6.0f,
                    cur ? 0.45f : 0.34f, cur ? 0.92f : 0.38f, cur ? 0.48f : 0.42f, 1.0f);
            char lb[8];
            {
                const int v = kStops[si];
                int k = 0;
                lb[k++] = (char)('0' + (v / 100) % 10);
                lb[k++] = '.';
                lb[k++] = (char)('0' + (v / 10) % 10);
                if (v % 10 != 0) lb[k++] = (char)('0' + v % 10);
                lb[k] = 0;
            }
            int n = 0;
            while (lb[n] != 0) ++n;
            const float wpx = n * 6.0f * lpx;
            float lx = tx - wpx * 0.5f;
            if (i == 0) lx = tx;                       // first: from the tick
            if (i == 4) lx = tx - wpx;                 // last: back to it
            if (lx < (float)kPad) lx = (float)kPad;
            if (lx + wpx > (float)(kPanW - kPad)) lx = (float)(kPanW - kPad) - wpx;
            ov_text(lb, lx, sy + 19.0f, lpx,
                    cur ? 0.45f : 0.44f, cur ? 0.95f : 0.46f, cur ? 0.48f : 0.50f);
        }
    }

    {
        const float fy = (float)(h - kFootH);
        ov_rect(in, fy, inw, 1.0f, 0.20f, 0.22f, 0.26f, 1.0f);
        ov_frame(in, fy + 10.0f, 34.0f, 18.0f, 0.35f, 0.85f, 0.38f, 1.0f);
        ov_text("ESC", in + 6.0f, fy + 15.0f, 1.6f, 0.45f, 0.95f, 0.48f);
        ov_text(g_ov_editing ? "CANCELS" : "CLOSES", in + 44.0f, fy + 15.0f, 1.6f,
                0.50f, 0.52f, 0.56f);
        ov_text("///", (float)(kPanW - kPad - 24), fy + 15.0f, 1.6f,
                0.25f, 0.60f, 0.30f);
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
