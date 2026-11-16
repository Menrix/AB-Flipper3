#include "gfx.h"

#include <furi.h>
#include <string.h>

#define PAT(name, ...)                                     \
    static const char* const name##_rows[] = {__VA_ARGS__}; \
    const Pat name = {name##_rows, sizeof(name##_rows) / sizeof(name##_rows[0])};

/* ---------- Icons ---------- */

PAT(gfx_ok, ".###.", "#...#", "#.#.#", "#...#", ".###.")
PAT(gfx_back, "..#.....", ".##.....", "#######.", ".##....#", "..#....#", ".......#", "..#####.")
PAT(gfx_up, "...#...", "..###..", ".#####.", "#######")
PAT(gfx_down, "#######", ".#####.", "..###..", "...#...")
PAT(gfx_left, "...#", "..##", ".###", "####", ".###", "..##", "...#")
PAT(gfx_right, "#...", "##..", "###.", "####", "###.", "##..", "#...")
PAT(gfx_lt, "...#", "..#.", ".#..", "#...", ".#..", "..#.", "...#")
PAT(gfx_rt, "#...", ".#..", "..#.", "...#", "..#.", ".#..", "#...")
PAT(gfx_bt_big,
    "...#...",
    "...##..",
    "...#.#.",
    "#..#..#",
    ".#.#.#.",
    "..###..",
    ".#.#.#.",
    "#..#..#",
    "...#.#.",
    "...##..",
    "...#...")
PAT(gfx_sliders,
    "..###......",
    "###.#######",
    "..###......",
    "...........",
    "......###..",
    "#######.###",
    "......###..",
    "...........",
    "...###.....",
    "####.######",
    "...###.....")
PAT(gfx_bt, "..#..", "..##.", "#.#.#", ".###.", "#.#.#", "..##.", "..#..")
PAT(gfx_wait, "#####", ".#.#.", ".###.", "..#..", ".#.#.", ".#.#.", "#####")
PAT(gfx_search, ".###...", "#...#..", "#...#..", "#...#..", ".###...", ".....#.", "......#")

uint8_t gfx_pat_width(const Pat* p) {
    size_t w = 0;
    for(uint8_t i = 0; i < p->count; i++) {
        size_t l = strlen(p->rows[i]);
        if(l > w) w = l;
    }
    return (uint8_t)w;
}

uint8_t gfx_pat_height(const Pat* p) {
    return p->count;
}

void gfx_pat(Canvas* canvas, int x, int y, const Pat* p, Color color) {
    canvas_set_color(canvas, color);
    for(uint8_t j = 0; j < p->count; j++) {
        const char* row = p->rows[j];
        for(int i = 0; row[i]; i++) {
            if(row[i] == '#') canvas_draw_dot(canvas, x + i, y + j);
        }
    }
    canvas_set_color(canvas, ColorBlack);
}

static int font_cap(Canvas* canvas, Font font) {
    const CanvasFontParameters* params = canvas_get_font_params(canvas, font);
    return params ? params->height : 7;
}

void gfx_pat_mid(Canvas* canvas, int x, int baseline, Font font, const Pat* p, Color color) {
    int cap = font_cap(canvas, font);
    int h = gfx_pat_height(p);
    int top = baseline - cap;
    int y = (cap >= h) ? top + (cap - h) / 2 : top - (h - cap + 1) / 2;
    gfx_pat(canvas, x, y, p, color);
}

/* ---------- Hand-drawn bold font: 9 rows, cap height 7 (rows 0..6), descender 2 ---------- */

typedef struct {
    char ch;
    const char* rows[9];
} BoldGlyph;

static const BoldGlyph bold_glyphs[] = {
    {'E', {"#####", "##...", "##...", "####.", "##...", "##...", "#####", "", ""}},
    {'F', {"#####", "##...", "##...", "####.", "##...", "##...", "##...", "", ""}},
    {'M', {"##...##", "###.###", "#######", "##.#.##", "##...##", "##...##", "##...##", "", ""}},
    {'R', {"#####.", "##..##", "##..##", "#####.", "##.##.", "##..##", "##..##", "", ""}},
    {'V', {"##..##", "##..##", "##..##", "##..##", ".####.", ".####.", "..##..", "", ""}},
    {'a', {"", "", ".####.", "....##", ".#####", "##..##", ".#####", "", ""}},
    {'b', {"##....", "##....", "#####.", "##..##", "##..##", "##..##", "#####.", "", ""}},
    {'d', {"....##", "....##", ".#####", "##..##", "##..##", "##..##", ".#####", "", ""}},
    {'e', {"", "", ".####.", "##..##", "######", "##....", ".####.", "", ""}},
    {'g', {"", "", ".#####", "##..##", "##..##", "##..##", ".#####", "....##", ".####."}},
    {'i', {"##", "", "##", "##", "##", "##", "##", "", ""}},
    {'l', {"##", "##", "##", "##", "##", "##", "##", "", ""}},
    {'m', {"", "", "#######.", "##.##.##", "##.##.##", "##.##.##", "##.##.##", "", ""}},
    {'n', {"", "", "#####.", "##..##", "##..##", "##..##", "##..##", "", ""}},
    {'o', {"", "", ".####.", "##..##", "##..##", "##..##", ".####.", "", ""}},
    {'p', {"", "", "#####.", "##..##", "##..##", "##..##", "#####.", "##....", "##...."}},
    {'r', {"", "", "##.##", "###..", "##...", "##...", "##...", "", ""}},
    {'t', {"", "##..", "####", "##..", "##..", "##..", ".###", "", ""}},
    {'x', {"", "", "##..##", ".####.", "..##..", ".####.", "##..##", "", ""}},
};

static const BoldGlyph* bold_find(char c) {
    for(size_t i = 0; i < COUNT_OF(bold_glyphs); i++) {
        if(bold_glyphs[i].ch == c) return &bold_glyphs[i];
    }
    return NULL;
}

static int bold_glyph_width(char c) {
    if(c == ' ') return 3;
    const BoldGlyph* g = bold_find(c);
    if(!g) return 3;
    int w = 0;
    for(int j = 0; j < 9; j++) {
        int l = (int)strlen(g->rows[j]);
        if(l > w) w = l;
    }
    return w;
}

int gfx_bold_width(const char* s) {
    int w = 0;
    for(int i = 0; s[i]; i++) w += bold_glyph_width(s[i]) + 1;
    return w > 0 ? w - 1 : 0;
}

int gfx_bold(Canvas* canvas, int x, int baseline, const char* s, Color color) {
    int top = baseline - 7;
    int start = x;
    canvas_set_color(canvas, color);
    for(int i = 0; s[i]; i++) {
        const BoldGlyph* g = bold_find(s[i]);
        if(g) {
            for(int j = 0; j < 9; j++) {
                for(int k = 0; g->rows[j][k]; k++) {
                    if(g->rows[j][k] == '#') canvas_draw_dot(canvas, x + k, top + j);
                }
            }
        }
        x += bold_glyph_width(s[i]) + 1;
    }
    canvas_set_color(canvas, ColorBlack);
    return x - start - 1;
}

/* ---------- Letters and round buttons ---------- */

// A is drawn bigger than B (A sits in the bigger circle)
static const char* const big_a[] = {
    ".....#####.....",
    "....#######....",
    "...####.####...",
    "...###...###...",
    "..####...####..",
    "..###.....###..",
    ".####.....####.",
    ".###.......###.",
    ".###.......###.",
    "####.......####",
    "###############",
    "###############",
    "###############",
    "###.........###",
    "###.........###",
    "###.........###",
    "###.........###",
    "###.........###",
};
static const char* const big_b[] = {
    "########..",
    "#########.",
    "###...###.",
    "###...###.",
    "###..####.",
    "########..",
    "#########.",
    "###....###",
    "###....###",
    "###....###",
    "###...####",
    "#########.",
    "########..",
};
static const char* const small_a[] = {".##.", "#..#", "#..#", "####", "#..#", "#..#"};
static const char* const small_b[] = {"###.", "#..#", "###.", "#..#", "#..#", "###."};

static bool in_disc(int dx, int dy, int r) {
    return dx * dx + dy * dy <= r * r + r;
}

static bool on_ring(int dx, int dy, int r) {
    int d = dx * dx + dy * dy;
    return d <= r * r + r && d > (r - 1) * (r - 1) + (r - 1);
}

void gfx_big_button(Canvas* canvas, int cx, int cy, int r, char letter, int style) {
    const char* const* g = (letter == 'A') ? big_a : big_b;
    const int gw = (letter == 'A') ? 15 : 10;
    const int gh = (letter == 'A') ? (int)COUNT_OF(big_a) : (int)COUNT_OF(big_b);
    // B looks heavier on its left (straight stem), so it sits 1px further right
    int x0 = cx - gw / 2 + ((letter == 'B') ? 1 : 0), y0 = cy - gh / 2;

    if(style == 0) {
        // Filled button, white letter
        canvas_set_color(canvas, ColorBlack);
        for(int dy = -r; dy <= r; dy++)
            for(int dx = -r; dx <= r; dx++)
                if(in_disc(dx, dy, r)) canvas_draw_dot(canvas, cx + dx, cy + dy);
        canvas_set_color(canvas, ColorWhite);
    } else if(style == 1) {
        // Searching: two lines with 1px gap, checkerboard grid inside,
        // white letter with a 1px black outline
        canvas_set_color(canvas, ColorBlack);
        int ri = r - 3;
        for(int dy = -r; dy <= r; dy++) {
            for(int dx = -r; dx <= r; dx++) {
                bool dot = on_ring(dx, dy, r) || on_ring(dx, dy, r - 2) ||
                           (in_disc(dx, dy, ri) && ((cx + dx + cy + dy) % 2 == 0));
                if(dot) canvas_draw_dot(canvas, cx + dx, cy + dy);
            }
        }
        for(int j = 0; j < gh; j++)
            for(int i = 0; g[j][i]; i++)
                if(g[j][i] == '#')
                    for(int oy = -1; oy <= 1; oy++)
                        for(int ox = -1; ox <= 1; ox++)
                            canvas_draw_dot(canvas, x0 + i + ox, y0 + j + oy);
        canvas_set_color(canvas, ColorWhite);
    } else {
        // Just sent: outline only, black letter
        canvas_set_color(canvas, ColorBlack);
        for(int dy = -r; dy <= r; dy++)
            for(int dx = -r; dx <= r; dx++)
                if(on_ring(dx, dy, r) || on_ring(dx, dy, r - 1))
                    canvas_draw_dot(canvas, cx + dx, cy + dy);
    }

    for(int j = 0; j < gh; j++)
        for(int i = 0; g[j][i]; i++)
            if(g[j][i] == '#') canvas_draw_dot(canvas, x0 + i, y0 + j);
    canvas_set_color(canvas, ColorBlack);
}

void gfx_small_button(Canvas* canvas, int cx, int cy, char letter, bool inverted) {
    // 10x10 disc covering cx-5..cx+4 / cy-5..cy+4 (even size so the 4x6 letter sits centered)
    canvas_set_color(canvas, inverted ? ColorWhite : ColorBlack);
    for(int y = -5; y <= 4; y++) {
        for(int x = -5; x <= 4; x++) {
            int fx = 2 * x + 1, fy = 2 * y + 1;
            if(fx * fx + fy * fy <= 104) canvas_draw_dot(canvas, cx + x, cy + y);
        }
    }
    const char* const* g = (letter == 'A') ? small_a : small_b;
    canvas_set_color(canvas, inverted ? ColorBlack : ColorWhite);
    for(int j = 0; j < 6; j++)
        for(int i = 0; g[j][i]; i++)
            if(g[j][i] == '#') canvas_draw_dot(canvas, cx - 2 + i, cy - 3 + j);
    canvas_set_color(canvas, ColorBlack);
}

/* ---------- Tabs ---------- */

#define TAB_H (11)

static void tab_draw(
    Canvas* canvas,
    int x0,
    int y,
    int w,
    const Pat* icon,
    const char* label,
    bool outline,
    int progress,
    bool extend) {
    canvas_set_font(canvas, FontSecondary);
    int cap = font_cap(canvas, FontSecondary);
    // All tabs are fully rounded, filled and outlined alike
    UNUSED(extend);
    const int bh = TAB_H;
    int content = gfx_pat_width(icon) + 3 + canvas_string_width(canvas, label);
    int cx = x0 + (w - content) / 2;
    int baseline = y + (TAB_H - cap) / 2 + cap;

    if(outline || progress >= 0) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, x0, y, w, TAB_H);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rframe(canvas, x0, y, w, bh, 3);
    } else {
        canvas_draw_rbox(canvas, x0, y, w, bh, 3);
    }

    Color fg = (outline || progress >= 0) ? ColorBlack : ColorWhite;
    gfx_pat_mid(canvas, cx, baseline, FontSecondary, icon, fg);
    canvas_set_color(canvas, fg);
    canvas_draw_str(canvas, cx + gfx_pat_width(icon) + 3, baseline, label);
    canvas_set_color(canvas, ColorBlack);

    if(progress >= 0) {
        // Fill from the left by inverting what is already drawn
        int fw = ((w - 2) * progress) / 1000;
        if(fw > 0) {
            canvas_set_color(canvas, ColorXOR);
            canvas_draw_box(canvas, x0 + 1, y + 1, fw, TAB_H - 2);
            canvas_set_color(canvas, ColorBlack);
        }
    }
}

void gfx_tab(Canvas* canvas, int side, const Pat* icon, const char* label, bool outline, int progress) {
    canvas_set_font(canvas, FontSecondary);
    int w = 3 + gfx_pat_width(icon) + 3 + canvas_string_width(canvas, label) + 3;
    int cw = canvas_width(canvas);
    int x0 = (side == 0) ? 0 : (side == 2) ? cw - w : (cw - w) / 2;
    tab_draw(canvas, x0, canvas_height(canvas) - TAB_H, w, icon, label, outline, progress, true);
}

void gfx_vtab(Canvas* canvas, int y, const Pat* icon, const char* label, bool outline, int progress) {
    tab_draw(
        canvas, 0, y, canvas_width(canvas), icon, label, outline, progress, y + TAB_H >= (int)canvas_height(canvas));
}
