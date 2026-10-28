// Opt-in battle prototype channel. The relay authenticates host/guest roles;
// the game thread validates the decision against the live native combatant.
#include "coop_battle_link.h"
#include "platform.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static CoopBattleLinkView view;
// The guest's whole party goes to the host (B2), so each player switches in only its own Pokemon.
static CoopParty local_party,peer_party;
static uint16_t chunks;
static bool have_party,accepted,pending;
static CoopBattleMove decision;
static uint64_t received_at;
static struct { uint8_t data[255];size_t size; } queue[16];
static unsigned first,count;
static CoopBattleMirrorSetup mirror;
static bool mirror_seed_ready,mirror_party_sent;
static uint16_t mirror_chunks;
static uint32_t mirror_received[2],mirror_published[2];
static struct {bool used;CoopBattleCanonical command;} mirror_commands[32];
// shortcut: 16 rounds of digests; a guest replay lagging further skips comparisons.
static struct {uint32_t round;uint64_t value[2];uint8_t have;} digests[16];
static uint32_t digest_sent;
static bool abort_unsent;
static unsigned link_rtt_ms;
static uint64_t pvp_consent; // guest: until then, a practice battle's offer is joined (coop_battle_link_pvp_consent)
static void clear_mirror(void){
    memset(&mirror,0,sizeof mirror);mirror_seed_ready=mirror_party_sent=abort_unsent=false;mirror_chunks=0;
    memset(mirror_received,0,sizeof mirror_received);memset(mirror_published,0,sizeof mirror_published);
    memset(mirror_commands,0,sizeof mirror_commands);memset(digests,0,sizeof digests);digest_sent=0;
}
// Called under the link mutex. notify queues one B10 so the peer stops too.
static void mirror_abort(uint8_t reason,bool notify){
    if(mirror.aborted)return;
    mirror.aborted=true;mirror.reason=reason;abort_unsent=notify && view.phase==COOP_LINK_ACTIVE;
}
static uint16_t u16(const uint8_t *p){return p[0]|(uint16_t)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return u16(p)|(uint32_t)u16(p+2)<<16;}
static uint64_t u64(const uint8_t *p){return u32(p)|(uint64_t)u32(p+4)<<32;}
static void p16(uint8_t *p,uint16_t x){p[0]=x;p[1]=x>>8;}
static void p32(uint8_t *p,uint32_t x){p16(p,x);p16(p+2,x>>16);}
static void p64(uint8_t *p,uint64_t x){p32(p,x);p32(p+4,x>>32);}
static bool enqueue(const uint8_t *p,size_t n){
    if(count==16 || n>255)return false;
    unsigned i=(first+count++)%16;memcpy(queue[i].data,p,n);queue[i].size=n;return true;
}
// On in normal play (Eevee 1.0.0 callers check their build); LARECOMP_COOP_GUEST_CONTROL=0 turns the channel off (QA).
bool coop_battle_link_enabled(void){const char *p=getenv("LARECOMP_COOP_GUEST_CONTROL");return !p || atoi(p)>0;}
// The "Team-up battles" option: off, the host never offers and the guest declines every offer.
static bool offers=true;
static uint64_t busy_since;
void coop_battle_link_set_offers(bool on){pthread_mutex_lock(&lock);offers=on;pthread_mutex_unlock(&lock);}
static void decline(uint64_t id){uint8_t p[10]={6};p64(p+1,id);p[9]=COOP_END_CANCELLED;enqueue(p,sizeof p);}
void coop_battle_link_guest_ready(bool ready){
    pthread_mutex_lock(&lock);uint64_t now=plat_now_ns();
    if(ready || view.host || view.phase!=COOP_LINK_OFFER)busy_since=0;
    else if(!busy_since)busy_since=now;
    // (0.5 s: a single frame without a field update also reads as busy)
    else if(now-busy_since>=500000000ull){decline(view.battle);memset(&view,0,sizeof view);busy_since=0;}
    pthread_mutex_unlock(&lock);
}
void coop_battle_link_reset(void){
    pthread_mutex_lock(&lock);memset(&view,0,sizeof view);
    // A guest replay still owns its native battle; keep it so the native side sees the lost link.
    if(mirror.available && !mirror.host){if(!mirror.ended)mirror_abort(COOP_MIRROR_LINK_LOST,false);abort_unsent=false;}
    else clear_mirror();
    chunks=0;accepted=pending=have_party=false;first=count=0;pvp_consent=0;
    pthread_mutex_unlock(&lock);
}
void coop_battle_link_tick(void){
    pthread_mutex_lock(&lock);
    if((view.phase==COOP_LINK_OFFER || view.phase==COOP_LINK_PREPARING) && plat_now_ns()>=view.deadline_ns){
        uint8_t p[10]={6};p64(p+1,view.battle);p[9]=COOP_END_EXPIRED;enqueue(p,sizeof p);
        memset(&view,0,sizeof view);chunks=0;accepted=pending=false;
    }
    pthread_mutex_unlock(&lock);
}
void coop_battle_link_party(const CoopParty *party){
    if(!coop_party_valid(party))return;
    pthread_mutex_lock(&lock);local_party=*party;have_party=true;view.party_ready=true;pthread_mutex_unlock(&lock);
}
bool coop_battle_link_local_party(CoopParty *out){
    pthread_mutex_lock(&lock);bool ok=have_party && accepted;
    if(ok)*out=local_party;pthread_mutex_unlock(&lock);return ok;
}
void coop_battle_link_view(CoopBattleLinkView *out){pthread_mutex_lock(&lock);*out=view;out->party_ready=have_party;pthread_mutex_unlock(&lock);}
bool coop_battle_link_pop(uint8_t body[255],size_t *n){
    pthread_mutex_lock(&lock);bool ok=count>0;
    if(abort_unsent){uint8_t p[10]={10};p64(p+1,mirror.battle);p[9]=mirror.reason;memcpy(body,p,sizeof p);*n=sizeof p;abort_unsent=false;ok=true;}
    else if(ok){*n=queue[first].size;memcpy(body,queue[first].data,*n);first=(first+1)%16;count--;}
    pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_offer(uint64_t id,unsigned timeout,unsigned trainer,unsigned hold){
    if(!hold)hold=45000;
    if(!coop_battle_link_enabled() || !id || timeout<1000 || timeout>60000 || ((!trainer || trainer>601) && !coop_battle_wild(trainer) && !coop_battle_pvp(trainer)) || hold<1000 || hold>45000)return false;
    // (a practice battle was asked for by both players: the "Team-up battles" option is not about it)
    pthread_mutex_lock(&lock);bool ok=(offers || coop_battle_pvp(trainer)) && view.phase==COOP_LINK_NONE;
    if(ok){uint8_t p[15]={0};p64(p+1,id);p16(p+9,timeout);p16(p+11,trainer);p16(p+13,hold);ok=enqueue(p,sizeof p);
        if(ok){clear_mirror();view=(CoopBattleLinkView){.phase=COOP_LINK_PREPARING,.host=true,.battle=id,.timeout_ms=timeout,.trainer=trainer,
            .deadline_ns=plat_now_ns()+hold*1000000ull};chunks=0;accepted=pending=false;}}
    pthread_mutex_unlock(&lock);return ok;
}
// Called under the link mutex.
static bool accept_offer(void){
    bool ok=view.phase==COOP_LINK_OFFER && have_party && count+1+2*local_party.count<=16 && plat_now_ns()<view.deadline_ns;
    if(ok){uint8_t p[141]={1};p64(p+1,view.battle);enqueue(p,9);
        p[0]=2;p[9]=local_party.count;
        for(unsigned i=0;i<2u*local_party.count;i++){p[10]=i;memcpy(p+11,(const uint8_t *)local_party.pokemon+130*i,130);enqueue(p,sizeof p);}
        view.phase=COOP_LINK_ACTIVE;view.waiting=false;accepted=true;}
    return ok;
}
bool coop_battle_link_accept(void){pthread_mutex_lock(&lock);bool ok=accept_offer();pthread_mutex_unlock(&lock);return ok;}
void coop_battle_link_pvp_consent(uint64_t until){pthread_mutex_lock(&lock);pvp_consent=until;pthread_mutex_unlock(&lock);}
bool coop_battle_link_guest_party(CoopParty *out,uint64_t *id){
    pthread_mutex_lock(&lock);
    bool ok=view.host && accepted && peer_party.count && chunks==(1u<<(2*peer_party.count))-1 && coop_party_valid(&peer_party);
    if(ok){*out=peer_party;*id=view.battle;view.phase=COOP_LINK_ACTIVE;}
    pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_lead(uint8_t lead[COOP_PB7_BYTES],uint64_t *id){
    CoopParty party;bool ok=coop_battle_link_guest_party(&party,id);
    if(ok)memcpy(lead,party.pokemon[0],COOP_PB7_BYTES);
    return ok;
}
bool coop_battle_link_request(const CoopAllyWait *w,const uint16_t opponents[2],const uint16_t hp[2],const uint16_t max_hp[2]){
    pthread_mutex_lock(&lock);bool ok=view.host && view.phase==COOP_LINK_ACTIVE && view.battle==w->battle;
    if(ok){uint8_t p[54]={3};p64(p+1,w->battle);p32(p+9,w->turn);p32(p+13,w->sequence);
        uint64_t now=plat_now_ns();unsigned ms=w->deadline_ns>now?(unsigned)((w->deadline_ns-now+999999)/1000000):1;if(ms>60000)ms=60000;
        p16(p+17,ms);p[19]=w->choices.party_index;
        for(unsigned i=0;i<4;i++){p16(p+20+2*i,w->choices.moves[i]);p[28+i]=w->choices.pp[i];p16(p+32+2*i,w->choices.targets[i]);}
        for(unsigned i=0;i<2;i++){p16(p+40+2*i,opponents[i]);p16(p+44+4*i,hp[i]);p16(p+46+4*i,max_hp[i]);}
        p[52]=w->choices.kind;p[53]=w->choices.switches;
        ok=enqueue(p,sizeof p);
        if(ok){view.waiting=true;view.submitted=false;view.turn=w->turn;view.sequence=w->sequence;view.deadline_ns=w->deadline_ns;
            view.choices=w->choices;pending=false;view.outcome=0;}}
    pthread_mutex_unlock(&lock);return ok;
}
static bool choose(unsigned index,unsigned target,unsigned switch_to){
    pthread_mutex_lock(&lock);bool ok=!view.host && view.phase==COOP_LINK_ACTIVE && view.waiting && !view.submitted &&
        plat_now_ns()<view.deadline_ns && (switch_to ? switch_to<=6 && (view.choices.switches&(1u<<(switch_to-1))) :
        index<4 && target<16 && view.choices.moves[index] && view.choices.pp[index] && (view.choices.targets[index]&(1u<<target)));
    if(ok){uint8_t p[23]={4};p64(p+1,view.battle);p32(p+9,view.turn);p32(p+13,view.sequence);
        p[17]=view.choices.party_index;if(!switch_to){p[18]=index;p16(p+19,view.choices.moves[index]);p[21]=target;}p[22]=switch_to;
        ok=enqueue(p,sizeof p);if(ok)view.submitted=true;}
    pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_choose(unsigned index,unsigned target){return choose(index,target,0);}
bool coop_battle_link_choose_switch(unsigned slot){return slot<6 && choose(0,0,slot+1);}
bool coop_battle_link_take(CoopBattleMove *m,uint64_t *at){pthread_mutex_lock(&lock);bool ok=pending;if(ok){*m=decision;*at=received_at;pending=false;}pthread_mutex_unlock(&lock);return ok;}
void coop_battle_link_complete(bool ai){
    pthread_mutex_lock(&lock);uint8_t p[18]={5};p64(p+1,view.battle);p32(p+9,view.turn);p32(p+13,view.sequence);p[17]=ai?2:1;
    if(view.host && view.phase==COOP_LINK_ACTIVE){
        enqueue(p,sizeof p);view.waiting=false;view.outcome=ai?2:1;view.toast_until_ns=plat_now_ns()+3000000000ull;
    }
    // A disconnect may have cleared the link while the native wait was open.
    // Its NPC recovery must not resurrect a toast/session on an offline view.
    pending=false;pthread_mutex_unlock(&lock);
}
void coop_battle_link_end(void){
    pthread_mutex_lock(&lock);
    if(view.phase!=COOP_LINK_NONE){
        uint8_t p[18]={6};p64(p+1,view.battle);p[9]=view.host && view.phase==COOP_LINK_ACTIVE?COOP_END_FINISHED:COOP_END_CANCELLED;
        // Final per-actor counts let the guest prove it received every command.
        if(view.host && mirror.available){p32(p+10,mirror_published[0]);p32(p+14,mirror_published[1]);enqueue(p,18);}
        else enqueue(p,10);
    }
    memset(&view,0,sizeof view);clear_mirror();pending=accepted=false;chunks=0;pthread_mutex_unlock(&lock);
}
// The guest replays the battle (native menu, partner avatar). LARECOMP_COOP_BATTLE_MIRROR=0: the overlay panel only (QA).
bool coop_battle_link_mirror_enabled(void){
    const char *p=getenv("LARECOMP_COOP_BATTLE_MIRROR");return coop_battle_link_enabled() && (!p || atoi(p)>0);
}
bool coop_battle_link_mirror_party(const CoopParty *party){
    if(!coop_battle_link_mirror_enabled() || !coop_party_valid(party))return false;
    pthread_mutex_lock(&lock);
    bool ok=view.host && view.phase==COOP_LINK_ACTIVE && !mirror.aborted;
    if(ok && !mirror_party_sent){
        unsigned needed=2*party->count;ok=count+needed<=16;
        if(ok){
            clear_mirror();mirror.battle=view.battle;mirror.host=true;mirror.trainer=view.trainer;mirror.host_party=*party;
            const uint8_t *bytes=(const uint8_t *)party->pokemon;
            for(unsigned i=0;i<needed;i++){
                uint8_t p[141]={7};p64(p+1,view.battle);p[9]=party->count;p[10]=i;memcpy(p+11,bytes+130*i,130);enqueue(p,sizeof p);
            }
            mirror_party_sent=true;
        }
    }
    pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_mirror_wild(const uint8_t pokemon[COOP_PB7_BYTES]){
    if(!coop_battle_link_mirror_enabled() || !coop_party_pokemon_valid(pokemon))return false;
    pthread_mutex_lock(&lock);
    bool ok=view.host && view.phase==COOP_LINK_ACTIVE && coop_battle_wild(view.trainer) && mirror_party_sent && !mirror_seed_ready && count+2<=16;
    for(unsigned i=0;ok && i<2;i++){
        uint8_t p[141]={7};p64(p+1,view.battle);p[9]=mirror.host_party.count;p[10]=12+i;memcpy(p+11,pokemon+130*i,130);enqueue(p,sizeof p);
    }
    if(ok)memcpy(mirror.wild,pokemon,COOP_PB7_BYTES);
    pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_mirror_seed(uint64_t seed,uint8_t own,uint8_t language){
    if(!coop_battle_link_mirror_enabled())return false;
    pthread_mutex_lock(&lock);bool ok=view.host && view.phase==COOP_LINK_ACTIVE && mirror_party_sent && !mirror_seed_ready;
    if(ok){uint8_t p[19]={8};p64(p+1,view.battle);p64(p+9,seed);p[17]=own;p[18]=language;ok=enqueue(p,sizeof p);
        if(ok){mirror.seed=seed;mirror.host_own=own;mirror.host_language=language;mirror_seed_ready=true;mirror.available=true;}}
    pthread_mutex_unlock(&lock);return ok;
}
// Every native decision (move, switch, item, replacement) travels as its command word; the guest's own
// engine executes it and the per-round digest catches a divergence.
// remote: the guest's trainer slot (coop_battle_remote_slot); actor 0 is the host's slot 0, actor 1 the guest's.
static bool canonical_valid(const CoopBattleMove *m,bool ai,unsigned remote){
    return m && m->battle && m->turn && m->sequence && (m->trainer==0 || m->trainer==remote) && m->party_index<6 &&
        (m->kind==2 || m->kind==4 || m->kind==5) && m->count<=10 && (!ai || m->trainer==remote);
}
bool coop_battle_link_mirror_commit(const CoopBattleMove *m,bool ai){
    if(!coop_battle_link_mirror_enabled())return false;
    pthread_mutex_lock(&lock);unsigned actor=m && m->trainer!=0;
    if(!canonical_valid(m,ai,coop_battle_remote_slot(view.trainer))){pthread_mutex_unlock(&lock);return false;}
    bool ok=view.host && view.phase==COOP_LINK_ACTIVE && mirror.available && !mirror.aborted &&
        m->battle==mirror.battle && mirror_published[actor]!=UINT32_MAX && m->sequence==mirror_published[actor]+1;
    if(ok){uint8_t p[30]={9};p64(p+1,m->battle);p32(p+9,m->turn);p32(p+13,m->sequence);
        p[17]=m->trainer;p[18]=m->party_index;p[19]=m->move_index;p16(p+20,m->move);p[22]=m->target;p[23]=ai;
        p[24]=m->kind;p[25]=m->count;p32(p+26,m->packet);
        ok=enqueue(p,sizeof p);if(ok)mirror_published[actor]=m->sequence;else mirror_abort(COOP_MIRROR_OVERFLOW,true);
    }
    pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_mirror_setup(CoopBattleMirrorSetup *out){
    if(!out)return false;pthread_mutex_lock(&lock);*out=mirror;bool ok=mirror.available;pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_mirror_take(uint64_t battle,unsigned trainer,uint32_t sequence,CoopBattleCanonical *out){
    if(!out || !battle || trainer>2)return false;
    pthread_mutex_lock(&lock);bool ok=false;
    if(!mirror.aborted)for(unsigned i=0;i<32;i++)if(mirror_commands[i].used && mirror_commands[i].command.move.battle==battle && mirror_commands[i].command.move.trainer==trainer &&
        mirror_commands[i].command.move.sequence==sequence){*out=mirror_commands[i].command;mirror_commands[i].used=false;ok=true;break;}
    pthread_mutex_unlock(&lock);return ok;
}
void coop_battle_link_mirror_clear(void){pthread_mutex_lock(&lock);clear_mirror();pthread_mutex_unlock(&lock);}
// side 0 is this guest's replay, 1 the host's authoritative value.
static void digest_record(unsigned side,uint32_t round,uint64_t value){
    unsigned i=round%16;
    if(digests[i].round!=round){memset(&digests[i],0,sizeof digests[i]);digests[i].round=round;}
    digests[i].value[side]=value;digests[i].have|=1u<<side;
    if(digests[i].have==3 && digests[i].value[0]!=digests[i].value[1])mirror_abort(COOP_MIRROR_DIGEST,true);
}
bool coop_battle_link_mirror_digest(uint32_t round,uint64_t digest){
    if(!coop_battle_link_mirror_enabled() || !round)return false;
    pthread_mutex_lock(&lock);bool ok=mirror.available && !mirror.aborted;
    if(ok && mirror.host){
        ok=view.phase==COOP_LINK_ACTIVE && round>digest_sent;
        if(ok){uint8_t p[21]={11};p64(p+1,mirror.battle);p32(p+9,round);p64(p+13,digest);ok=enqueue(p,sizeof p);if(ok)digest_sent=round;}
    } else if(ok){digest_record(0,round,digest);ok=!mirror.aborted;}
    pthread_mutex_unlock(&lock);return ok;
}
bool coop_battle_link_mirror_matched(uint32_t round){
    pthread_mutex_lock(&lock);unsigned i=round%16;bool ok=round && !mirror.aborted && digests[i].round==round && digests[i].have==3;
    pthread_mutex_unlock(&lock);return ok;
}
void coop_battle_link_latency(unsigned rtt_ms){pthread_mutex_lock(&lock);link_rtt_ms=rtt_ms>60000?60000:rtt_ms;pthread_mutex_unlock(&lock);}
// Called under the link mutex, only for an accepted current battle.
static void mirror_message(const uint8_t *p,size_t n,uint64_t id){
    if(view.host || view.phase!=COOP_LINK_ACTIVE || !accepted)return;
    // (chunks 12 and 13: a wild team-up's wild Pokemon)
    bool wild=coop_battle_wild(view.trainer);
    if(p[0]==7 && n==141 && p[9]>=1 && p[9]<=6 && (p[10]<2*p[9] || (wild && (p[10]==12 || p[10]==13)))){
        if(!mirror.battle){mirror.battle=id;mirror.trainer=view.trainer;}
        if(!mirror.host_party.count)mirror.host_party.count=p[9];
        if(mirror.battle!=id || mirror.host_party.count!=p[9] || (mirror_chunks&(1u<<p[10])))return;
        if(p[10]>=12)memcpy(mirror.wild+130*(p[10]-12),p+11,130);
        else memcpy((uint8_t *)mirror.host_party.pokemon+130*p[10],p+11,130);
        mirror_chunks|=1u<<p[10];
        if(mirror_chunks==(((1u<<(2*p[9]))-1)|(wild?0x3000u:0))){
            if(!coop_party_valid(&mirror.host_party) || (wild && !coop_party_pokemon_valid(mirror.wild))){mirror_abort(COOP_MIRROR_BAD_PARTY,true);return;}
            mirror_party_sent=true;mirror.available=mirror_seed_ready;
        }
    } else if(p[0]==8 && n==19 && !mirror_seed_ready){
        if(!mirror.battle){mirror.battle=id;mirror.trainer=view.trainer;}
        if(mirror.battle!=id)return;
        mirror.seed=u64(p+9);mirror.host_own=p[17];mirror.host_language=p[18];mirror_seed_ready=true;mirror.available=mirror_party_sent;
    } else if(p[0]==9 && n==30 && mirror.available && !mirror.aborted){
        CoopBattleCanonical command={.move={.battle=id,.turn=u32(p+9),.sequence=u32(p+13),.trainer=p[17],.party_index=p[18],
            .move_index=p[19],.move=u16(p+20),.target=p[22],.kind=p[24],.count=p[25],.packet=u32(p+26)},.native_ai=p[23]!=0};
        if(p[23]>1 || !canonical_valid(&command.move,command.native_ai,coop_battle_remote_slot(view.trainer)))return;
        unsigned actor=command.move.trainer!=0;
        if(command.move.sequence<=mirror_received[actor])return;
        if(mirror_received[actor]==UINT32_MAX || command.move.sequence!=mirror_received[actor]+1){mirror_abort(COOP_MIRROR_GAP,true);return;}
        for(unsigned i=0;i<32;i++)if(!mirror_commands[i].used){
            mirror_commands[i].used=true;mirror_commands[i].command=command;mirror_received[actor]=command.move.sequence;return;
        }
        mirror_abort(COOP_MIRROR_OVERFLOW,true);
    } else if(p[0]==11 && n==21 && mirror.available && !mirror.aborted && u32(p+9))digest_record(1,u32(p+9),u64(p+13));
}
void coop_battle_link_message(const uint8_t *p,size_t n){
    if(!coop_battle_link_enabled() || n<9)return;
    uint64_t id=u64(p+1),now=plat_now_ns();if(!id)return;
    pthread_mutex_lock(&lock);
    // A finished replay may still be draining; a new offer must not wipe it.
    bool pvp=p[0]==0 && n==15 && coop_battle_pvp(u16(p+11));
    if(p[0]==0 && n==15 && view.phase==COOP_LINK_NONE && (pvp?now>=pvp_consent:!offers))decline(id);
    else if(p[0]==0 && n==15 && view.phase==COOP_LINK_NONE && u16(p+9)>=1000 && u16(p+9)<=60000 && u16(p+13)>=1000 && u16(p+13)<=45000 &&
        !(mirror.available && !mirror.host && !mirror.aborted)){
        // The host's hold (a trainer's held text, 10 s) started a one-way trip ago; an answer needs another to arrive.
        unsigned ms=u16(p+13),margin=2*link_rtt_ms;if(margin>ms/4)margin=ms/4;
        clear_mirror();busy_since=0;view=(CoopBattleLinkView){.phase=COOP_LINK_OFFER,.battle=id,.timeout_ms=u16(p+9),.trainer=u16(p+11),.deadline_ns=now+(uint64_t)(ms-margin)*1000000};
        // Both players already agreed to this practice battle: join at once (no prompt), or decline without a party.
        if(pvp){pvp_consent=0;if(!accept_offer()){decline(id);memset(&view,0,sizeof view);}}
    } else if(id==view.battle && view.phase!=COOP_LINK_NONE){
        if(p[0]==1 && n==9 && view.host && view.phase==COOP_LINK_PREPARING)accepted=true;
        else if(p[0]==2 && n==141 && view.host && accepted && p[9]>=1 && p[9]<=6 && p[10]<2*p[9] && (!chunks || peer_party.count==p[9]) && !(chunks&(1u<<p[10]))){
            peer_party.count=p[9];memcpy((uint8_t *)peer_party.pokemon+130*p[10],p+11,130);chunks|=1u<<p[10];}
        else if(p[0]==3 && n==54 && !view.host && accepted && u32(p+9)>view.turn && u32(p+13)>view.sequence && u16(p+17)>0 && u16(p+17)<=60000 && p[19]<6){
            // The host's deadline started a one-way trip ago and the choice needs another to return.
            unsigned ms=u16(p+17),margin=2*link_rtt_ms;if(margin>ms/4)margin=ms/4;
            view.turn=u32(p+9);view.sequence=u32(p+13);view.deadline_ns=now+(uint64_t)(ms-margin)*1000000;
            view.choices.party_index=p[19];for(unsigned i=0;i<4;i++){view.choices.moves[i]=u16(p+20+2*i);view.choices.pp[i]=p[28+i];view.choices.targets[i]=u16(p+32+2*i);}
            for(unsigned i=0;i<2;i++){view.opponents[i]=u16(p+40+2*i);view.hp[i]=u16(p+44+4*i);view.max_hp[i]=u16(p+46+4*i);}
            view.choices.kind=p[52];view.choices.switches=p[53];
            view.waiting=true;view.submitted=false;view.outcome=0;
        } else if(p[0]==4 && n==23 && view.host && view.waiting && !pending && !view.submitted && u32(p+9)==view.turn && u32(p+13)==view.sequence && p[18]<4 && p[21]<16 && p[22]<=6){
            decision=(CoopBattleMove){.battle=id,.turn=u32(p+9),.sequence=u32(p+13),.trainer=2,.party_index=p[17],.move_index=p[18],.move=u16(p+19),.target=p[21],.switch_to=p[22]};
            received_at=now;pending=view.submitted=true;
        } else if(p[0]==5 && n==18 && !view.host && u32(p+9)==view.turn && u32(p+13)==view.sequence && (p[17]==1 || p[17]==2)){
            view.waiting=false;view.submitted=false;view.outcome=p[17];view.toast_until_ns=now+3000000000ull;
        } else if(p[0]==6 && (n==10 || n==18)){
            if(mirror.battle==id && mirror.available && !mirror.host){
                if(n==18 && p[9]==COOP_END_FINISHED && u32(p+10)==mirror_received[0] && u32(p+14)==mirror_received[1])mirror.ended=!mirror.aborted;
                else mirror_abort(COOP_MIRROR_INCOMPLETE_END,false);
            } else clear_mirror();
            memset(&view,0,sizeof view);accepted=pending=false;chunks=0;
        } else if(p[0]==10 && n==10 && mirror.battle==id){
            if(!mirror.aborted){mirror.aborted=true;mirror.reason=p[9];}
        } else if(coop_battle_link_mirror_enabled())mirror_message(p,n,id);
    }
    pthread_mutex_unlock(&lock);
}
