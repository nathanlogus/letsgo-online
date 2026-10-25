#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "settings_ui.h"
#include "coop_ui.h"
extern "C" {
#include "../coop_net.h"
uint64_t plat_now_ns(void);
}

static const char *activity_name(CoopActivity a) {
    switch (a) {
    case COOP_OVERWORLD: return "Exploring";
    case COOP_MENU: return "Menu / loading";
    case COOP_BATTLE: return "In battle";
    case COOP_STORY: return "Story scene";
    default: return "Loading";
    }
}

static void trainer_portrait(uint8_t slot, const char *name) {
    // A compact trainer silhouette; the native overworld model retains the actual outfit.
    ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetFrameHeight() * 1.4f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 colour = slot == 1 ? IM_COL32(255,174,186,255) : IM_COL32(142,213,255,255);
    dl->AddCircleFilled(ImVec2(p.x+h*.5f,p.y+h*.5f), h*.48f, colour);
    dl->AddCircleFilled(ImVec2(p.x+h*.5f,p.y+h*.39f), h*.16f, IM_COL32(255,237,213,255));
    dl->AddRectFilled(ImVec2(p.x+h*.30f,p.y+h*.59f), ImVec2(p.x+h*.70f,p.y+h*.84f), IM_COL32(58,72,88,255), h*.08f);
    dl->AddRectFilled(ImVec2(p.x+h*.29f,p.y+h*.23f), ImVec2(p.x+h*.75f,p.y+h*.34f), IM_COL32(58,72,88,255), h*.04f);
    ImGui::Dummy(ImVec2(h,h)); ImGui::SameLine(); ImGui::Text("%s  ·  Player %u", name, slot+1);
}

bool coop_ui_page(bool launcher) {
    static CoopOptions options;
    static bool loaded, show_code, inspect;
    if (!loaded) { coop_net_options(&options); loaded = true; }
    CoopSession session = {}; coop_net_session(&session);
    CoopRemote peer = {}; bool present = coop_net_remote(&peer);
    bool active = session.connection == COOP_CONNECTED;
    CoopTeamUp team={};coop_net_teamup(&team);
    bool pending = session.connection == COOP_CONNECTING || session.connection == COOP_RECONNECTING;
    ImGui::TextWrapped("Social co-op · two trainers, independent saves and progression.");
    ImGui::Spacing();
    if (active || pending) {
        const char *state = active ? "Connected" : session.connection == COOP_RECONNECTING ? "Reconnecting" : "Connecting";
        ImGui::Text("%s%s", state, launcher ? " when you press Play" : "");
        if (pending && !launcher) ImGui::Text("Attempt %u / 3", session.attempt);
        if (session.room[0]) {
            ImGui::Text("Room: %s", show_code ? session.room : "LG-******");
            ImGui::SameLine(); if (ui_button(show_code ? "Hide##room" : "Show##room")) show_code = !show_code;
            ImGui::SameLine(); if (ui_button("Copy##room")) ImGui::SetClipboardText(session.room);
        }
        if (active) {
            trainer_portrait(session.local_slot, session.name);
            ImGui::TextDisabled("You · %s%s", activity_name(session.activity), session.local_slot == session.host_slot ? " · Host" : "");
            if (present) {
                trainer_portrait(peer.slot, peer.name);
                ImGui::TextDisabled("%s%s", activity_name(peer.activity), peer.slot == session.host_slot ? " · Host" : "");
                if (session.snapshot_age_ms > 1000) ImGui::TextDisabled("Waiting for trainer updates...");
                if (ui_button("Inspect trainer")) inspect = !inspect;
                ImGui::SameLine();
                ImGui::BeginDisabled(!coop_net_social_near(3000));
                if (ui_button("Say hello")) coop_net_emote(1);
                ImGui::SameLine(); if (ui_button("Cheer")) coop_net_emote(2);
                ImGui::EndDisabled();
                ImGui::TextDisabled("Quick emotes in the overworld: R3 / F10");
                if (inspect) {
                    ImGui::TextWrapped("%s · %s", peer.name,
                        peer.appearance.valid ? peer.appearance.gender ? "Female trainer" : "Male trainer" : "Appearance loading");
                    ImGui::TextDisabled("Trainer ID: %02X%02X-%02X%02X", peer.identity[0],peer.identity[1],peer.identity[2],peer.identity[3]);
                    ImGui::TextWrapped("Party, items and story remain private to this trainer's save.");
                }
            } else ImGui::TextWrapped("Waiting for your friend. A disconnected trainer has 30 seconds to reconnect.");
            if(present) {
                ImGui::Separator();
                ImGui::TextWrapped("Social tag-up: travel together. Battles and progression remain individual.");
                if(team.state==2) {
                    ImGui::TextUnformatted("Tagged up");
                    if(ui_button("End tag-up"))coop_net_teamup_action(3);
                } else if(team.state==1) {
                    bool incoming=team.from_slot!=session.local_slot;
                    ImGui::Text("%s (%.0f s)",incoming ? "Tag-up invitation received" : "Waiting for your friend",
                        team.expires_ns>plat_now_ns() ? (team.expires_ns-plat_now_ns())/1e9 : 0);
                    if(incoming) {
                        if(ui_button("Accept tag-up"))coop_net_teamup_action(1);
                        ImGui::SameLine();if(ui_button("Decline tag-up"))coop_net_teamup_action(2);
                    }else if(ui_button("Cancel invitation"))coop_net_teamup_action(3);
                } else {
                    ImGui::BeginDisabled(!coop_net_social_near(3000));
                    if(ui_button("Invite to tag up"))coop_net_teamup_action(0);
                    ImGui::EndDisabled();
                }
                if(team.reason) {
                    const char *reasons[]={"","Invitation declined","Tag-up ended","Another invitation is active",
                        "Both trainers must be nearby in the overworld","Invitation expired","Friend disconnected","Invitation is no longer valid"};
                    ImGui::TextWrapped("%s",reasons[team.reason<=7 ? team.reason : 7]);
                }
            }
            if (session.local_slot == session.host_slot) {
                if (ui_button(session.locked ? "Unlock world" : "Lock world")) coop_net_lock(!session.locked);
                ImGui::SameLine(); ImGui::BeginDisabled(!present);
                if (ui_button("Remove player")) ImGui::OpenPopup("Remove trainer?");
                ImGui::EndDisabled();
                if (ImGui::BeginPopupModal("Remove trainer?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped("Remove this trainer from the current room?");
                    if (ui_button("Remove")) { coop_net_remove_peer(); ImGui::CloseCurrentPopup(); }
                    ImGui::SameLine(); if (ui_button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            CoopNetStats stats = {}; coop_net_stats(&stats);
            if (stats.relay_rtt_ms >= 0) {
                if (present && session.snapshot_age_ms >= 0) ImGui::TextDisabled("Relay %.0f ms · updates %.1f ms · latest %.0f ms ago", stats.relay_rtt_ms, stats.snapshot_interval_ms, session.snapshot_age_ms);
                else ImGui::TextDisabled("Relay %.0f ms",stats.relay_rtt_ms);
            }
        }
        if (ui_button("Leave room / play solo")) { coop_net_leave(); inspect = false; }
        ImGui::TextWrapped("Each trainer saves their own game. Leaving never copies another trainer's progress.");
        return false;
    }
    if (session.connection == COOP_ERROR) {
        ImGui::TextWrapped("%s", session.detail);
        if (ui_button("Retry")) { coop_net_connect(&options); return launcher; }
        ImGui::SameLine(); if (ui_button("Play solo")) coop_net_leave();
        ImGui::Separator();
    }
    ImGui::TextUnformatted("Trainer name");
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##coop-name", "Trainer name", options.name, sizeof options.name);
    ImGui::TextUnformatted("Relay server");
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##coop-server", "Relay address (host:port)", options.server, sizeof options.server);
    ImGui::BeginDisabled(!options.name[0] || !options.server[0]);
    if (ui_button("Host a world")) { ImGui::EndDisabled(); options.host = true; coop_net_connect(&options); return launcher; }
    ImGui::Spacing();
    ImGui::TextUnformatted("Room code");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##coop-code", "LG-ABC234", options.room, sizeof options.room, ImGuiInputTextFlags_CharsUppercase);
    ImGui::BeginDisabled(strlen(options.room) != 9 || strncmp(options.room,"LG-",3));
    if (ui_button("Join world")) { ImGui::EndDisabled(); ImGui::EndDisabled(); options.host = false; coop_net_connect(&options); return launcher; }
    ImGui::EndDisabled(); ImGui::EndDisabled();
    CoopOptions saved; coop_net_options(&saved);
    ui_heading("Saved worlds");
    for (unsigned i = 0; i < 4; i++) if (saved.saved_rooms[i][0]) {
        ImGui::PushID((int)i);
        char label[40]; snprintf(label,sizeof label,"Join saved world %u",i+1);
        if (ui_button(show_code ? saved.saved_rooms[i] : label)) {
            snprintf(options.room, sizeof options.room, "%s", saved.saved_rooms[i]); options.host = false;
            coop_net_connect(&options); ImGui::PopID(); return launcher;
        }
        ImGui::SameLine(); if (ui_button("Forget")) coop_net_forget_room(i);
        ImGui::PopID();
    }
    if (ui_button(show_code ? "Hide saved codes" : "Show saved codes")) show_code = !show_code;
    ImGui::TextWrapped("Host and guest need the same game build and online protocol. Open Friends in-game with F8 or from the pause overlay.");
    return false;
}
