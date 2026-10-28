// The online trade overlay (runtime/coop_trade.c): the incoming request pop-up (as the tag-up invitation's), the trade
// window with both offers in detail and both confirmations, and the notices until the trade is in a save.
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "settings_ui.h"
#include "coop_ui.h"
extern "C" {
#include "../coop_net.h"
#include "../coop_trade.h"
#include "../coop_battle_names.h"
uint64_t plat_now_ns(void);
void input_reset(void);
void input_quarantine_pad(void);
}

static bool g_captures;
bool coop_trade_ui_captures(void) { return g_captures; }

static void mon_details(const uint8_t *pb7, bool ready) {
    CoopTradeMon m;
    if (!ready || !coop_trade_describe(pb7, &m)) { ImGui::TextDisabled("No Pokemon chosen yet"); return; }
    const char *species = coop_battle_species_name(m.species);
    ImGui::Text("%s%s  Lv. %u%s", m.nickname, m.shiny ? "  *shiny*" : "", m.level, m.gender == 0 ? "  (M)" : m.gender == 1 ? "  (F)" : "");
    if (m.nicknamed || strcmp(m.nickname, species)) ImGui::TextDisabled("%s", species);
    ImGui::Text("OT: %s  (ID %05u)", m.ot, m.tid);
    if (m.held_item) ImGui::Text("Held item: #%u", m.held_item); else ImGui::TextDisabled("Held item: none");
    for (unsigned i = 0; i < 4; i++) if (m.moves[i]) ImGui::BulletText("%s", coop_battle_move_name(m.moves[i]));
}

void coop_trade_ui_draw(float scale, bool allowed) {
    CoopTradeView v;
    coop_trade_view(&v);
    uint64_t now = plat_now_ns();
    CoopRemote peer = {};
    const char *name = v.peer[0] ? v.peer : coop_net_remote(&peer) ? peer.name : "Your friend";
    bool incoming = v.phase == COOP_TRADE_ASKED && now < v.expires_ns, open = v.phase == COOP_TRADE_OPEN;
    bool capture = allowed && (incoming || open);
    static uint64_t shown_at;
    if (g_captures && !capture) { input_quarantine_pad(); input_reset(); }
    if (!g_captures && capture) shown_at = now;
    g_captures = capture;
    bool notice = v.phase == COOP_TRADE_ASKING || v.phase == COOP_TRADE_PREPARED || v.phase == COOP_TRADE_COMMIT ||
                  v.phase == COOP_TRADE_APPLIED || v.phase == COOP_TRADE_DOUBT || (v.phase == COOP_TRADE_IDLE && now < v.result_until_ns);
    if (!allowed || (!capture && !notice)) return;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, 24 * scale), ImGuiCond_Always, ImVec2(.5f, 0));
    if (capture) ImGui::SetNextWindowSize(ImVec2((open ? 860 : 510) * scale, 0), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    if (!capture) flags |= ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    static uint64_t focused;
    bool new_prompt = capture && focused != (v.id ^ open);
    if (new_prompt) { ImGui::SetNavCursorVisible(true); ImGui::SetNextWindowFocus(); focused = v.id ^ open; }
    ImGui::Begin("Trade", nullptr, flags);
    bool armed = now - shown_at > 400000000ull;  // (an A pressed for the game as the window appears answers nothing)
    bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    if (incoming) {
        ImGui::TextWrapped("%s wants to trade Pokemon!", name);
        ImGui::TextDisabled("Expires in %us", (unsigned)((v.expires_ns - now + 999999999) / 1000000000));
        if (new_prompt) ImGui::SetKeyboardFocusHere();
        bool accept = ui_button("Trade"); ImGui::SameLine(); bool decline = ui_button("Decline") || back;
        if (armed && (accept || decline)) {
            coop_net_remote(&peer);
            coop_trade_answer(accept, peer.identity, peer.name);
            input_quarantine_pad(); input_reset();
        }
    } else if (open) {
        static int refused = COOP_OFFER_OK;
        ImGui::Text("Trade with %s", name);
        ImGui::TextDisabled("Each of you offers one Pokemon from the party. Changing an offer clears both confirmations.");
        if (ImGui::BeginTable("trade", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ui_heading("Your party");
            if (new_prompt) ImGui::SetKeyboardFocusHere();
            for (unsigned i = 0; i < v.party.count; i++) {
                CoopTradeMon m;
                if (!coop_trade_describe(v.party.pokemon[i], &m)) continue;
                char label[120];
                snprintf(label, sizeof label, "%s%s Lv. %u%s##trade-slot-%u", v.mine_slot == (int)i ? "> " : "", m.nickname, m.level,
                         coop_trade_hp(v.party.pokemon[i]) ? "" : " (fainted)", i);
                if (ui_button(label) && armed) { refused = coop_trade_offer(i); input_quarantine_pad(); input_reset(); }
            }
            static const char *why[] = {"", "", "Your partner Pokemon stays with you.", "Keep at least one Pokemon that can battle.",
                                        "This Pokemon can't be traded.", ""};
            if (refused > 0 && refused <= COOP_OFFER_PHASE) ImGui::TextWrapped("%s", why[refused]);
            ImGui::Separator();
            ImGui::TextUnformatted("Your offer");
            mon_details(v.mine, v.mine_ready);
            ImGui::TextDisabled(v.mine_confirmed ? "You confirmed" : "You haven't confirmed");
            ImGui::TableNextColumn();
            ui_heading(name);
            mon_details(v.theirs, v.theirs_ready);
            ImGui::TextDisabled(v.theirs_confirmed ? "%s confirmed" : "%s hasn't confirmed", name);
            ImGui::EndTable();
        }
        ImGui::BeginDisabled(!v.mine_ready || !v.theirs_ready || v.mine_confirmed);
        if (ui_button("Confirm trade") && armed) { coop_trade_confirm(); input_quarantine_pad(); input_reset(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ((ui_button("Cancel trade") || back) && armed) { refused = COOP_OFFER_OK; coop_trade_cancel(); input_quarantine_pad(); input_reset(); }
    } else if (v.phase == COOP_TRADE_ASKING) {
        ImGui::Text("Waiting for %s to answer the trade... (cancel in Friends)", name);
    } else if (v.phase == COOP_TRADE_PREPARED || v.phase == COOP_TRADE_COMMIT) ImGui::TextUnformatted("Trading... don't close the game.");
    else if (v.phase == COOP_TRADE_DOUBT) ImGui::TextWrapped("A trade with %s is waiting for the server or %s to confirm it. Your Pokemon stay as they are until then.", name, name);
    else if (v.phase == COOP_TRADE_APPLIED) ImGui::TextUnformatted("Trade complete! Save your game to keep it.");
    else {
        static const char *messages[] = {"", "The trade was cancelled.", "%s disconnected. Nothing was traded.", "The trade request expired.",
            "The trade timed out. Nothing was traded.", "The offers didn't match. Nothing was traded.",
            "%s can't trade now (be nearby or tagged up, out of battle).", "%s offered a Pokemon that can't be traded.",
            "Your save folder can't be written. Nothing was traded.", "Trade saved!", "A trade record didn't match the save. Nothing was changed."};
        ImGui::Text(messages[v.result <= COOP_TRADE_QUARANTINED ? v.result : 0], name);
    }
    ImGui::End();
}
