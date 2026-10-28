// Shared Adventure: the scene table, the ready prompt over the relay's Y channel and the ledger <save>.scene. See
// docs/COOP-SHARED-ADVENTURE.md for the flow, the messages and why a gift can't be given twice.
#include "coop_scene.h"
#include "platform.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG(...) fprintf(stderr, __VA_ARGS__)  // (as rt.h)
#define OFFER_NS 60000000000ull                 // the prompt's lifetime (the relay closes it then too)

// Static evidence: tests/logs/shared-adventure/ (flag tables bin/flagwork/*.tbl, script 470 = road001's sub event 1:
// FlagGet(FE_SUB_01_CLEAR), else ItemAdd(17 Potion, 1) and FlagSet(FE_SUB_01_CLEAR)).
const CoopSceneDef coop_scenes[COOP_SCENES] = {
    {1, "Route 1: the Poke Mart clerk's free sample", {"FE_SUB_01_CLEAR"}, {"FSYS_ARRIVE_ROAD001"}, 17, 1, "Potion"},
};

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint64_t u64(const uint8_t *p) { uint64_t x = 0; for (int i = 7; i >= 0; i--) x = x << 8 | p[i]; return x; }
static void p64(uint8_t *p, uint64_t x) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(x >> 8 * i); }

uint64_t coop_scene_hash(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    while (*s) h = (h ^ (unsigned char)*s++) * 0x100000001b3ull;
    return h;
}
int coop_scene_index(uint16_t id) {
    for (int i = 0; i < COOP_SCENES; i++) if (coop_scenes[i].id == id) return i;
    return -1;
}
bool coop_scene_personal(const char *flag) {
    static const char *const words[] = {"GYM", "RIVAL", "BADGE", "CHAMP", "LEAGUE", "GAME_CLEAR", "_WIN", "BTL_RED", "MASTER_RED"};
    if (!strncmp(flag, "TRID_", 5)) return true;  // (every trainer's beaten flag: battles stay personal)
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++) if (strstr(flag, words[i])) return true;
    return false;
}
bool coop_scene_valid(const CoopSceneDef *d) {
    if (!d || !d->id || !d->done[0] || (d->item == 0) != (d->count == 0) || d->count > 99) return false;
    for (int i = 0; i < 2; i++)
        if ((d->done[i] && coop_scene_personal(d->done[i])) || (d->needs[i] && coop_scene_personal(d->needs[i]))) return false;
    return true;
}
int coop_scene_check(const CoopSceneDef *d, bool (*get)(void *, uint64_t), void *ctx) {
    if (!coop_scene_valid(d)) return COOP_SCENE_PERSONAL;
    if (get(ctx, coop_scene_hash(d->done[0]))) return COOP_SCENE_ALREADY;
    for (int i = 0; i < 2; i++) if (d->needs[i] && !get(ctx, coop_scene_hash(d->needs[i]))) return COOP_SCENE_NOT_REACHED;
    return COOP_SCENE_OK;
}

// ------------------------------------------------------------------ state
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static CoopSceneView view;
static bool mode_sent, was_pending;
static uint8_t mode_last;
static bool applied_run[COOP_SCENES], in_image[COOP_SCENES];
static uint64_t retry_ns[COOP_SCENES], answered_id;
static uint8_t answered;
static struct { uint8_t data[16]; size_t size; } queue[16];
static unsigned first, count;
static char save_path[1100];

static void enqueue(const uint8_t *p, size_t n) {
    if (count == 16) first = (first + 1) % 16, count--;  // (the oldest goes: modes are resent, answers are local-final)
    unsigned i = (first + count++) % 16;
    memcpy(queue[i].data, p, n), queue[i].size = n;
}
static void send_byte(uint8_t op, uint64_t id, uint8_t value) { uint8_t b[10] = {op}; p64(b + 1, id); b[9] = value; enqueue(b, sizeof b); }
static void toast(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(view.toast, sizeof view.toast, fmt, a); va_end(a);
    view.toast_until_ns = plat_now_ns() + 6000000000ull;
    LOG("[coop-scene] %s\n", view.toast);
}
static uint8_t mode(void) { return view.shared ? view.consent ? COOP_MODE_ACTIVE : COOP_MODE_SHARED : COOP_MODE_SOCIAL; }
static uint64_t new_id(void) {
    static uint64_t counter;
    uint64_t x = plat_now_ns() ^ (++counter << 48) ^ (uint64_t)(uintptr_t)&counter;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull; x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return (x ^ (x >> 31)) | 1;
}

// <save>.scene: "LGSCENE1", then "scene <id> pending|accepted|done|declined" lines. Written to .tmp and renamed over it.
static const char *const state_name[] = {"", "pending", "accepted", "done", "declined"};
static bool ledger_write(void) {
    if (!save_path[0]) return false;
    char path[1200], tmp[1210];
    snprintf(path, sizeof path, "%s.scene", save_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fputs("LGSCENE1\n", f) >= 0;
    for (int i = 0; ok && i < COOP_SCENES; i++)
        if (view.ledger[i]) ok = fprintf(f, "scene %u %s\n", coop_scenes[i].id, state_name[view.ledger[i]]) > 0;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f) ok = !fclose(f) && ok;
    ok = ok && plat_replace_file(tmp, path);
    if (!ok) { LOG("[coop-scene] cannot write %s\n", path); unlink(tmp); }
    return ok;
}
static void set_ledger(int s, uint8_t state) {
    if (view.ledger[s] == state) return;
    LOG("[coop-scene] scene %u: %s -> %s\n", coop_scenes[s].id, view.ledger[s] ? state_name[view.ledger[s]] : "none",
        state ? state_name[state] : "none");
    view.ledger[s] = state;
    ledger_write();
}

// ------------------------------------------------------------------ UI / settings
void coop_scene_set_mode(bool shared, bool nearby) {
    pthread_mutex_lock(&lock);
    view.shared = shared, view.nearby = nearby;
    if (!shared) view.consent = false;
    pthread_mutex_unlock(&lock);
}
void coop_scene_consent(bool on) { pthread_mutex_lock(&lock); view.consent = on && view.shared; pthread_mutex_unlock(&lock); }
void coop_scene_view(CoopSceneView *out) { pthread_mutex_lock(&lock); *out = view; pthread_mutex_unlock(&lock); }
static bool offer(int s) {
    if (s < 0 || s >= COOP_SCENES || mode() != COOP_MODE_ACTIVE || view.peer_mode != COOP_MODE_ACTIVE ||
        !view.completed[s] || view.out_phase == 1 || !coop_scene_valid(&coop_scenes[s])) return false;
    uint8_t b[12] = {1};
    view.out_id = new_id(), view.out_scene = s, view.out_phase = 1, view.out_expires_ns = plat_now_ns() + OFFER_NS;
    p64(b + 1, view.out_id); b[9] = (uint8_t)coop_scenes[s].id, b[10] = (uint8_t)(coop_scenes[s].id >> 8); b[11] = view.nearby;
    enqueue(b, sizeof b);
    LOG("[coop-scene] offered scene %u (%s)\n", coop_scenes[s].id, view.nearby ? "nearby" : "everyone");
    return true;
}
bool coop_scene_offer(int s) { pthread_mutex_lock(&lock); bool ok = offer(s); pthread_mutex_unlock(&lock); return ok; }
bool coop_scene_answer(int answer) {
    pthread_mutex_lock(&lock);
    bool ok = view.asked && answer >= COOP_ANSWER_READY && answer <= COOP_ANSWER_STAY_OUT;
    if (ok) {
        int s = view.in_scene;
        // Ready before this save reached the scene's story point keeps it for later.
        if (answer == COOP_ANSWER_READY && view.observed[s] && !view.reached[s]) answer = COOP_ANSWER_NOT_YET;
        // Consent is local and final: the initiator's cancel counts only until this answer.
        set_ledger(s, answer == COOP_ANSWER_READY ? COOP_LEDGER_ACCEPTED : answer == COOP_ANSWER_STAY_OUT ? COOP_LEDGER_DECLINED : COOP_LEDGER_PENDING);
        send_byte(2, view.in_id, (uint8_t)answer);
        answered_id = view.in_id, answered = (uint8_t)answer, view.asked = false;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_scene_cancel(bool continue_without) {
    pthread_mutex_lock(&lock);
    bool ok = view.out_phase == 1;
    if (ok) {
        send_byte(3, view.out_id, continue_without ? 2 : 1);
        view.out_phase = 0;
        LOG("[coop-scene] %s\n", continue_without ? "continued without the partner" : "offer cancelled");
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
bool coop_scene_catch_up(int s) {
    pthread_mutex_lock(&lock);
    bool ok = s >= 0 && s < COOP_SCENES && view.shared && view.ledger[s] == COOP_LEDGER_PENDING;
    if (ok) set_ledger(s, COOP_LEDGER_ACCEPTED);
    pthread_mutex_unlock(&lock);
    return ok;
}
void coop_scene_dismiss(void) { pthread_mutex_lock(&lock); view.toast_until_ns = 0; pthread_mutex_unlock(&lock); }

// ------------------------------------------------------------------ relay worker
void coop_scene_tick(void) {
    pthread_mutex_lock(&lock);
    uint64_t now = plat_now_ns();
    if (!mode_sent || mode_last != mode()) { uint8_t b[2] = {0, mode()}; enqueue(b, 2); mode_sent = true, mode_last = b[1]; }
    if (view.asked && now >= view.in_expires_ns) {
        view.asked = false;
        toast("The shared scene waits in Friends > Catch up");
    }
    if (view.out_phase == 1 && now >= view.out_expires_ns) view.out_phase = 2, view.out_result = 10 + COOP_CLOSED_EXPIRED, view.out_until_ns = now + 6000000000ull;
    if (view.out_phase == 2 && now >= view.out_until_ns) view.out_phase = 0;
    pthread_mutex_unlock(&lock);
}
bool coop_scene_pop(uint8_t body[255], size_t *size) {
    pthread_mutex_lock(&lock);
    bool ok = count > 0;
    if (ok) { memcpy(body, queue[first].data, queue[first].size); *size = queue[first].size; first = (first + 1) % 16; count--; }
    pthread_mutex_unlock(&lock);
    return ok;
}
// The partner's side of an offer: an answer without a prompt when this save has already decided.
static void offered(uint64_t id, int s) {
    if (id == answered_id) { send_byte(2, id, answered); return; }  // (the relay resent it after a reconnect)
    uint8_t auto_answer = mode() != COOP_MODE_ACTIVE ? COOP_ANSWER_NOT_SHARING :
        view.ledger[s] == COOP_LEDGER_DONE || view.completed[s] ? COOP_ANSWER_ALREADY :
        view.ledger[s] == COOP_LEDGER_DECLINED ? COOP_ANSWER_STAY_OUT : view.ledger[s] == COOP_LEDGER_ACCEPTED ? COOP_ANSWER_READY : 0;
    if (auto_answer) { send_byte(2, id, auto_answer); answered_id = id, answered = auto_answer; return; }
    was_pending = view.ledger[s] == COOP_LEDGER_PENDING;
    set_ledger(s, COOP_LEDGER_PENDING);  // (an unanswered offer stays for Catch up)
    view.asked = true, view.in_id = id, view.in_scene = s, view.in_expires_ns = plat_now_ns() + OFFER_NS;
    LOG("[coop-scene] offer of scene %u\n", coop_scenes[s].id);
}
void coop_scene_message(const uint8_t *b, size_t n) {
    pthread_mutex_lock(&lock);
    if (n == 2 && b[0] == 0 && b[1] <= COOP_MODE_ACTIVE) view.peer_mode = b[1];
    else if (n == 12 && b[0] == 1 && b[11] <= 1) {
        int s = coop_scene_index(u16(b + 9));
        if (s >= 0 && u64(b + 1)) offered(u64(b + 1), s);
    } else if (n == 10 && b[0] == 2 && view.out_phase == 1 && u64(b + 1) == view.out_id && b[9] >= 1 && b[9] <= COOP_ANSWER_NOT_SHARING) {
        view.out_phase = 2, view.out_result = b[9], view.out_until_ns = plat_now_ns() + 6000000000ull;
    } else if (n == 10 && b[0] == 3 && view.asked && u64(b + 1) == view.in_id && (b[9] == 1 || b[9] == 2)) {
        view.asked = false;
        if (b[9] == 1 && !was_pending) set_ledger(view.in_scene, COOP_LEDGER_NONE);
        toast(b[9] == 1 ? "Your friend cancelled the shared scene" : "Your friend went on: catch up later in Friends");
    } else if (n == 10 && b[0] == 4 && b[9] >= 1 && b[9] <= COOP_CLOSED_STALE) {
        uint64_t id = u64(b + 1);
        if (view.out_phase == 1 && id == view.out_id) view.out_phase = 2, view.out_result = 10 + b[9], view.out_until_ns = plat_now_ns() + 6000000000ull;
        if (view.asked && id == view.in_id) { view.asked = false; toast("The shared scene waits in Friends > Catch up"); }
    }
    pthread_mutex_unlock(&lock);
}
void coop_scene_link_lost(void) {
    pthread_mutex_lock(&lock);
    // The prompt and the wait stay: the relay keeps the offer through the 30 s reconnect grace and resends it.
    mode_sent = false, view.peer_mode = COOP_MODE_SOCIAL;
    pthread_mutex_unlock(&lock);
}

// ------------------------------------------------------------------ game thread
void coop_scene_observe(int s, bool done, bool reached) {
    if (s < 0 || s >= COOP_SCENES) return;
    pthread_mutex_lock(&lock);
    view.reached[s] = reached;
    if (!view.observed[s]) {
        view.observed[s] = true, view.completed[s] = done;
        // The save that was loaded already has it: done (a native completion, or an apply saved before a restart).
        if (done && view.ledger[s] == COOP_LEDGER_ACCEPTED) set_ledger(s, COOP_LEDGER_DONE);
    } else if (done && !view.completed[s]) {
        view.completed[s] = true;
        if (view.ledger[s] == COOP_LEDGER_ACCEPTED) applied_run[s] = true;  // (done natively while waiting: the next save)
        else if (!applied_run[s] && !offer(s) && view.shared) toast("Scene complete: share it with your friend from Friends");
    }
    pthread_mutex_unlock(&lock);
}
bool coop_scene_to_apply(int *scene) {
    pthread_mutex_lock(&lock);
    uint64_t now = plat_now_ns();
    bool ok = false;
    for (int s = 0; !ok && s < COOP_SCENES; s++)
        if (view.shared && view.ledger[s] == COOP_LEDGER_ACCEPTED && view.observed[s] && !view.completed[s] && !applied_run[s] && now >= retry_ns[s])
            *scene = s, ok = true;
    pthread_mutex_unlock(&lock);
    return ok;
}
void coop_scene_applied(int s, int result) {
    if (s < 0 || s >= COOP_SCENES) return;
    pthread_mutex_lock(&lock);
    const CoopSceneDef *d = &coop_scenes[s];
    if (result == COOP_SCENE_OK) {
        applied_run[s] = view.completed[s] = true;
        toast(d->item ? "Shared scene: you received %s" : "Shared scene complete%s", d->item ? d->item_name : "");
    } else if (result == COOP_SCENE_ALREADY) applied_run[s] = view.completed[s] = true;
    else if (result == COOP_SCENE_NOT_REACHED) { set_ledger(s, COOP_LEDGER_PENDING); toast("Not at this point of the story yet: it waits in Catch up"); }
    else if (result == COOP_SCENE_PERSONAL) set_ledger(s, COOP_LEDGER_DECLINED);
    else { retry_ns[s] = plat_now_ns() + 10000000000ull; toast("No room in the Bag for the %s: make room to receive it", d->item_name); }
    pthread_mutex_unlock(&lock);
}
void coop_scene_serialized(void) {
    pthread_mutex_lock(&lock);
    for (int s = 0; s < COOP_SCENES; s++) in_image[s] = applied_run[s] && view.ledger[s] == COOP_LEDGER_ACCEPTED;
    pthread_mutex_unlock(&lock);
}
void coop_scene_after_replace(bool ok) {
    pthread_mutex_lock(&lock);
    for (int s = 0; s < COOP_SCENES; s++) {
        if (ok && in_image[s] && view.ledger[s] == COOP_LEDGER_ACCEPTED) set_ledger(s, COOP_LEDGER_DONE);
        in_image[s] = false;
    }
    pthread_mutex_unlock(&lock);
}

// ------------------------------------------------------------------ durability
void coop_scene_recover(const char *save) {
    pthread_mutex_lock(&lock);
    if (save_path[0]) { pthread_mutex_unlock(&lock); return; }  // (once per boot)
    snprintf(save_path, sizeof save_path, "%s", save);
    char path[1200], line[128], state[16];
    unsigned id;
    snprintf(path, sizeof path, "%s.scene", save);
    FILE *f = fopen(path, "rb");
    bool ok = true;
    if (f) {
        ok = fgets(line, sizeof line, f) && !strcmp(line, "LGSCENE1\n");
        while (ok && fgets(line, sizeof line, f)) {
            int s = sscanf(line, "scene %u %15s", &id, state) == 2 && id <= 0xffff ? coop_scene_index((uint16_t)id) : -2;
            uint8_t k = 0;
            for (uint8_t i = 1; i <= COOP_LEDGER_DECLINED; i++) if (s != -2 && !strcmp(state, state_name[i])) k = i;
            ok = s != -2 && k;
            if (ok && s >= 0) view.ledger[s] = k;  // (a scene this build doesn't map is dropped)
        }
        fclose(f);
    }
    if (!ok) {
        // Nothing is lost for exactly-once: the scene's flag in the save is what stops a second gift.
        char aside[1250];
        snprintf(aside, sizeof aside, "%s.scene-quarantine", save);
        LOG("[coop-scene] %s is malformed: moved to %s, the ledger starts empty\n", path, aside);
        rename(path, aside);
        memset(view.ledger, 0, sizeof view.ledger);
    }
    pthread_mutex_unlock(&lock);
}
void coop_scene_reset(void) {
    pthread_mutex_lock(&lock);
    bool shared = view.shared, nearby = view.nearby;
    memset(&view, 0, sizeof view);
    view.shared = shared, view.nearby = nearby;
    memset(applied_run, 0, sizeof applied_run), memset(in_image, 0, sizeof in_image), memset(retry_ns, 0, sizeof retry_ns);
    mode_sent = was_pending = false, mode_last = 0, answered_id = 0, answered = 0, first = count = 0, save_path[0] = 0;
    pthread_mutex_unlock(&lock);
}
