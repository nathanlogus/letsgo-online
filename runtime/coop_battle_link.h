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
void coop_battle_link_reset(void);
void coop_battle_link_tick(void);
void coop_battle_link_party(const CoopParty *);
bool coop_battle_link_local_lead(uint8_t lead[COOP_PB7_BYTES]);
void coop_battle_link_view(CoopBattleLinkView *);
bool coop_battle_link_offer(uint64_t id, unsigned timeout_ms, unsigned trainer);
bool coop_battle_link_accept(void);
bool coop_battle_link_lead(uint8_t lead[COOP_PB7_BYTES], uint64_t *id);
bool coop_battle_link_request(const CoopAllyWait *, const uint16_t opponents[2], const uint16_t hp[2], const uint16_t max_hp[2]);
bool coop_battle_link_choose(unsigned move_index, unsigned target);
bool coop_battle_link_take(CoopBattleMove *, uint64_t *received_ns);
void coop_battle_link_complete(bool use_ai);
void coop_battle_link_end(void);
// Only the connected worker consumes the queue or supplies received payloads.
bool coop_battle_link_pop(uint8_t body[255], size_t *size);
void coop_battle_link_message(const uint8_t *body, size_t size);

// Experimental native battle presentation. All fields are copied values; no
// native pointers or complete saves cross this channel.
typedef struct {
    uint64_t battle,seed;
    unsigned trainer;
    bool host,available,ended,aborted;
    CoopParty host_party;
} CoopBattleMirrorSetup;
typedef struct {CoopBattleMove move;bool native_ai;} CoopBattleCanonical;
bool coop_battle_link_mirror_enabled(void);
bool coop_battle_link_mirror_party(const CoopParty *);
bool coop_battle_link_mirror_seed(uint64_t seed);
bool coop_battle_link_mirror_commit(const CoopBattleMove *,bool native_ai);
bool coop_battle_link_mirror_setup(CoopBattleMirrorSetup *);
bool coop_battle_link_mirror_take(uint64_t battle,unsigned trainer,uint32_t sequence,CoopBattleCanonical *);
void coop_battle_link_mirror_clear(void);
