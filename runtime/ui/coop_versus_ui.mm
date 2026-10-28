// Practice battles and spectating (runtime/coop_versus.c): the Friends controls, and the spectator's read-only panel.
// The request pop-up is the invitation's (coop_ui.mm, invite_popup k 2).
#include <stdio.h>
#include "imgui.h"
#include "settings_ui.h"
extern "C" {
#include "../coop_net.h"
#include "../coop_versus.h"
#include "../coop_battle_link.h"
#include "../coop_battle_names.h"
uint64_t plat_now_ns(void);
}

void coop_versus_friends_ui(void) {
    CoopSession session = {}; coop_net_session(&session);
    CoopRemote peer = {}; if (!coop_net_remote(&peer)) return;
    CoopTeamUp v = {}; coop_versus_request(&v);
    CoopBattleLinkView link; coop_battle_link_view(&link);
    uint8_t local = session.local_slot;
    if (v.state == 1 && v.from_slot != local) {
        if (ui_button("Accept practice battle")) coop_versus_action(1, local);
        ImGui::SameLine(); if (ui_button("Decline practice battle")) coop_versus_action(2, local);
    } else if (v.state == 1) {
        if (ui_button("Cancel practice battle")) coop_versus_action(3, local);
    } else {
        ImGui::BeginDisabled(session.activity != COOP_OVERWORLD || peer.activity != COOP_OVERWORLD || link.phase != COOP_LINK_NONE);
        if (ui_button("Practice battle")) coop_versus_action(0, local);
        ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextDisabled("Nothing is saved: no EXP, money or items");
    }
    if (!v.state && v.reason >= 3 && v.reason <= 7) {
        const char *why[] = {"Another request or battle is active", "Both trainers must be on the overworld", "Request expired",
            "Friend disconnected", "Request is no longer valid"};
        ImGui::TextWrapped("%s", why[v.reason - 3]);
    }
    CoopWatchView w; coop_versus_watch_view(&w);
    if (w.state == 1 && w.watcher == local) { if (ui_button("Stop watching")) coop_versus_watch(false); }
    else if (w.state == 1) ImGui::TextDisabled("%s is watching your battle", peer.name);
    else {
        ImGui::BeginDisabled(peer.activity != COOP_BATTLE || session.activity == COOP_BATTLE);
        if (ui_button("Watch their battle")) coop_versus_watch(true);
        ImGui::EndDisabled();
        if (w.reason == 3) { ImGui::SameLine(); ImGui::TextDisabled("They are not in a battle"); }
    }
}

// The spectator's panel: the battle's Pokemon as the battler's game last reported them. It takes no input; Friends
// (F8) stops watching. Returns true when it drew.
bool coop_watch_ui_draw(float scale, bool allowed) {
    CoopWatchView w; coop_versus_watch_view(&w);
    static bool was; static uint64_t over_until; uint64_t now = plat_now_ns();
    bool watching = w.state == 1 && w.watcher == coop_net_local_slot();
    if (was && !watching && w.reason == 2) over_until = now + 4000000000ull;
    was = watching;
    if (!allowed || (!watching && now >= over_until)) return false;
    CoopRemote peer = {}; const char *name = coop_net_remote(&peer) ? peer.name : "Your friend";
    ImGui::SetNextWindowPos(ImVec2(24 * scale, 24 * scale), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(380 * scale, 0), ImGuiCond_Always);
    ImGui::Begin("Watching", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (!watching) ImGui::Text("%s's battle is over.", name);
    else {
        ImGui::Text("Watching %s's battle", name);
        if (!w.have) ImGui::TextDisabled("Waiting for their next turn...");
        else {
            ImGui::TextDisabled("Turn %u", w.summary.turn);
            static uint16_t logged;
            if (logged != w.summary.turn) { logged = w.summary.turn; fprintf(stderr, "[coop-ui] watching: turn %u, %u/%u HP vs %u/%u HP\n",
                w.summary.turn, w.summary.mons[0].hp, w.summary.mons[0].max_hp, w.summary.mons[1].hp, w.summary.mons[1].max_hp); }
            // Slots 0 and 2 are the battler's side, 1 and 3 the other side.
            for (unsigned side = 0; side < 2; side++) {
                if (side) ImGui::Separator();
                for (unsigned t = side; t < 4; t += 2) {
                    const CoopWatchMon *m = &w.summary.mons[t];
                    if (!m->species) continue;
                    ImGui::Text("%s  Lv. %u  %u/%u HP", coop_battle_species_name(m->species), m->level, m->hp, m->max_hp);
                    ImGui::ProgressBar(m->max_hp ? (float)m->hp / m->max_hp : 0, ImVec2(-1, 6 * scale), "");
                }
            }
        }
        ImGui::TextDisabled("Stop watching: Friends (F8)");
    }
    ImGui::End();
    return true;
}
