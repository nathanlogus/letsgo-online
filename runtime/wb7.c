// Mystery Gift cards for the game's own "Get via internet" (docs/ACCOUNTS-MYSTERY-GIFTS-ROADMAP.md, native Mystery
// Gift). Offsets: PKHeX's WB7 (tests/tools/gift-items layout); the checks are the game's card parser 0x70001c7370:
// size 0x310, the u16 at 0x202 = CRC-16/CCITT-FALSE of the card with that field zero, a one-time card (0x25a bit 0)
// skipped when its id's received flag is set (ids above 0x7ff count as received).
#include <string.h>
#include "wb7.h"

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, unsigned v) { put16(p, v & 0xffff), put16(p + 2, v >> 16); }

static unsigned crc16(const uint8_t *d, unsigned n) {
    unsigned crc = 0xffff;
    for (unsigned i = 0; i < n; i++) {
        crc ^= (unsigned)d[i] << 8;
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1) & 0xffff;
    }
    return crc;
}

bool wb7_build(const Wb7Gift *g, uint8_t out[WB7_SIZE]) {
    memset(out, 0, WB7_SIZE);
    if (g->card_id > 0xffff) return false;  // (a one-time id above 0x7ff is never offered: zone's filler)
    out[0] = 3;                                         // RestrictVersion: both Let's Go games
    put16(out + 0x208, g->card_id);
    unsigned y = g->date / 10000, m = g->date / 100 % 100, d = g->date % 100;
    put32(out + 0x254, (y - 2000) * 10000 + m * 100 + d);
    out[0x25a] = g->one_time ? 1 : 0;                   // CardFlags bit 0
    if (!g->pokemon) {
        if (!g->nitems || g->nitems > WB7_MAX_ITEMS) return false;
        out[0x259] = 1;                                 // CardType: item
        for (unsigned i = 0; i < g->nitems; i++) {
            if (!g->items[i][0] || g->items[i][0] > 0xffff || !g->items[i][1] || g->items[i][1] > 999) return false;
            put16(out + 0x270 + 4 * i, g->items[i][0]), put16(out + 0x272 + 4 * i, g->items[i][1]);
        }
    } else {
        if (!g->species || g->species > 809 || !g->level || g->level > 100 || g->form > 31 || !g->ball || g->ball > 26) return false;
        // CardType 0: a Pokemon. TID/SID, OT names, origin game and encryption constant stay zero: the game's own
        // values (the receiving player's).
        out[0x27e] = (uint8_t)g->ball;
        for (int i = 0; i < 4; i++) put16(out + 0x282 + 2 * i, g->moves[i]);
        put16(out + 0x28a, g->species);
        out[0x28c] = (uint8_t)g->form;
        out[0x2a8] = g->nature < 0 ? 0xff : (uint8_t)g->nature;
        out[0x2a9] = g->gender < 0 ? 3 : (uint8_t)g->gender;
        out[0x2aa] = 3;                                 // AbilityType: either of the two
        out[0x2ab] = g->shiny == 2 ? 2 : g->shiny == 1 ? 1 : 3;  // ShinyType: always / random / never
        out[0x2b0] = (uint8_t)g->level;                 // met level
        for (int i = 0; i < 6; i++) out[0x2b7 + i] = g->ivs[i] < 0 ? 0xff : (uint8_t)g->ivs[i];
        out[0x2d8] = (uint8_t)g->level;
    }
    put16(out + 0x202, crc16(out, WB7_SIZE));
    return true;
}

// The save's gift records (block at 0x45600, received flags at +0xc80: bit id % 8 of byte id / 8)
bool wb7_received(const uint8_t *save, unsigned card_id) {
    return card_id <= 0x7ff && (save[0x45600 + 0xc80 + card_id / 8] >> (card_id % 8) & 1);
}
