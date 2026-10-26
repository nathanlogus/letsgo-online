// Social avatar bridge for Eevee 1.0.0. Use the game's trainer resolver, field transform
// layout and P2 model factories. The network worker only ever receives copied values.
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "../hle.h"
#include "../coop_net.h"
#include "coop_actor.h"
#include "coop_battle.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *, uint64_t));
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t rt_addr(uint64_t addr);
void input_coop_left_stick(int16_t *x, int16_t *y);
bool input_remote_avatar_active(void);
void input_remote_avatar_rejoin(void);
bool input_remote_avatar_refresh(void);

#define TRAINER_GET 0x7000245750ull // w0 = slot; x8 = out shared_ptr
#define ZONE_GET 0x700024a600ull    // current player zone hash
#define FIELD_TRANSFORM 0x7000213c80ull
#define P2_SPAWN_POSITION 0x700026cfa0ull
#define P2_MODEL 0x7000afced0ull
#define P2_MODEL_BUDDY 0x7000afc810ull
#define WORLD_GOT 0x70015fad08ull
#define COOP_MODE 0x7000142720ull
#define PARTY_SELECTION 0x70002462e0ull
#define P2_EJECT 0x700026cf40ull
#define RECORD_ADD 0x70001cad40ull // x0 = record block, w1 = record id
#define RECORD_P2_JOIN 0x1e9      // save CaptureRecord +0x7A4; added by the controller-mode update (0x7000142840)

static _Atomic uint64_t g_p2;
static CoopAppearance g_model_appearance;
static bool g_had_remote;
static uint64_t g_missing_since;
static pthread_mutex_t g_pose_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { bool visible; float x, y, z, yaw; uint32_t area; uint64_t at; } g_pose;

static bool guest(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI; }
static uint64_t ptr(uint64_t p) { return guest(p) ? *(const uint64_t *)(uintptr_t)p : 0; }
static uint64_t field_manager(void) { return ptr(ptr(ptr(WORLD_GOT)) + 0x98); }

static uint64_t trainer(Cpu *c, unsigned slot) {
    uint64_t result = 0;
    c->x[0] = slot;
    c->x[8] = (uint64_t)(uintptr_t)&result;
    rt_call_original(c, TRAINER_GET);
    return guest(result) ? result : 0;
}
static void release(Cpu *c, uint64_t actor) {
    if (!actor) return;
    uint64_t fn = ptr(ptr(actor + 0x48) + 0x18);
    if (!guest(fn)) return;
    c->x[0] = actor + 0x48;
    guest_call(c, fn);
}

static void apply_pose(uint64_t actor) {
    if (!actor || !input_remote_avatar_active()) return;
    // The game's SetFieldObjectActive{Static,Dynamic}Collision_ natives resolve
    // these components and write the same active byte. A social replica must not
    // push local actors or touch an encounter / item trigger.
    const unsigned collision[] = {0x320, 0x328};
    for (unsigned i = 0; i < 2; i++) {
        uint64_t component = ptr(ptr(actor + collision[i]) + 0x60);
        if (guest(component)) *(uint8_t *)(uintptr_t)(component + 0x10) = 0;
    }
    pthread_mutex_lock(&g_pose_lock);
    // SetFieldObjectVisibility_ writes this same inherited field-object flag.
    bool visible = g_pose.visible && coop_net_remote_near(20000000);
    *(uint8_t *)(uintptr_t)(actor + 0x290) = visible;
    if (visible) {
        float xyz[3] = {g_pose.x, g_pose.y, g_pose.z};
        float q[4] = {0, sinf(g_pose.yaw * 0.5f), 0, cosf(g_pose.yaw * 0.5f)};
        memcpy((void *)(uintptr_t)(actor + 0xb0), xyz, sizeof xyz);
        memcpy((void *)(uintptr_t)(actor + 0xa0), q, sizeof q);
        // Same transform-dirty flag used by SetFieldObjectPositionXYZ_ / angle setter.
        *(uint8_t *)(uintptr_t)(actor + 0xc0) = 1;
    }
    pthread_mutex_unlock(&g_pose_lock);
}

static int transform_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[0] == atomic_load(&g_p2)) apply_pose(c->x[0]);
    return 0;
}

static int spawn_position_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    CoopRemote peer;
    if (!input_remote_avatar_active() || !coop_net_remote(&peer) || !coop_net_remote_near(20000000)) return 0;
    float v[4] = {peer.x, peer.y, peer.z, 0};
    memcpy(&c->v[0], v, sizeof v);
    LOG("[coop-actor] native P2 spawn at peer position %.1f %.1f %.1f\n", peer.x, peer.y, peer.z);
    return 1;
}

static int model_hook(Cpu *c, uint64_t addr) {
    CoopRemote peer;
    uint64_t mgr = field_manager();
    if (!input_remote_avatar_active() || !mgr || !coop_net_remote(&peer) || !peer.appearance.valid) return 0;
    // These two P2 factories copy the manager's 0x50-byte resource-ID block. Scope the
    // substitution to the factory call; the main trainer's save and appearance stay local.
    uint8_t saved[COOP_APPEARANCE_BYTES];
    // In the factories x25 is already manager+0x48 when they add 0xbf0.
    void *style = (void *)(uintptr_t)(mgr + 0xc38);
    memcpy(saved, style, sizeof saved);
    memcpy(style, peer.appearance.parts, sizeof saved);
    rt_call_original(c, addr);
    memcpy(style, saved, sizeof saved);
    g_model_appearance=peer.appearance;
    LOG("[coop-actor] P2 model built with peer appearance: gender %u, style %u\n", peer.appearance.gender, peer.appearance.style);
    return 1;
}

void coop_actor_field(Cpu *c) {
    if (!rt_addr(TRAINER_GET)) return;
    coop_battle_field(c);
    Cpu call = *c;
    uint64_t local = trainer(&call, 0);
    if (!local) return;
    const float *p = (const float *)(uintptr_t)(local + 0xb0);
    const float *q = (const float *)(uintptr_t)(local + 0xa0);
    float local_xyz[3]; memcpy(local_xyz, p, sizeof local_xyz);
    uint64_t mgr = field_manager(), status = ptr(ptr(mgr + 0x58) + 0x78);
    if (status) {
        CoopAppearance appearance = {.valid = true};
        appearance.gender = *(const uint8_t *)(uintptr_t)(status + 0x5d);
        appearance.style = *(const uint8_t *)(uintptr_t)(status + 0x8d);
        memcpy(appearance.parts, (const void *)(uintptr_t)(status + 0x160), sizeof appearance.parts);
        if (appearance.gender <= 1 && appearance.style <= 10) coop_net_publish_appearance(&appearance);
    }
    rt_call_original(&call, ZONE_GET);
    uint64_t zone = call.x[0];
    uint32_t area = (uint32_t)zone ^ (uint32_t)(zone >> 32);
    int16_t sx = 0, sy = 0;
    input_coop_left_stick(&sx, &sy);
    float yaw = atan2f(2 * (q[3] * q[1] + q[0] * q[2]), 1 - 2 * (q[1] * q[1] + q[2] * q[2]));
    if (isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && isfinite(yaw))
        coop_net_publish_position(p[0], p[1], p[2], yaw, area, (sx || sy) ? 1 : 0, sx, sy);
    static uint64_t logged_local;
    if (logged_local != local) {
        logged_local = local;
        LOG("[coop-actor] P1 %llx position %.1f %.1f %.1f zone %llx\n", (unsigned long long)local, p[0], p[1], p[2], (unsigned long long)zone);
    }
    release(&call, local);

    if (!coop_net_enabled()) return; // Solo emotes only need the local anchor.

    CoopRemote peer;
    bool visible = input_remote_avatar_active() && coop_net_remote(&peer) && coop_net_remote_near(20000000);
    if(visible && g_model_appearance.valid && memcmp(&g_model_appearance,&peer.appearance,sizeof g_model_appearance) &&
        input_remote_avatar_refresh()) {
        g_model_appearance=peer.appearance;
    }
    uint64_t remote = input_remote_avatar_active() ? trainer(&call, 1) : 0;
    atomic_store(&g_p2, remote);
    uint64_t now = plat_now_ns();
    if (remote) { g_had_remote = true; g_missing_since = 0; }
    else if (g_had_remote && coop_net_avatar_ready()) {
        if (!g_missing_since) g_missing_since = now;
        if (now >= g_missing_since && now - g_missing_since >= 1500000000ull) {
            input_remote_avatar_rejoin(); g_missing_since = now + 1500000000ull;
        }
    } else g_missing_since = 0;
    pthread_mutex_lock(&g_pose_lock);
    if (visible) {
        float dx = peer.x - g_pose.x, dy = peer.y - g_pose.y, dz = peer.z - g_pose.z;
        bool snap = !g_pose.visible || area != g_pose.area || dx*dx + dy*dy + dz*dz > 500*500;
        float dt = g_pose.at && now >= g_pose.at ? (float)(now - g_pose.at) / 1e9f : 0;
        CoopNetStats stats; coop_net_stats(&stats);
        float smoothing = fminf(fmaxf(stats.snapshot_interval_ms * 0.0015f, 0.025f), 0.075f);
        float a = snap ? 1 : fminf(dt / smoothing, 1);
        g_pose.x += dx*a; g_pose.y += dy*a; g_pose.z += dz*a;
        float turn = remainderf(peer.yaw - g_pose.yaw, 2 * 3.14159265358979323846f);
        g_pose.yaw += turn*a;
        g_pose.area = area;
    }
    g_pose.visible = visible && remote;
    g_pose.at = now;
    // A trainer's hat top is approximately 150 cm above its field-object origin.
    coop_net_set_avatar_head(g_pose.visible, g_pose.x, g_pose.y + 150, g_pose.z);
    pthread_mutex_unlock(&g_pose_lock);
    apply_pose(remote);
    if (remote && getenv("LARECOMP_COOP_DEBUG")) {
        static unsigned samples;
        if ((samples++ % 300) == 0) {
            const float *actual = (const float *)(uintptr_t)(remote + 0xb0);
            uint64_t dynamic = ptr(ptr(remote + 0x328) + 0x60), fixed = ptr(ptr(remote + 0x320) + 0x60);
            LOG("[coop-social] P1 %.1f %.1f %.1f P2 %.1f %.1f %.1f visible %u collisions %d/%d\n",
                local_xyz[0],local_xyz[1],local_xyz[2],actual[0],actual[1],actual[2],
                *(const uint8_t *)(uintptr_t)(remote+0x290),
                guest(dynamic) ? *(const uint8_t *)(uintptr_t)(dynamic+0x10) : -1,
                guest(fixed) ? *(const uint8_t *)(uintptr_t)(fixed+0x10) : -1);
        }
    }
    release(&call, remote);
}

static int social_party_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!coop_net_enabled() || (uint32_t)c->x[0] != 1) return 0;
    static bool logged;
    if (!logged) { logged = true; LOG("[coop-social] blocked local P2 party selection\n"); }
    // Remote trainers never borrow a second Pokémon from this save's party.
    if (guest(c->x[1])) *(uint32_t *)(uintptr_t)c->x[1] = 0;
    c->x[0] = 0; return 1;
}
static int social_mode_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    // The mapped battle consumers of the native controller-mode accessor must
    // see solo mode. Preserve the field controller lifecycle so it can draw P2.
    uint64_t caller = c->x[30];
    if (coop_net_enabled() && caller == 0x70002f87f8ull) {
        // This one field-lifecycle call gates the distance / viewport recovery
        // branch. Other calls in that function still observe real join state.
        c->x[0] = 0; return 1;
    }
    if (!coop_net_enabled() || caller < 0x7000700000ull || caller >= 0x7000b00000ull) return 0;
    static bool logged;
    if (!logged) { logged = true; LOG("[coop-social] battle mode forced solo (caller %llx)\n", (unsigned long long)caller); }
    c->x[0] = 0; return 1;
}
static int social_eject_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    // Remote position is authoritative. Native viewport / script ejection cannot
    // teleport the other save's trainer; visibility is controlled by scene state.
    return coop_net_enabled() && input_remote_avatar_active();
}

static int social_record_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    // The social P2 is the other player's trainer, not a local 2-player session: keep this save's record unchanged.
    if (!coop_net_enabled() || (uint32_t)c->x[1] != RECORD_P2_JOIN) return 0;
    LOG("[coop-social] skipped the native 2-player join record\n");
    c->x[0] = 0; return 1;
}

void coop_actor_configure(void) {
    atomic_store(&g_p2, 0);
    g_had_remote = false; g_missing_since = 0;
    memset(&g_model_appearance,0,sizeof g_model_appearance);
    memset(&g_pose, 0, sizeof g_pose);
    rt_register_fnhook(FIELD_TRANSFORM, transform_hook);
    rt_register_fnhook(P2_SPAWN_POSITION, spawn_position_hook);
    rt_register_fnhook(P2_MODEL, model_hook);
    rt_register_fnhook(P2_MODEL_BUDDY, model_hook);
    rt_register_fnhook(PARTY_SELECTION, social_party_hook);
    rt_register_fnhook(COOP_MODE, social_mode_hook);
    rt_register_fnhook(P2_EJECT, social_eject_hook);
    rt_register_fnhook(RECORD_ADD, social_record_hook);
}
