// Classic rules for the regular wild encounters created by wildbattle.c.
// Eevee 1.0.0 only. Story encounters retain their original setup and capture flow.
#include <math.h>
#include "../hle.h"
#include "../gamesetup.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *, uint64_t));
void rt_call_original(Cpu *c, uint64_t addr);
bool wildbattle_active(void);

#define SETUP_WILD       0x700028c770ull
#define POST_KIND        0x700031a400ull
#define CONTEXT_UPDATE   0x70009d2ce0ull
#define MATERIAL_INDEX   0x7000651b40ull
#define BUDDY_ANIMATION  0x7000a7e090ull
#define BAG_CONFIG       0x7000776ac0ull
#define BAG_SELECTION    0x7000779550ull
#define POKEMON_AT       0x58
#define WILD_HASH        0x57494c4442415454ull
#define BOX_CAPACITY     1000u

static bool classic_enabled, caught_stored;
static unsigned last_ball = 4;
static uint32_t bag_items[256]; // the native bag borrows these until it closes
static uint64_t catch_scene;
static bool catch_success, catch_hidden, catch_hand_removed, catch_ball_removed;
static unsigned catch_target;
static unsigned catch_last_frame;
static float catch_start[4], catch_hit[4], catch_land[4];
static uint64_t read64(uint64_t p);
static int optional_battle_effect_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    // The sequence player explicitly reaches this branch when an optional
    // particle instance could not be created. Its play method assumes non-null.
    return classic_enabled && wildbattle_active() && !c->x[0] && c->x[30] == 0x7000ae0aa0ull;
}

// The native ee420 timeline provides the trainer, camera, effects and sounds.
// Its ball command is stubbed in Let's Go; the implementation survives nearby.
// These hooks restore it only for our regular wild encounters.
enum { BALL_RELEASE = 92, BALL_HIT = 151, BALL_LAND = 170, BALL_BREAKOUT = 220, BALL_END = 250 };
static uint64_t catch_ball(unsigned id) {
    uint64_t map = read64(catch_scene + 0x4e0), buckets = read64(map + 0x10), count = read64(map + 0x20);
    uint64_t node = count ? read64(buckets + 8 * (id % count)) : 0;
    if (node) node = read64(node);
    for (unsigned i = 0; node && i < 256; i++, node = read64(node)) {
        if (read64(node + 8) != id) continue;
        uint64_t group = read64(node + 0x18);
        return read64(group + 0xa8) ? read64(read64(group + 0xa0)) : 0;
    }
    return 0;
}
static void hide_ball(Cpu *c, unsigned id) {
    // Creation already schedules the native lifetime job. Only hide here:
    // scheduling another retirement can unlink the same scene object twice.
    uint64_t ball = catch_ball(id);
    if (ball && read64(ball + 0x2a0)) {
        c->x[0] = ball; c->x[1] = 0;
        rt_call_original(c, 0x7000a9fdc0ull);
    }
}

static int catch_ball_create_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!catch_scene || c->x[1] != catch_scene) return 0;
    uint32_t info[6];
    memcpy(info, (void *)(uintptr_t)c->x[2], sizeof info);
    if (info[3] == 2) info[2] = BALL_END;
    c->x[2] = (uint64_t)(uintptr_t)info;
    rt_call_original(c, 0x7000ae4bf0ull);
    return 1;
}

static int catch_sequence_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    // Remove send-out text, Pokemon visibility/movement and the ball's release
    // animation. The capture arc and outcome control these explicitly.
    if (!catch_scene || read64(c->x[0] + 8) != catch_scene) return 0;
    unsigned op = *(uint32_t *)(uintptr_t)c->x[2];
    unsigned track = *(uint32_t *)(uintptr_t)(c->x[2] + 12);
    if (op == 7 || op == 12 || op == 16 || op == 178 || op == 116 || (op == 106 && track == 2)) return 1;
    if (op == 81) return 1; // the capture projectile follows its own arc
    return 0;
}

static int catch_animation_hook(Cpu *c, uint64_t addr) {
    if (!classic_enabled || !wildbattle_active() || c->x[1] >= 4 ||
        !read64(c->x[0] + 0x448 + 8 * c->x[1])) return 0;
    uint64_t scene = c->x[0];
    catch_scene = scene;
    catch_success = c->x[4] & 1;
    catch_target = (unsigned)c->x[1];
    catch_hidden = catch_hand_removed = catch_ball_removed = false;
    catch_last_frame = 0;
    rt_call_original(c, addr);
    Cpu saved = *c;
    uint64_t target = read64(scene + 0x448 + 8 * catch_target);
    c->x[0] = target; c->x[1] = 4; c->x[2] = (uint64_t)(uintptr_t)catch_hit;
    rt_call_original(c, 0x7000a9ca30ull);
    c->x[0] = target; c->x[1] = 0; c->x[2] = (uint64_t)(uintptr_t)catch_land;
    rt_call_original(c, 0x7000a9ca30ull);
    catch_land[1] += 28;
    if (catch_hit[1] < catch_land[1]) catch_hit[1] = catch_land[1];
    catch_start[0] = -35; catch_start[1] = 170; catch_start[2] = 485; catch_start[3] = 0;
    *(uint32_t *)(uintptr_t)(scene + 0x9a0) = catch_target;
    *(uint32_t *)(uintptr_t)(scene + 0x9a8) = catch_target;
    *(uint32_t *)(uintptr_t)(scene + 0x9b8) = 0;
    *(uint32_t *)(uintptr_t)(scene + 0x9d4) = last_ball;
    uint64_t sequence[6] = {0};
    c->x[0] = 0x7000f27725ull; // ee420: standard throw
    c->x[8] = (uint64_t)(uintptr_t)sequence;
    rt_call_original(c, 0x7000a81e90ull);
    // Match the native send-out call. The true mode bypasses the resource/map
    // cleanup at a7d344, leaving battle teardown waiting behind a black fade.
    c->x[0] = scene; c->x[1] = (uint64_t)(uintptr_t)sequence; c->x[2] = 0;
    rt_call_original(c, 0x7000a81690ull);
    for (int i = 4; i >= 0; i -= 4) if (sequence[i]) {
        c->x[0] = sequence[i] + 0x48;
        rt_call_original(c, *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)c->x[0] + 0x18));
    }
    *c = saved;
    return 1;
}
static int catch_animation_wait_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!classic_enabled || !wildbattle_active() || !catch_scene) return 0;
    Cpu saved = *c;
    uint64_t manager = read64(catch_scene + 0x3e0), process = read64(manager + 0x18);
    unsigned frame = process ? *(uint32_t *)(uintptr_t)(process + 0x180) : BALL_END;
    if (process) catch_last_frame = frame;
    else if (getenv("LARECOMP_WILDBATTLE_STATE_LOG")) LOG("[wild-classic] throw ended after frame %u\n", catch_last_frame);
    bool hidden = frame >= BALL_HIT && (catch_success || frame < BALL_BREAKOUT);
    if (hidden != catch_hidden) {
        uint64_t actor = read64(catch_scene + 0x448 + 8 * catch_target);
        if (actor) {
            c->x[0] = actor; c->x[1] = !hidden;
            rt_call_original(c, read64(read64(actor) + 0x58));
        }
        catch_hidden = hidden;
    }
    uint64_t held_ball = catch_ball(1);
    // Detach before the native frame-92 lifetime job releases its model pointer.
    if (frame >= BALL_RELEASE - 2 && !catch_hand_removed) {
        hide_ball(c, 1);
        catch_hand_removed = true;
    }
    uint64_t obj = frame < BALL_RELEASE ? held_ball : catch_ball(2);
    if (obj) {
        if (!catch_success && frame >= BALL_BREAKOUT && !catch_ball_removed) {
            hide_ball(c, 2);
            catch_ball_removed = true;
        }
        float *position = (float *)(uintptr_t)(obj + 0xb0);
        if (frame < BALL_RELEASE) memcpy(catch_start, position, sizeof catch_start);
        else {
            if (frame < BALL_HIT) {
                float t = (frame - BALL_RELEASE) / (float)(BALL_HIT - BALL_RELEASE);
                for (unsigned i = 0; i < 3; i++) position[i] = catch_start[i] + (catch_hit[i] - catch_start[i]) * t;
                position[1] += 100 * 4 * t * (1 - t);
            } else {
                float t = fminf(1, (frame - BALL_HIT) / (float)(BALL_LAND - BALL_HIT));
                for (unsigned i = 0; i < 3; i++) position[i] = catch_hit[i] + (catch_land[i] - catch_hit[i]) * t;
            }
            float angle = frame >= BALL_LAND && frame < 245 ? sinf((frame - BALL_LAND) * .25f) * .16f : 0;
            float *rotation = (float *)(uintptr_t)(obj + 0xa0);
            rotation[0] = rotation[1] = 0;
            rotation[2] = sinf(angle * .5f); rotation[3] = cosf(angle * .5f);
            *(uint8_t *)(uintptr_t)(obj + 0xc0) = 1;
        }
    }
    *c = saved;
    // The legacy UI's completion flag can clear before the added sequence ends.
    // Wait for the actual sequence process, so outcome text cannot race the ball.
    c->x[0] = !read64(manager + 0x18);
    if (c->x[0]) catch_scene = 0;
    return 1;
}

static bool guest(uint64_t p, size_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}
static uint64_t read64(uint64_t p) { return *(const uint64_t *)(uintptr_t)p; }
static bool active(void) { return classic_enabled && wildbattle_active(); }
static uint64_t manager(void) { return read64(read64(read64(0x70015fad08ull)) + 0x98); }
static uint64_t storage(void) { return read64(read64(manager() + 0x58) + 0xb0); }
static uint64_t allocator(void) { return read64(read64(read64(0x70015faab8ull)) + 0x68); }

bool wildbattle_classic_enabled(void) { return classic_enabled; }

static int setup_hook(Cpu *c, uint64_t addr) {
    if (!classic_enabled || !guest(c->x[1], 16)) return 0;
    uint64_t process = c->x[0], cfg = read64(c->x[1] + 8);
    if (!guest(cfg, 0x2b8) || read64(cfg + 0x2b0) != WILD_HASH) return 0;
    caught_stored = false;
    catch_scene = 0;
    last_ball = 4;
    rt_call_original(c, addr);
    // BattleSetup begins at +0xe0. Remove the legendary flag, XP exclusion
    // and timer which the static-wild builder adds after the common setup.
    *(uint32_t *)(uintptr_t)(process + 0x574) &= ~0x80u;
    *(uint8_t *)(uintptr_t)(process + 0x584) = 0;
    *(uint16_t *)(uintptr_t)(process + 0x562) = 0;
    *(uint8_t *)(uintptr_t)(process + 0x736) = 0;
    return 1;
}
static int context_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!active() || *(const uint32_t *)(uintptr_t)c->x[1] != 0) return 0;
    uint64_t setup = read64(c->x[0] + 0x80);
    // Apply this after any common rule preset, before the context copies timers.
    *(uint16_t *)(uintptr_t)(setup + 0x482) = 0;
    *(uint16_t *)(uintptr_t)(setup + 0x484) = 0;
    *(uint16_t *)(uintptr_t)(setup + 0x486) = 0;
    return 0;
}
static int material_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!active() || c->x[30] != 0x7000a9c9a0ull || c->x[0] != 0x210) return 0;
    // The wild faint effect can lack this optional material resource. Its
    // caller handles a missing material, but first dereferences the null model.
    c->x[0] = UINT32_MAX; // the native lookup's not-found result
    return 1;
}
static int buddy_animation_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!active()) return 0;
    // Static encounters have no transferred partner field model. Use the
    // standard send-out/switch animations, including for the starter itself.
    c->x[0] = 0;
    return 1;
}
static int buddy_model_form_hook(Cpu *c, uint64_t addr) {
    if (!active() || (c->x[30] != 0x70009d5bbCull && c->x[30] != 0x7000175ec8ull)) return 0;
    // Both render descriptors (party preload and actor creation) must agree on
    // the ordinary model. Static encounters cannot transfer the field buddy.
    // Battle form/stat lookups and the saved Pokemon remain untouched.
    Cpu saved = *c;
    rt_call_original(c, addr);
    unsigned form = c->x[0];
    *c = saved;
    rt_call_original(c, 0x7000724950ull);
    bool buddy = (c->x[0] == 133 && form == 1) || (c->x[0] == 25 && form == 8);
    *c = saved;
    if (!buddy) return 0;
    c->x[0] = 0;
    return 1;
}
static int bag_selection_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!active()) return 0;
    uint64_t model = read64(c->x[0] + 0x290);
    uint64_t count = read64(model + 0xe8);
    if (c->x[1] < count) {
        uint64_t entries = read64(model + 0xe0);
        uint64_t item = read64(entries + 16 * c->x[1]);
        unsigned id = *(const uint32_t *)(uintptr_t)item & 0x7fff;
        if (id && id <= 16) last_ball = id;
    }
    return 0;
}
static int bag_config_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (getenv("LARECOMP_WILDBATTLE_STATE_LOG")) LOG("[wild-classic] Bag setup: active %u, mode %u\n", active(), *(uint32_t *)(uintptr_t)c->x[1]);
    if (!active()) return 0;
    uint64_t cfg = c->x[1];
    // This is the battle bag. Capture/field bags have different pocket layouts.
    if (*(const uint32_t *)(uintptr_t)cfg != 1) return 0;
    Cpu saved = *c;
    c->x[0] = storage();
    rt_call_original(c, 0x70001c0f30ull);
    bool room = c->x[0] < BOX_CAPACITY;
    uint32_t count = 0;
    c->x[0] = read64(cfg + 0x10); // live save inventory
    c->x[1] = 4; // the game's capture pocket
    c->x[2] = (uint64_t)(uintptr_t)&count;
    rt_call_original(c, 0x70001c8fd0ull);
    const uint32_t *balls = (const uint32_t *)(uintptr_t)c->x[0];
    uint32_t medicine_count = 0;
    c->x[0] = read64(cfg + 0x10); c->x[1] = 0;
    c->x[2] = (uint64_t)(uintptr_t)&medicine_count;
    rt_call_original(c, 0x70001c8fd0ull);
    const uint32_t *medicine = (const uint32_t *)(uintptr_t)c->x[0];
    *c = saved;
    unsigned n = 0;
    // Items pack ID in bits 0..14 and quantity in bits 15..29. Keep real ball
    // quantities; GO-style berries stay in the separate capture system.
    if (room && count <= 50) for (unsigned i = 0; i < count; i++) {
        unsigned id = balls[i] & 0x7fff, qty = (balls[i] >> 15) & 0x7fff;
        if (id && id <= 16 && qty) bag_items[n++] = balls[i];
    }
    for (unsigned i = 0; i < medicine_count && n < sizeof bag_items / sizeof *bag_items; i++)
        if ((medicine[i] & 0x7fff) && ((medicine[i] >> 15) & 0x7fff)) bag_items[n++] = medicine[i];
    *(uint64_t *)(uintptr_t)(cfg + 0x58) = (uint64_t)(uintptr_t)bag_items;
    *(uint32_t *)(uintptr_t)(cfg + 0x60) = n;
    return 0;
}
static void store_caught(Cpu *c, uint64_t process) {
    Cpu saved = *c;
    uint64_t enemy_party = read64(process + 0xc8);
    c->x[0] = enemy_party;
    c->x[1] = 0; // these regular encounters have one opponent
    rt_call_original(c, read64(read64(enemy_party) + POKEMON_AT));
    uint64_t mon = c->x[0];
    // The combat engine resolves catches and consumes balls, but the static
    // encounter's field process normally leaves registration to GO capture.
    // Use the same native ownership/registration and party/box insertion path.
    c->x[0] = allocator(); c->x[1] = (uint64_t)(uintptr_t)&mon;
    c->x[2] = 0; c->x[3] = 0;
    rt_call_original(c, 0x7000b081d0ull);
    c->x[0] = mon; c->x[1] = last_ball;
    rt_call_original(c, 0x7000724d30ull);
    c->x[0] = read64(manager() + 0x60); c->x[1] = mon;
    rt_call_original(c, 0x7000171730ull);
    if (c->x[0]) caught_stored = true;
    else {
        c->x[0] = storage(); c->x[1] = mon;
        rt_call_original(c, 0x70001c0f50ull);
        caught_stored = c->x[0] < BOX_CAPACITY;
    }
    LOG("[wild-classic] caught Pokemon stored: %u, ball %u\n", caught_stored, last_ball);
    *c = saved;
}
static int post_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!classic_enabled || c->x[1] < 0xd0) return 0;
    uint64_t process = c->x[1] - 0xd0;
    if (!guest(process, 0x740)) return 0;
    uint64_t cfg = read64(process + 0xc0);
    if (!guest(cfg, 0x2b8) || read64(cfg + 0x2b0) != WILD_HASH) return 0;
    if (*(const uint32_t *)(uintptr_t)(process + 0x5e8) == 5 && !caught_stored) store_caught(c, process);
    // Keep common party updates/evolutions, and skip the legendary GO capture.
    *(uint32_t *)(uintptr_t)(c->x[0] + 0xb8) = 1;
    return 1;
}
static int xp_write_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!active()) return 0;
    Cpu saved = *c;
    rt_call_original(c, 0x700072afe0ull);
    uint32_t before = (uint32_t)c->x[0], after = (uint32_t)saved.x[1];
    *c = saved;
    if (before != after) LOG("[wild-classic] XP %u -> %u\n", before, after);
    return 0;
}
void wildbattle_classic_configure(int build) {
    static bool hooked;
    const char *override = getenv("LARECOMP_WILDBATTLE_CLASSIC");
    classic_enabled = build == GAME_BUILD_EEVEE_100 && (!override || atoi(override) != 0);
    if (!classic_enabled || hooked) return;
    hooked = true;
    rt_register_fnhook(SETUP_WILD, setup_hook);
    rt_register_fnhook(CONTEXT_UPDATE, context_hook);
    rt_register_fnhook(MATERIAL_INDEX, material_hook);
    rt_register_fnhook(BUDDY_ANIMATION, buddy_animation_hook);
    rt_register_fnhook(0x7000726a50ull, buddy_model_form_hook);
    rt_register_fnhook(BAG_CONFIG, bag_config_hook);
    rt_register_fnhook(BAG_SELECTION, bag_selection_hook);
    rt_register_fnhook(POST_KIND, post_hook);
    const char *animation = getenv("LARECOMP_WILDBATTLE_CATCH_ANIMATION");
    if (!animation || atoi(animation) != 0) {
        rt_register_fnhook(0x7000a8d6e0ull, catch_animation_hook);
        rt_register_fnhook(0x7000a8d720ull, catch_animation_wait_hook);
        rt_register_fnhook(0x7000aa0f60ull, catch_sequence_hook);
        rt_register_fnhook(0x7000ae5170ull, catch_ball_create_hook);
        rt_register_fnhook(0x700067c6b0ull, optional_battle_effect_hook);
    }
    if (getenv("LARECOMP_WILDBATTLE_STATE_LOG")) rt_register_fnhook(0x7000731d00ull, xp_write_hook);
    LOG("[wild-classic] native combat, defeat XP and battle bag catching enabled\n");
}
