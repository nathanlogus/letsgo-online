// Co-op trial battle: draw the team-up trainers with the players' own avatars (Eevee 1.0.0).
// The battle's trainer-model builder 0x7000a99160 asks the main module for the slot's MyStatus
// (vtable +0x848, 0x7000a7e170) and trainer type (+0x790, 0x7000a7e1e0). With a status it builds
// the player avatar from the status' 0x50-byte resource-ID block (+0x160, the field's dressup);
// without one (an NPC: the ally, slot 2) it loads the trainer type's model. Scoped to that one
// builder call: the ally gets a MyStatus whose block is the avatar it stands for, and the type
// is the player class 0x20 + gender, as the native P2 path (slot 2, mode 3) also does.
#include <string.h>
#include "../hle.h"
#include "../coop_net.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *, uint64_t));
void rt_call_original(Cpu *c, uint64_t addr);
bool coop_battle_trial_model(bool *mirrored);

#define TRAINER_MODEL 0x7000a99160ull // x0 = view (slot at +0xe8)
#define SLOT_STATUS 0x7000a7e170ull   // x0 = main module, w1 = slot, x8 = out shared_ptr (null for an NPC)
#define SLOT_TYPE 0x7000a7e1e0ull     // x0 = main module, w1 = slot; w0 = trainer type
#define WORLD_GOT 0x70015fad08ull
#define STATUS_PARTS 0x160            // MyStatus resource block (0x70001c9f30)
#define STATUS_GENDER 0x5d            // (0x70001c9d30)

static bool g_building;
static uint64_t g_swapped;
static uint8_t g_saved[COOP_APPEARANCE_BYTES];

static bool guest(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI; }
static uint64_t ptr(uint64_t p) { return guest(p) ? *(const uint64_t *)(uintptr_t)p : 0; }
static uint64_t local_status(void) { return ptr(ptr(ptr(ptr(ptr(WORLD_GOT)) + 0x98) + 0x58) + 0x78); }

// The avatar slot `slot` stands for: 1 the peer, 2 this player, 0 leave it native.
// Host: 0 is the host, 2 the guest. Mirrored guest: 0 replays the host, 2 is the guest itself.
static int avatar(unsigned slot, CoopAppearance *peer_out) {
    bool mirrored;
    if (!g_building || !coop_battle_trial_model(&mirrored)) return 0;
    int who = slot == 2 ? (mirrored ? 2 : 1) : slot == 0 && mirrored ? 1 : 0;
    CoopRemote peer;
    if (who == 1 && (!coop_net_remote(&peer) || !peer.appearance.valid)) return 0;
    if (who == 1) *peer_out = peer.appearance;
    return who;
}

static int model_hook(Cpu *c, uint64_t addr) {
    bool mirrored;
    if (!coop_battle_trial_model(&mirrored)) return 0;
    g_building = true;
    rt_call_original(c, addr);
    g_building = false;
    if (g_swapped) { memcpy((void *)(uintptr_t)(g_swapped + STATUS_PARTS), g_saved, sizeof g_saved); g_swapped = 0; }
    return 1;
}

static int status_hook(Cpu *c, uint64_t addr) {
    CoopAppearance peer;
    unsigned slot = (uint8_t)c->x[1];
    int who = avatar(slot, &peer);
    if (!who) return 0;
    uint64_t out = c->x[8];
    rt_call_original(c, addr);
    uint64_t status = ptr(out);
    if (!status) {
        // The NPC ally has no status: lend the save's own one, with the reference the caller releases.
        status = local_status();
        uint64_t retain = ptr(ptr(status + 0x48) + 0x10);
        if (!guest(retain)) return 1;
        *(uint64_t *)(uintptr_t)out = status;
        Cpu call = *c; call.x[0] = status + 0x48; guest_call(&call, retain);
    }
    if (who == 1 && !g_swapped) {
        memcpy(g_saved, (const void *)(uintptr_t)(status + STATUS_PARTS), sizeof g_saved);
        memcpy((void *)(uintptr_t)(status + STATUS_PARTS), peer.parts, sizeof g_saved);
        g_swapped = status;
    }
    LOG("[coop-battle] trainer slot %u drawn as the %s avatar (model gender %u)\n", slot, who == 1 ? "peer's" : "local",
        *(const uint8_t *)(uintptr_t)(status + STATUS_PARTS));
    return 1;
}

static int type_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    CoopAppearance peer;
    int who = avatar((uint8_t)c->x[1], &peer);
    uint64_t status = local_status();
    if (!who || (who == 2 && !status)) return 0;
    c->x[0] = 0x20 + (who == 1 ? peer.gender : *(const uint8_t *)(uintptr_t)(status + STATUS_GENDER)) % 2;
    return 1;
}

void coop_battle_model_install(void) {
    g_building = false; g_swapped = 0;
    rt_register_fnhook(TRAINER_MODEL, model_hook);
    rt_register_fnhook(SLOT_STATUS, status_hook);
    rt_register_fnhook(SLOT_TYPE, type_hook);
}
