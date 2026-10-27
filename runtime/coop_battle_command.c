#include "coop_battle_command.h"
#include <limits.h>
#include <string.h>

bool coop_battle_gate_start(CoopBattleGate *gate, uint64_t battle) {
    if (!gate || !battle) return false;
    memset(gate, 0, sizeof *gate);
    gate->battle = battle;
    gate->next_sequence[0] = gate->next_sequence[1] = 1;
    return true;
}

bool coop_battle_gate_open(CoopBattleGate *gate, uint32_t turn, const CoopBattleChoices choices[2]) {
    if (!gate || !gate->battle || !choices || gate->phase != COOP_BATTLE_READY ||
        gate->turn == UINT32_MAX || turn != gate->turn + 1) return false;
    for (unsigned p = 0; p < 2; p++) {
        if (choices[p].party_index >= 6) return false;
        for (unsigned m = 0; m < 4; m++)
            if (choices[p].targets[m] & ~UINT32_C(0xffff)) return false;
    }
    memcpy(gate->choices, choices, sizeof gate->choices);
    memset(gate->commands, 0, sizeof gate->commands);
    gate->turn = turn;
    gate->received = 0;
    gate->phase = COOP_BATTLE_CHOOSING;
    return true;
}

bool coop_battle_gate_submit(CoopBattleGate *gate, unsigned sender, const CoopBattleMove *command) {
    if (!gate || !command || sender >= 2 || gate->phase != COOP_BATTLE_CHOOSING ||
        command->battle != gate->battle || command->turn != gate->turn ||
        command->sequence != gate->next_sequence[sender] ||
        gate->next_sequence[sender] == UINT32_MAX || (gate->received & (1u << sender))) return false;
    const CoopBattleChoices *choice = &gate->choices[sender];
    // Native multi-trainer slots 0 and 2 are the allied trainers.
    if (command->trainer != sender * 2 || command->party_index != choice->party_index ||
        command->move_index >= 4 || command->target >= 16) return false;
    unsigned m = command->move_index;
    if (!command->move || command->move != choice->moves[m] || !choice->pp[m] ||
        !(choice->targets[m] & (UINT32_C(1) << command->target))) return false;
    gate->commands[sender] = *command;
    gate->next_sequence[sender]++;
    gate->received |= 1u << sender;
    return true;
}

bool coop_battle_gate_take(CoopBattleGate *gate, CoopBattleMove commands[2]) {
    if (!gate || !commands || gate->phase != COOP_BATTLE_CHOOSING || gate->received != 3) return false;
    // Receipt order cannot change execution order. The native battle engine
    // remains responsible for move priority/speed once it receives both choices.
    memcpy(commands, gate->commands, sizeof gate->commands);
    gate->phase = COOP_BATTLE_RESOLVING;
    return true;
}

bool coop_battle_gate_finish(CoopBattleGate *gate) {
    if (!gate || gate->phase != COOP_BATTLE_RESOLVING) return false;
    memset(gate->commands, 0, sizeof gate->commands);
    memset(gate->choices, 0, sizeof gate->choices);
    gate->received = 0;
    gate->phase = COOP_BATTLE_READY;
    return true;
}

static void close_gate(CoopBattleGate *gate, CoopBattlePhase phase) {
    if (!gate || gate->phase == COOP_BATTLE_ENDED || gate->phase == COOP_BATTLE_ABORTED) return;
    memset(gate->commands, 0, sizeof gate->commands);
    memset(gate->choices, 0, sizeof gate->choices);
    gate->received = 0;
    gate->phase = phase;
}

void coop_battle_gate_end(CoopBattleGate *gate) { close_gate(gate, COOP_BATTLE_ENDED); }
void coop_battle_gate_disconnect(CoopBattleGate *gate, unsigned sender) {
    if (sender < 2) close_gate(gate, COOP_BATTLE_ABORTED);
}

bool coop_ally_begin(CoopAllyWait *w, uint64_t battle, uint32_t turn, uint32_t sequence,
                    const CoopBattleChoices *choices, uint64_t now, unsigned timeout_ms) {
    if (!w || !battle || !turn || !sequence || !choices || choices->party_index >= 6 ||
        w->phase == COOP_ALLY_WAITING || w->phase == COOP_ALLY_REMOTE || w->phase == COOP_ALLY_AI ||
        !timeout_ms || timeout_ms > 120000 || UINT64_MAX-now < (uint64_t)timeout_ms*1000000) return false;
    *w = (CoopAllyWait){.battle=battle, .turn=turn, .sequence=sequence, .choices=*choices,
        .deadline_ns=now+(uint64_t)timeout_ms*1000000, .phase=COOP_ALLY_WAITING};
    return true;
}

bool coop_ally_submit(CoopAllyWait *w, const CoopBattleMove *m, uint64_t received) {
    if (!w || !m || w->phase != COOP_ALLY_WAITING || received >= w->deadline_ns ||
        m->battle != w->battle || m->turn != w->turn || m->sequence != w->sequence ||
        m->trainer != 2 || m->party_index != w->choices.party_index || m->move_index >= 4 || m->target >= 16)
        return false;
    if (m->switch_to) {
        if (m->switch_to > 6 || !(w->choices.switches & (1u << (m->switch_to - 1)))) return false;
        w->command=*m;w->phase=COOP_ALLY_REMOTE;return true;
    }
    unsigned i=m->move_index;
    if (!m->move || m->move != w->choices.moves[i] || !w->choices.pp[i] ||
        !(w->choices.targets[i] & (UINT32_C(1)<<m->target))) return false;
    w->command=*m;w->phase=COOP_ALLY_REMOTE;return true;
}

CoopAllyPhase coop_ally_poll(CoopAllyWait *w, uint64_t now) {
    if (!w) return COOP_ALLY_IDLE;
    if (w->phase==COOP_ALLY_WAITING && now>=w->deadline_ns) w->phase=COOP_ALLY_AI;
    return w->phase;
}

bool coop_ally_consume(CoopAllyWait *w, CoopBattleMove *m, bool *ai) {
    if (!w || !m || !ai || (w->phase!=COOP_ALLY_AI && w->phase!=COOP_ALLY_REMOTE)) return false;
    *ai=w->phase==COOP_ALLY_AI;*m=w->command;w->phase=COOP_ALLY_DONE;return true;
}
