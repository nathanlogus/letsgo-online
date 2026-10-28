// Online trades: negotiation (relay W channel), the durable record <save>.trade and its recovery. See
// docs/COOP-TRADES.md for the states, the messages and what each side does after a crash or a lost link.
#include "coop_trade.h"
#include "platform.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG(...) fprintf(stderr, __VA_ARGS__)  // (as rt.h)

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static void p32(uint8_t *p, uint32_t x) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(x >> 8 * i); }
static void p64(uint8_t *p, uint64_t x) { p32(p, (uint32_t)x); p32(p + 4, (uint32_t)(x >> 32)); }

// ------------------------------------------------------------------ PB7 fields
// Decrypted, in logical block order: header 0..7, blocks A B C D at 8 + 56k, party stats at 232 (PKHeX PB7 offsets).
static bool decrypt(const uint8_t d[COOP_PB7_BYTES], uint8_t pk[COOP_PB7_BYTES]) {
    if (!coop_party_pokemon_valid(d)) return false;
    uint32_t seed = u32(d);
    uint8_t core[224], stats[28];
    for (unsigned i = 0; i < 224; i += 2) { seed = seed * 0x41c64e6du + 0x6073u; uint16_t v = u16(d + 8 + i) ^ (seed >> 16); core[i] = v, core[i + 1] = v >> 8; }
    seed = u32(d);
    for (unsigned i = 0; i < 28; i += 2) { seed = seed * 0x41c64e6du + 0x6073u; uint16_t v = u16(d + 232 + i) ^ (seed >> 16); stats[i] = v, stats[i + 1] = v >> 8; }
    unsigned rank = ((u32(d) >> 13) & 31) % 24, remaining[4] = {0, 1, 2, 3}, physical[4];
    const unsigned factorial[4] = {6, 2, 1, 1};
    for (unsigned slot = 0; slot < 4; slot++) {
        unsigned digit = rank / factorial[slot]; rank %= factorial[slot];
        physical[remaining[digit]] = slot;
        for (unsigned j = digit; j < 3 - slot; j++) remaining[j] = remaining[j + 1];
    }
    memcpy(pk, d, 8);
    for (unsigned b = 0; b < 4; b++) memcpy(pk + 8 + 56 * b, core + physical[b] * 56, 56);
    memcpy(pk + 232, stats, 28);
    return true;
}
static void utf8(const uint8_t *s, unsigned units, char *out, size_t size) {
    size_t n = 0;
    for (unsigned i = 0; i < units; i++) {
        uint32_t c = u16(s + 2 * i);
        if (!c) break;
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < units) c = 0x10000 + ((c - 0xd800) << 10) + (u16(s + 2 * ++i) & 0x3ff);
        char b[4]; unsigned k = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (k == 1) b[0] = (char)c;
        else for (unsigned j = k; j-- > 0; c >>= 6) b[j] = j ? (char)(0x80 | (c & 0x3f)) : (char)((0xf00 >> k) | c);
        if (n + k >= size) break;
        memcpy(out + n, b, k); n += k;
    }
    if (size) out[n] = 0;
}
bool coop_trade_describe(const uint8_t d[COOP_PB7_BYTES], CoopTradeMon *m) {
    uint8_t pk[COOP_PB7_BYTES];
    memset(m, 0, sizeof *m);
    if (!decrypt(d, pk)) return false;
    uint32_t pid = u32(pk + 0x18), iv = u32(pk + 0x74);
    m->species = u16(pk + 8), m->held_item = u16(pk + 0x0a), m->tid = u16(pk + 0x0c), m->sid = u16(pk + 0x0e);
    m->form = pk[0x1d] >> 3, m->gender = (pk[0x1d] >> 1) & 3, m->level = pk[232 + 4];
    for (unsigned i = 0; i < 4; i++) m->moves[i] = u16(pk + 0x5a + 2 * i);
    m->shiny = (m->tid ^ m->sid ^ (pid >> 16) ^ (pid & 0xffff)) < 16;
    m->nicknamed = iv >> 31;
    utf8(pk + 0x40, 13, m->nickname, sizeof m->nickname);
    utf8(pk + 0xb0, 13, m->ot, sizeof m->ot);
    return true;
}
bool coop_trade_partner(const uint8_t d[COOP_PB7_BYTES]) {
    CoopTradeMon m;
    return coop_trade_describe(d, &m) && ((m.species == 25 && m.form == 8) || (m.species == 133 && m.form == 1));
}
bool coop_trade_offer_valid(const uint8_t d[COOP_PB7_BYTES]) {
    uint8_t pk[COOP_PB7_BYTES];
    if (!decrypt(d, pk)) return false;
    unsigned species = u16(pk + 8);
    // (Let's Go has Kanto's 151, Meltan and Melmetal; no eggs)
    if (species > 151 && species != 808 && species != 809) return false;
    if (u32(pk + 0x74) >> 30 & 1) return false;
    return !coop_trade_partner(d);
}
bool coop_trade_same(const uint8_t a[COOP_PB7_BYTES], const uint8_t b[COOP_PB7_BYTES]) {
    uint8_t x[COOP_PB7_BYTES], y[COOP_PB7_BYTES];
    return decrypt(a, x) && decrypt(b, y) && u32(x) == u32(y) && u32(x + 0x18) == u32(y + 0x18);
}
uint16_t coop_trade_hp(const uint8_t d[COOP_PB7_BYTES]) {
    uint8_t pk[COOP_PB7_BYTES];
    return decrypt(d, pk) ? u16(pk + 232 + 8) : 0;
}
static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222645ull;
    while (n--) h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}

// ------------------------------------------------------------------ state
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static CoopTradeView view;
static uint8_t peer_identity[16];
static uint32_t mine_rev, theirs_rev;
static uint8_t theirs_chunks;
static uint8_t theirs_buf[COOP_PB7_BYTES];
static uint32_t theirs_buf_rev;
static bool from_disk, in_image, queried, unknown;
static uint64_t next_ask_ns;
static struct { uint8_t data[255]; size_t size; } queue[16];
static unsigned first, count;
static char save_path[1100];
// <save>.trade history: "committed <id>" / "aborted <id>", so the peer's status question has an answer after the record
// is gone (64 lines kept).
#define HISTORY 64
static struct { uint64_t id; bool committed; } history[HISTORY];
static unsigned history_n;

static bool enqueue(const uint8_t *p, size_t n) {
    if (count == 16 || n > 255) return false;
    unsigned i = (first + count++) % 16;
    memcpy(queue[i].data, p, n), queue[i].size = n;
    return true;
}
static void send_simple(uint8_t op, uint64_t id) { uint8_t b[9] = {op}; p64(b + 1, id); enqueue(b, sizeof b); }
static void finish(uint8_t result) {
    CoopParty party = view.party;
    memset(&view, 0, sizeof view);
    view.party = party, view.mine_slot = -1, view.result = result;
    view.result_until_ns = plat_now_ns() + 6000000000ull;
    mine_rev = theirs_rev = theirs_buf_rev = 0, theirs_chunks = 0, from_disk = in_image = queried = unknown = false;
}
static uint64_t new_id(void) {
    static uint64_t counter;
    uint64_t x = plat_now_ns() ^ fnv(peer_identity, 16) ^ (++counter << 48) ^ (uint64_t)(uintptr_t)&counter;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull; x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return (x ^ (x >> 31)) | 1;
}
static void hex(char *out, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", p[i]); }
static bool unhex(const char *s, uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { unsigned b; if (sscanf(s + 2 * i, "%2x", &b) != 1) return false; p[i] = (uint8_t)b; }
    return s[2 * n] == 0 || s[2 * n] == '\n' || s[2 * n] == '\r' || s[2 * n] == ' ';
}

// ------------------------------------------------------------------ the record
// <save>.trade: "LGTRADE1", then at most one active trade ("state prepared|commit <id>", "peer <identity> <name>",
// "give <pb7>", "take <pb7>", hex), then the history. Written to <save>.trade.tmp and renamed over it, durably.
static bool record_write(bool active) {
    if (!save_path[0]) return false;
    char path[1200], tmp[1210];
    snprintf(path, sizeof path, "%s.trade", save_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fputs("LGTRADE1\n", f) >= 0;
    if (ok && active) {
        char a[COOP_PB7_BYTES * 2 + 1], b[COOP_PB7_BYTES * 2 + 1], who[33];
        hex(a, view.mine, COOP_PB7_BYTES), hex(b, view.theirs, COOP_PB7_BYTES), hex(who, peer_identity, 16);
        ok = fprintf(f, "state %s %016llx\nslot %d\npeer %s %s\ngive %s\ntake %s\n",
                     view.phase == COOP_TRADE_PREPARED || view.phase == COOP_TRADE_DOUBT ? "prepared" : "commit",
                     (unsigned long long)view.id, view.mine_slot, who, view.peer, a, b) > 0;
    }
    for (unsigned i = 0; ok && i < history_n; i++)
        ok = fprintf(f, "%s %016llx\n", history[i].committed ? "committed" : "aborted", (unsigned long long)history[i].id) > 0;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f) ok = !fclose(f) && ok;
    ok = ok && plat_replace_file(tmp, path);
    if (!ok) { LOG("[coop-trade] cannot write %s\n", path); unlink(tmp); }
    return ok;
}
static void remember(uint64_t id, bool committed) {
    if (history_n == HISTORY) memmove(history, history + 1, sizeof *history * --history_n);
    history[history_n].id = id, history[history_n++].committed = committed;
}
// 0 unknown, 1 prepared (in doubt), 2 committed, 3 aborted: this side's answer to the peer's status question.
static uint8_t status_of(uint64_t id) {
    if (id == view.id && (view.phase == COOP_TRADE_PREPARED || view.phase == COOP_TRADE_DOUBT)) return 1;
    if (id == view.id && (view.phase == COOP_TRADE_COMMIT || view.phase == COOP_TRADE_APPLIED)) return 2;
    for (unsigned i = 0; i < history_n; i++) if (history[i].id == id) return history[i].committed ? 2 : 3;
    return 0;
}
// QA (LARECOMP_TRADE_FAULT=prepared|commit|applied): the process ends right there, for the recovery tests.
static void fault(const char *at) {
    const char *f = getenv("LARECOMP_TRADE_FAULT");
    if (f && !strcmp(f, at)) { LOG("[coop-trade] QA fault: end at %s\n", at); _exit(3); }
}
static void decide(bool commit, uint8_t reason) {
    if (!commit) {
        LOG("[coop-trade] trade %016llx aborted (reason %u): nothing changes\n", (unsigned long long)view.id, reason);
        if (view.phase == COOP_TRADE_PREPARED || view.phase == COOP_TRADE_DOUBT) { remember(view.id, false); view.phase = COOP_TRADE_IDLE; record_write(false); }
        finish(reason);
        return;
    }
    LOG("[coop-trade] trade %016llx committed by the relay: applying\n", (unsigned long long)view.id);
    view.phase = COOP_TRADE_COMMIT;
    // (if this write fails the trade still goes into the running game and its next save; a crash before that save
    // loses it on this side only: logged as the one risk left)
    if (!record_write(true)) LOG("[coop-trade] trade %016llx: the commit isn't on disk\n", (unsigned long long)view.id);
    fault("commit");
}
// Both confirmations are for the current offers: the record goes to disk before "prepared" leaves.
static void maybe_prepare(void) {
    if (view.phase != COOP_TRADE_OPEN || !view.mine_confirmed || !view.theirs_confirmed) return;
    view.phase = COOP_TRADE_PREPARED;
    if (!record_write(true)) { send_simple(6, view.id); finish(COOP_TRADE_SAVE_ERROR); return; }
    fault("prepared");  // (on disk, "prepared" not sent: the relay aborts at the disconnect)
    uint8_t b[17] = {4}; p64(b + 1, view.id); p64(b + 9, fnv(view.mine, COOP_PB7_BYTES) + fnv(view.theirs, COOP_PB7_BYTES));
    enqueue(b, sizeof b);
    LOG("[coop-trade] trade %016llx prepared\n", (unsigned long long)view.id);
}
static bool busy(void) { return view.phase != COOP_TRADE_IDLE; }

// ------------------------------------------------------------------ UI / game thread
void coop_trade_view(CoopTradeView *out) { pthread_mutex_lock(&lock); *out = view; pthread_mutex_unlock(&lock); }
static void set_peer(const uint8_t identity[16], const char *name) {
    memcpy(peer_identity, identity, 16);
    snprintf(view.peer, sizeof view.peer, "%s", name ? name : "");
}
bool coop_trade_request(const uint8_t identity[16], const char *name) {
    pthread_mutex_lock(&lock);
    bool ok = !busy() && save_path[0];
    if (ok) {
        CoopParty party = view.party;
        memset(&view, 0, sizeof view); view.party = party; view.mine_slot = -1;
        set_peer(identity, name);
        view.phase = COOP_TRADE_ASKING, view.id = new_id(), view.expires_ns = plat_now_ns() + 45000000000ull;
        send_simple(0, view.id);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_trade_answer(bool accept, const uint8_t identity[16], const char *name) {
    pthread_mutex_lock(&lock);
    bool ok = view.phase == COOP_TRADE_ASKED;
    if (ok && accept) { set_peer(identity, name); view.phase = COOP_TRADE_OPEN; send_simple(1, view.id); }
    else if (ok) { send_simple(6, view.id); finish(COOP_TRADE_CANCELLED); }
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_trade_cancel(void) {
    pthread_mutex_lock(&lock);
    bool ok = view.phase >= COOP_TRADE_ASKING && view.phase <= COOP_TRADE_OPEN;
    if (ok) { send_simple(6, view.id); finish(COOP_TRADE_CANCELLED); }
    pthread_mutex_unlock(&lock);
    return ok;
}
void coop_trade_set_party(const CoopParty *party) {
    pthread_mutex_lock(&lock);
    if (party && party->count <= COOP_PARTY_MAX) view.party = *party;
    pthread_mutex_unlock(&lock);
}
static void send_offer(void) {
    for (uint8_t chunk = 0; chunk < 2; chunk++) {
        uint8_t b[144] = {2}; p64(b + 1, view.id); p32(b + 9, mine_rev); b[13] = chunk;
        memcpy(b + 14, view.mine + 130 * chunk, 130);
        enqueue(b, sizeof b);
    }
}
int coop_trade_offer(unsigned slot) {
    pthread_mutex_lock(&lock);
    int why = COOP_OFFER_OK;
    const CoopParty *p = &view.party;
    if (view.phase != COOP_TRADE_OPEN) why = COOP_OFFER_PHASE;
    else if (slot >= p->count) why = COOP_OFFER_NO_SLOT;
    else if (!coop_party_pokemon_valid(p->pokemon[slot])) why = COOP_OFFER_INVALID;
    else if (coop_trade_partner(p->pokemon[slot])) why = COOP_OFFER_PARTNER;
    else {
        // Never the last Pokemon that can still battle (the received one may have fainted).
        unsigned standing = 0;
        for (unsigned i = 0; i < p->count; i++) standing += i != slot && coop_trade_hp(p->pokemon[i]) > 0;
        if (!standing) why = COOP_OFFER_LAST;
    }
    if (!why) {
        memcpy(view.mine, p->pokemon[slot], COOP_PB7_BYTES);
        view.mine_slot = (int)slot, view.mine_ready = true, mine_rev++;
        view.mine_confirmed = view.theirs_confirmed = false;  // any offer change clears both confirmations
        send_offer();
    }
    pthread_mutex_unlock(&lock);
    return why;
}
bool coop_trade_confirm(void) {
    pthread_mutex_lock(&lock);
    bool ok = view.phase == COOP_TRADE_OPEN && view.mine_ready && view.theirs_ready && !view.mine_confirmed;
    if (ok) {
        uint8_t b[17] = {3}; p64(b + 1, view.id); p32(b + 9, mine_rev); p32(b + 13, theirs_rev);
        enqueue(b, sizeof b);
        view.mine_confirmed = true;
        maybe_prepare();
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
void coop_trade_dismiss(void) { pthread_mutex_lock(&lock); view.result_until_ns = 0; pthread_mutex_unlock(&lock); }

// ------------------------------------------------------------------ relay worker
void coop_trade_tick(void) {
    pthread_mutex_lock(&lock);
    uint64_t now = plat_now_ns();
    if ((view.phase == COOP_TRADE_ASKING || view.phase == COOP_TRADE_ASKED) && now >= view.expires_ns) {
        send_simple(6, view.id); finish(COOP_TRADE_EXPIRED);
    }
    // In doubt: ask the relay for its decision once per connection; when it has none (it restarted), ask the peer.
    if (view.phase == COOP_TRADE_DOUBT && !queried) { send_simple(7, view.id); queried = true; }
    if (view.phase == COOP_TRADE_DOUBT && unknown && now >= next_ask_ns) {
        uint8_t b[11] = {8}; p64(b + 1, view.id); b[9] = 1, b[10] = 1;
        enqueue(b, sizeof b); next_ask_ns = now + 5000000000ull;
    }
    pthread_mutex_unlock(&lock);
}
bool coop_trade_pop(uint8_t body[255], size_t *size) {
    pthread_mutex_lock(&lock);
    bool ok = count > 0;
    if (ok) { memcpy(body, queue[first].data, queue[first].size); *size = queue[first].size; first = (first + 1) % 16; count--; }
    pthread_mutex_unlock(&lock);
    return ok;
}
void coop_trade_message(const uint8_t *b, size_t n) {
    if (n < 9) return;
    uint8_t op = b[0];
    uint64_t id = u64(b + 1);
    pthread_mutex_lock(&lock);
    bool current = id == view.id && view.id;
    if (op == 0 && n == 9) {
        if (busy() || !save_path[0]) send_simple(6, id);  // (one trade at a time; no save open yet)
        else {
            CoopParty party = view.party;
            memset(&view, 0, sizeof view); view.party = party; view.mine_slot = -1;
            view.phase = COOP_TRADE_ASKED, view.id = id, view.expires_ns = plat_now_ns() + 45000000000ull;
        }
    } else if (op == 1 && n == 9 && current && view.phase == COOP_TRADE_ASKING) view.phase = COOP_TRADE_OPEN;
    else if (op == 2 && n == 144 && current && view.phase == COOP_TRADE_OPEN && b[13] < 2) {
        uint32_t rev = u32(b + 9);
        if (rev != theirs_buf_rev) theirs_buf_rev = rev, theirs_chunks = 0;
        memcpy(theirs_buf + 130 * b[13], b + 14, 130);
        theirs_chunks |= 1 << b[13];
        if (theirs_chunks == 3 && rev > theirs_rev) {
            if (!coop_trade_offer_valid(theirs_buf)) {
                LOG("[coop-trade] the peer's offer is not a valid Pokemon: trade cancelled\n");
                send_simple(6, view.id); finish(COOP_TRADE_INVALID);
            } else {
                memcpy(view.theirs, theirs_buf, COOP_PB7_BYTES);
                theirs_rev = rev, view.theirs_ready = true;
                view.mine_confirmed = view.theirs_confirmed = false;  // any offer change clears both confirmations
            }
        }
    } else if (op == 3 && n == 17 && current && view.phase == COOP_TRADE_OPEN) {
        // Their confirmation counts only for the two offers on screen now.
        if (u32(b + 9) == theirs_rev && u32(b + 13) == mine_rev && view.theirs_ready && view.mine_ready) {
            view.theirs_confirmed = true;
            maybe_prepare();
        }
    } else if (op == 5 && n == 11 && current) {
        bool settled = view.phase == COOP_TRADE_PREPARED || view.phase == COOP_TRADE_DOUBT;
        if (b[9] == 1 && settled) decide(true, 0);
        else if (b[9] == 2 && (settled || view.phase <= COOP_TRADE_OPEN)) decide(false, b[10]);
        else if (b[9] == 0 && view.phase == COOP_TRADE_DOUBT) { unknown = true; next_ask_ns = 0; }
    } else if (op == 6 && n == 9 && current && view.phase >= COOP_TRADE_ASKING && view.phase <= COOP_TRADE_OPEN) {
        finish(COOP_TRADE_CANCELLED);
    } else if (op == 8 && n == 11 && b[9] <= 3) {
        if (b[10]) { uint8_t r[11] = {8}; p64(r + 1, id); r[9] = status_of(id); r[10] = 0; enqueue(r, sizeof r); }
        // The relay has no decision: the peer's own state settles it. Only a peer that committed (it got the relay's
        // commit) means commit; a peer still in doubt, aborted or without the trade never applied it.
        else if (current && view.phase == COOP_TRADE_DOUBT && unknown) {
            LOG("[coop-trade] trade %016llx: the relay has no decision, the peer's state is %u\n", (unsigned long long)id, b[9]);
            if (b[9] == 2) decide(true, 0); else decide(false, COOP_TRADE_DISCONNECTED);
        }
    }
    pthread_mutex_unlock(&lock);
}
void coop_trade_link_lost(void) {
    pthread_mutex_lock(&lock);
    if (view.phase >= COOP_TRADE_ASKING && view.phase <= COOP_TRADE_OPEN) finish(COOP_TRADE_DISCONNECTED);
    else if (view.phase == COOP_TRADE_PREPARED) view.phase = COOP_TRADE_DOUBT;
    queried = unknown = false;  // (ask again on the next connection)
    first = count = 0;
    pthread_mutex_unlock(&lock);
}

// ------------------------------------------------------------------ durability
// The record goes aside as <save>.trade-quarantine-<id> (kept for a person to look at) and nothing is applied.
static void quarantine(void) {
    char path[1200], aside[1250];
    snprintf(path, sizeof path, "%s.trade", save_path);
    snprintf(aside, sizeof aside, "%s.trade-quarantine-%016llx", save_path, (unsigned long long)view.id);
    LOG("[coop-trade] record moved to %s, nothing is applied\n", aside);
    rename(path, aside);
    memset(&view, 0, sizeof view), history_n = 0;
    finish(COOP_TRADE_QUARANTINED);
}
void coop_trade_recover(const char *save) {
    pthread_mutex_lock(&lock);
    if (save_path[0]) { pthread_mutex_unlock(&lock); return; }  // (once per boot)
    snprintf(save_path, sizeof save_path, "%s", save);
    char path[1200], line[1200], state[16], who[40];
    snprintf(path, sizeof path, "%s.trade", save);
    FILE *f = fopen(path, "rb");
    bool ok = true, active = false;
    unsigned long long id;
    history_n = 0;
    if (f) {
        ok = fgets(line, sizeof line, f) && !strcmp(line, "LGTRADE1\n");
        while (ok && fgets(line, sizeof line, f)) {
            if (sscanf(line, "state %15s %llx", state, &id) == 2 && !active) {
                active = true; view.id = id;
                view.phase = !strcmp(state, "prepared") ? COOP_TRADE_DOUBT : !strcmp(state, "commit") ? COOP_TRADE_COMMIT : COOP_TRADE_IDLE;
                ok = view.phase != COOP_TRADE_IDLE;
            } else if (active && sscanf(line, "slot %d", &view.mine_slot) == 1) ok = view.mine_slot >= 0 && view.mine_slot < COOP_PARTY_MAX;
            else if (active && sscanf(line, "peer %39s", who) == 1) {
                ok = strlen(who) == 32 && unhex(who, peer_identity, 16);
                const char *name = strchr(line + 5, ' ');
                snprintf(view.peer, sizeof view.peer, "%s", name ? name + 1 : "");
                view.peer[strcspn(view.peer, "\r\n")] = 0;
            } else if (active && !strncmp(line, "give ", 5)) ok = unhex(line + 5, view.mine, COOP_PB7_BYTES), view.mine_ready = true;
            else if (active && !strncmp(line, "take ", 5)) ok = unhex(line + 5, view.theirs, COOP_PB7_BYTES), view.theirs_ready = true;
            else if (sscanf(line, "committed %llx", &id) == 1) remember(id, true);
            else if (sscanf(line, "aborted %llx", &id) == 1) remember(id, false);
            else ok = false;
        }
        fclose(f);
        ok = ok && (!active || (view.mine_ready && view.theirs_ready && coop_party_pokemon_valid(view.mine) && coop_trade_offer_valid(view.theirs)));
    }
    if (!ok) {
        LOG("[coop-trade] %s is malformed\n", path);
        quarantine();
    } else if (active) {
        from_disk = true;
        LOG("[coop-trade] recovery: trade %016llx is %s\n", (unsigned long long)view.id,
            view.phase == COOP_TRADE_DOUBT ? "in doubt: the relay or the peer decides" : "committed: checked against the save");
    } else view.mine_slot = -1;
    pthread_mutex_unlock(&lock);
}
bool coop_trade_to_apply(uint8_t give[COOP_PB7_BYTES], uint8_t take[COOP_PB7_BYTES], bool *disk) {
    pthread_mutex_lock(&lock);
    bool ok = view.phase == COOP_TRADE_COMMIT;
    if (ok) memcpy(give, view.mine, COOP_PB7_BYTES), memcpy(take, view.theirs, COOP_PB7_BYTES), *disk = from_disk;
    pthread_mutex_unlock(&lock);
    return ok;
}
static void committed(void) {
    LOG("[coop-trade] trade %016llx is in the save: done\n", (unsigned long long)view.id);
    remember(view.id, true);
    view.phase = COOP_TRADE_IDLE;
    record_write(false);
    finish(COOP_TRADE_TRADED);
}
void coop_trade_applied(int result) {
    pthread_mutex_lock(&lock);
    if (view.phase == COOP_TRADE_COMMIT) {
        if (result > 0) { view.phase = COOP_TRADE_APPLIED; in_image = false; view.result = COOP_TRADE_TRADED; view.result_until_ns = plat_now_ns() + 6000000000ull; fault("applied"); }
        else if (!result) committed();
        // (the save holds neither the state before nor after this trade)
        else { LOG("[coop-trade] trade %016llx doesn't match the save\n", (unsigned long long)view.id); quarantine(); }
    }
    pthread_mutex_unlock(&lock);
}
void coop_trade_serialized(void) {
    pthread_mutex_lock(&lock);
    if (view.phase == COOP_TRADE_APPLIED) in_image = true;
    pthread_mutex_unlock(&lock);
}
void coop_trade_after_replace(bool ok) {
    pthread_mutex_lock(&lock);
    if (view.phase == COOP_TRADE_APPLIED && in_image) { if (ok) committed(); else in_image = false; }
    pthread_mutex_unlock(&lock);
}
void coop_trade_reset(void) {
    pthread_mutex_lock(&lock);
    memset(&view, 0, sizeof view); view.mine_slot = -1;
    memset(peer_identity, 0, sizeof peer_identity);
    mine_rev = theirs_rev = theirs_buf_rev = 0, theirs_chunks = 0, history_n = 0;
    from_disk = in_image = queried = unknown = false, next_ask_ns = 0;
    first = count = 0, save_path[0] = 0;
    pthread_mutex_unlock(&lock);
}
