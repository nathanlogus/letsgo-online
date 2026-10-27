// Mystery Gift item adapter (A04b, docs/ACCOUNTS-MYSTERY-GIFTS-ROADMAP.md): adds an item to the native bag the way the
// game's own scripts do, and binds it to the save that carries it (receipt below). The claim comes from the launcher's
// handoff file (runtime/account_delivery.inc) or, for QA, LARECOMP_GIFT_QA=<item>:<count>:<claim>.
//
// Bag add 0x70001c8e20(bag, item, count, mark_new) -> bool: the pocket is 0x700071d340(item, 0xb) (0..6, else no such
// item); pocket p's slot table is at bag+0xde8+8p, with 60/108/200/150/50/150/150 u32 slots (item bits 0-14, count
// 15-29, "new" bit 30). It fails only on a full pocket or a stack already at 999, and otherwise caps the stack at 999
// silently, so gift_room() checks the whole count fits first. bag = world[+0x98] (save data)[+0x58][+0x68], as the
// script item command 0x7000195410 finds it.
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include "../hle.h"
#include "../fnhook.h"
#include "../wb7.h"

uint64_t rt_addr(uint64_t addr);

#define WORLD_GOT 0x70015fad08ull
#define FIELD_UPDATE 0x70002f8bb0ull
#define ITEM_POCKET 0x700071d340ull
#define BAG_ADD 0x70001c8e20ull
#define SERIALIZE 0x70001cddf0ull  // the save menu's save builds the 1 MB image here (game thread), then a job writes it
#define ITEM_TABLE 0x700163ea48ull  // -> one byte per item id, ids 0..ITEM_MAX
#define ITEM_MAX 0x421

static const uint16_t pocket_slots[7] = {60, 108, 200, 150, 50, 150, 150};
// Items a gift may give (207): PKHeX's legal Let's Go items (tests/tools/gift-items) without TMs (pocket 1) and key
// items (bit 7 of the game's item byte; dump: LARECOMP_GIFT_QA=table). The game takes any id: ids above 0x421 land in
// pocket 0, and unused ids below it are stored as they are.
static const uint16_t giftable[] = {
    1, 2, 3, 4, 12, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 38, 39, 40, 41, 50, 51, 53, 55, 56,
    57, 58, 59, 60, 61, 62, 76, 77, 78, 79, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 101, 102, 103, 121, 122,
    123, 124, 125, 126, 127, 128, 164, 166, 168, 571, 656, 659, 660, 661, 662, 663, 671, 672, 675, 676, 678, 679, 709,
    760, 762, 770, 795, 796, 849, 861, 862, 863, 864, 865, 866, 885, 886, 887, 888, 889, 890, 891, 892, 893, 894, 895,
    896, 900, 901, 902, 903, 960, 961, 962, 963, 964, 965, 966, 967, 968, 969, 970, 971, 972, 973, 974, 975, 976, 977,
    978, 979, 980, 981, 982, 983, 984, 985, 986, 987, 988, 989, 990, 991, 992, 993, 994, 995, 996, 997, 998, 999, 1000,
    1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016, 1017, 1018, 1019,
    1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038,
    1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057,
};
static int id_cmp(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }
static RuntimeFnHook previous_field, previous_serialize;
static unsigned qa_updates;
static atomic_bool qa_done;

static bool guest(uint64_t p) { return p >= GUEST_LO && p < GUEST_HI; }
static uint64_t ptr(uint64_t p) { return guest(p) ? *(const uint64_t *)(uintptr_t)p : 0; }
static uint64_t bag(void) { return ptr(ptr(ptr(ptr(ptr(WORLD_GOT)) + 0x98) + 0x58) + 0x68); }

static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Cpu k = *c;  // (the hooked function's own registers stay as they were)
    k.x[0] = a0, k.x[1] = a1, k.x[2] = a2, k.x[3] = a3;
    guest_call(&k, fn);
    return k.x[0];
}

// The item's pocket table and slot count, or 0 when the game has no such item.
static uint32_t *pocket(Cpu *c, uint64_t b, unsigned item, unsigned *slots) {
    uint32_t p = (uint32_t)call(c, ITEM_POCKET, item, 0xb, 0, 0);
    if (p > 6) return NULL;
    uint64_t t = ptr(b + 0xde8 + 8 * p);
    *slots = pocket_slots[p];
    return guest(t) ? (uint32_t *)(uintptr_t)t : NULL;
}

// How many of the item the bag holds (-1: no such item / no bag), and whether count more fit without the 999 cap.
static int gift_held(Cpu *c, uint64_t b, unsigned item, unsigned count, bool *room) {
    unsigned slots;
    uint32_t *t = pocket(c, b, item, &slots);
    if (!t) return -1;
    bool empty = false;
    for (unsigned i = 0; i < slots; i++) {
        unsigned id = t[i] & 0x7fff, n = (t[i] >> 15) & 0x7fff;
        if (id == item) { *room = n + count <= 999; return (int)n; }
        if (!id) empty = true;
    }
    *room = empty && count <= 999;
    return 0;
}

// ------------------------------------------------------------------ the claim
#define MAX_ITEMS 32  // (server Reward: 1..32 distinct items, 1..999 each)
static char g_claim[64];            // the gift waiting for its save ("": none)
static unsigned g_items[MAX_ITEMS][2], g_nitems;
static atomic_bool g_in_image;      // added to the bag and serialized: the next whole save write carries it
static bool g_quarantine, g_recovered, g_bag_ok;  // (g_bag_ok: the bag's addresses are known: Eevee)
static char g_committed[16384];     // "committed <claim> <save sha256>\n" lines
static char g_pending[256];         // the pending line during a replace
static char g_new_sha[65];          // the hash of the save being written
// Native cards (runtime/account_delivery.inc): <save>.bcat/cards.txt maps the served card ids to claims. A card the game
// received is in its save's received flags: the claim commits with the save that carries the flag (no pending line:
// recovery reads the flags of the save on disk).
#define MAX_CARDS 1024
static struct { unsigned id; char claim[64]; } g_cards[MAX_CARDS];
static unsigned g_ncards;
static unsigned g_received[MAX_CARDS], g_nreceived;  // indexes into g_cards: newly received in the save being written
static char g_bcat[1200];                            // <save>.bcat when it exists (bcat.c serves it)
static char g_save[1100];                            // the save (gift_recover)
static atomic_bool g_cards_stale;                    // a card was claimed in game (serial.c): cards.txt changed
static const char *g_reason;                         // why claim_apply refused ("no-room", "not-giftable", "no-bag")

// <save>.gift-status: what the launcher tells the player ("no-room <claim>", "not-giftable <claim>", "quarantined");
// removed when nothing needs them.
static void status_write(const char *line) {
    char path[1200];
    snprintf(path, sizeof path, "%s.gift-status", g_save);
    if (!g_save[0]) return;
    if (!line) { unlink(path); return; }
    FILE *f = fopen(path, "wb");
    if (f) fputs(line, f), fclose(f);
}

// Adds every item of the claim or none. False: an item isn't giftable, no bag, or it doesn't all fit (bag unchanged).
static bool claim_apply(Cpu *c) {
    uint64_t b = bag();
    unsigned need[7] = {0}, empty[7] = {0}, slots;
    int held[MAX_ITEMS], pocket_of[MAX_ITEMS];
    if (!b) { LOG("[gift] no bag yet\n"); g_reason = "no-bag"; return false; }
    for (unsigned k = 0; k < g_nitems; k++) {
        unsigned item = g_items[k][0], count = g_items[k][1];
        uint16_t key = (uint16_t)item;
        if (item > 0xffff || !bsearch(&key, giftable, sizeof giftable / sizeof *giftable, sizeof *giftable, id_cmp)) {
            LOG("[gift] item %u: not a giftable item\n", item);
            g_reason = "not-giftable";
            return false;
        }
        uint32_t p = (uint32_t)call(c, ITEM_POCKET, item, 0xb, 0, 0);
        uint32_t *t = pocket(c, b, item, &slots);
        if (!t || p > 6) { LOG("[gift] item %u: no pocket\n", item); g_reason = "not-giftable"; return false; }
        pocket_of[k] = (int)p, held[k] = -1, empty[p] = 0;
        for (unsigned i = 0; i < slots; i++) {
            unsigned id = t[i] & 0x7fff, n = (t[i] >> 15) & 0x7fff;
            if (id == item) held[k] = (int)n;
            if (!id) empty[p]++;
        }
        if (held[k] < 0) need[p]++, held[k] = 0;
        else if (held[k] + count > 999) { LOG("[gift] item %u x%u: no room (holds %d)\n", item, count, held[k]); g_reason = "no-room"; return false; }
    }
    for (int p = 0; p < 7; p++)
        if (need[p] > empty[p]) { LOG("[gift] pocket %d: %u new items, %u free slots: no room\n", p, need[p], empty[p]); g_reason = "no-room"; return false; }
    for (unsigned k = 0; k < g_nitems; k++) {
        bool room;
        bool ok = call(c, BAG_ADD, b, g_items[k][0], g_items[k][1], 1) & 1;
        int after = gift_held(c, b, g_items[k][0], 0, &room);
        LOG("[gift] item %u x%u: %s (held %d -> %d, pocket %d)\n", g_items[k][0], g_items[k][1], ok ? "added" : "refused", held[k],
            after, pocket_of[k]);
        // (can't happen after the checks above; the claim still commits, so a partial gift is never given twice)
        if (!ok || after != held[k] + (int)g_items[k][1]) LOG("[gift] claim %s: item %u not added as checked\n", g_claim, g_items[k][0]);
    }
    return true;
}

// ------------------------------------------------------------------ receipt
// <save>.gift: "committed <claim> <save sha256>" lines, and at most one "pending <claim> <old save sha256> <new save
// sha256>" while a save carrying that claim is being replaced. The gift goes into the bag on the game thread right
// before the game builds its save image (SERIALIZE), so the save written next carries it; fs.c's save_replace writes the
// pending line after the new image is flushed and before it replaces the save, and commits the claim after. Recovery
// when the save is first opened: the save's hash is the new one -> committed; the old one -> the replace never happened,
// the claim is still pending (the bag change was lost with the process); anything else -> quarantine: no gift is
// applied. The launcher (runtime/account_delivery.inc) reads the committed lines to acknowledge the claims, and hands
// the next claim over in <save>.gift-inbox ("claim <id>", "item <id> <count>" lines).
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
static bool sha_hex(const void *data, size_t n, char out[65]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    unsigned char h[32];
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0;
    if (ok) ok = BCryptHash(alg, NULL, 0, (PUCHAR)data, (ULONG)n, h, 32) == 0;
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; ok && i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
    return ok;
}
#else
static bool sha_hex(const void *data, size_t n, char out[65]) { (void)data, (void)n, (void)out; return false; }  // (no gifts)
#endif

static bool file_sha(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static uint8_t buf[0x100000];  // (the save is 1 MB; recovery and saves never overlap)
    size_t n = fread(buf, 1, sizeof buf, f);
    bool ok = !ferror(f) && fgetc(f) == EOF;
    fclose(f);
    return ok && sha_hex(buf, n, out);
}

static bool receipt_write(const char *save) {
    char path[1200], tmp[1210];
    snprintf(path, sizeof path, "%s.gift", save);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fputs(g_committed, f) >= 0 && fputs(g_pending, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f) ok = !fclose(f) && ok;
    ok = ok && plat_replace_file(tmp, path);
    if (!ok) { LOG("[gift] cannot write %s\n", path); unlink(tmp); }
    return ok;
}

static void commit_line(const char *claim, const char *sha) {
    char line[160];
    snprintf(line, sizeof line, "committed %s %s\n", claim, sha);
    strlcat(g_committed, line, sizeof g_committed);
}

static bool committed(const char *claim) {
    char key[80];
    snprintf(key, sizeof key, "committed %s ", claim);
    return strstr(g_committed, key) != NULL;  // (claims are single tokens: a line can't match inside another)
}

// The launcher's handoff (<save>.gift-inbox) as the pending claim. A malformed file gives no claim.
static void inbox_read(const char *save) {
    char path[1200], line[128];
    snprintf(path, sizeof path, "%s.gift-inbox", save);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    bool ok = fgets(line, sizeof line, f) && sscanf(line, "claim %63[A-Za-z0-9-]", g_claim) == 1;
    g_nitems = 0;
    while (ok && fgets(line, sizeof line, f))
        ok = g_nitems < MAX_ITEMS && sscanf(line, "item %u %u", &g_items[g_nitems][0], &g_items[g_nitems][1]) == 2 &&
             g_items[g_nitems][1] >= 1 && g_items[g_nitems][1] <= 999, g_nitems++;
    fclose(f);
    if (!ok || !g_nitems) { LOG("[gift] %s is malformed: no gift\n", path); g_claim[0] = 0, g_nitems = 0; return; }
    LOG("[gift] claim %s (%u items) waits for the next save\n", g_claim, g_nitems);
}

const char *gift_bcat_dir(void) { return g_bcat[0] ? g_bcat : NULL; }

static void cards_read(const char *save) {
    char path[1200], line[128];
    struct stat st;
    snprintf(path, sizeof path, "%s.bcat", save);
    if (!stat(path, &st) && S_ISDIR(st.st_mode)) snprintf(g_bcat, sizeof g_bcat, "%s", path);
    snprintf(path, sizeof path, "%s.bcat/cards.txt", save);
    FILE *f = fopen(path, "rb");
    while (f && g_ncards < MAX_CARDS && fgets(line, sizeof line, f))
        if (sscanf(line, "card %u %63[A-Za-z0-9-]", &g_cards[g_ncards].id, g_cards[g_ncards].claim) == 2 && g_cards[g_ncards].id < 2048) g_ncards++;
    if (f) fclose(f);
    if (g_ncards) LOG("[gift] %u Mystery Gift card(s) offered\n", g_ncards);
}

// serial.c's thread: re-read cards.txt at the next save (the save path is the only reader of g_cards)
void gift_cards_reload(void) { atomic_store(&g_cards_stale, true); }

// The served cards the save image has received whose claims aren't committed yet (into g_received).
static unsigned cards_received(const uint8_t *image, size_t size) {
    if (atomic_exchange(&g_cards_stale, false) && g_save[0]) g_ncards = 0, cards_read(g_save);
    g_nreceived = 0;
    for (unsigned i = 0; size >= 0x100000 && i < g_ncards; i++)
        if (wb7_received(image, g_cards[i].id) && !committed(g_cards[i].claim)) g_received[g_nreceived++] = i;
    return g_nreceived;
}

static void cards_commit(const char *sha) {
    for (unsigned k = 0; k < g_nreceived; k++) {
        commit_line(g_cards[g_received[k]].claim, sha);
        LOG("[gift] card %u (claim %s) received and saved\n", g_cards[g_received[k]].id, g_cards[g_received[k]].claim);
    }
}

// A save that received cards whose receipt line was lost (the process ended between the replace and the receipt).
static void cards_recover(const char *save) {
    static uint8_t buf[0x100000];
    char sha[65];
    FILE *f = g_ncards ? fopen(save, "rb") : NULL;
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    if (n != sizeof buf || !cards_received(buf, n) || !sha_hex(buf, n, sha)) return;
    LOG("[gift] recovery: the save holds received cards without their receipt\n");
    cards_commit(sha);
    receipt_write(save);
}

// fs.c: the game opened its save (once per run, before any save is built).
void gift_recover(const char *save) {
    if (g_recovered) return;
    g_recovered = true;
    snprintf(g_save, sizeof g_save, "%s", save);
    if (!g_claim[0] && g_bag_ok) inbox_read(save);  // (LARECOMP_GIFT_QA's claim comes first)
    cards_read(save);
    char path[1200], line[256], claim[64], old_sha[65], new_sha[65], now[65];
    snprintf(path, sizeof path, "%s.gift", save);
    FILE *f = fopen(path, "rb");
    if (!f) { cards_recover(save); return; }
    bool rewrite = false;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "committed %63s %64s", claim, new_sha) == 2) { commit_line(claim, new_sha); continue; }
        if (sscanf(line, "pending %63s %64s %64s", claim, old_sha, new_sha) != 3) { g_quarantine = true; continue; }
        rewrite = true;
        if (!file_sha(save, now)) g_quarantine = true;
        else if (!strcmp(now, new_sha)) {
            LOG("[gift] recovery: claim %s reached the save before the last run ended: committed\n", claim);
            commit_line(claim, new_sha);
        } else if (!strcmp(now, old_sha)) LOG("[gift] recovery: claim %s never reached the save: still pending\n", claim);
        else g_quarantine = true;
    }
    fclose(f);
    if (g_quarantine) {
        LOG("[gift] recovery: %s doesn't match the save: quarantined, no gift is applied\n", path);
        status_write("quarantined\n");
        return;
    }
    g_pending[0] = 0;
    if (rewrite) receipt_write(save);
    cards_recover(save);
}

// fs.c save_replace, after the new image is flushed to <save>.tmp: false = don't replace the save.
bool gift_before_replace(const char *save, const void *image, size_t size) {
    bool cards = !g_quarantine && cards_received(image, size) && sha_hex(image, size, g_new_sha);
    if (!atomic_load(&g_in_image)) return true;
    char old_sha[65];
    if (!file_sha(save, old_sha) || (!cards && !sha_hex(image, size, g_new_sha))) return false;
    snprintf(g_pending, sizeof g_pending, "pending %s %s %s\n", g_claim, old_sha, g_new_sha);
    return receipt_write(save);
}

// fs.c save_replace, after the replace (ok) or its failure.
void gift_after_replace(const char *save, bool ok) {
    if (g_nreceived && ok) { cards_commit(g_new_sha); if (!atomic_load(&g_in_image)) receipt_write(save); }
    if (!ok) g_nreceived = 0;
    if (!atomic_load(&g_in_image)) { g_nreceived = 0; return; }
    atomic_store(&g_in_image, false);
    g_pending[0] = 0;
    if (!ok) { LOG("[gift] claim %s: the save failed, still pending\n", g_claim); return; }
    commit_line(g_claim, g_new_sha);
    LOG("[gift] claim %s committed with the save\n", g_claim);
    status_write(NULL);
    receipt_write(save);  // (if this fails the pending line stays and recovery commits it: the save has the new hash)
    g_claim[0] = 0, g_nreceived = 0;
}

// The game builds its save image (x0 = the save object, image at +0x90): the gift goes into the bag first.
static int serialize_hook(Cpu *c, uint64_t addr) {
    if (g_claim[0] && g_recovered && !g_quarantine && !atomic_load(&g_in_image)) {
        if (committed(g_claim)) { LOG("[gift] claim %s is already in this save: not applied again\n", g_claim); g_claim[0] = 0; }
        else if (g_reason = "no-room", claim_apply(c)) atomic_store(&g_in_image, true);
        else {
            char line[128];
            LOG("[gift] claim %s not applied: stays pending\n", g_claim);
            snprintf(line, sizeof line, "%s %s\n", g_reason, g_claim);
            status_write(line);
            g_claim[0] = 0;
        }
    }
    return previous_serialize ? previous_serialize(c, addr) : 0;
}

// LARECOMP_GIFT_QA=table: the game's per-item byte (pocket in bits 0-2; bits 6, 7 read by 0x700071d370 / 0x700071d3a0).
static void dump_table(void) {
    uint64_t t = ptr(ITEM_TABLE);
    for (unsigned i = 1; t && i <= ITEM_MAX; i++)
        if (*(const uint8_t *)(uintptr_t)(t + i)) LOG("[gift] table %u %02x\n", i, *(const uint8_t *)(uintptr_t)(t + i));
}

static int field_hook(Cpu *c, uint64_t addr) {
    if (!atomic_load(&qa_done) && ++qa_updates == 30) atomic_store(&qa_done, true), dump_table();
    return previous_field ? previous_field(c, addr) : 0;
}

// After fps60_configure (its FIELD_UPDATE hook is chained). QA: LARECOMP_GIFT_QA=<item>:<count>:<claim> gives that
// claim instead of the launcher's handoff (copied saves only).
void gift_configure(void) {
    const char *qa = getenv("LARECOMP_GIFT_QA");
    qa_updates = 0, atomic_store(&qa_done, false), atomic_store(&g_in_image, false);
    g_claim[0] = g_committed[0] = g_pending[0] = g_bcat[0] = g_save[0] = 0, g_nitems = g_ncards = g_nreceived = 0;
    g_quarantine = g_recovered = false;
    // shortcut: the bag route (lgpe-items-v1) has Eevee addresses only; map them before Pikachu gets that route. Native
    // cards need no address (the save image's received flags): both games.
    g_bag_ok = rt_addr(BAG_ADD) == BAG_ADD;
    if (!g_bag_ok) return;  // (cards still recover and commit; no bag gift: a handoff waits)
    if (qa && !strcmp(qa, "table")) { rt_register_fnhook_chain(FIELD_UPDATE, field_hook, &previous_field); return; }
    if (qa && sscanf(qa, "%u:%u:%63[A-Za-z0-9_-]", &g_items[0][0], &g_items[0][1], g_claim) == 3) g_nitems = 1;
    else if (qa) g_claim[0] = 0, LOG("[gift] QA: <item>:<count>:<claim>\n");
    rt_register_fnhook_chain(SERIALIZE, serialize_hook, &previous_serialize);
    if (g_claim[0]) LOG("[gift] QA: claim %s = item %u x%u at the next save\n", g_claim, g_items[0][0], g_items[0][1]);
}
