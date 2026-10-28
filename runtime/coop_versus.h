// Practice battles (V) and spectating (O) between the room's two players. docs/COOP-ONLINE.md "Practice battles".
// Pure state and wire handling (no game calls), so tests/tools/pvp-test.c runs it alone.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "coop_net.h"

// V: a practice battle request, with the meet-up's commands (0 request, 1 accept, 2 decline, 3 cancel), states and
// reasons; state 2 (accepted) comes once. Then the room's host offers the battle (B0 COOP_BATTLE_PVP) and the guest
// joins it without a prompt (coop_battle_link_pvp_consent).
void coop_versus_request(CoopTeamUp *out);
bool coop_versus_action(uint8_t command, uint8_t local_slot);
// Host, game thread: true once, within 20 s of an accept: start the practice battle.
bool coop_versus_take_start(void);

// O: watching the other player's battle. The battler sends a summary at each of its turns while watched; the watcher
// only reads it, and the relay drops the watcher's battle messages.
typedef struct { uint16_t species, hp, max_hp; uint8_t level; } CoopWatchMon;
typedef struct { uint16_t turn; CoopWatchMon mons[4]; } CoopWatchSummary;  // battle trainer slots 0-3; species 0: none
// reason: 0 none, 1 stopped, 2 the battle is over, 3 refused (they are not in a battle, or you are), 4 disconnected
typedef struct { uint8_t state, watcher, reason; bool have; CoopWatchSummary summary; uint64_t received_ns; } CoopWatchView;
void coop_versus_watch_view(CoopWatchView *out);
bool coop_versus_watch(bool start);
bool coop_versus_watched(uint8_t local_slot);
void coop_versus_publish(const CoopWatchSummary *summary, uint8_t local_slot);

// Network worker: p is the whole message (p[0] 'V' or 'O'); pop gives one frame to send (its opcode and body).
void coop_versus_message(const uint8_t *p, size_t n);
bool coop_versus_pop(uint8_t *op, uint8_t body[64], size_t *n);
void coop_versus_link_lost(void);
