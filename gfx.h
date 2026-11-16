#pragma once

/*
 * Small pixel graphics used by the app: icons, a hand-drawn bold label font,
 * big A/B letters, round buttons and the black/outlined bottom tabs.
 * Everything is drawn dot by dot, so it looks exactly like the design.
 */

#include <gui/canvas.h>

typedef struct {
    const char* const* rows; // '#' = pixel, anything else = empty
    uint8_t count;
} Pat;

// Icons
extern const Pat gfx_ok; // 5x5 OK circle
extern const Pat gfx_back; // 8x7 back arrow
extern const Pat gfx_up; // 7x4 triangles
extern const Pat gfx_down;
extern const Pat gfx_left; // 4x7 triangles
extern const Pat gfx_right;
extern const Pat gfx_lt; // 4x7 thin chevrons
extern const Pat gfx_rt;
extern const Pat gfx_bt_big; // 7x11 Bluetooth (headline)
extern const Pat gfx_sliders; // 11x11 settings (headline)
extern const Pat gfx_bt; // 5x7 Bluetooth (status)
extern const Pat gfx_wait; // 5x7 hourglass (status)
extern const Pat gfx_search; // 7x7 magnifier (status)

uint8_t gfx_pat_width(const Pat* p);
uint8_t gfx_pat_height(const Pat* p);

// Draw pattern with its top left corner at x/y in the given color
void gfx_pat(Canvas* canvas, int x, int y, const Pat* p, Color color);

// Draw pattern vertically centered on the capitals of `font` at `baseline`
void gfx_pat_mid(Canvas* canvas, int x, int baseline, Font font, const Pat* p, Color color);

// Hand-drawn small bold font (cap height 7, 2px strokes). Returns the width.
int gfx_bold_width(const char* s);
int gfx_bold(Canvas* canvas, int x, int baseline, const char* s, Color color);

// Big round A/B button. style: 0 = filled (connected), 1 = grid (searching),
// 2 = outline (just sent)
void gfx_big_button(Canvas* canvas, int cx, int cy, int r, char letter, int style);

// Small 10px A/B button used in the settings list
void gfx_small_button(Canvas* canvas, int cx, int cy, char letter, bool inverted);

// Bottom tab in landscape. side: 0 = left, 1 = center, 2 = right.
// outline = secondary action. progress 0..1000 fills the tab (or -1).
void gfx_tab(Canvas* canvas, int side, const Pat* icon, const char* label, bool outline, int progress);

// Full-width tab for the portrait layout, top edge at y
void gfx_vtab(Canvas* canvas, int y, const Pat* icon, const char* label, bool outline, int progress);
