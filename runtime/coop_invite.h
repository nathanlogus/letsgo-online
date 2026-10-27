// Tag-up invitation pop-up decisions (runtime/ui/coop_ui.mm), kept free of UI code for tests/tools/invite-popup-test.c.
#pragma once
#include "coop_net.h"

// 0 nothing, 1 an incoming invitation to answer, 2 the sender's waiting notice. `answered` hides an invitation
// already answered here until the relay replies; a rejected answer (reason set) shows it again.
static inline int coop_invite_mode(const CoopTeamUp *t, uint8_t local_slot, uint64_t now, uint32_t answered) {
    if (t->state != 1 || now >= t->expires_ns) return 0;
    if (t->from_slot == local_slot) return 2;
    return t->id == answered && !t->reason ? 0 : 1;
}

// The sender's result for its invitation `sent`: -1 still pending, 0 no notice (cancelled here or the session reset),
// 1 accepted, 2 declined, 3 expired, 4 friend disconnected.
static inline int coop_invite_result(const CoopTeamUp *t, uint8_t local_slot, uint32_t sent) {
    if (t->id == sent && t->state == 1 && t->from_slot == local_slot) return -1;
    if (t->id == sent && t->state == 2) return 1;
    if (t->id != sent || t->state) return 0;
    return t->reason == 1 ? 2 : t->reason == 5 ? 3 : t->reason == 6 ? 4 : 0;
}
