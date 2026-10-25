// Extra settings in the game's own Options list: native text, arrows, scrolling
// and descriptions. Layouts verified on Eevee 1.0.0; other builds retain F12.
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../hle.h"
#include "../gamesetup.h"
#include "../settings.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *, uint64_t));
void rt_call_original(Cpu *c, uint64_t addr);

#define LIST_BUILD  0x70000426a0ull
#define LIST_APPEND 0x7000046980ull
#define ROW_TEXT    0x7000044cc0ull
#define ROW_CHANGE  0x700003f860ull
#define ROW_FOCUS   0x7000044a20ull
#define ROW_SELECT  0x7000044840ull
#define ROW_BIND    0x7000044890ull
#define EDIT_ARROWS 0x7000043930ull
#define GET_STATE   0x700003e970ull
#define HELP_UPDATE 0x700003eb50ull
#define MESSAGE_GET 0x70006788a0ull
#define UI_GET      0x70001266e0ull
#define UI_TEXT     0x700036cb70ull
#define UI_INPUT    0x7000126670ull
#define UI_REARM    0x70003623c0ull
#define FIRST_ROW 11u
#define PC_HELP_ID 0x6c61726570637000ull

enum { CHOICE, PERCENT, TOGGLE, RESET_PICTURE, RESET_ADVANCED };
enum { ALWAYS, WITH_FSR3, WITH_TAA, WITH_SSAO, WITH_GI, WITH_FG };
typedef struct {
    const char *label, *help;
    int type, when;
    size_t offset;
    const char *const *choices;
    int count;
    float lo, hi;
} Row;
static const char *const rates[] = {"30 fps", "60 fps", "120 fps"};
static const char *const quality[] = {"Off", "Low", "Medium", "High", "Ultra"};
static const char *const shadows[] = {"Original", "Bilinear", "Smooth"};
static const char *const aa[] = {"Off", "SMAA", "TAA", "FSR 3.1"};
static const char *const fsr[] = {"Native AA", "Quality", "Balanced", "Performance", "Ultra performance"};
static const char *const fg[] = {"Off", "Auto", "2x", "3x", "4x"};
static const char *const fg_methods[] = {"Re-render", "Optical flow"};
#define FIELD(name) offsetof(Settings, name)
#define CH(label, help, field, choices, n, when) {label, help, CHOICE, when, FIELD(field), choices, n, 0, 0}
#define PC(label, help, field, low, high, when) {label, help, PERCENT, when, FIELD(field), NULL, 0, low, high}
#define TG(label, help, field, when) {label, help, TOGGLE, when, FIELD(field), NULL, 0, 0, 0}
static const Row rows[] = {
    CH("Frame rate", "World frame rate. Battles and captures run at 30 fps. 120 fps is experimental.", fps, rates, 3, ALWAYS),
    CH("Ambient occlusion", "Soft shadows in corners. Ultra also adds contact shadows under feet and small objects.", ssao_level, quality, 5, ALWAYS),
    CH("Indirect light", "Screen-space bounced light. Higher quality uses more GPU time.", gi_level, quality, 4, ALWAYS),
    CH("Shadow filter", "Choose the original shadows, bilinear filtering, or smoother shadow edges.", shadow_filter, shadows, 3, ALWAYS),
    CH("Anti-aliasing", "Smooth jagged edges with SMAA, TAA, or FSR 3.1. FSR 3.1 also upscales the scene.", antialias, aa, 4, ALWAYS),
    CH("FSR 3.1 mode", "Render scale for FSR 3.1. Available when anti-aliasing is set to FSR 3.1.", fsr3_preset, fsr, 5, WITH_FSR3),
    PC("FSR 3.1 sharpness", "Sharpen the FSR 3.1 image. Available when anti-aliasing is set to FSR 3.1.", fsr3_sharpness, 0, 1, WITH_FSR3),
    CH("Frame generation", "Extra images between world frames. Auto follows your display and GPU performance.", frame_gen, fg, 5, ALWAYS),
    CH("Frame generation method", "Re-render scene geometry, or use faster optical-flow interpolation between images.", fg_method, fg_methods, 2, WITH_FG),
    PC("Master volume", "Overall PC audio volume. The original music, effects and cries settings still apply.", volume, 0, 2, ALWAYS),
    PC("Brightness", "Adjust image brightness. 100% preserves the original brightness.", brightness, .5f, 1.5f, ALWAYS),
    PC("Contrast", "Adjust image contrast. 100% preserves the original contrast.", contrast, .5f, 1.5f, ALWAYS),
    PC("Saturation", "Adjust image colour intensity. 100% preserves the original colours.", saturation, 0, 2, ALWAYS),
    PC("TAA sharpness", "Sharpen the TAA image. Available when anti-aliasing is set to TAA.", taa_sharpness, 0, 1, WITH_TAA),
    TG("Object motion", "Track moving objects as well as the camera for TAA, FSR 3.1 and frame generation.", mv_objects, ALWAYS),
    TG("Show occlusion", "Debug view: show ambient occlusion alone. Requires ambient occlusion to be enabled.", ssao_debug, WITH_SSAO),
    TG("Show indirect light", "Debug view: show indirect light alone. Requires indirect light to be enabled.", gi_debug, WITH_GI),
    TG("Show motion vectors", "Debug view: show the scene's motion vectors.", mv_debug, ALWAYS),
    TG("FPS counter", "Show the PC frame-rate counter while playing.", show_fps, ALWAYS),
    TG("Wild battles", "Classic combat with XP on defeat and catching with balls from the battle Bag.", wild_battle, ALWAYS),
    {"Reset picture settings", "Reset volume, brightness, contrast, saturation and TAA sharpness.", RESET_PICTURE},
    {"Reset advanced settings", "Turn off debug views and enable object motion vectors.", RESET_ADVANCED},
};
#define NROWS ((unsigned)(sizeof rows / sizeof *rows))
static _Thread_local int help_row = -1;
static _Thread_local int binding_row = -1;
static uint64_t visible_app;
static int visible_rows[10];

static bool guest(uint64_t p, size_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}
static uint64_t ptr(uint64_t p) {
    return guest(p, 8) ? *(const uint64_t *)(uintptr_t)p : 0;
}
static int custom(uint32_t id) { return id >= FIRST_ROW && id < FIRST_ROW + NROWS ? (int)(id - FIRST_ROW) : -1; }
// The list owns 8-byte records: { uint32_t option; bool removed; padding }.
static int row_at(uint64_t app, uint32_t index) {
    uint64_t model = ptr(app + 0x278), records = ptr(model + 0xe0), count = ptr(model + 0xe8);
    if (index >= count || count > 64 || !guest(records, count * 8)) return -1;
    return custom(*(const uint32_t *)(uintptr_t)(records + index * 8));
}
static bool enabled(const Row *r) {
    switch (r->when) {
    case WITH_FSR3: return g_settings.antialias == AA_FSR3;
    case WITH_FG: return g_settings.frame_gen != 0;
    case WITH_TAA: return g_settings.antialias == AA_TAA;
    case WITH_SSAO: return g_settings.ssao;
    case WITH_GI: return g_settings.gi;
    default: return true;
    }
}
static void value(const Row *r, char *out, size_t cap) {
    const uint8_t *p = (const uint8_t *)&g_settings + r->offset;
    if (!enabled(r)) { snprintf(out, cap, "--"); return; }
    if (r->type == PERCENT) snprintf(out, cap, "%.0f%%", *(const float *)p * 100);
    else if (r->type == TOGGLE) snprintf(out, cap, "%s", *(const bool *)p ? "On" : "Off");
    else if (r->type == CHOICE) {
        int v = *(const int *)p;
        if (r->offset == FIELD(fps)) v = v == 120 ? 2 : v == 60 ? 1 : 0;
        snprintf(out, cap, "%s", r->choices[v >= 0 && v < r->count ? v : 0]);
    } else out[0] = 0;
}
static void change(const Row *r, int direction) {
    if (!enabled(r)) return;
    uint8_t *p = (uint8_t *)&g_settings + r->offset;
    if (r->type == CHOICE) {
        int v = *(int *)p;
        if (r->offset == FIELD(fps)) v = v == 120 ? 2 : v == 60 ? 1 : 0;
        v = (v + direction + r->count) % r->count;
        *(int *)p = r->offset == FIELD(fps) ? (int[]){30, 60, 120}[v] : v;
    } else if (r->type == PERCENT) {
        float v = roundf(*(float *)p * 20) / 20 + direction * .05f;
        *(float *)p = fminf(r->hi, fmaxf(r->lo, v));
    } else if (r->type == TOGGLE) *(bool *)p = !*(bool *)p;
    else if (r->type == RESET_PICTURE) {
        g_settings.volume = g_settings.brightness = g_settings.contrast = g_settings.saturation = 1;
        g_settings.taa_sharpness = .5f;
    } else {
        g_settings.ssao_debug = g_settings.gi_debug = g_settings.mv_debug = false;
        g_settings.mv_objects = true;
    }
    settings_clamp_color();
    settings_save();
    LOG("[pc-settings] changed %s\n", r->label);
}
static uint64_t hash(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    while (*s) h = (h ^ (unsigned char)*s++) * 0x100000001b3ull;
    return h;
}
// UI_TEXT copies the StrBuf's UTF-16 data synchronously into the native pane.
static void text(Cpu *c, uint64_t ui, const char *pane, const char *s) {
    uint16_t chars[256];
    size_t n = 0;
    while (s[n] && n + 1 < sizeof chars / sizeof *chars) { chars[n] = (unsigned char)s[n]; n++; }
    chars[n] = 0;
    uint64_t strbuf[16] = {0}, key = hash(pane);
    strbuf[0x58 / 8] = (uint64_t)(uintptr_t)chars;
    c->x[0] = ui; c->x[1] = (uint64_t)(uintptr_t)&key;
    c->x[2] = (uint64_t)(uintptr_t)strbuf; c->x[3] = 10;
    rt_call_original(c, UI_TEXT);
}
static void draw_row(Cpu *c, uint64_t app, unsigned slot, int row) {
    if (slot >= 10) return;
    c->x[0] = app; c->x[1] = 0;
    rt_call_original(c, UI_GET);
    uint64_t ui = c->x[0];
    const Row *r = &rows[row];
    char pane[64], val[64];
    value(r, val, sizeof val);
    for (int field = 0; field < 3; field++) {
        snprintf(pane, sizeof pane, "pane_L_button_%02u_T_text_%02d", slot, field);
        const char *s = r->type >= RESET_PICTURE ? (field == 2 ? r->label : "") :
                        field == 0 ? r->label : field == 1 ? val : "";
        text(c, ui, pane, s);
    }
}
static void refresh(Cpu *c, uint64_t app) {
    if (app != visible_app) return;
    for (unsigned slot = 0; slot < 10; slot++)
        if (visible_rows[slot] >= 0) draw_row(c, app, slot, visible_rows[slot]);
}
static int build_hook(Cpu *c, uint64_t addr) {
    uint64_t app = c->x[0];
    rt_call_original(c, addr);
    Cpu saved = *c;
    visible_app = app;
    for (unsigned slot = 0; slot < 10; slot++) visible_rows[slot] = -1;
    uint64_t model = ptr(app + 0x278);
    if (guest(model, 0x100)) for (unsigned i = 0; i < NROWS; i++) {
        uint64_t record = FIRST_ROW + i;
        c->x[0] = model + 0xa0; c->x[1] = (uint64_t)(uintptr_t)&record;
        rt_call_original(c, LIST_APPEND);
    }
    LOG("[pc-settings] appended %u native settings\n", NROWS);
    *c = saved;
    return 1;
}
static int text_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    int row = binding_row >= 0 ? binding_row : custom((uint32_t)c->x[2]);
    if (c->x[0] == visible_app && (uint32_t)c->x[1] < 10) visible_rows[(uint32_t)c->x[1]] = row;
    if (row < 0) return 0;
    Cpu saved = *c;
    draw_row(c, saved.x[0], (uint32_t)saved.x[1], row);
    *c = saved;
    return 1;
}
static int bind_hook(Cpu *c, uint64_t addr) {
    uint64_t app = c->x[0];
    unsigned index = (uint32_t)c->x[1];
    int row = row_at(app, index);
    if (row < 0 || rows[row].type < RESET_PICTURE) return 0;
    uint64_t records = ptr(ptr(app + 0x278) + 0xe0);
    uint32_t *option = (uint32_t *)(uintptr_t)(records + index * 8);
    int previous = binding_row;
    binding_row = row;
    *option = 0; // native action-button appearance; this model belongs to this UI thread
    rt_call_original(c, addr);
    *option = FIRST_ROW + row;
    binding_row = previous;
    return 1;
}
static int focus_hook(Cpu *c, uint64_t addr) {
    uint64_t app = c->x[0];
    int row = row_at(app, (uint32_t)c->x[1]);
    if (row < 0) return 0;
    rt_call_original(c, addr);
    Cpu saved = *c;
    c->x[0] = app;
    c->x[1] = enabled(&rows[row]) && rows[row].type < RESET_PICTURE;
    rt_call_original(c, EDIT_ARROWS);
    *c = saved;
    return 1;
}
static int select_hook(Cpu *c, uint64_t addr) {
    uint64_t app = c->x[0];
    int row = row_at(app, (uint32_t)c->x[1]);
    if (row < 0) return 0;
    // Keep the native list's input armed. Confirm advances a setting or resets
    // its group, so keyboard, controller and pointer use the same native path.
    rt_call_original(c, addr);
    Cpu saved = *c;
    change(&rows[row], 1);
    refresh(c, app);
    *c = saved;
    return 1;
}
static int change_hook(Cpu *c, uint64_t addr) {
    Cpu saved = *c;
    int direction = (int32_t)c->x[1] < 0 ? -1 : 1;
    rt_call_original(c, GET_STATE);
    uint64_t state = c->x[0], handle = ptr(state + 0x78), object = ptr(handle + 0x60);
    uint64_t app = object >= 0x50 ? object - 0x50 : 0;
    int row = guest(app, 0x290) ? custom(*(const uint32_t *)(uintptr_t)(app + 0x284)) : -1;
    *c = saved;
    if (row < 0) return 0;
    change(&rows[row], direction);
    refresh(c, app);
    // The native arrow consumes input until its edit callback rearms the UI.
    c->x[0] = app; c->x[1] = 0;
    rt_call_original(c, UI_INPUT);
    rt_call_original(c, UI_REARM);
    *c = saved;
    (void)addr;
    return 1;
}
static int help_hook(Cpu *c, uint64_t addr) {
    int row = custom((uint32_t)c->x[1]);
    if (row < 0) return 0;
    // The original description table has 11 entries. Borrow index zero, then
    // provide our text through the message reader, including deferred reads.
    int previous = help_row;
    uint64_t app = c->x[0];
    help_row = row; c->x[1] = 0;
    rt_call_original(c, addr);
    help_row = previous;
    uint64_t model = ptr(app + 0xc0);
    if (guest(model, 0x70)) *(uint64_t *)(uintptr_t)(model + 0x58) = PC_HELP_ID + row;
    return 1;
}
static int message_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t key = ptr(c->x[1]);
    int row = help_row;
    if (row < 0 && key >= PC_HELP_ID && key < PC_HELP_ID + NROWS) row = (int)(key - PC_HELP_ID);
    if (row < 0) return 0;
    uint64_t buf = c->x[2];
    if (guest(buf, 0x80) && *(const uint8_t *)(uintptr_t)(buf + 0x7c)) {
        uint64_t data = ptr(buf + 0x58);
        unsigned cap = *(const uint16_t *)(uintptr_t)(buf + 0x78);
        if (cap && guest(data, cap * 2)) {
            unsigned n = 0;
            const char *s = rows[row].help;
            // Native text fits a single line unless given an explicit break.
            size_t len = strlen(s), split = len;
            if (len > 55) {
                split = len / 2;
                while (split && s[split] != ' ') split--;
            }
            while (s[n] && n + 1 < cap) {
                ((uint16_t *)(uintptr_t)data)[n] = n == split ? '\n' : (unsigned char)s[n];
                n++;
            }
            ((uint16_t *)(uintptr_t)data)[n] = 0;
            *(uint16_t *)(uintptr_t)(buf + 0x7a) = n;
        }
    }
    c->x[0] = 0;
    return 1;
}
void pc_settings_configure(int build) {
    static bool hooked;
    if (build != GAME_BUILD_EEVEE_100 || hooked) return;
    hooked = true;
    rt_register_fnhook(LIST_BUILD, build_hook);
    rt_register_fnhook(ROW_TEXT, text_hook);
    rt_register_fnhook(ROW_SELECT, select_hook);
    rt_register_fnhook(ROW_BIND, bind_hook);
    rt_register_fnhook(ROW_FOCUS, focus_hook);
    rt_register_fnhook(ROW_CHANGE, change_hook);
    rt_register_fnhook(HELP_UPDATE, help_hook);
    rt_register_fnhook(MESSAGE_GET, message_hook);
}
