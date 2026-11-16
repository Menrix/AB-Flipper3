/*
 * AB Shutter Remote
 * Turns the Flipper Zero into an "AB Shutter3" style Bluetooth remote with two
 * buttons, A and B - for e-readers, phones and cameras that accept such remotes.
 *
 * Screens:
 *   Pair remote  - first start, nothing paired yet (OK = pair, Up = setup, Back = exit)
 *   Waiting      - pairing mode, device can now connect (Back = cancel, Up = setup)
 *   Remote       - A / B buttons (hold Back = exit, free arrow key = setup)
 *   Settings     - button mapping, exit hold time, orientation, vibration, forget device
 *   Confirm      - "Forget device?"
 *
 * Remote and Settings follow the "Remote" orientation setting (horizontal / vertical,
 * each also turned by 180 degrees),
 * Pair and Waiting are always horizontal. In vertical mode the firmware turns the
 * arrow keys with the screen, so "Left" is always the key that points left in your hand.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <bt/bt_service/bt.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <notification/notification_messages.h>
#include <assets_icons.h>

#include "shutter_profile.h"
#include "gfx.h"

#define KEYS_STORAGE_PATH APP_DATA_PATH(".ab_shutter_remote.keys")
#define SETTINGS_PATH APP_DATA_PATH("settings.bin")
#define SETTINGS_MAGIC (0xAB)
#define SETTINGS_VERSION (5)

// What the two remote buttons send (HID usage codes), like an AB Shutter3:
// A = Volume Up  -> camera shutter on iPhone and Android, "next" on e-readers
// B = Enter      -> a second, different code, so devices that learn buttons
//                   can tell A and B apart
#define CONSUMER_VOLUME_UP (0x00E9)
#define KEY_ENTER (0x28)

#define TAP_MS (40) // how long a button press is held on the device
#define FLASH_MS (150) // how long a sent button is shown "pressed"
#define EXIT_SHOW_MS (300) // exit progress appears after holding Back this long
#define TICK_MS (100) // screen refresh while animating
#define DOTS_MS (500) // speed of the "..." animation
#define SCROLL_MS (350) // speed of long values scrolling in the settings

/* ---------- Button mapping ---------- */

typedef enum {
    TrigOk,
    TrigOkHold,
    TrigBack,
    TrigUp,
    TrigUpHold,
    TrigDown,
    TrigDownHold,
    TrigLeft,
    TrigLeftHold,
    TrigRight,
    TrigRightHold,
    TrigCount,
} Trigger;

static const char* const trig_names[TrigCount] = {
    "OK",
    "Hold OK",
    "Back",
    "Up",
    "Hold Up",
    "Down",
    "Hold Down",
    "Left",
    "Hold Left",
    "Right",
    "Hold Right",
};

static const InputKey trig_keys[TrigCount] = {
    InputKeyOk,
    InputKeyOk,
    InputKeyBack,
    InputKeyUp,
    InputKeyUp,
    InputKeyDown,
    InputKeyDown,
    InputKeyLeft,
    InputKeyLeft,
    InputKeyRight,
    InputKeyRight,
};

static const bool trig_hold[TrigCount] = {
    false, true, false, false, true, false, true, false, true, false, true};

static const Pat* key_icon(InputKey key) {
    switch(key) {
    case InputKeyOk:
        return &gfx_ok;
    case InputKeyBack:
        return &gfx_back;
    case InputKeyUp:
        return &gfx_up;
    case InputKeyDown:
        return &gfx_down;
    case InputKeyLeft:
        return &gfx_left;
    default:
        return &gfx_right;
    }
}

enum {
    BtnA,
    BtnB,
    BtnCount,
};

/* ---------- Settings ---------- */

typedef struct {
    uint8_t trigger[BtnCount];
    uint8_t exit_secs; // 2..5
    uint8_t vertical; // remote + settings in portrait
    uint8_t flip; // remote + settings turned by 180 degrees
    uint8_t vibrate;
    uint8_t paired; // a device has been paired before
} Settings;

static const Settings settings_default = {
    .trigger = {TrigOk, TrigBack},
    .exit_secs = 2,
    .vertical = 0,
    .flip = 0,
    .vibrate = 1,
    .paired = 0,
};

// Screen orientation for remote + settings: Remote (horizontal / vertical) and
// Flip (turned by 180 degrees). The firmware turns the arrow keys with the
// screen, so "Left" is always the key pointing left in your hand.
static ViewPortOrientation orientation_of(bool vertical, bool flip) {
    if(vertical) return flip ? ViewPortOrientationVerticalFlip : ViewPortOrientationVertical;
    return flip ? ViewPortOrientationHorizontalFlip : ViewPortOrientationHorizontal;
}

enum {
    RowA,
    RowB,
    RowExit,
    RowRemote,
    RowFlip,
    RowVibrate,
    RowForget,
    RowCount,
};

/* ---------- App state ---------- */

typedef enum {
    ScreenPair,
    ScreenWaiting,
    ScreenRemote,
    ScreenSettings,
    ScreenConfirm,
} Screen;

typedef enum {
    EventInput,
    EventBtStatus,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
    BtStatus status;
} AppEvent;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriMutex* mutex;
    Bt* bt;
    NotificationApp* notifications;
    FuriHalBleProfileBase* profile;

    // Everything below is shared with the draw callback (mutex)
    Settings settings;
    BtStatus status;
    Screen screen;
    Screen settings_return; // where "Done" goes back to
    uint8_t row; // selected settings row
    uint8_t top; // first visible settings row
    uint32_t flash_until[BtnCount];
    bool back_down; // Back currently held on the remote screen
    uint32_t back_since;
    bool orientation_dirty; // screen orientation must be (re)applied
    uint32_t scroll_since; // selected settings value scrolls from this tick on
} App;

static const NotificationSequence sequence_tick = {
    &message_vibro_on,
    &message_delay_25,
    &message_vibro_off,
    NULL,
};

/* ---------- Helpers ---------- */

static bool tick_before(uint32_t now, uint32_t until) {
    return (int32_t)(until - now) > 0;
}

static void settings_load(App* app) {
    Settings s;
    bool ok = saved_struct_load(SETTINGS_PATH, &s, sizeof(s), SETTINGS_MAGIC, SETTINGS_VERSION);
    if(ok) {
        for(int i = 0; i < BtnCount; i++)
            if(s.trigger[i] >= TrigCount) ok = false;
        if(s.trigger[BtnA] == s.trigger[BtnB]) ok = false;
        if(s.exit_secs < 2 || s.exit_secs > 5) ok = false;
        if(s.vertical > 1 || s.flip > 1) ok = false;
    }
    app->settings = ok ? s : settings_default;
}

static void settings_save(App* app) {
    saved_struct_save(
        SETTINGS_PATH, &app->settings, sizeof(Settings), SETTINGS_MAGIC, SETTINGS_VERSION);
}

// Setup takes the first arrow key that neither A nor B uses (short or hold)
static InputKey setup_key(const Settings* s) {
    static const InputKey order[] = {InputKeyUp, InputKeyLeft, InputKeyDown, InputKeyRight};
    for(size_t i = 0; i < COUNT_OF(order); i++) {
        bool used = false;
        for(int b = 0; b < BtnCount; b++)
            if(trig_keys[s->trigger[b]] == order[i]) used = true;
        if(!used) return order[i];
    }
    return InputKeyUp; // not reachable: A and B can block at most two arrows
}

static bool screen_follows_setting(Screen s) {
    return s == ScreenRemote || s == ScreenSettings || s == ScreenConfirm;
}

// Only marks the orientation as changed. It is applied in the main loop
// WITHOUT holding app->mutex: the GUI thread holds the view port lock while it
// waits for app->mutex in draw_callback, so calling view port functions while
// holding app->mutex would make both threads wait for each other forever.
static void apply_orientation(App* app) {
    app->orientation_dirty = true;
}

static void go(App* app, Screen screen) {
    app->screen = screen;
    app->back_down = false;
    apply_orientation(app);
}

// Call WITHOUT holding app->mutex
static void sync_orientation(App* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool dirty = app->orientation_dirty;
    bool follows = screen_follows_setting(app->screen);
    ViewPortOrientation orient =
        follows ? orientation_of(app->settings.vertical, app->settings.flip) :
                  ViewPortOrientationHorizontal;
    app->orientation_dirty = false;
    furi_mutex_release(app->mutex);
    if(dirty) {
        view_port_set_orientation(app->view_port, orient);
    }
}

/* ---------- Bluetooth ---------- */

static void bt_status_callback(BtStatus status, void* context) {
    App* app = context;
    AppEvent event = {.type = EventBtStatus, .status = status};
    furi_message_queue_put(app->queue, &event, 0);
}

static void advertising(bool on) {
    if(on) {
        furi_hal_bt_start_advertising();
    } else {
        furi_hal_bt_stop_advertising();
    }
}

// Send one remote button press (or just show it, if nothing is connected)
static void remote_send(App* app, uint8_t button) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->flash_until[button] = furi_get_tick() + furi_ms_to_ticks(FLASH_MS);
    bool connected = app->status == BtStatusConnected;
    bool vibrate = app->settings.vibrate;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);

    if(!connected || !app->profile) return;

    notification_message(app->notifications, &sequence_blink_blue_10);
    if(vibrate) notification_message(app->notifications, &sequence_tick);

    if(button == BtnA) {
        shutter_profile_consumer_press(app->profile, CONSUMER_VOLUME_UP);
        furi_delay_ms(TAP_MS);
        shutter_profile_consumer_release(app->profile);
    } else {
        shutter_profile_key_press(app->profile, KEY_ENTER);
        furi_delay_ms(TAP_MS);
        shutter_profile_key_release(app->profile);
    }
}

static void forget_device(App* app) {
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_forget_bonded_devices(app->bt);
    advertising(false);
    app->settings.paired = 0;
    settings_save(app);
}

/* ---------- Drawing: shared parts ---------- */

static int dots_count(void) {
    return (int)((furi_get_tick() / furi_ms_to_ticks(DOTS_MS)) % 3) + 1;
}

// Big headline with icon, centered, with a line underneath
static void draw_headline(Canvas* canvas, int baseline, int line_y, const Pat* icon, const char* text) {
    canvas_set_font(canvas, FontPrimary);
    int cw = canvas_width(canvas);
    int w = gfx_pat_width(icon) + 3 + canvas_string_width(canvas, text);
    int x = (cw - w) / 2;
    gfx_pat_mid(canvas, x, baseline, FontPrimary, icon, ColorBlack);
    canvas_draw_str(canvas, x + gfx_pat_width(icon) + 3, baseline, text);
    canvas_draw_line(canvas, 0, line_y, cw - 1, line_y);
}

// Key icon + name, centered under a button
static void draw_key_label(Canvas* canvas, int cx, int baseline, uint8_t trig) {
    canvas_set_font(canvas, FontSecondary);
    const Pat* icon = key_icon(trig_keys[trig]);
    const char* name = trig_names[trig];
    int w = gfx_pat_width(icon) + 3 + canvas_string_width(canvas, name);
    int x = cx - w / 2;
    gfx_pat_mid(canvas, x, baseline, FontSecondary, icon, ColorBlack);
    canvas_draw_str(canvas, x + gfx_pat_width(icon) + 3, baseline, name);
}

/* ---------- Screens ---------- */

static void draw_pair(Canvas* canvas) {
    draw_headline(canvas, 10, 13, &gfx_bt_big, "Pair remote");
    canvas_set_font(canvas, FontKeyboard);
    canvas_draw_str_aligned(canvas, 64, 29, AlignCenter, AlignBottom, "No device paired.");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignBottom, "Press OK for pairing.");
    gfx_tab(canvas, 0, &gfx_back, "Exit", false, -1);
    gfx_tab(canvas, 1, &gfx_up, "Setup", true, -1);
    gfx_tab(canvas, 2, &gfx_ok, "Pair", false, -1);
}

static void draw_waiting(Canvas* canvas) {
    // Firmware image (59x54): drawn 1px high so it ends right above the tabs
    canvas_draw_icon(canvas, 0, -1, &I_DolphinWait_59x54);
    const int x = 62;
    canvas_set_font(canvas, FontSecondary);
    gfx_pat_mid(canvas, x, 9, FontSecondary, &gfx_wait, ColorBlack);
    int tx = x + gfx_pat_width(&gfx_wait) + 3;
    canvas_draw_str(canvas, tx, 9, "Waiting");
    static const char* const d[] = {".", "..", "..."};
    canvas_draw_str(canvas, tx + canvas_string_width(canvas, "Waiting"), 9, d[dots_count() - 1]);
    canvas_draw_str(canvas, x, 30, "Name:");
    canvas_set_font(canvas, FontKeyboard);
    canvas_draw_str(canvas, x, 43, SHUTTER_DEVICE_NAME);
    gfx_tab(canvas, 0, &gfx_back, "Cancel", false, -1);
    gfx_tab(canvas, 2, &gfx_up, "Setup", true, -1);
}

static int button_style(App* app, uint8_t b, uint32_t now) {
    if(tick_before(now, app->flash_until[b])) return 2; // just pressed
    return app->status == BtStatusConnected ? 0 : 1;
}

static int exit_progress(App* app, uint32_t now) {
    if(!app->back_down) return -1;
    uint32_t held = now - app->back_since;
    if(held < furi_ms_to_ticks(EXIT_SHOW_MS)) return -1;
    uint32_t total = furi_ms_to_ticks(app->settings.exit_secs * 1000);
    if(held >= total) return 1000;
    return (int)((held * 1000) / total);
}

static void draw_remote(Canvas* canvas, App* app) {
    // No status line: the buttons show it (filled = connected, grid = searching),
    // plus a small Bluetooth sign in the corner while connected.
    // A is the bigger button.
    uint32_t now = furi_get_tick();
    InputKey sk = setup_key(&app->settings);
    int progress = exit_progress(app, now);

    // Small Bluetooth sign in the top right corner while connected
    if(app->status == BtStatusConnected) {
        int cw = canvas_width(canvas);
        gfx_pat(canvas, cw - 1 - gfx_pat_width(&gfx_bt), 1, &gfx_bt, ColorBlack);
    }

    if(!app->settings.vertical) {
        // A (x 21..61) and B (x 76..106) together are centered on the screen
        gfx_big_button(canvas, 41, 21, 20, 'A', button_style(app, BtnA, now));
        gfx_big_button(canvas, 91, 26, 15, 'B', button_style(app, BtnB, now));
        draw_key_label(canvas, 41, 50, app->settings.trigger[BtnA]);
        draw_key_label(canvas, 91, 50, app->settings.trigger[BtnB]);
        gfx_tab(canvas, 0, &gfx_back, "Hold to exit", false, progress < 0 ? -1 : progress);
        gfx_tab(canvas, 2, key_icon(sk), "Setup", true, -1);
    } else {
        gfx_big_button(canvas, 32, 25, 22, 'A', button_style(app, BtnA, now));
        draw_key_label(canvas, 32, 57, app->settings.trigger[BtnA]);
        gfx_big_button(canvas, 32, 77, 16, 'B', button_style(app, BtnB, now));
        draw_key_label(canvas, 32, 101, app->settings.trigger[BtnB]);
        gfx_vtab(canvas, 104, key_icon(sk), "Setup", true, -1);
        canvas_set_font(canvas, FontSecondary);
        const char* exit_text = "Hold to exit";
        if(gfx_pat_width(&gfx_back) + 3 + canvas_string_width(canvas, exit_text) + 6 > 64) {
            exit_text = "Hold exit";
        }
        gfx_vtab(canvas, 117, &gfx_back, exit_text, false, progress < 0 ? -1 : progress);
    }
}

/* Settings rows */

static const char* row_label(uint8_t row) {
    switch(row) {
    case RowExit:
        return "Exit";
    case RowRemote:
        return "Remote";
    case RowFlip:
        return "Flip";
    case RowVibrate:
        return "Vibrate";
    default:
        return "Forget pairing";
    }
}

static void row_value(App* app, uint8_t row, char* out, size_t len) {
    const Settings* s = &app->settings;
    switch(row) {
    case RowA:
        snprintf(out, len, "%s", trig_names[s->trigger[BtnA]]);
        break;
    case RowB:
        snprintf(out, len, "%s", trig_names[s->trigger[BtnB]]);
        break;
    case RowExit:
        snprintf(out, len, "Hold %us", s->exit_secs);
        break;
    case RowRemote:
        snprintf(out, len, "%s", s->vertical ? "Vertical" : "Horizontal");
        break;
    case RowFlip:
        snprintf(out, len, "%s", s->flip ? "On" : "Off");
        break;
    case RowVibrate:
        snprintf(out, len, "%s", s->vibrate ? "On" : "Off");
        break;
    default:
        out[0] = '\0';
        break;
    }
}

// Settings value between the arrows (region x0..x0+w-1). Centered if it fits;
// otherwise it scrolls like in the Flipper's own menus (selected row) or ends
// with "..." (other rows).
static void draw_value(
    Canvas* canvas,
    App* app,
    int x0,
    int w,
    int baseline,
    const char* value,
    Color c,
    bool sel) {
    canvas_set_font(canvas, FontSecondary);
    canvas_set_color(canvas, c);
    int vw = canvas_string_width(canvas, value);
    if(vw <= w) {
        canvas_draw_str(canvas, x0 + (w - vw) / 2, baseline, value);
    } else {
        size_t scroll = 0;
        if(sel) scroll = (furi_get_tick() - app->scroll_since) / furi_ms_to_ticks(SCROLL_MS);
        FuriString* str = furi_string_alloc_set(value);
        elements_scrollable_text_line(canvas, x0, baseline, w, str, scroll, !sel);
        furi_string_free(str);
    }
    canvas_set_color(canvas, ColorBlack);
}

static void draw_arrows(Canvas* canvas, int l, int r, int baseline, Font font, Color c, bool sel) {
    gfx_pat_mid(canvas, l, baseline, font, sel ? &gfx_left : &gfx_lt, c);
    gfx_pat_mid(canvas, r, baseline, font, sel ? &gfx_right : &gfx_rt, c);
}

#define H_ROWS_VISIBLE (3)

static void draw_settings_h(Canvas* canvas, App* app) {
    draw_headline(canvas, 10, 13, &gfx_sliders, "Settings");
    const int rw = 123; // rows end before the scrollbar
    char value[16];

    for(int k = 0; k < H_ROWS_VISIBLE; k++) {
        uint8_t row = app->top + k;
        if(row >= RowCount) break;
        int y = 25 + k * 12;
        bool sel = row == app->row;
        Color c = sel ? ColorWhite : ColorBlack;
        if(sel) canvas_draw_rbox(canvas, 0, y - 10, rw, 12, 2);

        if(row == RowA || row == RowB) {
            gfx_small_button(canvas, 11, y - 4, row == RowA ? 'A' : 'B', sel);
        } else {
            gfx_bold(canvas, 5, y, row_label(row), c);
        }
        if(row != RowForget) {
            row_value(app, row, value, sizeof(value));
            const int l = 50, r = rw - 6;
            // All values in the small font, so nothing jumps while switching
            draw_arrows(canvas, l, r, y, FontSecondary, c, sel);
            draw_value(canvas, app, l + 6, r - (l + 6) - 1, y, value, c, sel);
        }
    }

    // Scrollbar
    canvas_draw_line(canvas, 126, 15, 126, 50);
    int track = 36;
    int thumb = (track * H_ROWS_VISIBLE) / RowCount;
    int ty = 15 + ((track - thumb) * app->top) / (RowCount - H_ROWS_VISIBLE);
    canvas_draw_box(canvas, 125, ty, 3, thumb);

    gfx_tab(canvas, 0, &gfx_back, "Done", false, -1);
}

#define V_LIST_TOP (17)
#define V_LIST_BOTTOM (114)

static int v_row_height(uint8_t row) {
    return (row == RowA || row == RowB) ? 12 : 19;
}

static void draw_settings_v(Canvas* canvas, App* app) {
    // Headline (portrait is too narrow for "Settings" next to the icon)
    draw_headline(canvas, 11, 14, &gfx_sliders, "Setup");
    const int rw = 60; // leave room for the scrollbar
    char value[16];

    int y0 = V_LIST_TOP;
    for(uint8_t row = app->top; row < RowCount; row++) {
        int h = v_row_height(row);
        if(y0 + h - 1 > V_LIST_BOTTOM) break;
        bool sel = row == app->row;
        Color c = sel ? ColorWhite : ColorBlack;
        if(sel) canvas_draw_rbox(canvas, 0, y0, rw, h, 3);
        row_value(app, row, value, sizeof(value));
        canvas_set_font(canvas, FontSecondary);

        if(row == RowA || row == RowB) {
            // One line: [button] < value >
            gfx_small_button(canvas, 8, y0 + 6, row == RowA ? 'A' : 'B', sel);
            int yb = y0 + 9, l = 15, r = rw - 5;
            draw_arrows(canvas, l, r, yb, FontSecondary, c, sel);
            draw_value(canvas, app, l + 6, r - (l + 6) - 1, yb, value, c, sel);
        } else if(row == RowForget) {
            // Bold label over two lines
            gfx_bold(canvas, rw / 2 - gfx_bold_width("Forget") / 2, y0 + 8, "Forget", c);
            gfx_bold(canvas, rw / 2 - gfx_bold_width("pairing") / 2, y0 + 17, "pairing", c);
        } else {
            // Bold label, value with arrows below
            const char* label = row_label(row);
            gfx_bold(canvas, rw / 2 - gfx_bold_width(label) / 2, y0 + 8, label, c);
            int yb = y0 + 17;
            draw_arrows(canvas, 3, rw - 5, yb, FontSecondary, c, sel);
            draw_value(canvas, app, 9, (rw - 5) - 9 - 1, yb, value, c, sel);
        }
        canvas_set_color(canvas, ColorBlack);
        y0 += h + 2;
    }

    // Scrollbar
    int total = 0, before = 0;
    for(uint8_t row = 0; row < RowCount; row++) {
        if(row < app->top) before += v_row_height(row) + 2;
        total += v_row_height(row) + 2;
    }
    int track = V_LIST_BOTTOM - V_LIST_TOP + 1;
    int thumb = (track * track) / total;
    if(thumb < 8) thumb = 8;
    int ty = V_LIST_TOP + ((track - thumb) * before) / (total - track > 0 ? total - track : 1);
    if(ty + thumb > V_LIST_BOTTOM + 1) ty = V_LIST_BOTTOM + 1 - thumb;
    canvas_draw_line(canvas, 62, V_LIST_TOP, 62, V_LIST_BOTTOM);
    canvas_draw_box(canvas, 61, ty, 3, thumb);

    gfx_vtab(canvas, 117, &gfx_back, "Done", false, -1);
}

static void draw_confirm(Canvas* canvas, App* app) {
    if(!app->settings.vertical) {
        draw_headline(canvas, 10, 13, &gfx_bt_big, "Forget device?");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignBottom, "The pairing will be");
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignBottom, "removed. Pair again");
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, "to reconnect.");
        gfx_tab(canvas, 0, &gfx_back, "Cancel", false, -1);
        gfx_tab(canvas, 2, &gfx_ok, "Forget", false, -1);
    } else {
        // Two-line headline: icon + "Forget" / "device?"
        canvas_set_font(canvas, FontPrimary);
        int w = gfx_pat_width(&gfx_bt_big) + 3 + canvas_string_width(canvas, "Forget");
        int x = (64 - w) / 2;
        gfx_pat_mid(canvas, x, 12, FontPrimary, &gfx_bt_big, ColorBlack);
        canvas_draw_str(canvas, x + gfx_pat_width(&gfx_bt_big) + 3, 12, "Forget");
        canvas_draw_str_aligned(canvas, 32, 25, AlignCenter, AlignBottom, "device?");
        canvas_draw_line(canvas, 0, 28, 63, 28);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 32, 44, AlignCenter, AlignBottom, "The pairing");
        canvas_draw_str_aligned(canvas, 32, 54, AlignCenter, AlignBottom, "will be removed.");
        canvas_draw_str_aligned(canvas, 32, 68, AlignCenter, AlignBottom, "Pair again");
        canvas_draw_str_aligned(canvas, 32, 78, AlignCenter, AlignBottom, "to reconnect.");
        gfx_vtab(canvas, 104, &gfx_ok, "Forget", false, -1);
        gfx_vtab(canvas, 117, &gfx_back, "Cancel", false, -1);
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    switch(app->screen) {
    case ScreenPair:
        draw_pair(canvas);
        break;
    case ScreenWaiting:
        draw_waiting(canvas);
        break;
    case ScreenRemote:
        draw_remote(canvas, app);
        break;
    case ScreenSettings:
        if(app->settings.vertical) {
            draw_settings_v(canvas, app);
        } else {
            draw_settings_h(canvas, app);
        }
        break;
    case ScreenConfirm:
        draw_confirm(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input, void* context) {
    App* app = context;
    AppEvent event = {.type = EventInput, .input = *input};
    furi_message_queue_put(app->queue, &event, FuriWaitForever);
}

/* ---------- Input ---------- */

// Step a button's trigger, skipping the one the other button already uses
static void step_trigger(Settings* s, uint8_t button, int dir) {
    uint8_t other = s->trigger[button == BtnA ? BtnB : BtnA];
    uint8_t v = s->trigger[button];
    do {
        v = (uint8_t)((v + TrigCount + dir) % TrigCount);
    } while(v == other);
    s->trigger[button] = v;
}

static void step_row_value(App* app, int dir) {
    Settings* s = &app->settings;
    switch(app->row) {
    case RowA:
        step_trigger(s, BtnA, dir);
        break;
    case RowB:
        step_trigger(s, BtnB, dir);
        break;
    case RowExit: {
        int v = s->exit_secs + dir;
        if(v < 2) v = 5;
        if(v > 5) v = 2;
        s->exit_secs = (uint8_t)v;
        break;
    }
    case RowRemote:
        s->vertical = !s->vertical;
        apply_orientation(app);
        break;
    case RowFlip:
        s->flip = !s->flip;
        apply_orientation(app);
        break;
    case RowVibrate:
        s->vibrate = !s->vibrate;
        break;
    default:
        break;
    }
}

// Keep the selected row inside the visible part of the list
static void settings_scroll(App* app) {
    if(app->row < app->top) app->top = app->row;
    if(!app->settings.vertical) {
        if(app->row >= app->top + H_ROWS_VISIBLE) app->top = app->row - H_ROWS_VISIBLE + 1;
    } else {
        for(;;) {
            int y = V_LIST_TOP;
            for(uint8_t r = app->top; r < app->row; r++) y += v_row_height(r) + 2;
            if(y + v_row_height(app->row) - 1 <= V_LIST_BOTTOM) break;
            app->top++;
        }
    }
}

static void open_settings(App* app) {
    app->settings_return = app->screen;
    app->row = RowA;
    app->top = 0;
    app->scroll_since = furi_get_tick();
    go(app, ScreenSettings);
}

// Which trigger does this event stand for? TrigCount = none
static Trigger event_trigger(const InputEvent* e) {
    if(e->type != InputTypeShort && e->type != InputTypeLong) return TrigCount;
    bool hold = e->type == InputTypeLong;
    for(int t = 0; t < TrigCount; t++) {
        if(trig_keys[t] == e->key && trig_hold[t] == hold) return (Trigger)t;
    }
    return TrigCount;
}

// returns false when the app should exit
static bool handle_input(App* app, const InputEvent* e) {
    bool running = true;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    switch(app->screen) {
    case ScreenPair:
        if(e->type == InputTypeShort) {
            if(e->key == InputKeyBack) {
                running = false;
            } else if(e->key == InputKeyOk) {
                go(app, ScreenWaiting);
                advertising(true);
            } else if(e->key == InputKeyUp) {
                open_settings(app);
            }
        }
        break;

    case ScreenWaiting:
        if(e->type == InputTypeShort) {
            if(e->key == InputKeyBack) {
                advertising(false);
                go(app, ScreenPair);
            } else if(e->key == InputKeyUp) {
                open_settings(app);
            }
        }
        break;

    case ScreenRemote: {
        // Back: short = maybe a remote button, held = exit (with progress)
        if(e->key == InputKeyBack) {
            if(e->type == InputTypePress) {
                app->back_down = true;
                app->back_since = furi_get_tick();
            } else if(e->type == InputTypeRelease) {
                app->back_down = false;
            }
        }
        if(e->type == InputTypeShort && e->key == setup_key(&app->settings)) {
            open_settings(app);
            break;
        }
        Trigger t = event_trigger(e);
        if(t == TrigCount) break;
        for(uint8_t b = 0; b < BtnCount; b++) {
            if(app->settings.trigger[b] == t) {
                furi_mutex_release(app->mutex);
                remote_send(app, b);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
            }
        }
        break;
    }

    case ScreenSettings:
        if(e->type != InputTypeShort && e->type != InputTypeRepeat) break;
        app->scroll_since = furi_get_tick(); // long values start scrolling from the beginning
        switch(e->key) {
        case InputKeyUp:
            app->row = (app->row + RowCount - 1) % RowCount;
            settings_scroll(app);
            break;
        case InputKeyDown:
            app->row = (app->row + 1) % RowCount;
            settings_scroll(app);
            break;
        case InputKeyLeft:
        case InputKeyRight:
            step_row_value(app, e->key == InputKeyRight ? 1 : -1);
            settings_scroll(app); // orientation may have changed
            break;
        case InputKeyOk:
            if(e->type != InputTypeShort) break;
            if(app->row == RowForget) {
                go(app, ScreenConfirm);
            } else {
                step_row_value(app, 1);
                settings_scroll(app);
            }
            break;
        case InputKeyBack:
            if(e->type != InputTypeShort) break;
            settings_save(app);
            go(app, app->settings_return);
            break;
        default:
            break;
        }
        break;

    case ScreenConfirm:
        if(e->type != InputTypeShort) break;
        if(e->key == InputKeyBack) {
            go(app, ScreenSettings);
        } else if(e->key == InputKeyOk) {
            furi_mutex_release(app->mutex);
            forget_device(app);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            go(app, ScreenPair);
        }
        break;
    }

    furi_mutex_release(app->mutex);
    return running;
}

static void handle_bt_status(App* app, BtStatus status) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->status = status;
    if(status == BtStatusConnected && !app->settings.paired) {
        app->settings.paired = 1;
        settings_save(app);
    }
    if(status == BtStatusConnected && app->screen == ScreenWaiting) {
        go(app, ScreenRemote);
    }
    if(status == BtStatusConnected && app->screen == ScreenSettings &&
       app->settings_return == ScreenWaiting) {
        app->settings_return = ScreenRemote;
    }
    furi_mutex_release(app->mutex);
}

/* ---------- Entry point ---------- */

int32_t ab_shutter_remote_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->queue = furi_message_queue_alloc(16, sizeof(AppEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->status = BtStatusOff;
    settings_load(app);

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    // Switch Bluetooth to our remote profile
    app->bt = furi_record_open(RECORD_BT);
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_storage_path(app->bt, KEYS_STORAGE_PATH);
    app->profile = bt_profile_start(app->bt, shutter_profile, NULL);
    bt_set_status_changed_callback(app->bt, bt_status_callback, app);

    // Already paired: straight to the buttons (reconnects by itself).
    // Not paired: stay invisible until the user starts pairing.
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->settings.paired) {
        advertising(true);
        go(app, ScreenRemote);
    } else {
        advertising(false);
        go(app, ScreenPair);
    }
    furi_mutex_release(app->mutex);
    sync_orientation(app);
    view_port_update(app->view_port);

    AppEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, furi_ms_to_ticks(TICK_MS)) == FuriStatusOk) {
            if(event.type == EventInput) {
                running = handle_input(app, &event.input);
            } else {
                handle_bt_status(app, event.status);
            }
        }

        // Exit once Back has been held long enough on the remote screen
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->screen == ScreenRemote && app->back_down &&
           furi_get_tick() - app->back_since >=
               furi_ms_to_ticks(app->settings.exit_secs * 1000)) {
            running = false;
        }
        furi_mutex_release(app->mutex);

        sync_orientation(app);
        view_port_update(app->view_port); // animations, flashes, exit progress
    }

    // Restore normal Flipper Bluetooth
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    if(app->profile) shutter_profile_release_all(app->profile);
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(app->bt);
    bool restored = bt_profile_restore_default(app->bt);
    UNUSED(restored);
    furi_record_close(RECORD_BT);

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
