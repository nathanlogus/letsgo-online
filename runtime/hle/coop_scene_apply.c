// Eevee 1.0.0: Shared Adventure's game side (runtime/coop_scene.c). Each mapped scene's flags are read with the game's
// FlagGet core (0x70001c3fb0, as the FlagGet native 0x70001a6870 calls it). An accepted scene goes in the way its own
// script ends: the gift with the Bag add the script's ItemAdd uses (0x70001c8e20, runtime/hle/gift.c), then the
// completion flags with the FlagSet core (0x70001c41e0, as the FlagSet native 0x70001a67b0 calls it, under the event
// work's lock). Both happen in one game-thread step, so every save image has both or neither: the flag is what makes
// a second gift impossible. Static analysis only: tests/cache/ghidra/out/sa-flags*.c, tests/logs/shared-adventure/.
#include "../hle.h"
#include "../fnhook.h"
#include "../coop_scene.h"

void rt_call_original(Cpu *, uint64_t);
uint64_t rt_addr(uint64_t addr);
bool fps60_social_overworld(void);
bool coop_menu_visible(void);
bool coop_meet_script_busy(void);

#define WORLD_GOT 0x70015fad08ull
#define FLAG_GET 0x70001c3fb0ull        // (event work, flag hash) -> bool
#define FLAG_SET 0x70001c41e0ull        // (event work, flag hash)
#define BAG_ADD 0x70001c8e20ull         // (bag, item, count, mark new) -> false: the pocket is full or the stack at 999
#define SAVE_SERIALIZE 0x70001cddf0ull  // the save menu builds the save image (runtime/hle/gift.c)

static bool supported;
static RuntimeFnHook previous_serialize;
static bool guest(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI; }
static uint64_t ptr(uint64_t p) { return guest(p) ? *(const uint64_t *)(uintptr_t)p : 0; }
static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Cpu k = *c;
    k.x[0] = a0, k.x[1] = a1, k.x[2] = a2, k.x[3] = a3;
    rt_call_original(&k, fn);
    return k.x[0];
}
typedef struct { Cpu *c; uint64_t work; } Ctx;
static bool flag(void *ctx, uint64_t hash) { Ctx *k = ctx; return call(k->c, FLAG_GET, k->work, hash, 0, 0) & 1; }

static int apply(Ctx *k, uint64_t bag, const CoopSceneDef *d) {
    int r = coop_scene_check(d, flag, k);
    if (r != COOP_SCENE_OK) return r;
    if (d->item && !(call(k->c, BAG_ADD, bag, d->item, d->count, 1) & 1)) return -1;
    uint64_t mutex = k->work + 0x48;  // (the FlagSet native's lock)
    call(k->c, ptr(ptr(mutex) + 0x10), mutex, 0, 0, 0);
    for (int i = 0; i < 2; i++) if (d->done[i]) call(k->c, FLAG_SET, k->work, coop_scene_hash(d->done[i]), 0, 0);
    call(k->c, ptr(ptr(mutex) + 0x18), mutex, 0, 0, 0);
    LOG("[coop-scene] applied scene %u: %s x%u, flag %s now %u\n", d->id, d->item_name ? d->item_name : "-", d->count, d->done[0],
        flag(k, coop_scene_hash(d->done[0])));
    return COOP_SCENE_OK;
}

// coop_actor_field (overworld updates, game thread).
void coop_scene_field(Cpu *c) {
    static uint64_t next;
    uint64_t now = plat_now_ns();
    if (!supported || now < next) return;
    next = now + 250000000ull;
    uint64_t fm = ptr(ptr(ptr(WORLD_GOT)) + 0x98), sd = ptr(fm + 0x58);
    Ctx k = {c, ptr(sd + 0x80)};
    uint64_t bag = ptr(sd + 0x68);
    if (!guest(fm) || !guest(k.work) || !guest(bag)) return;
    for (int s = 0; s < COOP_SCENES; s++) {
        bool reached = true;
        for (int i = 0; i < 2; i++) reached = reached && (!coop_scenes[s].needs[i] || flag(&k, coop_scene_hash(coop_scenes[s].needs[i])));
        coop_scene_observe(s, flag(&k, coop_scene_hash(coop_scenes[s].done[0])), reached);
    }
    // (not inside a script, a menu or a battle: the scene's own script may be running)
    int s;
    if (fps60_social_overworld() && !coop_menu_visible() && !coop_meet_script_busy() && coop_scene_to_apply(&s))
        coop_scene_applied(s, apply(&k, bag, &coop_scenes[s]));
}

static int serialize_hook(Cpu *c, uint64_t addr) {
    coop_scene_serialized();
    return previous_serialize ? previous_serialize(c, addr) : 0;
}

void coop_scene_configure(void) {
    supported = rt_addr(FLAG_SET) == FLAG_SET;  // shortcut: Eevee 1.0.0 addresses only; map them for Pikachu
    if (supported) rt_register_fnhook_chain(SAVE_SERIALIZE, serialize_hook, &previous_serialize);
}
RT_ON_RESET(coop_scene_on_reset) { coop_scene_reset(); }
