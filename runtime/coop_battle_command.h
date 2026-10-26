// Pointer-free move decisions and the authority's two-player turn barrier.
// This prototype is not wired to the social relay or native battle execution.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t battle;
    uint32_t turn, sequence;
    uint16_t move;
    uint8_t trainer, party_index, move_index, target;
} CoopBattleMove;

typedef struct {
    uint16_t moves[4];
    uint8_t pp[4], party_index;
    uint32_t targets[4]; // bit per native target position; supplied by the authority
} CoopBattleChoices;

typedef enum {
    COOP_BATTLE_READY, COOP_BATTLE_CHOOSING, COOP_BATTLE_RESOLVING,
    COOP_BATTLE_ENDED, COOP_BATTLE_ABORTED
} CoopBattlePhase;

typedef struct {
    uint64_t battle;
    uint32_t turn, next_sequence[2];
    CoopBattlePhase phase;
    uint8_t received;
    CoopBattleChoices choices[2];
    CoopBattleMove commands[2];
} CoopBattleGate;

bool coop_battle_gate_start(CoopBattleGate *gate, uint64_t battle);
bool coop_battle_gate_open(CoopBattleGate *gate, uint32_t turn, const CoopBattleChoices choices[2]);
// Sender comes from the authenticated session, never a field in the message.
bool coop_battle_gate_submit(CoopBattleGate *gate, unsigned sender, const CoopBattleMove *command);
bool coop_battle_gate_take(CoopBattleGate *gate, CoopBattleMove commands[2]);
bool coop_battle_gate_finish(CoopBattleGate *gate);
void coop_battle_gate_end(CoopBattleGate *gate);
void coop_battle_gate_disconnect(CoopBattleGate *gate, unsigned sender);

// One ally decision. Expiry selects the native AI for this decision only.
typedef enum { COOP_ALLY_IDLE, COOP_ALLY_WAITING, COOP_ALLY_REMOTE, COOP_ALLY_AI, COOP_ALLY_DONE } CoopAllyPhase;
typedef struct {
    uint64_t battle, deadline_ns;
    uint32_t turn, sequence;
    CoopBattleChoices choices;
    CoopBattleMove command;
    CoopAllyPhase phase;
} CoopAllyWait;
bool coop_ally_begin(CoopAllyWait *, uint64_t battle, uint32_t turn, uint32_t sequence,
                    const CoopBattleChoices *, uint64_t now_ns, unsigned timeout_ms);
bool coop_ally_submit(CoopAllyWait *, const CoopBattleMove *, uint64_t received_ns);
CoopAllyPhase coop_ally_poll(CoopAllyWait *, uint64_t now_ns);
bool coop_ally_consume(CoopAllyWait *, CoopBattleMove *command, bool *use_ai);
