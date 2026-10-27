// Gift delivery handoff (A05). The game can't reach the account service (this worker stops when the game starts), so
// once the local profile is linked the worker:
//  - acknowledges each claim the game's receipt <save dir>\save_0\savedata.bin.gift (runtime/hle/gift.c) lists as
//    "committed <claim> <save sha256>": the save hash is the receipt digest, sent only while the claim is "allocated";
//  - hands the oldest allocated lgpe-items-v1 claim of this profile that isn't committed to the game in
//    savedata.bin.gift-inbox: "claim <uuid>" then "item <id> <count>" lines (public claim data, no credentials). No
//    pending claim: the file is removed. The game applies it at its next save; nothing here touches the save itself.
//  - serves every allocated lgpe-wb7-v1 claim of this profile that isn't committed as a native Mystery Gift card
//    (runtime/wb7.c) in savedata.bin.bcat: normal\<card id>.wb7, a filler card in zone\ (the game needs a card there;
//    the filler's one-time id above 0x7ff makes the game skip it) and cards.txt ("card <id> <claim>" lines), which the
//    game's receipt (runtime/hle/gift.c) maps back to claims once the game has saved a received card. The game shows
//    them under Mystery Gift > Get via internet (runtime/services/bcat.c).
// Failures (service down, a write) leave everything as it was and are retried at the next sync.
#include "wb7.h"

static bool delivery_path(const ProfileContext *ctx, const WCHAR *suffix, WCHAR out[4096]) {
    return swprintf(out, 4096, L"%ls\\save_0\\savedata.bin%ls", ctx->directory, suffix) > 0;
}

// Whole small file, NUL-terminated; false when missing/unreadable/too big.
static bool delivery_read(const WCHAR *path, char *out, DWORD cap) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size; DWORD read = 0;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < cap && ReadFile(file, out, (DWORD)size.QuadPart, &read, NULL) && read == size.QuadPart;
    CloseHandle(file);
    if (ok) out[read] = 0;
    return ok;
}

static bool delivery_write(const WCHAR *path, const void *body, DWORD n) {
    WCHAR temp[4200];
    if (swprintf(temp, 4200, L"%ls.tmp", path) < 0) return false;
    HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    bool ok = WriteFile(file, body, n, &written, NULL) && written == n && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temp);
    return ok;
}

static void delivery_ack(const char *profile, const char *receipt) {
    char line[256], claim[64], sha[80], path[128], response[8192], state[40], owner[37], digest[80], body[512];
    for (const char *p = receipt; *p; p += strcspn(p, "\n") + (p[strcspn(p, "\n")] ? 1 : 0)) {
        size_t n = strcspn(p, "\n");
        if (n >= sizeof line) continue;
        memcpy(line, p, n); line[n] = 0;
        if (sscanf(line, "committed %63s %79s", claim, sha) != 2 || !uuid_valid(claim) || !digest_valid(sha)) continue;
        int status; snprintf(path, sizeof path, "/v1/claims/%s", claim);
        if (!authorized_json("GET", path, NULL, response, sizeof response, &status) || status != 200 ||
            !json_get(response, "state", state, sizeof state, false) || strcmp(state, "allocated") ||
            !json_get(response, "profile_id", owner, sizeof owner, false) || strcmp(owner, profile) ||
            !json_get(response, "reward_digest", digest, sizeof digest, false) || !digest_valid(digest)) continue;
        snprintf(path, sizeof path, "/v1/claims/%s/ack", claim);
        snprintf(body, sizeof body, "{\"profile_id\":\"%s\",\"reward_digest\":\"%s\",\"receipt_digest\":\"%s\"}", profile, digest, sha);
        authorized_json("POST", path, body, response, sizeof response, &status);
    }
}

// The reward's items of one claim object (its raw JSON) as "item <id> <count>\n" lines; false if malformed.
static bool delivery_items(const char *claim, char *out, size_t cap) {
    jsmn_parser parser; jsmntok_t t[512]; jsmn_init(&parser);
    int count = jsmn_parse(&parser, claim, strlen(claim), t, 512), reward = -1, items = -1, found = 0;
    if (count < 1 || t[0].type != JSMN_OBJECT) return false;
    for (int i = 1; i + 1 < count; i++)
        if (t[i].parent == 0 && t[i].type == JSMN_STRING && t[i].end - t[i].start == 6 && !memcmp(claim + t[i].start, "reward", 6)) reward = i + 1;
    if (reward < 0 || t[reward].type != JSMN_OBJECT) return false;
    for (int i = reward + 1; i + 1 < count; i++)
        if (t[i].parent == reward && t[i].type == JSMN_STRING && t[i].end - t[i].start == 5 && !memcmp(claim + t[i].start, "items", 5)) items = i + 1;
    if (items < 0 || t[items].type != JSMN_ARRAY) return false;
    out[0] = 0;
    for (int i = items + 1; i < count; i++) {
        if (t[i].parent != items) continue;
        char row[256], id[16], quantity[16], line[48]; int len = t[i].end - t[i].start;
        if (t[i].type != JSMN_OBJECT || len <= 0 || len >= (int)sizeof row) return false;
        memcpy(row, claim + t[i].start, len); row[len] = 0;
        if (!json_get(row, "item_id", id, sizeof id, true) || !json_get(row, "quantity", quantity, sizeof quantity, true)) return false;
        long a = atol(id), b = atol(quantity);
        if (a < 1 || a > 65535 || b < 1 || b > 999 || ++found > 32) return false;
        snprintf(line, sizeof line, "item %ld %ld\n", a, b);
        if (strlen(out) + strlen(line) >= cap) return false;
        strcat(out, line);
    }
    return found > 0;
}

// The raw text of a top-level object/array member of an object; false if missing or too long.
static bool json_member(const char *json, const char *key, char *out, size_t cap) {
    jsmn_parser parser; jsmntok_t t[512]; jsmn_init(&parser);
    int count = jsmn_parse(&parser, json, strlen(json), t, 512);
    for (int i = 1; count > 0 && t[0].type == JSMN_OBJECT && i + 1 < count; i++)
        if (t[i].parent == 0 && t[i].type == JSMN_STRING && t[i].end - t[i].start == (int)strlen(key) && !memcmp(json + t[i].start, key, strlen(key))) {
            int len = t[i + 1].end - t[i + 1].start;
            if ((t[i + 1].type != JSMN_OBJECT && t[i + 1].type != JSMN_ARRAY) || len >= (int)cap) return false;
            memcpy(out, json + t[i + 1].start, len); out[len] = 0;
            return true;
        }
    return false;
}

// Up to max integers of a JSON array's text ("[31,-1,...]"): the count, -1 if malformed.
static int json_ints(const char *array, int *out, int max) {
    jsmn_parser parser; jsmntok_t t[16]; jsmn_init(&parser);
    int count = jsmn_parse(&parser, array, strlen(array), t, 16), n = 0;
    if (count < 1 || t[0].type != JSMN_ARRAY || t[0].size > max) return -1;
    for (int i = 1; i < count; i++) {
        char v[16], *end; int len = t[i].end - t[i].start;
        if (t[i].type != JSMN_PRIMITIVE || len <= 0 || len >= (int)sizeof v) return -1;
        memcpy(v, array + t[i].start, len); v[len] = 0;
        long x = strtol(v, &end, 10);
        if (*end) return -1;
        out[n++] = (int)x;
    }
    return n;
}

// A native claim's card with that id (the reward; the card's date: created_at); false if the claim can't be a card. The
// server validates rewards (server/accounts/models.py); wb7_build refuses what a card can't hold.
static bool delivery_card(const char *claim, unsigned card_id, uint8_t card[WB7_SIZE]) {
    static char reward[8192], pokemon[4096], list[512], items[2048];
    char kind[24], created[40], text[16];
    unsigned y, m, d;
    Wb7Gift g = {.one_time = true};
    if (card_id < 1024 || card_id > 1999 || !json_member(claim, "reward", reward, sizeof reward) ||
        !json_get(reward, "kind", kind, sizeof kind, false) || !json_get(claim, "created_at", created, sizeof created, false) ||
        sscanf(created, "%4u-%2u-%2u", &y, &m, &d) != 3) return false;
    g.card_id = card_id, g.date = y * 10000 + m * 100 + d;
    if (!strcmp(kind, "item_bundle")) {
        if (!delivery_items(claim, items, sizeof items)) return false;
        for (const char *p = items; *p; p += strcspn(p, "\n") + 1, g.nitems++)
            if (g.nitems == WB7_MAX_ITEMS || sscanf(p, "item %u %u", &g.items[g.nitems][0], &g.items[g.nitems][1]) != 2) return false;
    } else if (!strcmp(kind, "pokemon") && json_member(reward, "pokemon", pokemon, sizeof pokemon)) {
        int ivs[6], moves[4], n;
        g.pokemon = true;
        g.species = (unsigned)json_long(pokemon, "species", 0), g.form = (unsigned)json_long(pokemon, "form", 0);
        g.level = (unsigned)json_long(pokemon, "level", 0), g.ball = (unsigned)json_long(pokemon, "ball", 4);
        g.nature = (int)json_long(pokemon, "nature", -1);
        if (g.nature < -1 || g.nature > 24) return false;
        if (!json_get(pokemon, "shiny", text, sizeof text, false)) return false;
        g.shiny = !strcmp(text, "always") ? 2 : !strcmp(text, "random") ? 1 : 0;
        if (!json_get(pokemon, "gender", text, sizeof text, false)) return false;
        g.gender = !strcmp(text, "male") ? 0 : !strcmp(text, "female") ? 1 : -1;
        if (!json_member(pokemon, "ivs", list, sizeof list) || json_ints(list, ivs, 6) != 6) return false;
        for (int i = 0; i < 6; i++) { if (ivs[i] < -1 || ivs[i] > 31) return false; g.ivs[i] = ivs[i]; }
        if (!json_member(pokemon, "moves", list, sizeof list) || (n = json_ints(list, moves, 4)) < 1) return false;
        for (int i = 0; i < n; i++) { if (moves[i] < 1 || moves[i] > 742) return false; g.moves[i] = (unsigned)moves[i]; }
    } else return false;
    return wb7_build(&g, card);
}

// A claim's card id, moved by the server while the save already has its received flag and the card was never served
// (an official distribution, or a save brought from elsewhere); 0 if there's none to serve now.
static unsigned delivery_card_id(const char *claim, const char *id, const char *profile, const char *served_before,
                                 const uint8_t *save) {
    char line[96], path[128], body[160], response[16384];
    long card = json_long(claim, "card_id", -1);
    for (int tries = 0; card >= 1024 && card <= 1999 && save && wb7_received(save, (unsigned)card); tries++) {
        snprintf(line, sizeof line, "card %ld %s\n", card, id);
        if (strstr(served_before, line) || tries == 3) break;  // (served: the game received it and commits it itself)
        int status;
        snprintf(path, sizeof path, "/v1/claims/%s/card", id);
        snprintf(body, sizeof body, "{\"profile_id\":\"%s\",\"card_id\":%ld}", profile, card);
        if (!authorized_json("POST", path, body, response, sizeof response, &status) || status != 200) return 0;
        card = json_long(response, "card_id", -1);
    }
    return card >= 1024 && card <= 1999 ? (unsigned)card : 0;
}

// Writes the cards of the profile's native claims (see the top); stale card files are removed.
static void delivery_cards(const ProfileContext *ctx, const char *data, const jsmntok_t *t, int count, const char *profile,
                           const char *receipt) {
    static char claim[16384], map[65536], before[65536];
    static uint8_t save[0x100000];
    static bool served[2048];
    WCHAR dir[4096], path[4200];
    uint8_t card[WB7_SIZE];
    if (!delivery_path(ctx, L".bcat", dir)) return;
    if (swprintf(path, 4200, L"%ls\\cards.txt", dir) < 0 || !delivery_read(path, before, sizeof before)) before[0] = 0;
    HANDLE file = delivery_path(ctx, L"", path) ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                                               : INVALID_HANDLE_VALUE;
    DWORD got = 0;
    bool have_save = file != INVALID_HANDLE_VALUE && ReadFile(file, save, sizeof save, &got, NULL) && got == sizeof save;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    CreateDirectoryW(dir, NULL);
    if (swprintf(path, 4200, L"%ls\\normal", dir) > 0) CreateDirectoryW(path, NULL);
    if (swprintf(path, 4200, L"%ls\\zone", dir) > 0) CreateDirectoryW(path, NULL);
    Wb7Gift filler = {.card_id = 0xffff, .one_time = true, .date = 20260101, .nitems = 1, .items = {{17, 1}}};
    if (wb7_build(&filler, card) && swprintf(path, 4200, L"%ls\\zone\\filler.wb7", dir) > 0) delivery_write(path, card, WB7_SIZE);
    memset(served, 0, sizeof served);
    map[0] = 0;
    for (int i = 1; i < count; i++) {
        if (t[i].parent != 0 || t[i].type != JSMN_OBJECT) continue;
        char id[37], owner[37], state[40], adapter[40], committed[64], line[96];
        int len = t[i].end - t[i].start;
        unsigned card_id;
        if (len >= (int)sizeof claim) continue;
        memcpy(claim, data + t[i].start, len); claim[len] = 0;
        if (!json_get(claim, "id", id, sizeof id, false) || !uuid_valid(id) ||
            !json_get(claim, "profile_id", owner, sizeof owner, false) || strcmp(owner, profile) ||
            !json_get(claim, "state", state, sizeof state, false) || strcmp(state, "allocated") ||
            !json_get(claim, "adapter", adapter, sizeof adapter, false) || strcmp(adapter, NATIVE_ADAPTER)) continue;
        snprintf(committed, sizeof committed, "committed %s ", id);
        if (strstr(receipt, committed) || !(card_id = delivery_card_id(claim, id, profile, before, have_save ? save : NULL)) ||
            served[card_id] || !delivery_card(claim, card_id, card)) continue;
        if (swprintf(path, 4200, L"%ls\\normal\\%u.wb7", dir, card_id) < 0 || !delivery_write(path, card, WB7_SIZE)) continue;
        served[card_id] = true;
        snprintf(line, sizeof line, "card %u %s\n", card_id, id);
        if (strlen(map) + strlen(line) < sizeof map) strcat(map, line);
    }
    if (swprintf(path, 4200, L"%ls\\cards.txt", dir) > 0) delivery_write(path, map, (DWORD)strlen(map));
    WIN32_FIND_DATAW found;
    HANDLE find = swprintf(path, 4200, L"%ls\\normal\\*", dir) > 0 ? FindFirstFileW(path, &found) : INVALID_HANDLE_VALUE;
    for (bool more = find != INVALID_HANDLE_VALUE; more; more = FindNextFileW(find, &found)) {
        unsigned n; WCHAR name[32];
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (swscanf(found.cFileName, L"%u", &n) == 1 && n < 2048 && served[n] && swprintf(name, 32, L"%u.wb7", n) > 0 &&
            !wcscmp(name, found.cFileName)) continue;
        if (swprintf(path, 4200, L"%ls\\normal\\%ls", dir, found.cFileName) > 0) DeleteFileW(path);
    }
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
}

static char g_gift_note[256];  // (under g_lock) the game's gift status as a sentence for the launcher

void account_gift_note(char *out, size_t cap) {
    pthread_mutex_lock(&g_lock);
    snprintf(out, cap, "%s", g_gift_note);
    pthread_mutex_unlock(&g_lock);
}

// <save>.gift-status (runtime/hle/gift.c) as the note.
static void delivery_note(const ProfileContext *ctx) {
    WCHAR path[4096]; char status[256] = "";
    if (!delivery_path(ctx, L".gift-status", path) || !delivery_read(path, status, sizeof status)) status[0] = 0;
    const char *note = !strncmp(status, "no-room ", 8) ? "Your bag had no room for a gift. Make room in the bag, then save in the game again." :
                       !strncmp(status, "not-giftable ", 13) ? "A gift could not be given in this game. It stays in your account; contact support." :
                       !strncmp(status, "no-bag ", 7) ? "A gift is waiting for your next save in the game." :
                       !strncmp(status, "quarantined", 11) ? "Gifts are paused: this save doesn't match its gift receipt (a restored or copied save). Contact support." : "";
    pthread_mutex_lock(&g_lock);
    snprintf(g_gift_note, sizeof g_gift_note, "%s", note);
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ in-game codes
// The game's Mystery Gift "Get with code/password" (runtime/hle/serial.c) asks the worker to claim a code for the linked
// profile as a native card; the card is then written like any other (delivery_sync) and the game is told its id. The
// game thread waits on g_redeem (never on the network itself).
static void delivery_sync(void);
static struct { int state; char code[65]; int result; unsigned card_id; } g_redeem;  // (under g_lock) state: 0 idle, 1 asked, 2 done

static void redeem_run(void) {
    char code[65], body[512], headers[128], key[37], result[16384], error[64], profile[37];
    int status = 0, outcome = ACCOUNT_REDEEM_FAILED;
    unsigned card = 0;
    pthread_mutex_lock(&g_lock);
    snprintf(code, sizeof code, "%s", g_redeem.code);
    snprintf(profile, sizeof profile, "%s", g_profile.id);
    bool ready = g_view.state == ACCOUNT_SIGNED_IN && g_profile.bound;
    pthread_mutex_unlock(&g_lock);
    if (!ready) outcome = ACCOUNT_REDEEM_SIGNED_OUT;
    else if (uuid_new(key) && snprintf(headers, sizeof headers, "Idempotency-Key: %s", key) > 0 &&
             snprintf(body, sizeof body, "{\"code\":\"%s\",\"profile_id\":\"%s\",\"adapter\":\"" NATIVE_ADAPTER "\"}", code, profile) > 0) {
        bool ok = http_json_extra("POST", "/v1/claims", body, g_session.access, headers, result, sizeof result, &status);
        if (ok && status == 401) { restore(); if (g_session.access[0]) ok = http_json_extra("POST", "/v1/claims", body, g_session.access, headers, result, sizeof result, &status); }
        long id = ok && status == 201 ? json_long(result, "card_id", -1) : -1;
        char err[160];
        error[0] = 0;
        if (ok && json_member(result, "error", err, sizeof err)) json_get(err, "code", error, sizeof error, false);
        if (id >= 1024 && id <= 1999) outcome = ACCOUNT_REDEEM_OK, card = (unsigned)id;
        else if (!ok || status >= 500 || status == 429) outcome = ACCOUNT_REDEEM_FAILED;
        else if (status == 404) outcome = ACCOUNT_REDEEM_INVALID;
        else if (!strcmp(error, "already_claimed") || !strcmp(error, "code_exhausted")) outcome = ACCOUNT_REDEEM_USED;
        else outcome = ACCOUNT_REDEEM_UNAVAILABLE;  // (window, stock, wrong game, a code-less campaign...)
        if (outcome == ACCOUNT_REDEEM_OK || outcome == ACCOUNT_REDEEM_USED) delivery_sync();  // (the card, or the earlier claim's)
    }
    SecureZeroMemory(code, sizeof code); SecureZeroMemory(body, sizeof body);
    pthread_mutex_lock(&g_lock);
    SecureZeroMemory(g_redeem.code, sizeof g_redeem.code);
    g_redeem.result = outcome, g_redeem.card_id = card, g_redeem.state = 2;
    pthread_mutex_unlock(&g_lock);
}

int account_redeem_code(const char *code, unsigned *card_id) {
    char normalized[65];
    if (!g_started) return ACCOUNT_REDEEM_SIGNED_OUT;
    if (!code_normalize(code, normalized)) return ACCOUNT_REDEEM_INVALID;
    pthread_mutex_lock(&g_lock);
    bool busy = g_redeem.state == 1;
    if (!busy) { snprintf(g_redeem.code, sizeof g_redeem.code, "%s", normalized); g_redeem.state = 1; }
    pthread_mutex_unlock(&g_lock);
    SecureZeroMemory(normalized, sizeof normalized);
    if (busy) return ACCOUNT_REDEEM_FAILED;
    for (int waited = 0; waited < 600; waited++) {  // (30 s: the HTTP timeouts are shorter)
        pthread_mutex_lock(&g_lock);
        bool done = g_redeem.state == 2;
        int result = g_redeem.result;
        if (done) *card_id = g_redeem.card_id, g_redeem.state = 0;
        pthread_mutex_unlock(&g_lock);
        if (done) return result;
        Sleep(50);
    }
    return ACCOUNT_REDEEM_FAILED;
}

static void delivery_sync(void) {
    ProfileContext ctx; char profile[37];
    if (!profile_context(&ctx)) return;
    pthread_mutex_lock(&g_lock);
    bool bound = g_profile.bound && ctx.generation == g_context_generation;
    snprintf(profile, sizeof profile, "%s", g_profile.id);
    pthread_mutex_unlock(&g_lock);
    if (!bound || !uuid_valid(profile)) return;
    WCHAR receipt_path[4096], inbox_path[4096];
    if (!delivery_path(&ctx, L".gift", receipt_path) || !delivery_path(&ctx, L".gift-inbox", inbox_path)) return;
    delivery_note(&ctx);
    static char receipt[65536], data[262144], claim[16384], next[4096];
    if (!delivery_read(receipt_path, receipt, sizeof receipt)) receipt[0] = 0;
    delivery_ack(profile, receipt);

    // shortcut: the first 100 claims of the account; page through /v1/claims once accounts can hold more
    int status;
    if (!authorized_json("GET", "/v1/claims?limit=100", NULL, data, sizeof data, &status) || status != 200) return;
    jsmn_parser parser; static jsmntok_t t[16384]; jsmn_init(&parser);
    int count = jsmn_parse(&parser, data, strlen(data), t, 16384);
    if (count < 1 || t[0].type != JSMN_ARRAY) return;
    delivery_cards(&ctx, data, t, count, profile, receipt);
    next[0] = 0;
    for (int i = 1; i < count && !next[0]; i++) {
        if (t[i].parent != 0 || t[i].type != JSMN_OBJECT) continue;
        char id[37], owner[37], state[40], adapter[40], committed[64], items[2048];
        int len = t[i].end - t[i].start;
        if (len >= (int)sizeof claim) continue;
        memcpy(claim, data + t[i].start, len); claim[len] = 0;
        if (!json_get(claim, "id", id, sizeof id, false) || !uuid_valid(id) ||
            !json_get(claim, "profile_id", owner, sizeof owner, false) || strcmp(owner, profile) ||
            !json_get(claim, "state", state, sizeof state, false) || strcmp(state, "allocated") ||
            !json_get(claim, "adapter", adapter, sizeof adapter, false) || strcmp(adapter, "lgpe-items-v1")) continue;
        snprintf(committed, sizeof committed, "committed %s ", id);
        if (strstr(receipt, committed)) continue;  // in the save already, only its ack failed: never handed over again
        if (!delivery_items(claim, items, sizeof items)) continue;  // (an unreadable reward is never handed to the game)
        snprintf(next, sizeof next, "claim %s\n%s", id, items);
    }
    if (next[0]) delivery_write(inbox_path, next, (DWORD)strlen(next));
    else DeleteFileW(inbox_path);
}
