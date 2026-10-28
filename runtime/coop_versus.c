// Practice battle requests (V) and spectating (O). The relay checks both players' states; this keeps the local view.
#include "coop_versus.h"
#include "coop_battle_link.h"
#include "platform.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SUMMARY_BYTES 31 // O 2, turn u16, then per trainer slot species u16, level u8, HP u16, max HP u16
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static CoopTeamUp versus;
static uint64_t start_until;
static CoopWatchView watch;
static struct { uint8_t op, body[64]; size_t n; } queue[4];
static unsigned count;

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static void p16(uint8_t *p, uint16_t x) { p[0] = x; p[1] = x >> 8; }
// Called under the lock. A newer summary replaces one not sent yet.
static bool enqueue(uint8_t op, const uint8_t *body, size_t n) {
    unsigned i = 0;
    while (i < count && !(op == 'O' && body[0] == 2 && queue[i].op == 'O' && queue[i].body[0] == 2)) i++;
    if (i == count && count == 4) return false;
    if (i == count) count++;
    queue[i].op = op; memcpy(queue[i].body, body, n); queue[i].n = n;
    return true;
}

void coop_versus_request(CoopTeamUp *out) { pthread_mutex_lock(&lock); *out = versus; pthread_mutex_unlock(&lock); }
bool coop_versus_action(uint8_t command, uint8_t local) {
    pthread_mutex_lock(&lock);
    // As a meet-up: an accepted request (state 2) is already over, so a new one may follow it.
    bool ok = command <= 3 && (command == 0 ? versus.state != 1 : command <= 2 ?
        versus.state == 1 && versus.from_slot != local : versus.state == 1 && versus.from_slot == local);
    if (ok) {
        uint8_t body[5] = {command}; uint32_t id = command ? versus.id : 0;
        for (unsigned i = 0; i < 4; i++) body[1 + i] = (uint8_t)(id >> 8 * i);
        ok = enqueue('V', body, sizeof body);
        if (ok) versus.reason = 0;
    }
    pthread_mutex_unlock(&lock); return ok;
}
bool coop_versus_take_start(void) {
    pthread_mutex_lock(&lock);
    bool ok = start_until && plat_now_ns() < start_until; start_until = 0;
    pthread_mutex_unlock(&lock); return ok;
}

void coop_versus_watch_view(CoopWatchView *out) { pthread_mutex_lock(&lock); *out = watch; pthread_mutex_unlock(&lock); }
bool coop_versus_watch(bool start) {
    pthread_mutex_lock(&lock);
    uint8_t body[1] = {start ? 0 : 1};
    bool ok = (start ? !watch.state : watch.state == 1) && enqueue('O', body, 1);
    pthread_mutex_unlock(&lock); return ok;
}
bool coop_versus_watched(uint8_t local) {
    pthread_mutex_lock(&lock); bool ok = watch.state == 1 && watch.watcher <= 1 && watch.watcher != local;
    pthread_mutex_unlock(&lock); return ok;
}
void coop_versus_publish(const CoopWatchSummary *s, uint8_t local) {
    pthread_mutex_lock(&lock);
    if (watch.state == 1 && watch.watcher <= 1 && watch.watcher != local) {
        uint8_t b[SUMMARY_BYTES] = {2}; p16(b + 1, s->turn);
        for (unsigned i = 0; i < 4; i++) {
            uint8_t *m = b + 3 + 7 * i;
            p16(m, s->mons[i].species); m[2] = s->mons[i].level; p16(m + 3, s->mons[i].hp); p16(m + 5, s->mons[i].max_hp);
        }
        enqueue('O', b, sizeof b);
    }
    pthread_mutex_unlock(&lock);
}

void coop_versus_message(const uint8_t *p, size_t n) {
    uint64_t now = plat_now_ns();
    if (n == 12 && p[0] == 'V' && p[1] <= 2 && (p[2] <= 1 || p[2] == 255) && p[3] <= 7) {
        pthread_mutex_lock(&lock);
        versus = (CoopTeamUp){.state = p[1], .from_slot = p[2], .reason = p[3], .id = u32(p + 4),
            .expires_ns = p[1] == 1 ? now + (uint64_t)u32(p + 8) * 1000000ull : 0};
        // Accepted: the host starts the battle; the guest joins its offer without asking again.
        if (p[1] == 2) start_until = now + 20000000000ull;
        pthread_mutex_unlock(&lock);
        if (p[1] == 2) coop_battle_link_pvp_consent(now + 20000000000ull);
        fprintf(stderr, "[coop] practice battle state %u, from %u, reason %u\n", p[1], p[2], p[3]);
    } else if (n == 5 && p[0] == 'O' && p[1] == 0x10 && p[2] <= 1 && (p[3] <= 1 || p[3] == 255) && p[4] <= 4) {
        pthread_mutex_lock(&lock);
        if (!p[2]) memset(&watch, 0, sizeof watch);
        watch.state = p[2]; watch.watcher = p[3]; watch.reason = p[4];
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "[coop] watch state %u, watcher %u, reason %u\n", p[2], p[3], p[4]);
    } else if (n == 1 + SUMMARY_BYTES && p[0] == 'O' && p[1] == 2) {
        pthread_mutex_lock(&lock);
        if (watch.state == 1) {
            const uint8_t *b = p + 1; watch.summary.turn = u16(b + 1);
            for (unsigned i = 0; i < 4; i++) {
                const uint8_t *m = b + 3 + 7 * i;
                watch.summary.mons[i] = (CoopWatchMon){.species = u16(m), .level = m[2], .hp = u16(m + 3), .max_hp = u16(m + 5)};
            }
            watch.have = true; watch.received_ns = now;
        }
        pthread_mutex_unlock(&lock);
    }
}
bool coop_versus_pop(uint8_t *op, uint8_t body[64], size_t *n) {
    pthread_mutex_lock(&lock); bool ok = count > 0;
    if (ok) { *op = queue[0].op; memcpy(body, queue[0].body, queue[0].n); *n = queue[0].n; memmove(queue, queue + 1, --count * sizeof *queue); }
    pthread_mutex_unlock(&lock); return ok;
}
void coop_versus_link_lost(void) {
    pthread_mutex_lock(&lock);
    memset(&versus, 0, sizeof versus); memset(&watch, 0, sizeof watch); start_until = 0; count = 0;
    pthread_mutex_unlock(&lock);
}
