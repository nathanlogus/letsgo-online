// Shared Adventure overlay (runtime/coop_scene.c): the ready prompt (Ready / Not Yet / Stay Out), the initiator's wait
// (Cancel / Continue Without) and its result, the notices, the mode settings and the Friends section with Catch up.
#include <stdio.h>
#include "imgui.h"
#include "settings_ui.h"
#include "coop_ui.h"
extern "C" {
#include "../coop_net.h"
#include "../coop_scene.h"
uint64_t plat_now_ns(void);
void input_reset(void);
void input_quarantine_pad(void);
}

static bool g_captures;
bool coop_scene_ui_captures(void) { return g_captures; }

static const char *peer_name(void) { static CoopRemote p; return coop_net_remote(&p) ? p.name : "Your friend"; }
static void gift_line(const CoopSceneDef *d) {
    if (d->item) ImGui::BulletText("%s x%u (one time)", d->item_name, d->count);
    ImGui::BulletText("The scene marked complete in your save");
}

void coop_scene_ui_draw(float scale, bool allowed) {
    CoopSceneView v;
    coop_scene_view(&v);
    uint64_t now = plat_now_ns();
    bool capture = allowed && v.asked;
    static uint64_t shown_at, focused;
    if (g_captures && !capture) { input_quarantine_pad(); input_reset(); }
    if (!g_captures && capture) shown_at = now;
    g_captures = capture;
    bool toast = now < v.toast_until_ns;
    if (!allowed || (!capture && !v.out_phase && !toast)) return;
    const char *name = peer_name();
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, 24 * scale), ImGuiCond_Always, ImVec2(.5f, 0));
    if (capture) ImGui::SetNextWindowSize(ImVec2(560 * scale, 0), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    if (!capture) flags |= ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | (v.out_phase == 1 ? 0 : ImGuiWindowFlags_NoInputs);
    bool new_prompt = capture && focused != v.in_id;
    if (new_prompt) { ImGui::SetNavCursorVisible(true); ImGui::SetNextWindowFocus(); focused = v.in_id; }
    if (!capture) focused = 0;
    ImGui::Begin("Shared Adventure", nullptr, flags);
    if (capture) {
        const CoopSceneDef *d = &coop_scenes[v.in_scene];
        bool armed = now - shown_at > 400000000ull;  // (an A pressed for the game as the prompt appears answers nothing)
        bool reached = !v.observed[v.in_scene] || v.reached[v.in_scene];
        ImGui::TextWrapped("%s completed a story scene: %s.", name, d->name);
        ImGui::TextWrapped("Ready gives your own save:");
        gift_line(d);
        if (!reached) ImGui::TextWrapped("Your story hasn't reached this point yet. Not Yet keeps it for Catch up.");
        ImGui::TextDisabled("Badges, gyms and the League stay personal. Expires in %us",
                            (unsigned)(v.in_expires_ns > now ? (v.in_expires_ns - now + 999999999) / 1000000000 : 0));
        ImGui::BeginDisabled(!reached);
        if (new_prompt && reached) ImGui::SetKeyboardFocusHere();
        bool ready = ui_button("Ready");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (new_prompt && !reached) ImGui::SetKeyboardFocusHere();
        bool later = ui_button("Not Yet") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        ImGui::SameLine();
        bool out = ui_button("Stay Out");
        if (armed && (ready || later || out)) {
            coop_scene_answer(ready ? COOP_ANSWER_READY : out ? COOP_ANSWER_STAY_OUT : COOP_ANSWER_NOT_YET);
            input_quarantine_pad(); input_reset();
        }
    } else if (v.out_phase == 1) {
        ImGui::Text("Waiting for %s: %s  %us", name, coop_scenes[v.out_scene].name,
                    (unsigned)(v.out_expires_ns > now ? (v.out_expires_ns - now + 999999999) / 1000000000 : 0));
        if (ui_button("Cancel")) coop_scene_cancel(false);
        ImGui::SameLine();
        if (ui_button("Continue Without")) coop_scene_cancel(true);
    } else if (v.out_phase == 2) {
        static const char *answers[] = {"", "%s is in: the scene is shared with them.", "%s: not yet. They can catch up later.",
            "%s stays out of this scene.", "%s has already completed this scene.", "%s isn't sharing story scenes."};
        static const char *closed[] = {"", "Both players need Shared Adventure on for this session.", "%s isn't nearby.",
            "Another scene offer is open.", "No friend in the room.", "%s didn't answer: it waits in their Catch up.",
            "%s left: it waits in their Catch up.", "The offer is no longer valid."};
        int r = v.out_result;
        ImGui::Text(r >= 11 && r <= 10 + COOP_CLOSED_STALE ? closed[r - 10] : r >= 1 && r <= COOP_ANSWER_NOT_SHARING ? answers[r] : "", name);
    } else ImGui::TextUnformatted(v.toast);
    ImGui::End();
}

// The overlay's mode line for the HUD.
const char *coop_scene_ui_mode(void) {
    CoopSceneView v;
    coop_scene_view(&v);
    if (!v.shared) return "Social mode";
    return v.consent ? v.peer_mode == COOP_MODE_ACTIVE ? "Shared Adventure: on" : "Shared Adventure: waiting for your friend"
                     : "Shared Adventure: not on this session";
}

// Settings (launcher, Friends): the mode in coop.ini; the session's consent and the scenes when connected.
void coop_scene_ui_settings(bool connected) {
    CoopOptions prefs;
    coop_net_options(&prefs);
    ImGui::Separator();
    ImGui::TextUnformatted("Adventure mode");
    bool shared = prefs.shared_adventure;
    if (ImGui::RadioButton("Social##adventure", !shared)) shared = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Shared##adventure", shared)) shared = true;
    if (shared != prefs.shared_adventure) { prefs.shared_adventure = shared; coop_net_preferences(&prefs); }
    if (!prefs.shared_adventure) {
        ImGui::TextWrapped("Social: your story, items and Pokedex stay your own. Nothing is shared.");
        return;
    }
    ImGui::TextWrapped("Shared: when your friend completes a mapped story scene, you choose to get its completion and one-time "
                       "gift in your own save. Badges, gyms, the rival and the League stay personal.");
    ImGui::TextUnformatted("Offer my scenes to"); ImGui::SameLine();
    bool nearby = prefs.scene_nearby;
    if (ImGui::RadioButton("Everyone##scene", !nearby)) nearby = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Nearby##scene", nearby)) nearby = true;
    if (nearby != prefs.scene_nearby) { prefs.scene_nearby = nearby; coop_net_preferences(&prefs); }
    CoopSceneView v;
    coop_scene_view(&v);
    if (connected) {
        bool consent = v.consent;
        if (ImGui::Checkbox("Share story scenes this session", &consent)) coop_scene_consent(consent);
        ImGui::TextDisabled("%s · your friend: %s", coop_scene_ui_mode(),
                            v.peer_mode == COOP_MODE_ACTIVE ? "sharing" : v.peer_mode == COOP_MODE_SHARED ? "Shared, not this session" : "Social");
    }
    ui_heading("Story scenes");
    for (int s = 0; s < COOP_SCENES; s++) {
        ImGui::PushID(s);
        static const char *state[] = {"", "Waiting: catch up when you're ready", "Accepted: arrives on the overworld",
                                      "Shared: in your save", "You stayed out"};
        ImGui::TextWrapped("%s%s", coop_scenes[s].name, v.completed[s] && !v.ledger[s] ? " (completed)" : "");
        if (v.ledger[s]) { ImGui::SameLine(); ImGui::TextDisabled("%s", state[v.ledger[s]]); }
        if (v.ledger[s] == COOP_LEDGER_PENDING) {
            ImGui::BeginDisabled(v.observed[s] && !v.reached[s]);
            if (ui_button("Catch up")) coop_scene_catch_up(s);
            ImGui::EndDisabled();
            if (v.observed[s] && !v.reached[s]) { ImGui::SameLine(); ImGui::TextDisabled("Your story hasn't reached it yet"); }
        } else if (connected && v.completed[s] && v.ledger[s] != COOP_LEDGER_ACCEPTED && v.ledger[s] != COOP_LEDGER_DONE) {
            ImGui::BeginDisabled(!v.consent || v.peer_mode != COOP_MODE_ACTIVE || v.out_phase == 1);
            if (ui_button("Share with your friend")) coop_scene_offer(s);
            ImGui::EndDisabled();
        }
        if (v.out_phase == 1 && v.out_scene == s) {
            if (ui_button("Cancel offer")) coop_scene_cancel(false);
            ImGui::SameLine();
            if (ui_button("Continue Without")) coop_scene_cancel(true);
        }
        ImGui::PopID();
    }
}
