// Meet-up for Eevee 1.0.0: after the partner accepts, the requester flies to the partner's town with the game's own
// Fly process, under the game's own Fly checks on the requester's save. Decisions: runtime/coop_meet.h.
// Static evidence (tests/logs/meetup/): the town map's choice is FNV-1a 64 of the town's zone name ("town002"), it is
// offered when FSYS_ARRIVE_<TOWN002> is set, the zone allows Fly (FLY_HERE) and the landing point exists (FLY_POINT);
// the field menu and the item process then start Fly with FLY_START(world, &choice).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "../hle.h"
#include "../coop_net.h"
#include "../coop_meet.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *, uint64_t));
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t rt_addr(uint64_t addr);
bool fps60_social_overworld(void);
bool coop_menu_visible(void);

#define WORLD_GOT 0x70015fad08ull
#define ZONE_GET 0x700024a600ull   // the player's zone hash
#define FLY_HERE 0x700024a670ull   // 0: this zone allows Fly (asked by the field menu and the town map)
#define FLY_POINT 0x70001331c0ull  // x0 field manager, x1 town zone hash: its landing point exists
#define FLAG_GET 0x70001c3fb0ull   // x0 event work, x1 flag hash (the FlagGet native's call)
#define FLY_START 0x700029dd70ull  // x0 world, x1 &town zone hash: the Fly process (field menu f_70002800f0)
#define AMX_EXEC 0x70006708a0ull   // the field script interpreter

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static CoopMeetPlan g_plan = {COOP_MEET_NO_PARTNER, -1, false};
static char g_toast[160];
static uint64_t g_toast_until;
static _Atomic uint64_t g_script_at;
static struct { int zone; float x, z; uint8_t identity[16]; } g_last = {-1};
static uint32_t g_handled, g_travel, g_notified;
static uint64_t g_travel_until;

static bool guest(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI; }
static uint64_t ptr(uint64_t p) { return guest(p) ? *(const uint64_t *)(uintptr_t)p : 0; }

static void toast(const char *s) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_toast, sizeof g_toast, "%s", s); g_toast_until = plat_now_ns() + 5000000000ull;
    pthread_mutex_unlock(&g_lock);
    LOG("[coop-meet] %s\n", s);
}
bool coop_meet_toast(char *out, unsigned size) {
    pthread_mutex_lock(&g_lock);
    bool on = plat_now_ns() < g_toast_until;
    if (on && size) snprintf(out, size, "%s", g_toast);
    pthread_mutex_unlock(&g_lock); return on;
}
void coop_meet_view(CoopMeetPlan *out) { pthread_mutex_lock(&g_lock); *out = g_plan; pthread_mutex_unlock(&g_lock); }

static int script_hook(Cpu *c, uint64_t addr) { (void)c, (void)addr; atomic_store(&g_script_at, plat_now_ns()); return 0; }
bool coop_meet_script_busy(void) {
    uint64_t at = atomic_load(&g_script_at), now = plat_now_ns();
    return at && now >= at && now - at < 500000000ull;
}

// The plan on the traveller's own save; the busy states of the traveller are checked when it acts.
static int plan(Cpu *c, int *town, bool *exact) {
    *town = -1; *exact = false;
    CoopRemote peer;
    if (!coop_net_remote(&peer)) { g_last.zone = -1; return COOP_MEET_NO_PARTNER; }
    if (memcmp(g_last.identity, peer.identity, 16)) { g_last.zone = -1; memcpy(g_last.identity, peer.identity, 16); }
    uint64_t now = plat_now_ns();
    bool ready = peer.activity == COOP_OVERWORLD && peer.world_ready && now >= peer.received_ns && now - peer.received_ns < 1000000000ull;
    int zone = coop_meet_zone_of(peer.area);
    if (ready && zone >= 0) g_last.zone = zone, g_last.x = peer.x, g_last.z = peer.z;
    if (!ready) return COOP_MEET_PARTNER_BUSY;
    uint64_t world = ptr(ptr(WORLD_GOT)), fm = ptr(world + 0x98), work = ptr(ptr(fm + 0x58) + 0x80);
    if (!guest(world) || !guest(fm) || !guest(work)) return COOP_MEET_BUSY;
    Cpu call = *c;
    rt_call_original(&call, FLY_HERE);
    if ((uint32_t)call.x[0]) return COOP_MEET_CANT_FLY_HERE;
    rt_call_original(&call, ZONE_GET);
    int here = coop_meet_town_of(call.x[0]);
    uint16_t usable = 0;
    for (int t = 0; t < COOP_MEET_TOWNS; t++) {
        char flag[32]; snprintf(flag, sizeof flag, "FSYS_ARRIVE_TOWN%03d", t + 1);
        call.x[0] = work; call.x[1] = coop_meet_hash(flag); rt_call_original(&call, FLAG_GET);
        if (!(call.x[0] & 1)) continue;
        call.x[0] = fm; call.x[1] = coop_meet_hash(coop_meet_town[t].zone); rt_call_original(&call, FLY_POINT);
        if (call.x[0] & 1) usable |= 1u << t;
    }
    int reason = coop_meet_plan(g_last.zone, g_last.x, g_last.z, usable, here, town, exact);
    static int logged = -1, logged_town = -2, logged_zone = -2;
    if (reason != logged || *town != logged_town || g_last.zone != logged_zone) {
        logged = reason, logged_town = *town, logged_zone = g_last.zone;
        LOG("[coop-meet] plan: friend in %s (%s), visited towns %03x, here %d -> %s%s\n",
            g_last.zone >= 0 ? coop_meet_zone[g_last.zone].zone : "?", zone >= 0 ? "outdoors" : "indoors / unknown", usable,
            here, *town >= 0 ? coop_meet_town[*town].name : "-", reason ? " (refused)" : *exact ? " (their town)" : " (nearest town)");
    }
    return reason;
}

void coop_meet_field(Cpu *c) {
    if (!rt_addr(FLY_START) || !coop_net_enabled()) return;
    uint64_t now = plat_now_ns();
    CoopTeamUp meet; coop_net_meetup(&meet);
    uint8_t local = coop_net_local_slot();
    CoopRemote peer; const char *name = coop_net_remote(&peer) ? peer.name : "Your friend";
    if (meet.state == 2 && meet.id != g_handled) {
        g_handled = meet.id;
        if (meet.from_slot == local) { g_travel = meet.id; g_travel_until = now + 20000000000ull; LOG("[coop-meet] accepted: travelling\n"); }
        else if (meet.id != g_notified) { char s[96]; g_notified = meet.id; snprintf(s, sizeof s, "%s is flying to meet you", name); toast(s); }
    }
    static uint64_t next;
    if (now < next && !g_travel) return;
    next = now + 250000000ull;
    CoopMeetPlan p = {0};
    p.reason = plan(c, &p.town, &p.exact);
    pthread_mutex_lock(&g_lock); g_plan = p; pthread_mutex_unlock(&g_lock);
    if (!g_travel) return;
    bool ready = fps60_social_overworld() && !coop_menu_visible() && !coop_meet_script_busy();
    if (p.reason == COOP_MEET_OK && ready) {
        uint64_t world = ptr(ptr(WORLD_GOT)), dest = coop_meet_hash(coop_meet_town[p.town].zone);
        Cpu call = *c;
        call.x[0] = world; call.x[1] = (uint64_t)(uintptr_t)&dest;
        rt_call_original(&call, FLY_START);
        char s[96]; snprintf(s, sizeof s, "Flying to %s", coop_meet_town[p.town].name); toast(s);
        g_travel = 0;
    } else if (now >= g_travel_until || (p.reason != COOP_MEET_OK && p.reason != COOP_MEET_PARTNER_BUSY && p.reason != COOP_MEET_BUSY)) {
        char s[160]; snprintf(s, sizeof s, "Meet-up cancelled: %s", coop_meet_reason(p.reason ? p.reason : COOP_MEET_BUSY)); toast(s);
        g_travel = 0;
    }
}

void coop_meet_configure(void) { rt_register_fnhook(AMX_EXEC, script_hook); }
RT_ON_RESET(coop_meet_reset) {
    g_travel = g_handled = g_notified = 0; g_last.zone = -1; g_toast_until = 0; atomic_store(&g_script_at, 0);
    g_plan = (CoopMeetPlan){COOP_MEET_NO_PARTNER, -1, false};
}
