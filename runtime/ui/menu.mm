// In-game overlay (Dear ImGui on the Vulkan renderer; input fed from SDL events): the "close the game?" question
// (Esc, F1 or Minus + Plus on a controller), the F12 settings (tabs: graphics, image and sound, advanced - the same
// pages as the start screen's, runtime/ui/settings_ui.mm), the name entry the game asks for (runtime/hle/swkbd.c), the
// FPS counter and the controller's finger on the touch screen (runtime/hle/input.c).
// LARECOMP_MENU_KEY=<SDL scancode name> ("F10", "Insert"...): another key for the F12 settings (Steam takes F12 for
// screenshots by default).
//
// Threads: SDL events arrive on the main thread and are queued; the overlay is laid out and drawn on the render
// thread inside backend_present, after the game image, into the same window image.
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "emote_wheel.h"
#include "osk.h"
#include "settings_ui.h"
#include "coop_ui.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
extern "C" uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);

extern "C" {
#include "../hle.h"
#include "../input_bind.h"
#include "../settings.h"
#include "menu.h"
#include "../paths.h"
void input_reset(void);
uint64_t rt_addr(uint64_t eevee);  // runtime/fnhook.c (0: a build without the tables the frame rate modes need)
bool input_finger_cursor(float *x, float *y, bool *down);  // runtime/hle/input.c
bool renderer_world_to_screen(float x, float y, float z, float *sx, float *sy, bool *on_screen);  // runtime/nvn/vulkan/exec.c
bool fps60_world_active(void);  // runtime/hle/fps60.c
bool fps60_social_overworld(void);
bool coop_menu_visible(void);  // runtime/hle/coop_menu.c
bool coop_menu_emote_page(int *page, int *pages, uint8_t ids[3]);
#include "../coop_net.h"
#include "../ui2x.h"
#include "../platform.h"
}

// ------------------------------------------------------------------ language
// English, Portuguese or Russian (issue #1), from the system's language (runtime/ui/settings_ui.h)
#define M UI_T

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};  // the "close the game?" question is up
static std::atomic<bool> g_cfg{false};   // the F12 settings are up
static std::atomic<bool> g_friends{false};
static int g_cfg_tab;                    // the F12 settings' tab: graphics, image and sound, advanced (render thread)
// Ctrl (the controller's top face button: Y on Xbox, X on Nintendo) in the F12 settings: half the size, in the bottom
// right corner, no darkening of the game - to tune the effects seeing most of the image. Hidden: not in the prompts.
// (LARECOMP_MENU_SMALL=1: so from the start, for screenshots)
static bool g_cfg_small = getenv("LARECOMP_MENU_SMALL") != NULL;
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

// SDL events are handled on the window thread. Preserve independent physical
// d-pad and synthesized stick directions when either source is released.
static bool nav_pad[4],nav_stick[4];
static const ImGuiKey nav_direction_keys[4]={ImGuiKey_GamepadDpadLeft,ImGuiKey_GamepadDpadRight,ImGuiKey_GamepadDpadUp,ImGuiKey_GamepadDpadDown};
static int nav_direction(ImGuiKey key){for(int i=0;i<4;i++)if(key==nav_direction_keys[i])return i;return -1;}
static void nav_stick_direction(int direction,bool down){
    nav_stick[direction]=down;QEvent q={};q.kind=QEvent::KEY;q.key=nav_direction_keys[direction];
    q.down=nav_stick[direction] || nav_pad[direction];push(q);
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the question had focus leaks into the game
    if (!open) settings_save();
}
static void set_cfg(bool open) {
    if (open == g_cfg.load()) return;
    if (open) set_open(false);
    g_cfg.store(open);
    input_reset();
    if (!open) settings_save();
}
// (LARECOMP_MENU_AT: runtime/nvn/vulkan/device.c; LARECOMP_MENU_PAGE=cfg / adv / <tab number>: the F12 settings on that
// tab instead - cfg the first, adv the advanced one)
extern "C" void menu_test_open(void) {
    const char *pg = getenv("LARECOMP_MENU_PAGE");
    if (!pg) { set_open(true); return; }
    g_cfg_tab = !strcmp(pg, "adv") ? 2 : atoi(pg);
    set_cfg(true);
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

// A confirms, B goes back, as in the game: by the controller layout (runtime/hle/bindings.c) - Nintendo: A the right
// face button, B the bottom one; Xbox: A the bottom one, B the right one.
static ImGuiKey key_from_pad(Uint8 b) {
    bool xbox = bind_layout() == BIND_LAYOUT_XBOX;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return xbox ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;   // activate (Nintendo)
    case SDL_GAMEPAD_BUTTON_SOUTH: return xbox ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;  // cancel / back (Nintendo)
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry) ------------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[1024];
static char g_kbd_title[128];
static char g_copy_text[256];
static std::atomic<bool> g_copy_pending{false};
extern "C" void menu_copy_text(const char *text) {
    pthread_mutex_lock(&q_lock); snprintf(g_copy_text,sizeof g_copy_text,"%s",text ? text : "");
    g_copy_pending = true; pthread_mutex_unlock(&q_lock);
}
static int g_kbd_max = 8;
static bool capturing() { return coop_battle_ui_captures() || coop_invite_ui_captures() || emote_wheel_open() || g_open.load() || g_cfg.load() || g_friends.load() || g_kbd.load() == 1; }
// The game gets no input while this is true (runtime/hle/input.c): the question, the settings or the name entry is up.
extern "C" bool menu_input_captured(void) { return capturing(); }
// Friends is a social overlay over a still-running overworld. Keep the trainer
// visible to peers while choosing emotes; game menus and modal settings are busy.
extern "C" bool menu_social_busy(void) { return g_open.load() || g_cfg.load() || g_kbd.load() == 1; }

static SDL_Scancode cfg_key(void) {
    static SDL_Scancode k = SDL_SCANCODE_UNKNOWN;
    if (k == SDL_SCANCODE_UNKNOWN) {
        const char *e = getenv("LARECOMP_MENU_KEY");
        k = e && *e ? SDL_GetScancodeFromName(e) : SDL_SCANCODE_UNKNOWN;
        if (k == SDL_SCANCODE_UNKNOWN) k = SDL_SCANCODE_F12;
    }
    return k;
}

bool menu_handle_event(const SDL_Event *e) {
    if (emote_wheel_event(e)) return true;
    static bool back_down, start_down;
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    bool typing = g_kbd.load() == 1;  // (the name entry: keys are text, not shortcuts)
    if (typing && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (typing && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // Battle prompts own Escape too; it must not open the quit dialog while
    // declining an invitation or returning from target selection.
    if((coop_battle_ui_captures() || coop_invite_ui_captures()) && e->type==SDL_EVENT_KEY_DOWN && e->key.scancode==SDL_SCANCODE_ESCAPE){
        QEvent q={};q.kind=QEvent::KEY;q.key=ImGuiKey_Escape;q.down=true;push(q);
        q.down=false;push(q);return true;
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!typing && !e->key.repeat && (e->key.scancode == SDL_SCANCODE_F8 ||
            (e->key.scancode == SDL_SCANCODE_ESCAPE && g_friends.load()))) {
            g_friends.store(e->key.scancode == SDL_SCANCODE_ESCAPE ? false : !g_friends.load());
            set_cfg(false); set_open(false); input_reset(); return true;
        }
        if (!e->key.repeat && !typing && (e->key.scancode == cfg_key() || (e->key.scancode == SDL_SCANCODE_ESCAPE && g_cfg.load()))) {
            set_cfg(e->key.scancode == SDL_SCANCODE_ESCAPE ? false : !g_cfg.load());  // (Esc closes the settings)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) { set_cfg(false); set_open(!g_open.load()); }  // (Esc with the question up goes to it below: "Não")
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        bool captured=capturing();
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        // A confirm can hide its overlay on key-down. Still deliver the later
        // release to ImGui, or the next prompt sees a permanently held key.
        if (!captured && down) return false;
        // This overlay feeds ImGui directly rather than through the launcher's
        // SDL backend. Modifier events are required for paste, select-all, etc.
        const ImGuiKey mods[] = {ImGuiMod_Ctrl,ImGuiMod_Shift,ImGuiMod_Alt,ImGuiMod_Super};
        const SDL_Keymod masks[] = {SDL_KMOD_CTRL,SDL_KMOD_SHIFT,SDL_KMOD_ALT,SDL_KMOD_GUI};
        for (int i=0;i<4;i++) { QEvent q={}; q.kind=QEvent::KEY; q.key=mods[i]; q.down=(e->key.mod&masks[i])!=0; push(q); }
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return captured;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_cfg(false);
            set_open(!g_open.load());
            return true;
        }
        bool captured=capturing();
        if (!captured && down) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        int direction=nav_direction(k);
        if(direction>=0){nav_pad[direction]=down;down=down || nav_stick[direction];}
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return captured;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        bool captured=capturing();
        // Neutralize navigation on releases after an overlay has closed too.
        float v = captured ? e->gaxis.value / 32767.0f : 0;
        bool battle_nav=captured && (coop_battle_ui_captures() || coop_invite_ui_captures());
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            nav_stick_direction(0,battle_nav && v<-.3f);nav_stick_direction(1,battle_nav && v>.3f);
            if(battle_nav)v=0; // ImGui otherwise uses the stick for scrolling.
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            nav_stick_direction(2,battle_nav && v<-.3f);nav_stick_direction(3,battle_nav && v>.3f);
            if(battle_nav)v=0;
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        default: break;
        }
        return captured;
    }
    case SDL_EVENT_MOUSE_MOTION:
        // (always: the sender's waiting notice takes a click on its Cancel without capturing the game)
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return capturing();
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing() && !coop_invite_ui_hovered()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    if (g_copy_pending.exchange(false)) {
        pthread_mutex_lock(&q_lock); SDL_SetClipboardText(g_copy_text); pthread_mutex_unlock(&q_lock);
    }
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    // F11 / Alt+Enter only: the setting itself is applied where the window is made (ui_window_init) and handed to the game
    // (runtime/ui/launcher_lg.mm window_as_game) - applied here on the first call too, the start screen (which calls this
    // each frame) went back to full screen right after taking its own size
    if (g_want_fullscreen_apply.exchange(false)) SDL_SetWindowFullscreen(w, g_settings.fullscreen);
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours: the start screen has its own context
static ImFont *g_font, *g_font_big;  // the FPS counter and the name entry
static ImFont *g_menu_font;          // the "close the game?" question (Baloo 2, as on the start screen)

static void init(void) {
    IMGUI_CHECKVERSION();
    ui_pick_language();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    UiFonts f = ui_load_fonts();
    g_font = f.body, g_font_big = f.big, g_menu_font = f.title;
    ui_style();
    g_ui_tips_in_box = true;  // (the settings show the descriptions in their box)
    vk_imgui_backend_init();
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

// Reboot (runtime/main.c): the game is gone - the question and the name entry close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    g_friends = false;
    set_open(false);
    set_cfg(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ui_window_backdrop();
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(UI_TEXT, "%s", g_kbd_title[0] ? g_kbd_title : M("Enter a name", "Digite o nome", "Введите имя"));
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
        ImGui::TextDisabled("%s      (%d / %d)", M("Enter  confirms      Esc  cancels", "Enter  confirma      Esc  cancela",
                                                   "Enter  готово      Esc  отмена"),
                            (int)strlen(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run_labeled(const char *title, const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    snprintf(g_kbd_title,sizeof g_kbd_title,"%s",title ? title : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}
extern "C" int menu_keyboard_run(const char *initial,int max_len,char *out,int cap) {
    return menu_keyboard_run_labeled(NULL,initial,max_len,out,cap);
}

// "Close the game?" - Não has the focus first (Enter / A by reflex keeps playing); B / Esc answer Não.
static void quit_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
    ImGui::Begin("##quit", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ui_window_backdrop();
    ui_title(g_menu_font, M("Close the game?", "Deseja fechar o jogo?", "Закрыть игру?"));
    const char *lost = M("Progress since the last save will be lost.", "O progresso desde o último save será perdido.",
                         "Прогресс после последнего сохранения будет потерян.");
    ImGui::TextDisabled("%s", lost);
    ImGui::Spacing();
    // the two answers side by side, Settings under them: together as wide as the text (at least 2 x 240 px)
    float sp = ImGui::GetStyle().ItemSpacing.x, w = fmaxf(ImGui::CalcTextSize(lost).x, 480 * scale + sp), bw = (w - sp) / 2;
    static bool focus_no;
    if (!focus_no) { ImGui::SetKeyboardFocusHere(1); focus_no = true; }
    if (ImGui::Button(M("Yes", "Sim", "Да"), ImVec2(bw, 0))) {
        settings_save();
        SDL_Event q = {};
        q.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&q);
    }
    ImGui::SameLine();
    bool no = ImGui::Button(M("No", "Não", "Нет"), ImVec2(bw, 0));
    if (ImGui::Button("Friends (F8)", ImVec2(w, 0))) {
        set_open(false); g_friends = true; input_reset();
    }
    if (ImGui::Button(M("Settings (F12)", "Configurações (F12)", "Настройки (F12)"), ImVec2(w, 0))) {
        focus_no = false;
        set_cfg(true);  // (closes the question: the way in from a controller)
    } else if (no || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        set_open(false);
        focus_no = false;
    }
    ImGui::End();
}

// A button prompt as in the game's bottom strip: the button (a coloured circle with its letter, or a key's name on a
// pill) then what it does, from x on the line centred at y. Returns its width; draw = false only measures it.
static float prompt(ImDrawList *dl, float x, float y, const char *key, ImU32 col, const char *text, bool draw) {
    float h = ImGui::GetFrameHeight() * 0.78f, gap = h * 0.25f;
    ImVec2 ks = ImGui::CalcTextSize(key), ts = ImGui::CalcTextSize(text);
    float kw = strlen(key) == 1 ? h : ks.x + h * 0.7f;
    if (draw) {
        ImVec2 a(x, y - h * 0.5f), b(x + kw, y + h * 0.5f);
        dl->AddRectFilled(a, b, col, h * 0.5f);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), h * 0.5f, 0, h * 0.07f);
        dl->AddText(ImVec2(x + (kw - ks.x) * 0.5f, y - ks.y * 0.5f), IM_COL32(255, 255, 255, 255), key);
        dl->AddText(ImVec2(x + kw + gap, y - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), text);
    }
    return kw + gap + ts.x;
}

// The description box: the text of the setting under the cursor (ui_tip), else the page's, on the rows' text column
// with as much room above it as at its left; at the text's size unless it is longer than the box (smaller then)
static void description(ImDrawList *dl, ImVec2 a, ImVec2 b, float pad_y, const char *text) {
    ui_info_box(dl, a, b);
    float in = ui_inset(), ww = b.x - a.x - in * 2, hh = b.y - a.y - pad_y * 2;
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    for (float f = 1.0f; f >= 0.6f; f -= 0.05f) {
        size = ImGui::GetFontSize() * f;
        if (font->CalcTextSizeA(size, FLT_MAX, ww, text).y <= hh) break;
    }
    dl->AddText(font, size, ImVec2(a.x + in, a.y + pad_y), ImGui::GetColorU32(ImGuiCol_Text), text, NULL, ww);
}

// F12: the settings, applied live (runtime/ui/settings_ui.mm: the start screen's pages); saved to larecomp.ini on closing.
// B / Esc / F12 close; L / R on a controller change the tab. The look of the game's Options screen: the title and the
// tabs, the description box, the settings as pills, the button prompts on a white strip at the bottom.
static void cfg_draw(ImGuiIO &io, float scale) {
    // (small: scale is already half the window's - menu_render; the margin a fortieth of the window's height)
    float margin = roundf(io.DisplaySize.y * 0.025f);
    if (!g_cfg_small) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 70));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    } else {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin), ImGuiCond_Always, ImVec2(1, 1));
    }
    // one size for every tab (a page taller than it scrolls; pad navigation follows)
    float ww = fminf(io.DisplaySize.x * 0.92f, 1120 * scale), wh = fminf(io.DisplaySize.y * 0.92f, 900 * scale);
    if (g_cfg_small) ww = fminf(ww, io.DisplaySize.x * 0.5f - margin), wh = fminf(wh, io.DisplaySize.y * 0.5f - margin);
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
    ImGui::Begin("##cfg", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ui_window_backdrop();
    const ImGuiStyle &st = ImGui::GetStyle();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    float fh = ImGui::GetFrameHeight();
    bool popup = ImGui::IsPopupOpen((const char *)NULL, ImGuiPopupFlags_AnyPopupId);

    // the title, and the tabs at its right as pills between L and R (a click picks one too)
    ui_title(g_menu_font, M("Settings", "Configurações", "Настройки"));
    ImVec2 t0 = ImGui::GetItemRectMin(), t1 = ImGui::GetItemRectMax();
    if (!popup) ui_tabs_begin(&g_cfg_tab, 3);
    {
        const char *names[] = {M("Graphics", "Gráficos", "Графика"), M("Image and sound", "Imagem e som", "Изображение и звук"),
                               M("Advanced", "Avançado", "Дополнительно")};
        float pad = fh * 0.55f, gap = st.ItemSpacing.x, chip = fh * 0.95f, w[3], total = chip * 2 + gap * 4;
        for (int t = 0; t < 3; t++) total += w[t] = ImGui::CalcTextSize(names[t]).x + pad * 2;
        float x = wp.x + ws.x - st.WindowPadding.x - total, y = (t0.y + t1.y) * 0.5f;
        auto shoulder = [&](float cx, const char *k) {  // L / R: a dark rounded chip
            ImVec2 a(cx, y - fh * 0.36f), b(cx + chip, y + fh * 0.36f), ks = ImGui::CalcTextSize(k);
            dl->AddRectFilled(a, b, IM_COL32(70, 76, 66, 255), fh * 0.2f);
            dl->AddText(ImVec2(cx + (chip - ks.x) * 0.5f, y - ks.y * 0.5f), IM_COL32(255, 255, 255, 255), k);
        };
        shoulder(x, "L");
        x += chip + gap;
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int t = 0; t < 3; t++) {
            ImVec2 a(x, y - fh * 0.5f), b(x + w[t], y + fh * 0.5f);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(t);
            if (ImGui::InvisibleButton("##tab", ImVec2(w[t], fh))) g_cfg_tab = t;
            ImGui::PopID();
            bool sel = g_cfg_tab == t;
            ui_pill(dl, a, b, sel);
            if (!sel && ImGui::IsItemHovered()) dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), fh * 0.5f, 0, 2.5f * scale);
            ImVec2 ts = ImGui::CalcTextSize(names[t]);
            dl->AddText(ImVec2(a.x + (w[t] - ts.x) * 0.5f, y - ts.y * 0.5f), ImGui::GetColorU32(sel ? ImGuiCol_Text : ImGuiCol_TextDisabled), names[t]);
            x += w[t] + gap;
        }
        ImGui::PopItemFlag();
        shoulder(x, "R");  // (the row ends on the box's right edge)
        ImGui::SetCursorScreenPos(ImVec2(t0.x, fmaxf(t1.y, y + fh * 0.5f) + 16 * scale));
    }

    // the description box: two lines of text
    {
        float pad_y = ui_inset() * 0.75f;
        ImVec2 a = ImGui::GetCursorScreenPos(), b(wp.x + ws.x - st.WindowPadding.x, a.y + ImGui::GetTextLineHeight() * 2 + pad_y * 2);
        const char *d = ui_tip_last();
        description(dl, a, b, pad_y, d ? d : M("Changes apply at once and are saved when this menu closes. Left / right change the highlighted setting.",
                                        "As mudanças valem na hora e são salvas ao fechar este menu. Esquerda / direita mudam a opção destacada.",
                                        "Изменения применяются сразу и сохраняются при закрытии меню. Влево / вправо — изменить выбранный параметр."));
        ImGui::Dummy(ImVec2(b.x - a.x, b.y - a.y + 16 * scale - st.ItemSpacing.y));  // (16 px to the page, as above the box)
    }

    // the page, above the strip at the bottom: as wide as the box (the cursor's arrow in the window's margin)
    float strip = fh * 1.45f, page_h = wp.y + ws.y - strip - st.ItemSpacing.y - ImGui::GetCursorScreenPos().y;
    bool known = rt_addr(0x70002f8bb0ull) != 0;
    ImGui::PushID(g_cfg_tab);  // (each page its own scroll)
    ui_page_begin("##page", ImVec2(0, page_h));
    static int shown = -1;  // (another page: the cursor on its first setting)
    if (shown != g_cfg_tab && shown >= 0 && GImGui->NavCursorVisible) ImGui::SetKeyboardFocusHere();
    shown = g_cfg_tab;
    if (g_cfg_tab == 0) ui_page_graphics(ui_graphics_x(NULL, 0), known);
    else if (g_cfg_tab == 1) ui_page_image_sound();
    else ui_page_advanced();
    ui_page_end();
    ImGui::PopID();

    // the strip: L / R change the tab, B closes (Esc / F12 on a keyboard); a click on Close closes too
    bool close = !popup && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    if (!popup && (ImGui::IsKeyPressed(ImGuiKey_LeftCtrl, false) || ImGui::IsKeyPressed(ImGuiKey_RightCtrl, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)))
        g_cfg_small = !g_cfg_small;
    {
        ImVec2 a(wp.x, wp.y + ws.y - strip), b(wp.x + ws.x, wp.y + ws.y);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 245), st.WindowRounding, ImDrawFlags_RoundCornersBottom);
        float y = (a.y + b.y) * 0.5f, x = b.x - st.WindowPadding.x, sp = fh * 0.7f;
        bool pad = g_last_pad.load();
        const char *l_close = M("Close", "Fechar", "Закрыть"), *l_tab = M("Tabs", "Abas", "Вкладки");
        char k_close[48];
        snprintf(k_close, sizeof k_close, pad ? "B" : "Esc / %s", SDL_GetScancodeName(cfg_key()));
        ImU32 orange = IM_COL32(240, 160, 36, 255), grey = IM_COL32(70, 76, 66, 255);
        float wc = prompt(dl, 0, 0, k_close, orange, l_close, false);
        x -= wc;
        prompt(dl, x, y, k_close, pad ? orange : grey, l_close, true);
        ImGui::SetCursorScreenPos(ImVec2(x, y - fh * 0.5f));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        if (ImGui::InvisibleButton("##close", ImVec2(wc, fh))) close = true;
        ImGui::PopItemFlag();
        if (pad) {  // (L R  Tabs)
            float wl = prompt(dl, 0, 0, "L", grey, "", false), wt = wl + prompt(dl, 0, 0, "R", grey, l_tab, false);
            x -= sp + wt;
            prompt(dl, x, y, "L", grey, "", true);
            prompt(dl, x + wl, y, "R", grey, l_tab, true);
        }
    }
    ImGui::End();
    if (close) set_cfg(false);
}

struct CoopPlate {
    bool visible = false;
    float px = 0, py = 0;
    ImVec2 a, b;
    const char *name = "";
    uint8_t slot = 0;
};

static void clamp_coop_plate(CoopPlate &p, float scale) {
    ImVec2 size(p.b.x - p.a.x, p.b.y - p.a.y), display = ImGui::GetIO().DisplaySize;
    p.a.x = fmaxf(8 * scale, fminf(p.a.x, display.x - size.x - 8 * scale));
    p.a.y = fmaxf(8 * scale, fminf(p.a.y, display.y - size.y - 8 * scale));
    p.b = ImVec2(p.a.x + size.x, p.a.y + size.y);
}

static CoopPlate make_coop_plate(const char *name, float px, float py, float scale, uint8_t slot) {
    CoopPlate p;
    p.visible = true; p.name = name; p.px = px; p.py = py; p.slot = slot;
    ImVec2 text = ImGui::CalcTextSize(name);
    float w = text.x * 1.15f + 24 * scale, h = text.y * 1.15f + 14 * scale;
    p.a = ImVec2(px - w * 0.5f, py - h - 12 * scale);
    p.b = ImVec2(p.a.x + w, p.a.y + h);
    clamp_coop_plate(p, scale);
    return p;
}

static bool plates_overlap(const CoopPlate &a, const CoopPlate &b) {
    return a.visible && b.visible && a.a.x < b.b.x && b.a.x < a.b.x && a.a.y < b.b.y && b.a.y < a.b.y;
}

static void separate_coop_plates(CoopPlate &local, CoopPlate &remote, float scale) {
    if (!plates_overlap(local, remote)) return;
    float gap = 10 * scale, mid = (local.px + remote.px) * 0.5f;
    float lw = local.b.x - local.a.x, rw = remote.b.x - remote.a.x;
    local.a.x = mid - gap * 0.5f - lw; local.b.x = local.a.x + lw;
    remote.a.x = mid + gap * 0.5f; remote.b.x = remote.a.x + rw;
    clamp_coop_plate(local, scale); clamp_coop_plate(remote, scale);
    if (plates_overlap(local, remote)) {
        float h = local.b.y - local.a.y;
        local.b.y = remote.a.y - gap; local.a.y = local.b.y - h;
        clamp_coop_plate(local, scale);
    }
}

struct CoopButtonTextures { ImTextureID fill = 0, line = 0; };

static const CoopButtonTextures &coop_button_textures(void) {
    static CoopButtonTextures textures;
    static bool tried;
    if (!tried) {
        tried = true;
        const char *names[] = {"button_option_44px_fill_00^s", "button_option_44px_line_00^s"};
        ImTextureID *ids[] = {&textures.fill, &textures.line};
        for (int i = 0; i < 2; i++) {
            uint8_t *rgba = NULL;
            uint32_t w = 0, h = 0;
            if (ui_game_texture_rgba("bin/app/system_window/bin/system_window_button_00.arc", names[i], &rgba, &w, &h)) {
                // The game uses BC4 single-channel masks, tinted by its UI material.
                for (uint64_t p = 0; p < (uint64_t)w * h; p++) {
                    rgba[p*4+3] = rgba[p*4];
                    rgba[p*4] = rgba[p*4+1] = rgba[p*4+2] = 255;
                }
                *ids[i] = (ImTextureID)vk_imgui_texture_rgba(rgba, (int)w, (int)h);
                free(rgba);
            }
        }
        LOG("[coop-ui] nameplates: %s; P1 blue / P2 coral\n", textures.fill && textures.line ? "native button textures" : "button-style fallback");
    }
    return textures;
}

static ImU32 coop_player_colour(uint8_t slot) {
    return slot == 1 ? IM_COL32(255, 174, 186, 255) : IM_COL32(142, 213, 255, 255);
}

static void coop_button_slice(ImDrawList *dl, ImTextureID texture, ImVec2 a, ImVec2 b, ImU32 tint) {
    // The native 16x44 texture is a left cap. Stretch its last column for the
    // centre and mirror the cap for the right end, preserving the button's corners.
    float cap = fminf((b.y - a.y) * (16.0f / 44.0f), (b.x - a.x) * 0.5f);
    dl->AddImage(texture, a, ImVec2(a.x + cap, b.y), ImVec2(0, 0), ImVec2(1, 1), tint);
    dl->AddImage(texture, ImVec2(a.x + cap, a.y), ImVec2(b.x - cap, b.y), ImVec2(15.5f/16, 0), ImVec2(15.5f/16, 1), tint);
    dl->AddImage(texture, ImVec2(b.x - cap, a.y), b, ImVec2(1, 0), ImVec2(0, 1), tint);
}

static void draw_coop_plate(ImDrawList *dl, const CoopPlate &p, float scale) {
    if (!p.visible) return;
    float cx = (p.a.x + p.b.x) * 0.5f;
    ImU32 colour = coop_player_colour(p.slot);
    dl->AddLine(ImVec2(p.px, p.py), ImVec2(cx, p.b.y), colour, 2 * scale);
    dl->AddCircleFilled(ImVec2(p.px, p.py), 3 * scale, colour, 12);
    const CoopButtonTextures &textures = coop_button_textures();
    if (textures.fill && textures.line) {
        coop_button_slice(dl, textures.fill, p.a, p.b, colour);
        coop_button_slice(dl, textures.line, p.a, p.b, IM_COL32(255, 255, 255, 245));
    } else {
        dl->AddRectFilled(p.a, p.b, colour, (p.b.y - p.a.y) * 0.5f);
        dl->AddRect(p.a, p.b, IM_COL32_WHITE, (p.b.y - p.a.y) * 0.5f, 0, 1.5f * scale);
    }
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.15f,
                ImVec2(p.a.x + 12 * scale, p.a.y + 7 * scale), IM_COL32(36, 46, 44, 255), p.name);
}

void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    bool open = g_open.load(), cfg = g_cfg.load(), kbd = g_kbd.load() == 1;
    emote_wheel_available(fps60_social_overworld() && !coop_battle_ui_captures() && !coop_invite_ui_captures() && !open && !cfg && !kbd && !g_friends.load() && !coop_menu_visible());
    float fx, fy;
    bool fdown, finger = !capturing() && input_finger_cursor(&fx, &fy, &fdown);
    CoopRemote peer = {};
    bool remote_peer = coop_net_enabled() && coop_net_remote(&peer);
    if (!open && !cfg && !kbd && !g_friends.load() && !g_settings.show_fps && !finger && !coop_net_enabled() && !fps60_world_active()) {
        // Keep releases queued in ImGui even when no overlay needs drawing.
        // Discarding them here leaves the previous confirm held on reopening.
        if (g_inited) drain_events(ImGui::GetIO());
        return;
    }
    if (!g_inited) init();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)target_w, (float)target_h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built for a ~1080 px tall output; scale to the window (spacing / rounding too)
    static UiScaler scaler;
    float scale = ui_scale(scaler, (float)target_h * (cfg && !open && g_cfg_small ? 0.5f : 1.0f));
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.5f));  // (dark: over the game, not the menus' look)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * scale);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps", io.Framerate);
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    if (coop_net_enabled() && !remote_peer && !capturing() && !coop_menu_visible()) {
        CoopSession session = {}; coop_net_session(&session);
        ImGui::SetNextWindowPos(ImVec2(16*scale,16*scale),ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(.72f);
        ImGui::Begin("##coop-status",NULL,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        if (session.connection == COOP_CONNECTED) ImGui::TextUnformatted("Online co-op | Waiting for a friend | Friends: F8");
        else if (session.connection == COOP_ERROR) ImGui::TextUnformatted("Online co-op | Connection failed | Friends: F8");
        else ImGui::Text("Online co-op | %s (%u/3) | Friends: F8",session.connection == COOP_RECONNECTING ? "Reconnecting" : "Connecting",session.attempt);
        ImGui::End();
    }
    if (remote_peer && !capturing() && !coop_menu_visible()) {
        ImGui::SetNextWindowPos(ImVec2(16 * scale, 16 * scale), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::Begin("##coop-peer", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        CoopNetStats net_stats = {};
        coop_net_stats(&net_stats);
        if (net_stats.relay_rtt_ms >= 0)
            ImGui::Text("Online co-op  |  %s  |  Relay %.0f ms", peer.name, net_stats.relay_rtt_ms);
        else
            ImGui::Text("Online co-op  |  %s", peer.name);
        CoopTeamUp team={};coop_net_teamup(&team);
        if(team.state==1 && team.from_slot!=coop_net_local_slot())
            ImGui::TextUnformatted("Tag-up invitation | Friends: F8 / Communicate");
        else if(team.state==2){CoopOptions o;coop_net_options(&o);
            ImGui::TextUnformatted(o.teamup_battles ? "Tagged up | Team-up trainer battles" : "Tagged up for travel | Individual battles");}
        ImGui::End();

        CoopPlate local_plate;
        char local_name[25] = {};
        float local_x, local_y, local_z, local_px, local_py;
        bool local_on_screen = false;
        if (fps60_world_active() && coop_net_local_head(&local_x, &local_y, &local_z, local_name) &&
            renderer_world_to_screen(local_x, local_y, local_z, &local_px, &local_py, &local_on_screen) && local_on_screen)
            local_plate = make_coop_plate(local_name, local_px, local_py, scale, coop_net_local_slot());

        float px, py, head_x, head_y, head_z;
        bool on_screen = false;
        // The game hook publishes the rendered native avatar's head anchor, after pose
        // correction. No marker is drawn before that model exists or in another room.
        if (fps60_world_active() && coop_net_avatar_head(&head_x, &head_y, &head_z) &&
            renderer_world_to_screen(head_x, head_y, head_z, &px, &py, &on_screen)) {
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            float r = (on_screen ? 4 : 11) * scale;
            ImVec2 at(px, py);
            dl->AddCircleFilled(at, r, on_screen ? coop_player_colour(peer.slot) : IM_COL32(255, 170, 40, 230), 20);
            dl->AddCircle(at, r, IM_COL32(255, 255, 255, 240), 20, 2.5f * scale);
            if (!on_screen) {
                // A chevron at the viewport edge points toward a peer outside the camera view.
                float dx = px - io.DisplaySize.x * 0.5f, dy = py - io.DisplaySize.y * 0.5f;
                float len = sqrtf(dx * dx + dy * dy);
                if (len > 0) dx /= len, dy /= len;
                float tx = -dy, ty = dx, tip = 15 * scale, wing = 8 * scale;
                dl->AddTriangleFilled(ImVec2(px + dx * tip, py + dy * tip),
                                      ImVec2(px - dx * tip + tx * wing, py - dy * tip + ty * wing),
                                      ImVec2(px - dx * tip - tx * wing, py - dy * tip - ty * wing),
                                      IM_COL32(255, 170, 40, 255));
            }
            CoopPlate remote_plate = make_coop_plate(peer.name, px, py, scale, peer.slot);
            separate_coop_plates(local_plate, remote_plate, scale);
            draw_coop_plate(dl, remote_plate, scale);
            ImVec2 label_pos = remote_plate.a;

            if (on_screen) emote_bubble_draw(peer.emote, peer.emote_received_ns, px, label_pos.y-8*scale, scale);
        }
        draw_coop_plate(ImGui::GetForegroundDrawList(), local_plate, scale);
    }
    if (fps60_social_overworld() && !capturing()) {
        uint8_t emote=0; uint64_t at=0; float x,y,z,px,py; bool visible=false;
        if (coop_net_local_emote(&emote,&at,&x,&y,&z) &&
            renderer_world_to_screen(x,y,z,&px,&py,&visible) && visible) {
            char name[25]; float hx,hy,hz;
            float bottom=py-14*scale;
            if (coop_net_local_head(&hx,&hy,&hz,name)) bottom=make_coop_plate(name,px,py,scale,coop_net_local_slot()).a.y-8*scale;
            emote_bubble_draw(emote,at,px,bottom,scale);
        }
    }
    int emote_page, emote_pages; uint8_t emote_ids[3];
    if (coop_menu_emote_page(&emote_page, &emote_pages, emote_ids)) {
        // Communicate's three-choice layout has no spare text pane: the page and its marks sit in a strip below it.
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        char page[32]; snprintf(page, sizeof page, "Page %d/%d", emote_page, emote_pages);
        ImU32 grey = IM_COL32(70, 76, 66, 255);
        float icon = 52 * scale, gap = 12 * scale, pad = 18 * scale, h = icon + 14 * scale;
        float w = pad * 2 + prompt(dl, 0, 0, "L", grey, "", false) + 3 * icon + 2 * gap + gap +
                  prompt(dl, 0, 0, "R", grey, page, false);
        ImVec2 a(io.DisplaySize.x * 0.5f - w * 0.5f, io.DisplaySize.y - h - 24 * scale), b(a.x + w, a.y + h);
        float x = a.x + pad, y = (a.y + b.y) * 0.5f;
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 251, 240), h * 0.5f);
        dl->AddRect(a, b, IM_COL32(210, 217, 211, 255), h * 0.5f, 0, 1.5f * scale);
        x += prompt(dl, x, y, "L", grey, "", true);
        for (int i = 0; i < 3; i++, x += icon + gap) emote_icon_draw(emote_ids[i], x + icon * 0.5f, y, icon);
        prompt(dl, x, y, "R", grey, page, true);
    }
    if (finger) {  // the controller's finger on the touch screen (right stick moves it, ZR touches)
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        float r = 26 * scale;
        dl->AddCircleFilled(ImVec2(fx, fy), r, fdown ? IM_COL32(255, 255, 255, 170) : IM_COL32(255, 255, 255, 60));
        dl->AddCircle(ImVec2(fx, fy), r, IM_COL32(255, 255, 255, 235), 0, 4 * scale);
    }
    if (g_friends.load() && !kbd) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0,0), io.DisplaySize, IM_COL32(0,0,0,90));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*.5f,io.DisplaySize.y*.5f), ImGuiCond_Always, ImVec2(.5f,.5f));
        ImGui::SetNextWindowSize(ImVec2(fminf(io.DisplaySize.x*.92f,900*scale),io.DisplaySize.y*.88f), ImGuiCond_Always);
        ImGui::Begin("Friends", NULL, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
        coop_ui_page(false);
        if (ui_button("Back to game (F8)" ) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) { g_friends = false; input_reset(); }
        ImGui::End();
    }
    else if (open) quit_draw(io, scale);
    else if (cfg && !kbd) cfg_draw(io, scale);
    if (kbd) keyboard_draw(io, scale);
    emote_wheel_draw(scale);
    coop_battle_ui_draw(scale);
    coop_invite_ui_draw(scale, fps60_social_overworld() && !open && !cfg && !kbd && !g_friends.load() && !emote_wheel_open() &&
                               !coop_menu_visible() && !coop_battle_ui_captures());
    ui_frame_end();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
}
