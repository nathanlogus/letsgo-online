// The Mystery Gift "Get with code/password" screen, answered from our account service (docs/ACCOUNTS-MYSTERY-GIFTS-
// ROADMAP.md, in-game codes). The game POSTs {"serialcode": ...} to api.fushigi.switch1.pokemon-gl.com/serial.auth (and
// serial.update after receiving) through its libcurl wrapper; the reply it reads is "serialcodeStatus" (0: valid) and
// "dataNo" (the gift). Here a request to that host never reaches the network: the account worker claims the code for
// the linked profile as a native card (runtime/account_delivery.inc) and the reply is written into the request object
// the way the wrapper's own job would.
//
// The wrapper (Eevee addresses): start 0x7000afe8e0(request, url, body) -> bool; busy 0x7000afed50 (true while its job
// [+0x10] runs); then, with no job, the getters read: +0x08 error, +0xb0 curl result (0: ok), +0xb8 HTTP status, +0x58
// {u32 size, pad, +0x60 char *body} (body: the request's 1 MB buffer at +0x18).
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../hle.h"
#include "../fnhook.h"
#include "../account.h"

uint64_t rt_addr(uint64_t addr);
void gift_cards_reload(void);  // (gift.c: the claimed card's receipt)

#define HTTP_START 0x7000afe8e0ull
#define HTTP_BUSY 0x7000afed50ull
#define HOST "https://api.fushigi.switch1.pokemon-gl.com/"
#define MAX_PENDING 4

static struct { _Atomic uint64_t request; char code[65]; bool update; } g_pending[MAX_PENDING];
static RuntimeFnHook previous_start, previous_busy;

#define U32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define U64(a) (*(volatile uint64_t *)(uintptr_t)(a))

// The game's statuses: 0 valid; the others are the screen's error messages (to be matched in game, see the roadmap).
static int game_status(int outcome) {
    switch (outcome) {
    case ACCOUNT_REDEEM_OK: return 0;
    case ACCOUNT_REDEEM_INVALID: return 1;
    case ACCOUNT_REDEEM_USED: return 2;
    case ACCOUNT_REDEEM_UNAVAILABLE: return 3;
    default: return 9;
    }
}

static void reply(uint64_t request, const char *json) {
    uint64_t buffer = U64(request + 0x18);
    size_t n = strlen(json);
    memcpy((void *)(uintptr_t)buffer, json, n + 1);
    U64(request + 0x60) = buffer;
    U32(request + 0x58) = (uint32_t)n;
    U32(request + 0x08) = 0, U32(request + 0xb0) = 0, U64(request + 0xb8) = 200, U64(request + 0x10) = 0;
}

static void *redeem(void *arg) {
    int k = (int)(intptr_t)arg;
    uint64_t request = atomic_load(&g_pending[k].request);
    char json[160];
    if (g_pending[k].update) snprintf(json, sizeof json, "{\"serialcodeStatus\":\"0\"}");  // (the receipt is the save's flag)
    else {
        unsigned card = 0;
        int outcome = account_redeem_code(g_pending[k].code, &card);
        LOG("[serial] code: %s\n", outcome == ACCOUNT_REDEEM_OK ? "claimed" : "refused");  // (never the code)
        if (outcome == ACCOUNT_REDEEM_OK) gift_cards_reload();
        snprintf(json, sizeof json, outcome == ACCOUNT_REDEEM_OK ? "{\"serialcodeStatus\":\"0\",\"dataNo\":\"%u\"}" : "{\"serialcodeStatus\":\"%d\"}",
                 outcome == ACCOUNT_REDEEM_OK ? card : (unsigned)game_status(outcome));
    }
    memset(g_pending[k].code, 0, sizeof g_pending[k].code);
    reply(request, json);
    atomic_store(&g_pending[k].request, 0);  // (after the reply: the busy hook then lets the getters read it)
    return NULL;
}

static int start_hook(Cpu *c, uint64_t addr) {
    const char *url = c->x[1] ? (const char *)(uintptr_t)c->x[1] : "";
    if (strncmp(url, HOST, strlen(HOST))) return previous_start ? previous_start(c, addr) : 0;
    const char *body = c->x[2] ? (const char *)(uintptr_t)c->x[2] : "", *code = strstr(body, "\"serialcode\":\"");
    uint64_t request = c->x[0];
    int k = 0;
    while (k < MAX_PENDING && atomic_load(&g_pending[k].request)) k++;
    c->x[0] = 0;
    if (k == MAX_PENDING || U64(request + 0x10)) return 1;  // (a request already running: refused like a busy handle)
    memset(g_pending[k].code, 0, sizeof g_pending[k].code);
    if (code) {
        code += 14;
        size_t n = strcspn(code, "\"");
        if (n < sizeof g_pending[k].code) memcpy(g_pending[k].code, code, n);
    }
    g_pending[k].update = strstr(url, "serial.update") != NULL;
    U32(request + 0x08) = 0;
    atomic_store(&g_pending[k].request, request);
    pthread_t t;
    if (pthread_create(&t, NULL, redeem, (void *)(intptr_t)k)) { atomic_store(&g_pending[k].request, 0); return 1; }
    pthread_detach(t);
    c->x[0] = 1;
    return 1;
}

static int busy_hook(Cpu *c, uint64_t addr) {
    for (int k = 0; k < MAX_PENDING; k++)
        if (atomic_load(&g_pending[k].request) == c->x[0]) { c->x[0] = 1; return 1; }
    return previous_busy ? previous_busy(c, addr) : 0;
}

void serial_configure(void) {
    // shortcut: Eevee addresses only; map the wrapper for Pikachu before its code screen goes through our service
    if (rt_addr(HTTP_START) != HTTP_START) return;
    rt_register_fnhook_chain(HTTP_START, start_hook, &previous_start);
    rt_register_fnhook_chain(HTTP_BUSY, busy_hook, &previous_busy);
}
