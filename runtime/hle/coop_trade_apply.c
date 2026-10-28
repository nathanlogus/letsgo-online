// Eevee 1.0.0: an online trade decided by the relay (runtime/coop_trade.c) goes into the game the way its own Link Trade
// finishes (0x7000886630 state 5, the NPC trade's 0x700019fc00): the received Pokemon gets this player as its handling
// trainer (0x7000b0c3b0) and is copied over the sent Pokemon's storage slot (0x70001c0fd0). Let's Go keeps every
// Pokemon in one 1000-slot storage and the party is indices into it, so nothing is removed or added and the party
// order stays. The Pokedex follows the trade demo (0x7000876570 state 2: 0x7000b081d0 type 4, which never rewrites the
// OT; its registration 0x70001cffe0) and the trade evolution follows the scene-less trade of 0x7000838900 (check
// 0x70007283e0, evolve 0x7000728450, register again). Static analysis only: tests/cache/ghidra/out/trade-*.c.
#include <string.h>
#include "../hle.h"
#include "../fnhook.h"
#include "../coop_trade.h"
#include "coop_battle.h"

void rt_call_original(Cpu *, uint64_t);
uint64_t rt_addr(uint64_t addr);

#define WORLD_GOT 0x70015fad08ull
#define ALLOC_GOT 0x70015faab8ull
#define SERIALIZE 0x7000729650ull
#define IMPORT 0x7000729660ull
#define CONSTRUCT 0x7000729340ull
#define BEGIN 0x7000728e90ull
#define END 0x7000728ee0ull
#define STORAGE_COUNT 0x70001c0f30ull
#define STORAGE_SET 0x70001c0fd0ull       // (storage, mon, index): copies mon over that slot
#define HANDLER 0x7000b0c3b0ull           // (mon, mystatus): handling trainer, unless this player is the OT
#define REGISTER 0x7000b081d0ull          // (alloc, &mon, type, 0): type 4 = traded; 0 -> register it ourselves
#define DEX_CAUGHT 0x70001cffe0ull        // (zukan, mon)
#define TRADE_EVOLUTION 0x70007283e0ull   // (mon, traded with, &species, &form) -> evolves
#define EVOLVE 0x7000728450ull            // (mon, species, form)
#define IS_PARTNER 0x70001760d0ull        // (mon): Pikachu form 8 / Eevee form 1
#define SAVE_SERIALIZE 0x70001cddf0ull    // the save menu builds the save image (runtime/hle/gift.c)

static bool supported;
static RuntimeFnHook previous_serialize;
static bool guest(uint64_t p, size_t n) { return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p; }
static uint64_t ptr(uint64_t p) { return guest(p, 8) ? *(const uint64_t *)(uintptr_t)p : 0; }
static uint64_t save_data(void) { return ptr(ptr(ptr(ptr(WORLD_GOT)) + 0x98) + 0x58); }
static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Cpu k = *c;
    k.x[0] = a0, k.x[1] = a1, k.x[2] = a2, k.x[3] = a3;
    rt_call_original(&k, fn);
    return k.x[0];
}
static void serialize(Cpu *c, uint64_t mon, uint8_t out[COOP_PB7_BYTES]) {
    uint64_t changed = call(c, BEGIN, mon, 0, 0, 0) & 1;
    call(c, SERIALIZE, mon, (uint64_t)(uintptr_t)out, 0, 0);
    call(c, END, mon, changed, 0, 0);
}
// Storage slots holding the given (sent) and taken (received) Pokemon, -1 when absent, -2 when more than one.
static void find(Cpu *c, uint64_t storage, const uint8_t give[COOP_PB7_BYTES], const uint8_t take[COOP_PB7_BYTES], int *gi, int *ti) {
    unsigned n = (unsigned)call(c, STORAGE_COUNT, storage, 0, 0, 0);
    *gi = *ti = -1;
    for (unsigned i = 0; i < n && i < 1000; i++) {
        uint64_t mon = ptr(storage + 0x3f800 + 8 * i);  // (the slot array STORAGE_SET writes)
        if (!guest(mon, 0xb8)) continue;
        uint8_t data[COOP_PB7_BYTES];
        serialize(c, mon, data);
        if (coop_trade_same(data, give)) *gi = *gi == -1 ? (int)i : -2;
        if (coop_trade_same(data, take)) *ti = *ti == -1 ? (int)i : -2;
    }
}
// The trade demo's registration (0x7000876570 state 2) holds its own reference across the call; 0: register it here.
static void dex(Cpu *c, uint64_t alloc, uint64_t zukan, uint64_t mon) {
    uint64_t ref = mon;
    call(c, ptr(ptr(mon + 0x48) + 0x10), mon + 0x48, 0, 0, 0);
    if (!(call(c, REGISTER, alloc, (uint64_t)(uintptr_t)&ref, 4, 0) & 1)) call(c, DEX_CAUGHT, zukan, mon, 0, 0);
    if (guest(ref, 0xb8)) call(c, ptr(ptr(ref + 0x48) + 0x18), ref + 0x48, 0, 0, 0);
}
// 1 applied, 0 already in this save, -1 neither state (quarantine).
static int apply(Cpu *c, const uint8_t give[COOP_PB7_BYTES], const uint8_t take[COOP_PB7_BYTES], bool disk) {
    uint64_t sd = save_data(), storage = ptr(sd + 0xb0), mystatus = ptr(sd + 0x78), zukan = ptr(sd + 0x88);
    uint64_t alloc = ptr(ptr(ptr(ALLOC_GOT)) + 0x68), allocate = ptr(ptr(alloc) + 0x10);
    if (!guest(storage, 0x3f800 + 8 * 1000) || !guest(mystatus, 8) || !guest(zukan, 8) || !guest(allocate, 4)) return -1;
    int gi, ti;
    find(c, storage, give, take, &gi, &ti);
    LOG("[coop-trade] storage: sent Pokemon at %d, received at %d\n", gi, ti);
    if (gi == -1 && ti >= 0) return disk ? 0 : 1;
    if (gi < 0 || ti != -1) return -1;
    uint64_t sent = ptr(storage + 0x3f800 + 8 * (unsigned)gi);
    if (call(c, IS_PARTNER, sent, 0, 0, 0) & 1) return -1;
    // A native Pokemon on the game's heap, owned by its shared reference as the game's own copies are (0x7000838900).
    uint64_t mon = call(c, allocate, alloc, 0xb8, 8, 0);
    if (!guest(mon, 0xb8)) return -1;
    call(c, CONSTRUCT, mon, alloc, 0, 0);
    uint64_t *meta = (uint64_t *)(uintptr_t)(mon + 8);
    meta[0] = alloc, meta[1] = mon, meta[2] = 0xb8, meta[3] = 8;
    call(c, ptr(ptr(mon + 0x48) + 0x10), mon + 0x48, 0, 0, 0);
    call(c, IMPORT, mon, (uint64_t)(uintptr_t)take, 0, 0);
    call(c, HANDLER, mon, mystatus, 0, 0);
    dex(c, alloc, zukan, mon);
    uint64_t species = 0, form = 0;  // (the game passes 8-byte locals)
    if (call(c, TRADE_EVOLUTION, mon, mon, (uint64_t)(uintptr_t)&species, (uint64_t)(uintptr_t)&form) & 1) {
        LOG("[coop-trade] trade evolution to species %u form %u\n", (unsigned)species, (unsigned)form);
        call(c, EVOLVE, mon, (uint32_t)species, (uint32_t)form, 0);
        dex(c, alloc, zukan, mon);
    }
    bool stored = call(c, STORAGE_SET, storage, mon, (uint64_t)gi, 0) & 1;
    call(c, ptr(ptr(mon + 0x48) + 0x18), mon + 0x48, 0, 0, 0);
    find(c, storage, give, take, &gi, &ti);
    LOG("[coop-trade] applied: stored %u, sent Pokemon at %d, received at %d\n", stored, gi, ti);
    return stored && gi == -1 && ti >= 0 ? 1 : -1;
}

// coop_actor_field (overworld updates, game thread).
void coop_trade_field(Cpu *c) {
    if (!supported) return;
    CoopTradeView v;
    coop_trade_view(&v);
    static uint64_t next;
    uint64_t now = plat_now_ns();
    if ((v.phase == COOP_TRADE_OPEN || v.phase == COOP_TRADE_ASKED || v.phase == COOP_TRADE_ASKING) && now >= next) {
        CoopParty party;  // (the offer list; a second is plenty while the trade window is open)
        if (coop_battle_capture_party(c, &party)) coop_trade_set_party(&party);
        next = now + 1000000000ull;
    }
    uint8_t give[COOP_PB7_BYTES], take[COOP_PB7_BYTES];
    bool disk;
    if (coop_trade_to_apply(give, take, &disk)) coop_trade_applied(apply(c, give, take, disk));
}

static int serialize_hook(Cpu *c, uint64_t addr) {
    coop_trade_serialized();
    return previous_serialize ? previous_serialize(c, addr) : 0;
}

void coop_trade_configure(void) {
    supported = rt_addr(STORAGE_SET) == STORAGE_SET;  // shortcut: Eevee 1.0.0 addresses only; map them for Pikachu
    if (supported) rt_register_fnhook_chain(SAVE_SERIALIZE, serialize_hook, &previous_serialize);
}
RT_ON_RESET(coop_trade_on_reset) { coop_trade_reset(); }
