#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include "imgui.h"
#include "settings_ui.h"
#include "coop_ui.h"
extern "C" {
#include "../coop_net.h"
#include "../coop_invite.h"
#include "../coop_battle_link.h"
#include "../coop_battle_names.h"
uint64_t plat_now_ns(void);
bool coop_battle_native_ui(void);
bool coop_battle_partner_finishing(void);
void input_reset(void);
void input_quarantine_pad(void);
}

bool coop_battle_ui_captures(void) {
    CoopBattleLinkView v;coop_battle_link_view(&v);
    return !v.host && plat_now_ns()<v.deadline_ns &&
        (v.phase==COOP_LINK_OFFER || (v.phase==COOP_LINK_ACTIVE && v.waiting && !coop_battle_native_ui()));
}

void coop_battle_ui_draw(float scale) {
    CoopBattleLinkView v;coop_battle_link_view(&v);uint64_t now=plat_now_ns();
    bool offer=v.phase==COOP_LINK_OFFER && now<v.deadline_ns;
    bool prepare=v.host && v.phase==COOP_LINK_PREPARING && now<v.deadline_ns;
    // Mirror trial: the guest chooses in the game's own battle menu, and both wait in its message window.
    bool native=coop_battle_native_ui();
    bool choosing=!v.host && v.phase==COOP_LINK_ACTIVE && v.waiting && !native;
    bool toast=v.outcome==2 && now<v.toast_until_ns;
    // The partner's replay ended first: its rewards wait for the host's end (a black screen until then).
    bool finishing=coop_battle_partner_finishing();
    static bool was_captured;bool capture=!v.host && now<v.deadline_ns && (offer || choosing);
    if(was_captured && !capture){input_quarantine_pad();input_reset();}was_captured=capture;
    if(!offer && !prepare && !choosing && !(v.waiting && !native) && !toast && !finishing)return;
    ImGuiIO &io=ImGui::GetIO();
    ImGui::SetNextWindowPos(v.host?ImVec2(24*scale,24*scale):ImVec2(io.DisplaySize.x*.5f,24*scale),ImGuiCond_Always,ImVec2(v.host?0:.5f,0));
    ImGui::SetNextWindowSize(ImVec2((v.host?430:510)*scale,0),ImGuiCond_Always);
    ImGuiWindowFlags flags=ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize;
    if(v.host || toast || finishing)flags|=ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoFocusOnAppearing;
    static uint64_t focused_battle;static uint32_t focused_turn;static bool focused_offer;
    bool new_prompt=capture && (focused_battle!=v.battle || focused_turn!=v.turn || focused_offer!=offer);
    if(new_prompt){ImGui::SetNavCursorVisible(true);ImGui::SetNextWindowFocus();focused_battle=v.battle;focused_turn=v.turn;focused_offer=offer;}
    ImGui::Begin("Battle team-up",nullptr,flags);
    unsigned seconds=now<v.deadline_ns?(unsigned)((v.deadline_ns-now+999999999)/1000000000):0;
    bool back=ImGui::IsKeyPressed(ImGuiKey_Escape,false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false);
    if(offer && back){input_quarantine_pad();input_reset();coop_battle_link_end();ImGui::End();return;}
    if(offer){
        CoopRemote host;const char *name=coop_net_remote(&host)?host.name:"Your friend";
        if(coop_battle_wild(v.trainer)){
            ImGui::TextWrapped("%s found a wild %s! Join the battle with your Pokemon?",name,coop_battle_species_name(v.trainer&0x7fff));
            ImGui::TextWrapped("Only %s can throw Poke Balls: a catch is theirs.",name);
        } else ImGui::TextWrapped("%s was challenged by %s! Join the battle with your Pokemon?",name,coop_battle_trainer_name(v.trainer));
        ImGui::TextWrapped("Your story progress stays independent.");ImGui::TextDisabled("Expires in %us",seconds);
        ImGui::BeginDisabled(!v.party_ready);
        if(new_prompt && v.party_ready)ImGui::SetKeyboardFocusHere();
        if(ui_button("Join battle")){input_quarantine_pad();input_reset();coop_battle_link_accept();}
        ImGui::EndDisabled();ImGui::SameLine();
        if(ui_button("Decline")){input_quarantine_pad();input_reset();coop_battle_link_end();}
    } else if(prepare)ImGui::Text("Waiting for ally to join... %us",seconds);
    else if(finishing){CoopRemote host;ImGui::TextWrapped("Waiting for %s to finish the battle...",coop_net_remote(&host)?host.name:"your friend");}
    else if(toast)ImGui::TextWrapped(v.host?"Ally did not answer. The NPC chose for this turn.":"Time ran out. The NPC chose this turn. You can choose again next turn.");
    else if(v.host){
        CoopRemote peer;const char *name=coop_net_remote(&peer)?peer.name:"your ally";
        ImGui::Text("Waiting for %s... %us",name,seconds);
        ImGui::TextWrapped("If time runs out, the NPC chooses this turn. Your ally can choose again next turn.");
        ImGui::ProgressBar(v.timeout_ms ? (float)(seconds*1000)/v.timeout_ms : 0,ImVec2(-1,8*scale),"");
    } else if(v.submitted)ImGui::TextWrapped("Choice sent. Waiting for the turn...");
    else if(!seconds)ImGui::TextWrapped("Time is up. The NPC will choose this turn.");
    else {
        static uint64_t battle;static uint32_t turn;static int selected=-1;static bool focus_target,focus_move;
        if(battle!=v.battle || turn!=v.turn){battle=v.battle;turn=v.turn;selected=-1;focus_move=true;focus_target=false;}
        if(back && selected>=0){selected=-1;focus_move=true;}
        ImGui::Text("Choose your move - %us",seconds);
        if(selected<0){
            for(unsigned i=0;i<4;i++){
                char label[100];snprintf(label,sizeof label,"%s  (%u PP)##ally-move-%u",coop_battle_move_name(v.choices.moves[i]),v.choices.pp[i],i);
                bool available=v.choices.moves[i] && v.choices.pp[i] && v.choices.targets[i];
                ImGui::BeginDisabled(!available);
                if(focus_move && available){ImGui::SetKeyboardFocusHere();focus_move=false;}
                if(ui_button(label)){
                    unsigned mask=v.choices.targets[i];selected=(int)i;focus_target=true;
                    // (one possible target, or a move that names no one, bit 4: sent at once)
                    if(mask && (!(mask&(mask-1)) || (mask&16))){unsigned t=mask&16?4:0;while(!(mask&(1u<<t)))t++;input_quarantine_pad();input_reset();coop_battle_link_choose(i,t);}
                }
                ImGui::EndDisabled();
            }
        } else {
            ImGui::Text("%s - choose a target",coop_battle_move_name(v.choices.moves[selected]));
            for(unsigned i=0;i<2;i++){
                unsigned t=1+2*i;if(!(v.choices.targets[selected]&(1u<<t)))continue;
                char label[120];snprintf(label,sizeof label,"%s  %u/%u HP##ally-target-%u",coop_battle_species_name(v.opponents[i]),v.hp[i],v.max_hp[i],t);
                if(focus_target){ImGui::SetKeyboardFocusHere();focus_target=false;}
                if(ui_button(label)){input_quarantine_pad();input_reset();coop_battle_link_choose(selected,t);}
            }
            // The partner's own position is 2, the host's Pokemon is at 0.
            for(unsigned t=0;t<4;t+=2){
                if(!(v.choices.targets[selected]&(1u<<t)))continue;
                if(focus_target){ImGui::SetKeyboardFocusHere();focus_target=false;}
                if(ui_button(t?"Your Pokemon##ally-target-2":"Ally's Pokemon##ally-target-0")){input_quarantine_pad();input_reset();coop_battle_link_choose(selected,t);}
            }
            if(ui_button("Back")){selected=-1;focus_move=true;}
        }
        ImGui::TextWrapped("No answer before time runs out: the NPC chooses only this turn.");
    }
    ImGui::End();
}

static std::atomic<bool> g_invite_open, g_invite_hovered;
bool coop_invite_ui_captures(void) { return g_invite_open; }
bool coop_invite_ui_hovered(void) { return g_invite_hovered; }

void coop_invite_ui_draw(float scale, bool allowed) {
    CoopTeamUp team={};coop_net_teamup(&team);CoopRemote peer={};
    const char *name=coop_net_remote(&peer)?peer.name:"Your friend";
    uint8_t local=coop_net_local_slot();uint64_t now=plat_now_ns();
    static uint32_t answered,sent;static uint64_t toast_until,shown_at;static int result;
    int mode=coop_invite_mode(&team,local,now,answered);
    if(mode==2)sent=team.id;
    else if(sent){int r=coop_invite_result(&team,local,sent);if(r>=0){sent=0;result=r;toast_until=r?now+4000000000ull:0;}}
    bool incoming=mode==1 && allowed,waiting=mode==2 && allowed,toast=now<toast_until && allowed;
    if(g_invite_open && !incoming){input_quarantine_pad();input_reset();}
    if(!g_invite_open && incoming){shown_at=now;fprintf(stderr,"[coop-ui] tag-up invitation pop-up shown\n");}
    g_invite_open=incoming;g_invite_hovered=false;
    if(!incoming && !waiting && !toast)return;
    ImGuiIO &io=ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*.5f,24*scale),ImGuiCond_Always,ImVec2(.5f,0));
    if(incoming)ImGui::SetNextWindowSize(ImVec2(510*scale,0),ImGuiCond_Always);
    ImGuiWindowFlags flags=ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_AlwaysAutoResize;
    if(!incoming)flags|=ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|(waiting?0:ImGuiWindowFlags_NoInputs);
    static uint32_t focused;bool new_prompt=incoming && focused!=team.id;
    if(new_prompt){ImGui::SetNavCursorVisible(true);ImGui::SetNextWindowFocus();focused=team.id;}
    if(!incoming)focused=0;
    ImGui::Begin("Tag-up invitation",nullptr,flags);
    unsigned seconds=team.expires_ns>now?(unsigned)((team.expires_ns-now+999999999)/1000000000):0;
    if(incoming){
        // An A pressed for the game just as the pop-up appears must not answer it.
        bool armed=now-shown_at>400000000ull;
        bool back=ImGui::IsKeyPressed(ImGuiKey_Escape,false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false);
        ImGui::TextWrapped("%s wants to tag up!",name);
        ImGui::TextWrapped("Travel together; progress stays individual.");ImGui::TextDisabled("Expires in %us",seconds);
        if(team.reason==4)ImGui::TextWrapped("Move closer to %s, then accept again.",name);
        if(new_prompt)ImGui::SetKeyboardFocusHere();
        bool accept=ui_button("Accept");ImGui::SameLine();bool decline=ui_button("Decline") || back;
        if(armed && (accept || decline) && coop_net_teamup_action(accept?1:2)){
            answered=team.id;g_invite_open=false;input_quarantine_pad();input_reset();
        }
    } else if(waiting){
        ImGui::Text("Waiting for %s... %us",name,seconds);ImGui::SameLine();
        if(ui_button("Cancel"))coop_net_teamup_action(3);
        g_invite_hovered=ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    } else {
        const char *messages[]={"","%s accepted. Tagged up!","%s declined the invitation.","%s did not answer in time.","%s disconnected."};
        ImGui::Text(messages[result],name);
    }
    ImGui::End();
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
                ImGui::TextWrapped("Social tag-up: travel together. Progression stays individual.");
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
                CoopOptions prefs;coop_net_options(&prefs);
                if(ImGui::Checkbox("Team-up battles",&prefs.teamup_battles))coop_net_preferences(&prefs);
                ImGui::TextWrapped("Tagged up, a trainer who challenges the host asks the partner to join.");
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
