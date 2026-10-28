// Online trades (docs/COOP-TRADES.md): negotiation over the relay's W channel, the durable record next to the save
// and its recovery. Native party work is in runtime/hle/coop_trade_apply.c; the overlay in runtime/ui/coop_trade_ui.mm.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "coop_party.h"

// IDLE -> ASKING (sent a request) / ASKED (received one) -> OPEN (both pick and confirm) -> PREPARED (both confirmed,
// the record is on disk, "prepared" sent) -> COMMIT (the relay decided: apply on the game thread) -> APPLIED (in the
// running game, waiting for a save) -> IDLE (the save carries it). DOUBT: prepared without a decision (link lost or a
// restart). QUARANTINE: the record doesn't match the save; nothing is applied.
typedef enum { COOP_TRADE_IDLE, COOP_TRADE_ASKING, COOP_TRADE_ASKED, COOP_TRADE_OPEN, COOP_TRADE_PREPARED,
               COOP_TRADE_COMMIT, COOP_TRADE_APPLIED, COOP_TRADE_DOUBT, COOP_TRADE_QUARANTINE } CoopTradePhase;
// Results (the last trade's end) and W5 abort reasons share these numbers.
enum { COOP_TRADE_DONE = 0, COOP_TRADE_CANCELLED = 1, COOP_TRADE_DISCONNECTED = 2, COOP_TRADE_EXPIRED = 3,
       COOP_TRADE_TIMEOUT = 4, COOP_TRADE_MISMATCH = 5, COOP_TRADE_REFUSED = 6, COOP_TRADE_INVALID = 7,
       COOP_TRADE_SAVE_ERROR = 8, COOP_TRADE_TRADED = 9, COOP_TRADE_QUARANTINED = 10 };
// Why an own Pokemon can't be offered (coop_trade_offer).
enum { COOP_OFFER_OK, COOP_OFFER_NO_SLOT, COOP_OFFER_PARTNER, COOP_OFFER_LAST, COOP_OFFER_INVALID, COOP_OFFER_PHASE };

typedef struct {
    uint16_t species, held_item, moves[4], tid, sid;
    uint8_t level, form, gender;
    bool shiny, nicknamed;
    char nickname[40], ot[40];  // UTF-8
} CoopTradeMon;

typedef struct {
    CoopTradePhase phase;
    uint64_t id, expires_ns, result_until_ns;
    uint8_t result;  // COOP_TRADE_* of the last trade, shown until result_until_ns
    bool mine_ready, theirs_ready, mine_confirmed, theirs_confirmed;
    int mine_slot;  // party slot offered, -1 none
    uint8_t mine[COOP_PB7_BYTES], theirs[COOP_PB7_BYTES];
    CoopParty party;  // the local party snapshot the offer is picked from
    char peer[25];
} CoopTradeView;

// Pure PB7 helpers (the offer panel and the checks).
bool coop_trade_describe(const uint8_t pb7[COOP_PB7_BYTES], CoopTradeMon *out);
// The partner Pikachu / Eevee (form 8 / 1): the game never lets it leave.
bool coop_trade_partner(const uint8_t pb7[COOP_PB7_BYTES]);
// The peer's offer at the trust boundary: checksum and field bounds (coop_party), Let's Go species, no egg, no partner.
bool coop_trade_offer_valid(const uint8_t pb7[COOP_PB7_BYTES]);
// Same Pokemon: encryption constant and PID (a received Pokemon keeps both).
bool coop_trade_same(const uint8_t a[COOP_PB7_BYTES], const uint8_t b[COOP_PB7_BYTES]);
uint16_t coop_trade_hp(const uint8_t pb7[COOP_PB7_BYTES]);

// UI / game thread.
void coop_trade_view(CoopTradeView *out);
bool coop_trade_request(const uint8_t peer_identity[16], const char *peer_name);
bool coop_trade_answer(bool accept, const uint8_t peer_identity[16], const char *peer_name);
bool coop_trade_cancel(void);
void coop_trade_set_party(const CoopParty *party);  // game thread, while a trade is open
int coop_trade_offer(unsigned slot);                  // COOP_OFFER_*
bool coop_trade_confirm(void);
void coop_trade_dismiss(void);                        // hide the last result

// Relay worker.
void coop_trade_tick(void);
bool coop_trade_pop(uint8_t body[255], size_t *size);
void coop_trade_message(const uint8_t *body, size_t size);
void coop_trade_link_lost(void);

// Durability. fs.c: the save opened (loads <save>.trade) and a whole-save write ended.
void coop_trade_recover(const char *save);
void coop_trade_after_replace(bool ok);
// Game thread: a decided trade to apply (give = own offered, take = received); from_disk: loaded at boot, not yet
// applied in this run (the save on disk then tells whether it is done).
bool coop_trade_to_apply(uint8_t give[COOP_PB7_BYTES], uint8_t take[COOP_PB7_BYTES], bool *from_disk);
// result: 1 applied now, 0 already in the save loaded from disk (committed), -1 the save doesn't match (quarantine).
void coop_trade_applied(int result);
void coop_trade_serialized(void);  // the game built a save image (SERIALIZE): an applied trade is in it
void coop_trade_reset(void);       // RT_ON_RESET / tests
