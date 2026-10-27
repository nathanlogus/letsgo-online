#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "coop_party.h"
#include "coop_battle_command.h"

typedef enum { COOP_LINK_NONE, COOP_LINK_OFFER, COOP_LINK_PREPARING, COOP_LINK_ACTIVE } CoopBattleLinkPhase;
typedef struct {
    CoopBattleLinkPhase phase;
    bool host, waiting, submitted, party_ready;
    uint64_t battle, deadline_ns, toast_until_ns;
    uint32_t turn, sequence;
    unsigned timeout_ms, trainer;
    CoopBattleChoices choices;
    uint16_t opponents[2], hp[2], max_hp[2];
    uint8_t outcome; // 1 remote decision, 2 native AI timeout
} CoopBattleLinkView;

bool coop_battle_link_enabled(void);
// The "Team-up battles" option (coop.ini): off, no offers as host and every offer declined as guest.
void coop_battle_link_set_offers(bool on);
// Guest, each network tick: false while it can't join (not on the overworld); an open offer is declined after 0.5 s.
void coop_battle_link_guest_ready(bool ready);
void coop_battle_link_reset(void);
void coop_battle_link_tick(void);
void coop_battle_link_party(const CoopParty *);
// Guest: the party it sent with its accept (B2); the battle's ally party is built from it.
bool coop_battle_link_local_party(CoopParty *);
void coop_battle_link_view(CoopBattleLinkView *);
bool coop_battle_link_offer(uint64_t id, unsigned timeout_ms, unsigned trainer);
// Host: an unanswered offer expires earlier (B6 reason 3), e.g. a trainer's held pre-battle text.
void coop_battle_link_offer_deadline(uint64_t ns);
bool coop_battle_link_accept(void);
bool coop_battle_link_lead(uint8_t lead[COOP_PB7_BYTES], uint64_t *id);
// Host: the guest's whole party, once every B2 chunk is in.
bool coop_battle_link_guest_party(CoopParty *, uint64_t *id);
bool coop_battle_link_request(const CoopAllyWait *, const uint16_t opponents[2], const uint16_t hp[2], const uint16_t max_hp[2]);
bool coop_battle_link_choose(unsigned move_index, unsigned target);
// Guest: switch in its own party slot (an action-select switch or a replacement after a faint).
bool coop_battle_link_choose_switch(unsigned party_slot);
bool coop_battle_link_take(CoopBattleMove *, uint64_t *received_ns);
void coop_battle_link_complete(bool use_ai);
void coop_battle_link_end(void);
// Only the connected worker consumes the queue or supplies received payloads.
bool coop_battle_link_pop(uint8_t body[255], size_t *size);
void coop_battle_link_message(const uint8_t *body, size_t size);
// Measured relay round trip; the guest's prompt closes early by this margin.
void coop_battle_link_latency(unsigned rtt_ms);

// B6 end reasons. Only a host FINISHED end carrying its final command counts
// lets a guest replay drain; every other end aborts it.
enum { COOP_END_FINISHED, COOP_END_CANCELLED, COOP_END_DISCONNECTED, COOP_END_EXPIRED };
// B10 desync reasons, also CoopBattleMirrorSetup.reason.
enum { COOP_MIRROR_OK, COOP_MIRROR_GAP, COOP_MIRROR_BAD_PARTY, COOP_MIRROR_OVERFLOW, COOP_MIRROR_DIGEST,
       COOP_MIRROR_INCOMPLETE_END, COOP_MIRROR_LINK_LOST };

// Experimental native battle presentation. All fields are copied values; no
// native pointers or complete saves cross this channel.
typedef struct {
    uint64_t battle,seed;
    unsigned trainer;
    bool host,available,ended,aborted;
    uint8_t reason; // COOP_MIRROR_* once aborted
    CoopParty host_party;
} CoopBattleMirrorSetup;
typedef struct {CoopBattleMove move;bool native_ai;} CoopBattleCanonical;
bool coop_battle_link_mirror_enabled(void);
bool coop_battle_link_mirror_party(const CoopParty *);
bool coop_battle_link_mirror_seed(uint64_t seed);
bool coop_battle_link_mirror_commit(const CoopBattleMove *,bool native_ai);
bool coop_battle_link_mirror_setup(CoopBattleMirrorSetup *);
// Both sides report the same per-round state digest. A mismatch aborts the
// replay on both clients (false on the guest). Aborted replays stop yielding commands.
bool coop_battle_link_mirror_digest(uint32_t round,uint64_t digest);
bool coop_battle_link_mirror_take(uint64_t battle,unsigned trainer,uint32_t sequence,CoopBattleCanonical *);
void coop_battle_link_mirror_clear(void);
