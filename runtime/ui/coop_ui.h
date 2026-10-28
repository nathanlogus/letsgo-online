#pragma once
// Shared launcher / in-game friends page. Returns true when the launcher should play.
bool coop_ui_page(bool launcher);
bool coop_battle_ui_captures(void);
void coop_battle_ui_draw(float scale);
// The tag-up invitation pop-up (receiver) and waiting notice (sender); `allowed`: on the overworld, no other overlay.
bool coop_invite_ui_captures(void);
bool coop_invite_ui_hovered(void);  // the mouse is over the sender's Cancel notice
void coop_invite_ui_draw(float scale, bool allowed);
// The online trade overlay (runtime/ui/coop_trade_ui.mm); `allowed` as for the invitation.
bool coop_trade_ui_captures(void);
void coop_trade_ui_draw(float scale, bool allowed);
// Shared Adventure (runtime/ui/coop_scene_ui.mm): the ready prompt and notices, the HUD mode line, the settings.
bool coop_scene_ui_captures(void);
void coop_scene_ui_draw(float scale, bool allowed);
const char *coop_scene_ui_mode(void);
void coop_scene_ui_settings(bool connected);
