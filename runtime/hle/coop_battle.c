// Eevee 1.0.0 battle preflight. Native allocation/accessor calls run only on the
// game thread. The temporary Pokemon never enter the host's party or storage.
#include <stdio.h>
#include <string.h>
#include "../hle.h"
#include "../gamesetup.h"
#include "../fnhook.h"
#include "coop_battle.h"
#include "../coop_net.h"
#include "../coop_battle_link.h"
#include "../coop_battle_names.h"
#include "../platform.h"
void rt_call_original(Cpu *,uint64_t);
uint64_t renderer_game_frame(void);
static bool supported,exported,imported,late_exported;
static bool trainer_started,battle_seen,trainer_offer_sent,trial_active,mirrored_guest,remote_waiting;
static CoopBattleMirrorSetup native_mirror;
static uint32_t remote_round;
static uint64_t trainer_process,world_return_frame,player_clone_party,saved_party_buffers[6];
static RuntimeFnHook previous_command,previous_trainer_party,previous_battle_party,previous_skill_pick,previous_move_result,previous_target_result,previous_ally_select,previous_npc_commit,previous_context,previous_rng_seed,previous_after_battle,previous_spirit,previous_remote_player,previous_battle_frame,previous_human_select,previous_msg;
static bool menu_open;
#define WORLD_GOT 0x70015fad08ull
#define ALLOC_GOT 0x70015faab8ull
#define COUNT     0x7000721b20ull
#define SERIALIZE 0x7000729650ull
#define IMPORT    0x7000729660ull
#define CONSTRUCT 0x7000729340ull
#define DESTROY   0x7000724790ull
#define BEGIN    0x7000728e90ull
#define END      0x7000728ee0ull
#define SPECIES  0x7000724950ull
#define COMMAND  0x7000ab1e80ull
#define OWNER    0x7000ab1670ull
static void battle_link_test_tick(void){
    CoopBattleLinkView view;coop_battle_link_view(&view);
        const char *state_path=ENV("LARECOMP_COOP_BATTLE_STATE_FILE");
        if(state_path){
            static unsigned last=~0u;unsigned signature=(unsigned)view.phase|(view.waiting?8:0)|(view.submitted?16:0)|(view.turn<<8);
            if(signature!=last){last=signature;FILE *f=fopen(state_path,"w");if(f){fprintf(f,"{\"phase\":%u,\"host\":%u,\"waiting\":%u,\"submitted\":%u,\"turn\":%u}\n",view.phase,view.host,view.waiting,view.submitted,view.turn);fclose(f);}}
        }
        if(!view.host && view.phase==COOP_LINK_OFFER && ENV("LARECOMP_COOP_BATTLE_AUTO_ACCEPT"))coop_battle_link_accept();
        if(!view.host && view.waiting && !view.submitted){
            const char *choice=ENV("LARECOMP_COOP_BATTLE_AUTO_MOVE"),*skip=ENV("LARECOMP_COOP_BATTLE_SKIP_TURN");
            const char *delay=ENV("LARECOMP_COOP_BATTLE_AUTO_DELAY_MS");
            uint64_t left=view.deadline_ns>plat_now_ns()?view.deadline_ns-plat_now_ns():0;
            bool due=!delay || left+(uint64_t)strtoul(delay,NULL,10)*1000000 <= (uint64_t)view.timeout_ms*1000000;
            if(choice && due && (!skip || strtoul(skip,NULL,10)!=view.turn)){
                unsigned i=(unsigned)strtoul(choice,NULL,10);unsigned t=0;
                while(t<16 && !(view.choices.targets[i<4?i:0]&(1u<<t)))t++;
                if(i<4 && t<16 && coop_battle_link_choose(i,t))LOG("[coop-ally] QA guest chose slot %u target %u turn %u\n",i,t,view.turn);
            }
        }
}
static int mirror_frame_hook(Cpu *c,uint64_t addr){
    if(supported && trial_active && mirrored_guest){
        battle_link_test_tick();
        extern void input_press_a_at(uint64_t frame);
        static uint64_t last;uint64_t frame=renderer_game_frame();
        // (not while the guest's own Fight menu is open: the player chooses there)
        if(!menu_open && (frame<last || frame>=last+12)){last=frame;input_press_a_at(frame+1);}
    }
    return previous_battle_frame?previous_battle_frame(c,addr):0;
}
static bool guest(uint64_t p,size_t n) {return p>=GUEST_LO && p<GUEST_HI && n<=GUEST_HI-p;}
static uint64_t ptr(uint64_t p){return guest(p,8) ? *(const uint64_t *)(uintptr_t)p : 0;}
static uint64_t manager(void){return ptr(ptr(ptr(WORLD_GOT))+0x98);}
static uint64_t allocator(void){return ptr(ptr(ptr(ALLOC_GOT))+0x68);}
static void serialize(Cpu *c,uint64_t mon,uint8_t data[COOP_PB7_BYTES]) {
    c->x[0]=mon;rt_call_original(c,BEGIN);uint64_t changed=c->x[0]&1;
    c->x[0]=mon;c->x[1]=(uint64_t)(uintptr_t)data;rt_call_original(c,SERIALIZE);
    c->x[0]=mon;c->x[1]=changed;rt_call_original(c,END);
}
static bool capture_native_party(Cpu *c,uint64_t party,CoopParty *out) {
    if(!c || !out)return false;
    memset(out,0,sizeof *out);Cpu call=*c;
    uint64_t vtable=ptr(party),at=ptr(vtable+0x58);
    if(!guest(party,0x90) || !guest(at,4))return false;
    call.x[0]=party;rt_call_original(&call,COUNT);unsigned count=(unsigned)call.x[0];
    if(!count || count>COOP_PARTY_MAX)return false;
    for(unsigned i=0;i<count;i++) {
        call.x[0]=party;call.x[1]=i;rt_call_original(&call,at);uint64_t mon=call.x[0];
        if(!mon)continue;
        if(!guest(mon,0xb8))return false;
        call.x[0]=mon;rt_call_original(&call,SPECIES);
        // Party storage can expose empty slots. Send occupied slots only,
        // retaining their native order.
        if(!call.x[0])continue;
        serialize(&call,mon,out->pokemon[out->count++]);
    }
    return coop_party_valid(out);
}
bool coop_battle_capture_party(Cpu *c,CoopParty *out) {
    return supported && capture_native_party(c,ptr(manager()+0x60),out);
}
bool coop_battle_check_import(Cpu *c,const CoopParty *party,uint16_t species[COOP_PARTY_MAX]) {
    if(!supported || !c || !species || !coop_party_valid(party))return false;
    uint64_t alloc=allocator();if(!guest(alloc,8))return false;
    Cpu call=*c;bool ok=true;
    memset(species,0,COOP_PARTY_MAX*sizeof *species);
    for(unsigned i=0;i<party->count;i++) {
        // The game constructor allocates its core/calculated buffers. Only the
        // outer object is scoped host memory; its destructor frees those buffers.
        uint64_t object[0xb8/8]={0};uint64_t mon=(uint64_t)(uintptr_t)object;
        call.x[0]=mon;call.x[1]=alloc;rt_call_original(&call,CONSTRUCT);
        call.x[0]=mon;call.x[1]=(uint64_t)(uintptr_t)party->pokemon[i];rt_call_original(&call,IMPORT);
        call.x[0]=mon;rt_call_original(&call,SPECIES);species[i]=(uint16_t)call.x[0];
        uint8_t roundtrip[COOP_PB7_BYTES];serialize(&call,mon,roundtrip);
        ok &= species[i]>=1 && species[i]<=809 && !memcmp(roundtrip,party->pokemon[i],sizeof roundtrip);
        call.x[0]=mon;rt_call_original(&call,DESTROY);
        if(!ok)break;
    }
    return ok;
}
// Build a battle-owned native party rather than borrowing SaveData's party.
// Match the game's allocator metadata and shared reference ownership exactly.
static void remember_saved_party_buffers(Cpu *cpu){
    memset(saved_party_buffers,0,sizeof saved_party_buffers);
    uint64_t party=ptr(manager()+0x60),at=ptr(ptr(party)+0x58);
    if(!guest(party,0x90) || !guest(at,4))return;
    Cpu call=*cpu;call.x[0]=party;rt_call_original(&call,COUNT);unsigned count=(unsigned)call.x[0];
    for(unsigned i=0;i<count && i<6;i++){
        call.x[0]=party;call.x[1]=i;rt_call_original(&call,at);
        if(guest(call.x[0],0xb8))saved_party_buffers[i]=ptr(ptr(call.x[0]+0x98)+0x10);
    }
}
static uint64_t make_party_clone(Cpu *cpu,const CoopParty *source){
    if(!coop_party_valid(source))return 0;
    uint64_t alloc=allocator(),allocate=ptr(ptr(alloc)+0x10);
    if(!guest(alloc,8) || !guest(allocate,4))return 0;
    Cpu call=*cpu;call.x[0]=alloc;call.x[1]=0x90;call.x[2]=8;rt_call_original(&call,allocate);
    uint64_t party=call.x[0];if(!guest(party,0x90))return 0;
    call.x[0]=party;call.x[1]=alloc;rt_call_original(&call,0x7000720f10ull);
    uint64_t *metadata=(uint64_t *)(uintptr_t)(party+8);
    metadata[0]=alloc;metadata[1]=party;metadata[2]=0x90;metadata[3]=8;
    call.x[0]=party+0x48;rt_call_original(&call,ptr(ptr(party+0x48)+0x10));
    for(unsigned i=0;i<source->count;i++){
        call.x[0]=party;call.x[1]=i;rt_call_original(&call,0x7000721b00ull);
        uint64_t mon=call.x[0];call.x[0]=mon;call.x[1]=(uint64_t)(uintptr_t)source->pokemon[i];rt_call_original(&call,IMPORT);
    }
    // The constructor creates six empty slots. Publish their imported count
    // only after every Pokemon is initialized; selection remains slot zero.
    *(uint8_t *)(uintptr_t)(party+0x88)=source->count;
    return party;
}
static void release_party_clone(Cpu *cpu,uint64_t party){
    Cpu call=*cpu;call.x[0]=party+0x48;rt_call_original(&call,ptr(ptr(party+0x48)+0x18));
}
static bool check_party_clone(Cpu *cpu,const CoopParty *source){
    uint64_t party=make_party_clone(cpu,source);if(!party)return false;
    Cpu call=*cpu;bool exact=true;
    call.x[0]=party;rt_call_original(&call,COUNT);exact=call.x[0]==source->count;
    for(unsigned i=0;i<source->count;i++){
        call.x[0]=party;call.x[1]=i;rt_call_original(&call,0x7000721b00ull);
        uint8_t wire[COOP_PB7_BYTES];serialize(&call,call.x[0],wire);
        exact &= !memcmp(wire,source->pokemon[i],sizeof wire);
    }
    release_party_clone(cpu,party);return exact;
}
static bool save_party(const char *dir,const char *name,const CoopParty *party) {
    char path[1400];snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE *f=fopen(path,"wb");if(!f)return false;
    bool ok=fwrite("LGPTY001",1,8,f)==8 && fwrite(&party->count,1,1,f)==1 &&
        fwrite(party->pokemon,COOP_PB7_BYTES,party->count,f)==party->count;
    return fclose(f)==0 && ok;
}
static bool load_party(const char *path,CoopParty *party) {
    FILE *f=fopen(path,"rb");if(!f)return false;
    char magic[8];memset(party,0,sizeof *party);
    bool ok=fread(magic,1,8,f)==8 && !memcmp(magic,"LGPTY001",8) && fread(&party->count,1,1,f)==1 &&
        party->count>=1 && party->count<=COOP_PARTY_MAX &&
        fread(party->pokemon,COOP_PB7_BYTES,party->count,f)==party->count && fgetc(f)==EOF;
    fclose(f);return ok && coop_party_valid(party);
}
extern uint64_t g_savedata_guest;
// QA: dump the whole in-memory save so a script can diff every block (money, items, flags, EXP).
static void dump_save(const char *name){
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");
    if(!dir || !*dir || !ENV("LARECOMP_COOP_SAVE_DUMP") || !guest(g_savedata_guest,1048576))return;
    char path[1400];snprintf(path,sizeof path,"%s/%s",dir,name);FILE *f=fopen(path,"wb");
    if(f){fwrite((const void *)(uintptr_t)g_savedata_guest,1,1048576,f);fclose(f);}
}
static void journal(const char *event,uint64_t frame,const uint32_t *choice) {
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");if(!dir || !*dir)return;
    char path[1400];snprintf(path,sizeof path,"%s/battle.jsonl",dir);FILE *f=fopen(path,"a");if(!f)return;
    fprintf(f,"{\"event\":\"%s\",\"frame\":%llu",event,(unsigned long long)frame);
    if(choice)fprintf(f,",\"menu_choice\":%u",*choice);
    fprintf(f,"}\n");fclose(f);
}
static bool dir_set(void){const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");return dir && *dir;}
static int command_hook(Cpu *c,uint64_t addr) {
    if(!supported)return previous_command ? previous_command(c,addr) : 0;
    uint64_t frame=c->x[0];
    if(!previous_command || !previous_command(c,addr))rt_call_original(c,addr);
    Cpu keep=*c;c->x[0]=frame;rt_call_original(c,OWNER);uint64_t owner=c->x[0];*c=keep;
    // This UI action can be destroyed when the Fight submenu opens. Copy
    // its value now; retaining its pointer across presents reads recycled memory.
    uint64_t choice=ptr(owner+0x78);uint32_t value=0;
    if(guest(choice,0x14))memcpy(&value,(const void *)(uintptr_t)(choice+0x10),sizeof value);
    if(player_clone_party){CoopParty current;const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");
        if(dir && coop_battle_capture_party(c,&current))save_party(dir,"party-at-command.bin",&current);}
    // (an online solo battle is none of this file's business: only team-ups and QA observe it)
    if(trial_active || dir_set())battle_seen=true;
    journal("top_level_choice",renderer_game_frame(),&value);return 1;
}
static uint8_t selected_move_slot[24];
static uint16_t selected_move_id[24];
static uint32_t observed_sequence;
static void journal_move(const char *event,unsigned actor,uint32_t packet) {
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");if(!dir || !*dir)return;
    char path[1400];snprintf(path,sizeof path,"%s/battle.jsonl",dir);FILE *f=fopen(path,"a");if(!f)return;
    unsigned slot=actor<24 && selected_move_id[actor]==((packet>>8)&0xffff) ? selected_move_slot[actor] : 255;
    fprintf(f,"{\"event\":\"%s\",\"frame\":%llu,\"actor\":%u,\"move_slot\":%u,\"move\":%u,\"target\":%u,\"native_packet\":%u,\"observed_sequence\":%u}\n",
        event,(unsigned long long)renderer_game_frame(),actor,slot,(packet>>8)&0xffff,(packet>>4)&15,packet,observed_sequence);
    fclose(f);
}
static int skill_pick_hook(Cpu *c,uint64_t addr) {
    uint64_t view=c->x[0];unsigned slot=(unsigned)c->x[1];
    if(!previous_skill_pick || !previous_skill_pick(c,addr))rt_call_original(c,addr);
    if(supported && trainer_started && slot<4) {
        Cpu call=*c;call.x[0]=view;rt_call_original(&call,0x7000aca530ull);
        uint64_t result=ptr(call.x[0]+0x78);
        if(guest(result,0x2c) && *(uint32_t *)(uintptr_t)(result+0x20)==1) {
            unsigned actor=*(uint8_t *)(uintptr_t)(result+0x18);
            if(actor<24){selected_move_slot[actor]=slot;selected_move_id[actor]=*(uint32_t *)(uintptr_t)(result+0x28);}
        }
    }
    return 1;
}
static int move_result_hook(Cpu *c,uint64_t addr) {
    uint64_t root=c->x[0];
    unsigned actor=guest(root,0xb1) ? *(uint8_t *)(uintptr_t)(root+0xb0) : 255;
    bool cancelled=guest(root,0xbc) && *(uint32_t *)(uintptr_t)(root+0xb8)==2;
    if(!previous_move_result || !previous_move_result(c,addr))rt_call_original(c,addr);
    if(supported && trainer_started && cancelled) {
        journal_move("move_cancelled",actor,0);
        if(actor<24){selected_move_slot[actor]=255;selected_move_id[actor]=0;}
    }
    return 1;
}
static int target_result_hook(Cpu *c,uint64_t addr) {
    uint64_t root=c->x[0],packet=ptr(root+0x320);
    unsigned actor=guest(root,0xb1) ? *(uint8_t *)(uintptr_t)(root+0xb0) : 255;
    if(!previous_target_result || !previous_target_result(c,addr))rt_call_original(c,addr);
    if(supported && trainer_started) {
        if((uint32_t)c->x[0]==1 && guest(packet,4)) {
            uint32_t value=*(uint32_t *)(uintptr_t)packet;
            if((value&15)==1) {
                // The native result getter has now committed both the move and
                // mapped target. Copy the value; no UI pointers leave this hook.
                bool mapped=actor<24 && selected_move_slot[actor]<4 && selected_move_id[actor]==((value>>8)&0xffff);
                // (journal only: the host's slot-0 decision is published when its handler completes, human_select_hook)
                if(mapped)observed_sequence++;
                journal_move(mapped ? "move_committed" : "move_unmapped",actor,value);
                if(actor<24){selected_move_slot[actor]=255;selected_move_id[actor]=0;}
            }
        } else if((uint32_t)c->x[0]==2) {
            journal_move("target_cancelled",actor,0);
            if(actor<24){selected_move_slot[actor]=255;selected_move_id[actor]=0;}
        }
    }
    return 1;
}
static CoopAllyWait ally_wait;
static uint32_t ally_round;
static bool ally_prepared,ally_native,ally_publish_ai;
// The context owns a native xoroshiro RNG at +0x90. Limit seed probing to
// initialization inside this game-thread context callback, never world/AI RNGs.
static _Thread_local uint64_t initializing_context;
static int context_update_hook(Cpu *c,uint64_t addr){
    uint64_t prior=initializing_context;
    initializing_context=supported && trial_active ? c->x[0]:0;
    if(!previous_context || !previous_context(c,addr))rt_call_original(c,addr);
    initializing_context=prior;return 1;
}
static int rng_seed_hook(Cpu *c,uint64_t addr){
    uint64_t object=c->x[0];
    bool trace=initializing_context && object==initializing_context+0x90 && guest(object,0x20);
    const char *seed=trace?ENV("LARECOMP_COOP_BATTLE_SEED"):NULL;
    if(trace && mirrored_guest){c->x[1]=native_mirror.seed;rt_call_original(c,0x70009e8a80ull);}
    else if(seed && *seed){
        // QA only: call the native explicit-seed initializer rather than
        // replacing the RNG algorithm or seeding unrelated game systems.
        c->x[1]=strtoull(seed,NULL,0);rt_call_original(c,0x70009e8a80ull);
    } else if(!previous_rng_seed || !previous_rng_seed(c,addr))rt_call_original(c,addr);
    if(trace){
        if(!mirrored_guest)coop_battle_link_mirror_seed(ptr(object+0x18));
        const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");if(!dir)return 1;char path[1400];
        snprintf(path,sizeof path,"%s/battle.jsonl",dir);FILE *f=fopen(path,"a");
        if(f){fprintf(f,"{\"event\":\"battle_seed\",\"frame\":%llu,\"seed\":\"%016llx\",\"forced\":%s}\n",
            (unsigned long long)renderer_game_frame(),(unsigned long long)ptr(object+0x18),seed && *seed?"true":"false");fclose(f);}
    }
    return 1;
}
static void hash_field(uint64_t *digest,uint64_t value,unsigned bytes){
    for(unsigned i=0;i<bytes;i++){*digest=(*digest^(value&255))*0x100000001b3ull;value>>=8;}
}
// Pointer-free per-round fingerprint, compared host vs guest through the link.
// Status words (+0xa0..+0xc7) are hashed explicitly; the rest of the battle
// Pokemon (stat stages, volatile counters) is hashed as raw bytes, skipping
// 8-byte slots that hold guest pointers, since heap addresses differ per client.
static uint64_t journal_turn_state(Cpu *cpu,uint64_t controller){
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");
    uint64_t context=ptr(controller+0x58),pool=ptr(controller+0x70);
    if(!guest(context,0xb0) || !guest(pool,0x140))return 0;
    FILE *f=NULL;if(dir && *dir){char path[1400];snprintf(path,sizeof path,"%s/battle.jsonl",dir);f=fopen(path,"a");}
    uint64_t rng[2]={ptr(context+0x98),ptr(context+0xa0)},digest=0xcbf29ce484222645ull;
    hash_field(&digest,rng[0],8);hash_field(&digest,rng[1],8);uint64_t partial=digest;
    if(f)fprintf(f,"{\"event\":\"turn_state\",\"turn\":%u,\"rng\":[\"%016llx\",\"%016llx\"],\"combatants\":[",ally_round,
        (unsigned long long)rng[0],(unsigned long long)rng[1]);
    Cpu call=*cpu;
    for(unsigned trainer=0;trainer<4;trainer++){
        uint16_t species=0,hp=0,maxhp=0,moves[4]={0};uint8_t pp[4]={0},status[5]={0};uint64_t raw=0xcbf29ce484222645ull;
        if(*(uint8_t *)(uintptr_t)(pool+0x60+0x38*trainer+0x30)){
            call.x[0]=pool;call.x[1]=trainer;call.x[2]=0;rt_call_original(&call,0x70009e14c0ull);uint64_t mon=call.x[0];
            if(guest(mon,0x2e1)){
                species=*(uint16_t *)(uintptr_t)(mon+0x78);hp=*(uint16_t *)(uintptr_t)(mon+0x7e);maxhp=*(uint16_t *)(uintptr_t)(mon+0x7c);
                for(unsigned i=0;i<5;i++)status[i]=*(uint8_t *)(uintptr_t)(mon+0xa0+8*i)&7;
                for(unsigned i=0;i<4;i++){
                    call.x[0]=mon;call.x[1]=i;rt_call_original(&call,0x70009e4950ull);moves[i]=(uint16_t)call.x[0];
                    call.x[0]=mon;call.x[1]=i;rt_call_original(&call,0x70009e4a80ull);pp[i]=(uint8_t)call.x[0];
                }
                // (+0x218 skipped: the same seeded battle shows bit 3 there or not from run to run on one engine,
                // likely a controller running in parallel with this snapshot; it aborted otherwise identical replays.)
                // LARECOMP_COOP_TURN_RAW=1 writes turn-raw.txt in the QA directory; =<file> writes there (no QA directory needed)
                const char *rawp=ENV("LARECOMP_COOP_TURN_RAW");char rp[1400]={0};
                if(rawp && dir && *dir)snprintf(rp,sizeof rp,"%s/turn-raw.txt",dir);else if(rawp && strlen(rawp)>1)snprintf(rp,sizeof rp,"%s",rawp);
                FILE *r=rp[0]?fopen(rp,"a"):NULL;
                for(unsigned o=0x78;o<0x2e0;o+=8){uint64_t v=*(uint64_t *)(uintptr_t)(mon+o);if(!guest(v,1) && o!=0x218)hash_field(&raw,v,8);
                    if(r)fprintf(r,"%u %u %03x %016llx%s\n",ally_round,trainer,o,(unsigned long long)(guest(v,1)?0:v),guest(v,1)?" ptr":"");}
                if(r)fclose(r);
            }
        }
        hash_field(&partial,trainer,1);hash_field(&partial,species,2);hash_field(&partial,hp,2);hash_field(&partial,maxhp,2);
        for(unsigned i=0;i<4;i++){hash_field(&partial,moves[i],2);hash_field(&partial,pp[i],1);}
        hash_field(&digest,trainer,1);hash_field(&digest,species,2);hash_field(&digest,hp,2);hash_field(&digest,maxhp,2);
        for(unsigned i=0;i<4;i++){hash_field(&digest,moves[i],2);hash_field(&digest,pp[i],1);}
        for(unsigned i=0;i<5;i++)hash_field(&digest,status[i],1);
        hash_field(&digest,raw,8);
        if(f)fprintf(f,"%s{\"trainer\":%u,\"species\":%u,\"hp\":%u,\"max_hp\":%u,\"moves\":[%u,%u,%u,%u],\"pp\":[%u,%u,%u,%u],\"status\":[%u,%u,%u,%u,%u],\"raw\":\"%016llx\"}",trainer?",":"",trainer,species,hp,maxhp,
            moves[0],moves[1],moves[2],moves[3],pp[0],pp[1],pp[2],pp[3],status[0],status[1],status[2],status[3],status[4],(unsigned long long)raw);
    }
    // QA fault injection: corrupt the guest's report for one round to exercise the abort path.
    const char *corrupt=ENV("LARECOMP_COOP_MIRROR_CORRUPT_ROUND");
    if(mirrored_guest && corrupt && strtoul(corrupt,NULL,10)==ally_round)digest^=1;
    if(f){fprintf(f,"],\"partial_digest\":\"%016llx\",\"digest\":\"%016llx\"}\n",(unsigned long long)partial,(unsigned long long)digest);fclose(f);}
    LOG("[coop-battle] round %u digest %016llx (rng %016llx %016llx)\n",ally_round,(unsigned long long)digest,(unsigned long long)rng[0],(unsigned long long)rng[1]);
    return digest;
}
static unsigned ally_timeout(void){const char *p=ENV("LARECOMP_COOP_ALLY_TIMEOUT_SECONDS");unsigned n=p?(unsigned)strtoul(p,NULL,10):30;return (n<1?1:n>60?60:n)*1000;}
static void ally_event(const char *event,const CoopBattleMove *m){
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");if(!dir)return;
    char path[1400];snprintf(path,sizeof path,"%s/battle.jsonl",dir);FILE *f=fopen(path,"a");if(!f)return;
    fprintf(f,"{\"event\":\"%s\",\"frame\":%llu,\"turn\":%u,\"move\":%u,\"target\":%u}\n",event,
        (unsigned long long)renderer_game_frame(),ally_wait.turn,m?m->move:0,m?m->target:0);fclose(f);
}
// Mirror trial: each player chooses in the game's own battle menu, and waits in its message window.
// LARECOMP_COOP_BATTLE_OVERLAY=1 (or a battle without a UI client) keeps the overlay panel instead.
static bool native_failed,wait_shown;
static uint32_t ally_done_round;
static bool native_ui(void){return supported && trial_active && coop_battle_link_mirror_enabled() && !native_failed && !ENV("LARECOMP_COOP_BATTLE_OVERLAY");}
bool coop_battle_native_ui(void){return native_ui();}
// The battle's one UI client belongs to the local player's controller: context+0x1a8[context+0x18c], +0x280.
static uint64_t local_ui(uint64_t controller){
    uint64_t ctx=ptr(controller+0x58);if(!guest(ctx,0x1d0))return 0;
    return ptr(ptr(ctx+0x1a8+8*(*(uint8_t *)(uintptr_t)(ctx+0x18c)&3))+0x280);
}
// btl_std line 201 ("The opposing player's time has run out!", link battles only) carries the waiting text.
#define WAIT_LINE 201
static uint16_t wait_text[80];
static uint8_t wait_param[0x24]; // message parameter (0x7000a6f000): u16 line, type 1 (btl_std), no arguments
static bool show_wait(Cpu *c,uint64_t ui){
    CoopRemote peer;uint64_t show=ptr(ptr(ui)+0x1b0); // UI +0x1b0: open a battle message (as 0x7000a5a180 state 8)
    if(!guest(show,4) || !coop_net_remote(&peer))return false;
    char s[96];snprintf(s,sizeof s,"Waiting for %s...",peer.name);unsigned n=0;
    for(const unsigned char *p=(const unsigned char *)s;*p && n+1<sizeof wait_text/2;){ // UTF-8 -> UTF-16 (BMP)
        uint32_t cp=*p++;unsigned more=cp>=0xe0?2:cp>=0xc0?1:0;if(more)cp&=0x3f>>more;
        while(more-- && (*p&0xc0)==0x80)cp=(cp<<6)|(*p++&0x3f);wait_text[n++]=(uint16_t)cp;}
    wait_text[n]=0;memset(wait_param,0,sizeof wait_param);wait_param[0]=WAIT_LINE;wait_param[3]=1;
    Cpu call=*c;call.x[0]=ui;call.x[1]=(uint64_t)(uintptr_t)wait_param;rt_call_original(&call,show);
    ally_event("native_wait_shown",NULL);return true;
}
static int wait_text_hook(Cpu *c,uint64_t addr){ // 0x70006786b0 decrypts a message line (runtime/hle/wildbattle.c)
    uint64_t md=c->x[0],sb=c->x[4],lr=c->x[30],sp=c->sp,table=ptr(md+0x90);
    bool mine=supported && trial_active && wait_text[0] && ((unsigned)c->x[3]&0xffff)==WAIT_LINE && guest(table,4) && *(uint16_t *)(uintptr_t)(table+2)==392;
    if(!previous_msg || !previous_msg(c,addr))rt_call_original(c,addr);
    uint16_t *buf=mine?(uint16_t *)(uintptr_t)ptr(sb+0x58):NULL;unsigned cap=buf?*(uint16_t *)(uintptr_t)(sb+0x78):0,n=0;
    if(!cap)return 1;
    for(;wait_text[n] && n+1<cap;n++)buf[n]=wait_text[n];
    buf[n]=0;*(uint16_t *)(uintptr_t)(sb+0x7a)=(uint16_t)n;c->x[30]=lr;c->sp=sp;c->x[0]=sb;return 1;
}
static bool local_done;
// A broken replay never falls back silently: journal the reason once, stop
// taking commands and let the native AI finish locally. The mirrored result
// phase and partner writes stay discarded, so the guest save is untouched.
static bool mirror_aborted;
static void mirror_abort(const CoopBattleMirrorSetup *status,bool ready){
    if(mirror_aborted)return;
    mirror_aborted=true;
    unsigned reason=status->aborted?status->reason:!ready?COOP_MIRROR_LINK_LOST:status->ended?COOP_MIRROR_INCOMPLETE_END:COOP_MIRROR_DIGEST;
    LOG("[coop-battle] mirrored replay aborted, reason %u; finishing locally without results\n",reason);
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");if(!dir)return;
    char path[1400];snprintf(path,sizeof path,"%s/battle.jsonl",dir);FILE *f=fopen(path,"a");
    if(f){fprintf(f,"{\"event\":\"mirror_aborted\",\"frame\":%llu,\"round\":%u,\"reason\":%u}\n",(unsigned long long)renderer_game_frame(),ally_round,reason);fclose(f);}
}
// Battle-party slots this controller's trainer may send in: past its active ones (0x9daa20) and not fainted.
// The ally's battle party is the guest's own party (trainer_party_hook), so the guest only ever sends in its own.
static uint8_t party_switches(Cpu *cpu,uint64_t controller){
    uint64_t ctx=ptr(controller+0x58),party=ptr(controller+0x268);if(!guest(ctx,0x1d0) || !guest(party,8))return 0;
    Cpu call=*cpu;call.x[0]=party;rt_call_original(&call,0x70009df300ull);unsigned n=(uint8_t)call.x[0];
    call.x[0]=ctx;call.x[1]=*(uint8_t *)(uintptr_t)(controller+0x1fa);rt_call_original(&call,0x70009daa20ull);
    uint8_t mask=0;
    for(unsigned p=(uint8_t)call.x[0];p<n && p<6;p++){
        call.x[0]=party;call.x[1]=p;rt_call_original(&call,0x70009df470ull);uint64_t mon=call.x[0];
        if(!guest(mon,0x2e1))continue;
        call.x[0]=mon;rt_call_original(&call,0x70009e3a20ull);if(!(call.x[0]&1))mask|=1u<<p;
    }
    return mask;
}
static bool ally_choices(Cpu *cpu,uint64_t controller,CoopBattleChoices *out,uint16_t species[2],uint16_t hp[2],uint16_t maxhp[2]){
    Cpu call=*cpu;uint64_t mon=ptr(controller+0x78),pool=ptr(controller+0x70),ctx=ptr(controller+0x58);
    if(!guest(mon,0x2e1) || !guest(pool,0x140))return false;
    uint16_t targets=0;memset(out,0,sizeof *out);memset(species,0,4);memset(hp,0,4);memset(maxhp,0,4);
    for(unsigned i=0;i<2;i++){
        unsigned trainer=1+2*i;
        if(!*(uint8_t *)(uintptr_t)(pool+0x60+0x38*trainer+0x30))continue;
        call.x[0]=pool;call.x[1]=trainer;call.x[2]=0;rt_call_original(&call,0x70009e14c0ull);uint64_t enemy=call.x[0];
        if(!guest(enemy,0x2e1))continue;
        call.x[0]=enemy;rt_call_original(&call,0x70009e3a20ull);if(call.x[0]&1)continue;
        species[i]=*(uint16_t *)(uintptr_t)(enemy+0x78);hp[i]=*(uint16_t *)(uintptr_t)(enemy+0x7e);maxhp[i]=*(uint16_t *)(uintptr_t)(enemy+0x7c);
        call.x[0]=ctx;call.x[1]=pool;call.x[2]=*(uint8_t *)(uintptr_t)(enemy+0x87);rt_call_original(&call,0x70009d9d50ull);
        unsigned position=(unsigned)call.x[0];
        // This is already an absolute battlefield position. 9d9c80 chooses an
        // actor's default opponent; applying it again would target our own side.
        if(position<4)targets|=1u<<position;
    }
    out->kind=2;out->switches=party_switches(cpu,controller);
    bool any=out->switches!=0;
    for(unsigned i=0;i<4;i++){
        call.x[0]=mon;call.x[1]=i;rt_call_original(&call,0x70009e4950ull);out->moves[i]=(uint16_t)call.x[0];
        if(!out->moves[i])continue;
        call.x[0]=mon;call.x[1]=i;rt_call_original(&call,0x70009e4a80ull);out->pp[i]=(uint8_t)call.x[0];
        call.x[0]=out->moves[i];rt_call_original(&call,0x700073d500ull);unsigned target_type=(unsigned)call.x[0];
        // First slice: ordinary selected-opponent moves. Other targeting rules
        // retain the game's AI rather than accepting an incorrectly mapped target.
        if(target_type!=0){out->pp[i]=0;continue;}
        call.x[0]=controller;call.x[1]=mon;call.x[2]=out->moves[i];call.x[3]=0;call.x[4]=0;rt_call_original(&call,0x7000a5bda0ull);
        if(call.x[0]&1)out->pp[i]=0;
        out->targets[i]=targets;any|=out->pp[i] && targets;
    }
    return any;
}
static bool native_command_valid(Cpu *c,uint64_t controller,const CoopBattleMove *move){
    uint64_t mon=ptr(controller+0x78);if(!guest(mon,0x2e1) || move->move_index>=4 || move->target>=4)return false;
    Cpu call=*c;call.x[0]=mon;call.x[1]=move->move_index;rt_call_original(&call,0x70009e4950ull);
    if(call.x[0]!=move->move)return false;
    call.x[0]=mon;call.x[1]=move->move_index;rt_call_original(&call,0x70009e4a80ull);if(!call.x[0])return false;
    call.x[0]=move->move;rt_call_original(&call,0x700073d500ull);
    if(call.x[0]==0){CoopBattleChoices choices;uint16_t species[2],hp[2],maxhp[2];
        if(!ally_choices(c,controller,&choices,species,hp,maxhp) || !(choices.targets[move->move_index]&(1u<<move->target)))return false;}
    return true;
}
// Host: one decision of trainer slot 0 or 2 for the guest's replay. Every native decision (move, switch, item,
// replacement after a faint) is a command word in the controller's reply (+0x230 count, +0x234 first command).
static uint32_t local_seq;
static void publish(Cpu *c,uint64_t controller,unsigned trainer,uint32_t seq,unsigned kind,bool ai,uint32_t packet,unsigned count){
    CoopBattleLinkView link;coop_battle_link_view(&link);
    CoopBattleMove m={.battle=link.battle,.turn=seq,.sequence=seq,.trainer=trainer,.kind=kind,.count=count,.packet=packet};
    uint64_t mon=ptr(controller+0x78);
    if((packet&15)==1){
        m.move=(packet>>8)&0xffff;m.target=(packet>>4)&15;
        for(unsigned i=0;i<4 && guest(mon,0x2e1);i++){Cpu call=*c;call.x[0]=mon;call.x[1]=i;rt_call_original(&call,0x70009e4950ull);if(call.x[0]==m.move){m.move_index=i;break;}}
    }
    if(!coop_battle_link_mirror_commit(&m,ai))LOG("[coop-battle] slot %u decision %u not published\n",trainer,seq);
    else LOG("[coop-battle] slot %u decision %u published: request %u, command %08x x%u%s\n",trainer,seq,kind,packet,count,ai?" (NPC)":"");
    ally_event(trainer?"slot2_published":"slot0_published",&m);
}
// A controller's reply set to the host's decision (only the first command: one active Pokemon per trainer).
static void reply(uint64_t controller,const CoopBattleMove *m){
    uint8_t *ctl=(uint8_t *)(uintptr_t)controller;
    ctl[0x230]=m->count;memcpy(ctl+0x234,&m->packet,4);memcpy(ctl+0x288,&m->packet,4);
    *(uint64_t *)(ctl+0x220)=controller+0x230;*(uint32_t *)(ctl+0x228)=0x2c;
}
// Guest: the host's decision for slot 0, taken once per native decision (kinds 2, 4, 5) in the host's order.
static CoopBattleCanonical slot0;
static bool slot0_have,slot0_pending;
static bool slot0_take(Cpu *c,uint64_t controller,unsigned kind){
    if(slot0_have)return true;
    if(!remote_waiting){remote_waiting=true;remote_round++;battle_seen=true;wait_shown=false;}
    CoopBattleMirrorSetup status;bool ready=coop_battle_link_mirror_setup(&status);
    if(!mirror_aborted && coop_battle_link_mirror_take(native_mirror.battle,0,remote_round,&slot0)){
        LOG("[coop-battle] replaying host slot 0 decision %u: request %u, command %08x\n",remote_round,slot0.move.kind,slot0.move.packet);
        if(slot0.move.kind==kind)return slot0_have=true;
        LOG("[coop-battle] host decision %u is kind %u, this replay is at kind %u\n",remote_round,slot0.move.kind,kind);
        CoopBattleMirrorSetup bad={.aborted=true,.reason=COOP_MIRROR_DIGEST};mirror_abort(&bad,true);
    } else if(!mirror_aborted && ready && !status.aborted && !status.ended){
        // The guest has chosen for slot 2 this turn; the host has not chosen yet.
        if(kind==2 && native_ui() && !wait_shown && ally_done_round==remote_round)wait_shown=show_wait(c,ptr(controller+0x280));
    } else mirror_abort(&status,ready);
    return false;
}
// 1 once the guest's slot-0 reply is the host's (0: still waiting for it).
static uint64_t finish_slot0(Cpu *c,uint64_t controller,unsigned kind){
    if(!mirror_aborted && !slot0_take(c,controller,kind) && !mirror_aborted)return 0;
    if(slot0_have && !mirror_aborted)reply(controller,&slot0.move),ally_event("slot0_replayed",&slot0.move);
    slot0_have=slot0_pending=remote_waiting=false;return 1;
}
static int remote_player_test_hook(Cpu *c,uint64_t addr){
    uint64_t controller=c->x[0],state=c->x[1];const char *choice=ENV("LARECOMP_COOP_BATTLE_REMOTE_MOVE");
    if(!supported || !trial_active || !player_clone_party || (!choice && !mirrored_guest) || !guest(controller,0x3d0) || !guest(state,4) ||
        *(uint8_t *)(uintptr_t)(controller+0x1fa)!=0 || *(uint32_t *)(uintptr_t)state!=2)
        return previous_remote_player?previous_remote_player(c,addr):0;
    CoopBattleMove move={0};
    if(mirrored_guest){
        if(slot0_take(c,controller,2))move=slot0.move;
        else if(!mirror_aborted){c->x[0]=0;return 1;}
    }
    if(!move.kind){
        CoopBattleChoices choices;uint16_t species[2],hp[2],maxhp[2];unsigned index=choice?(unsigned)strtoul(choice,NULL,10):0;
        if(!ally_choices(c,controller,&choices,species,hp,maxhp))return previous_remote_player?previous_remote_player(c,addr):0;
        if(index>=4 || !choices.pp[index] || !choices.targets[index]){
            for(index=0;index<4 && (!choices.pp[index] || !choices.targets[index]);index++){}
            if(index==4)return 0;
        }
        unsigned target=0;while(!(choices.targets[index]&(1u<<target)))target++;
        move=(CoopBattleMove){.battle=native_mirror.battle,.turn=remote_round,.sequence=remote_round,.trainer=0,.move_index=index,.move=choices.moves[index],.target=target};
    }
    // (the host's own decision, whatever its targeting, is replayed as it is; only the local fallback move is checked)
    if(!move.kind && !native_command_valid(c,controller,&move)){ally_event("mirror_command_rejected",&move);c->x[0]=0;return 1;}
    uint64_t output=ptr(controller+0x80);if(!guest(output,4))return 0;
    Cpu call=*c;
    if(move.kind)memcpy((void *)(uintptr_t)output,&move.packet,4);
    else{call.x[0]=output;call.x[1]=move.move;call.x[2]=move.target;call.x[3]=0;rt_call_original(&call,0x7000a50a90ull);}
    *(uint8_t *)(uintptr_t)(controller+0x20c)=1;*(uint8_t *)(uintptr_t)(controller+0x388)=0;*(uint32_t *)(uintptr_t)state=3;
    call=*c;rt_call_original(&call,0x7000a577f0ull);*c=call;
    if(!slot0_have)remote_waiting=false;
    battle_seen=true;ally_event("remote_player_injected",&move);
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");CoopParty current;
    if(dir && coop_battle_capture_party(c,&current))save_party(dir,"party-at-command.bin",&current);
    return 1;
}
// Slot 0's action select (human handler 0x7000a574f0). Host: its choice is complete when the handler returns 1, and
// the reply is published. Guest: a forced action (never reaching the menu step) still takes the host's decision, so
// both sides count every slot-0 decision.
static int human_select_hook(Cpu *c,uint64_t addr){
    uint64_t controller=c->x[0],state=c->x[1];bool first=guest(state,4) && *(uint32_t *)(uintptr_t)state==0;
    bool slot=supported && trial_active && guest(controller,0x3d0) && *(uint8_t *)(uintptr_t)(controller+0x1fa)==0;
    bool replay=slot && mirrored_guest && !mirror_aborted;
    if(replay && slot0_pending){c->x[0]=finish_slot0(c,controller,2);return 1;}
    if(!previous_human_select || !previous_human_select(c,addr))rt_call_original(c,addr);
    if(!slot)return 1;
    if(first)local_done=false;
    if(!(c->x[0]&1))return 1;
    local_done=true;
    if(replay){slot0_pending=true;c->x[0]=finish_slot0(c,controller,2);}
    else if(!mirrored_guest)publish(c,controller,0,++local_seq,2,false,*(uint32_t *)(uintptr_t)(controller+0x234),*(uint8_t *)(uintptr_t)(controller+0x230));
    return 1;
}
// Slot 0's switch-in (human 0x7000a57ff0 after a faint, request 4; 0x7000a584f0 mid-turn, request 5). The host picks
// natively and publishes; the guest never opens the host's party list and replies with the host's pick.
static RuntimeFnHook previous_switch_in[2];
static int switch_in_hook(Cpu *c,uint64_t addr){
    unsigned kind=addr==0x7000a57ff0ull?4:5;RuntimeFnHook previous=previous_switch_in[kind-4];uint64_t controller=c->x[0];
    bool slot=supported && trial_active && guest(controller,0x3d0) && *(uint8_t *)(uintptr_t)(controller+0x1fa)==0;
    if(slot && mirrored_guest && !mirror_aborted){c->x[0]=finish_slot0(c,controller,kind);return 1;}
    if(!previous || !previous(c,addr))rt_call_original(c,addr);
    if(slot && !mirrored_guest && (c->x[0]&1))
        publish(c,controller,0,++local_seq,kind,false,*(uint32_t *)(uintptr_t)(controller+0x234),*(uint8_t *)(uintptr_t)(controller+0x230));
    return 1;
}
static int npc_commit_hook(Cpu *c,uint64_t addr){
    uint64_t controller=c->x[0];
    if(!previous_npc_commit || !previous_npc_commit(c,addr))rt_call_original(c,addr);
    if(supported && coop_battle_link_enabled() && trial_active && guest(controller,0x3d0) &&
        *(uint8_t *)(uintptr_t)(controller+0x1fa)==2 && guest(c->x[0],4)){
        uint64_t command=c->x[0];uint32_t packet=*(uint32_t *)(uintptr_t)command;
        if((packet&15)==1){
            CoopBattleChoices choices;uint16_t species[2],hp[2],maxhp[2];
            if(ally_choices(c,controller,&choices,species,hp,maxhp)){
                for(unsigned i=0;i<4;i++)if(choices.moves[i]==((packet>>8)&65535) && choices.targets[i]){
                    unsigned target=(packet>>4)&15;
                    if(!(choices.targets[i]&(1u<<target))){
                        for(target=0;target<16 && !(choices.targets[i]&(1u<<target));target++){}
                        Cpu call=*c;call.x[0]=command;call.x[1]=target;rt_call_original(&call,0x7000a50ac0ull);
                        LOG("[coop-ally] NPC target normalized to opposing position %u\n",target);
                    }
                    break;
                }
            }
        }
        // Whatever the NPC chose this round (a move, a switch or an item) is what the guest replays.
        if(ally_publish_ai && !mirrored_guest){
            ally_publish_ai=false;publish(c,controller,2,ally_round,2,true,*(uint32_t *)(uintptr_t)command,1);ally_event("ally_npc_choice",NULL);
        }
    }
    return 1;
}
// Guest: the game's own Fight menu for its slot-2 Pokemon. That slot is the native ally AI controller, so the
// local human action handler (0x7000a574f0) runs on it with the battle's UI client lent to it (setter
// 0x7000a57000) and its own state; the controller fields the human flow writes are restored afterwards, so
// the replay continues exactly as without the menu. The chosen move goes to the host like the overlay's;
// the host's canonical command for slot 2 is still what this engine executes. At the deadline (or once the
// host's command is in) the native link-battle time limit closes the menu: +0x260 a limit, +0x200 time up.
#define HUMAN_SELECT 0x7000a574f0ull
#define SET_UI 0x7000a57000ull
static CoopBattleCanonical held;
static bool have_held,menu_forced;
static uint32_t menu_state,menu_turn;
static uint8_t menu_saved[0xa0],menu_saved_commands[0x28],menu_saved_steps[0x14],menu_saved_list[0x38];
// The UI reads "its" party (party lists, UI +0x6b8 -> 0x7000a64700) from its owner controller at UI +0x60, set
// when the battle built it for the local slot: while lent, the owner is the slot-2 controller, so the lists show
// the guest's own party with the slot numbers the commands use.
static uint64_t ui_owner;
static void lend_ui(Cpu *c,uint64_t controller,uint64_t ui){
    uint64_t lent=ui?ui:ptr(controller+0x280);
    if(guest(lent,0x68)){
        if(ui){ui_owner=ptr(ui+0x60);*(uint64_t *)(uintptr_t)(ui+0x60)=controller;}
        else if(ui_owner){*(uint64_t *)(uintptr_t)(lent+0x60)=ui_owner;ui_owner=0;}
    }
    Cpu call=*c;call.x[0]=controller;call.x[1]=(uint64_t)(uintptr_t)&ui;rt_call_original(&call,SET_UI);
}
// kind 2: the action menu (Fight / Pokemon; the guest's Bag is refused, see below). kind 4/5: the party list of a
// replacement (human 0x7000a57ff0 / 0x7000a584f0), which shows this controller's battle party: the guest's own.
static bool native_menu(Cpu *c,uint64_t controller,unsigned kind){
    CoopBattleLinkView v;coop_battle_link_view(&v);uint64_t now=plat_now_ns();
    uint8_t *ctl=(uint8_t *)(uintptr_t)controller;
    if(!menu_open){
        if(!native_ui() || have_held || mirror_aborted || !v.waiting || v.submitted || v.choices.kind!=kind || now>=v.deadline_ns || menu_turn==v.turn)return false;
        uint64_t ui=local_ui(controller);
        if(!ui){native_failed=true;LOG("[coop-ally] no battle UI client; the overlay chooses\n");return false;}
        memcpy(menu_saved,ctl+0x1e0,sizeof menu_saved);memcpy(menu_saved_commands,ctl+0x288,sizeof menu_saved_commands);
        memcpy(menu_saved_steps,ctl+0x3b8,sizeof menu_saved_steps);memcpy(menu_saved_list,ctl+0x418,sizeof menu_saved_list);
        lend_ui(c,controller,ui);menu_open=true;menu_forced=false;menu_state=0;menu_turn=v.turn;ally_event("native_menu_open",NULL);
    }
    if(!menu_forced && (have_held || mirror_aborted || !v.waiting || now>=v.deadline_ns)){
        *(uint16_t *)(ctl+0x260)=1;ctl[0x200]=1;menu_forced=true;ally_event("native_menu_timeout",NULL);
    }
    Cpu call=*c;call.x[0]=controller;call.x[1]=(uint64_t)(uintptr_t)&menu_state;
    rt_call_original(&call,kind==2?HUMAN_SELECT:kind==4?0x7000a57ff0ull:0x7000a584f0ull);
    if(!(call.x[0]&1))return true;
    uint64_t out=ptr(controller+0x80);uint32_t packet=kind!=2?(ctl[0x230]?*(uint32_t *)(ctl+0x234):0):guest(out,4)?*(uint32_t *)(uintptr_t)out:0;
    unsigned move=(packet>>8)&0xffff,target=(packet>>4)&15,index=0;
    bool sent=false;
    if(menu_forced){}
    // A switch (change command, bits 7-9 the battle-party slot; bit 10 set means "no change").
    else if((packet&15)==3 && !(packet&0x400))sent=coop_battle_link_choose_switch((packet>>7)&7);
    else if(kind==2 && (packet&15)==1){
        while(index<4 && v.choices.moves[index]!=move)index++;
        // The menu's cursor may rest on a fainted foe (the engine would redirect): with one foe left, that one.
        unsigned mask=index<4?v.choices.targets[index]:0;
        if(mask && !(mask&(1u<<target)) && !(mask&(mask-1)))for(target=0;!(mask&(1u<<target));target++){}
        sent=coop_battle_link_choose(index,target);
    }
    lend_ui(c,controller,0);
    memcpy(ctl+0x1e0,menu_saved,sizeof menu_saved);memcpy(ctl+0x288,menu_saved_commands,sizeof menu_saved_commands);
    memcpy(ctl+0x3b8,menu_saved_steps,sizeof menu_saved_steps);memcpy(ctl+0x418,menu_saved_list,sizeof menu_saved_list);menu_open=false;
    CoopBattleMove m={.move=move,.target=target,.packet=packet};ally_event(sent?"native_menu_choice":menu_forced?"native_menu_closed":"native_menu_retry",&m);
    // An item that still gets here (the Bag is refused, bag_check_hook) or an unsupported move: the menu again.
    if(!sent && !menu_forced && kind==2){menu_turn=0;return true;}
    return false;
}
// Guest: the host's decision for slot 2 this round, once it is in; it must be of the kind this engine is at.
static void take_held(unsigned kind){
    if(have_held || mirror_aborted || !coop_battle_link_mirror_take(native_mirror.battle,2,ally_round,&held))return;
    have_held=true;LOG("[coop-battle] replaying host slot 2 decision %u: request %u, command %08x%s\n",ally_round,held.move.kind,held.move.packet,held.native_ai?" (NPC)":"");
    if(held.move.kind!=kind){
        LOG("[coop-battle] host decision %u for slot 2 is kind %u, this replay is at kind %u\n",ally_round,held.move.kind,kind);
        CoopBattleMirrorSetup bad={.aborted=true,.reason=COOP_MIRROR_DIGEST};mirror_abort(&bad,true);
    }
}
static int mirror_ally_select(Cpu *c,uint64_t controller,uint64_t state){
    Cpu call=*c;
    if(!ally_prepared){
        call.x[0]=ptr(controller+0x368);rt_call_original(&call,0x7000a6d310ull);
        call.x[0]=ptr(controller+0x368);rt_call_original(&call,0x7000a6d230ull);
        if(!(call.x[0]&1)){c->x[0]=0;return 1;}
        ally_prepared=true;ally_round++;have_held=false;ally_event("mirror_ally_waiting",NULL);
        CoopBattleMirrorSetup status;bool ready=coop_battle_link_mirror_setup(&status);
        if(!coop_battle_link_mirror_digest(ally_round,journal_turn_state(c,controller)))mirror_abort(&status,ready);
    }
    take_held(2);
    if(native_menu(c,controller,2)){c->x[0]=0;return 1;}
    CoopBattleCanonical canonical=held;CoopBattleMirrorSetup status;
    bool ready=coop_battle_link_mirror_setup(&status);
    if(mirror_aborted || !have_held){
        if(!mirror_aborted && ready && !status.aborted && !status.ended){c->x[0]=0;return 1;}
        mirror_abort(&status,ready);ally_native=true;return 0;
    }
    have_held=false;ally_done_round=ally_round;
    // The NPC's choice (any targeting) is replayed as it is; the guest's own move must still fit its Pokemon.
    if(!canonical.native_ai && (canonical.move.packet&15)==1 && !native_command_valid(c,controller,&canonical.move)){
        ally_event("mirror_command_rejected",&canonical.move);CoopBattleMirrorSetup bad={.aborted=true,.reason=COOP_MIRROR_DIGEST};
        mirror_abort(&bad,true);ally_native=true;return 0;
    }
    if(canonical.native_ai){
        call=*c;call.x[0]=ptr(controller+0x368);rt_call_original(&call,0x7000a6d240ull);uint64_t result=call.x[0];
        call.x[0]=controller;call.x[1]=ptr(controller+0x80);call.x[2]=controller+0x20b;call.x[3]=*(uint8_t *)(uintptr_t)(controller+0x270);
        call.x[4]=ptr(controller+0x78);call.x[5]=ptr(controller+0x268);call.x[6]=result;rt_call_original(&call,0x7000a5c9a0ull);
        *(uint64_t *)(uintptr_t)(controller+0x80)=call.x[0];
    }
    uint64_t output=ptr(controller+0x80);if(guest(output,4))memcpy((void *)(uintptr_t)output,&canonical.move.packet,4);
    *(uint8_t *)(uintptr_t)(controller+0x20c)=1;
    *(uint32_t *)(uintptr_t)state=3;c->x[0]=0;ally_event("mirror_ally_injected",&canonical.move);return 1;
}
static int ally_select_hook(Cpu *c,uint64_t addr){
    uint64_t controller=c->x[0],state=c->x[1];
    if(!supported || !trial_active || !coop_battle_link_enabled() || !guest(controller,0x3d0) || !guest(state,4) ||
        *(uint8_t *)(uintptr_t)(controller+0x1fa)!=2)return previous_ally_select ? previous_ally_select(c,addr):0;
    unsigned phase=*(uint32_t *)(uintptr_t)state;
    if(!phase){ally_prepared=ally_native=ally_publish_ai=false;memset(&ally_wait,0,sizeof ally_wait);}
    if(phase!=2 || (*(uint8_t *)(uintptr_t)(controller+0x270)!=0 && !menu_open) || ally_native)return previous_ally_select ? previous_ally_select(c,addr):0;
    if(mirrored_guest)return mirror_ally_select(c,controller,state);
    CoopBattleLinkView link;coop_battle_link_view(&link);uint64_t now=plat_now_ns();
    if(!ally_prepared){
        if(!link.host || link.phase!=COOP_LINK_ACTIVE)return previous_ally_select ? previous_ally_select(c,addr):0;
        // State 0 falls directly through initialization to state 2. Wait at
        // state 2, after the native AI worker has finished but before its command
        // is consumed. Forced/fainted decisions bypass this state natively.
        Cpu call=*c;call.x[0]=ptr(controller+0x368);rt_call_original(&call,0x7000a6d310ull);
        call.x[0]=ptr(controller+0x368);rt_call_original(&call,0x7000a6d230ull);
        if(!(call.x[0]&1)){c->x[0]=0;return 1;}
        uint64_t mon=ptr(controller+0x78),command=ptr(controller+0x80);
        if(!guest(mon,0x2e1) || !guest(command,4))return previous_ally_select ? previous_ally_select(c,addr):0;
        // Every round reaching this state counts on both clients (the guest replay counts it too), even when the NPC
        // decides it: its decision is published then (npc_commit_hook).
        ally_round++;coop_battle_link_mirror_digest(ally_round,journal_turn_state(c,controller));
        CoopBattleChoices choices;uint16_t species[2],hp[2],maxhp[2];
        if(!ally_choices(c,controller,&choices,species,hp,maxhp)){ally_native=ally_publish_ai=true;return previous_ally_select ? previous_ally_select(c,addr):0;}
        if(!coop_ally_begin(&ally_wait,link.battle,ally_round,ally_round,&choices,now,ally_timeout())){ally_native=ally_publish_ai=true;return 0;}
        ally_prepared=true;wait_shown=local_done=false;
        if(!coop_battle_link_request(&ally_wait,species,hp,maxhp))ally_wait.deadline_ns=now;
        CoopBattleMirrorSetup status;
        if(coop_battle_link_mirror_setup(&status) && status.aborted && !mirror_aborted){
            // The host keeps its battle and NPC fallback; only log the guest replay failure.
            mirror_aborted=true;LOG("[coop-battle] guest replay reported aborted, reason %u\n",status.reason);ally_event("mirror_aborted_host",NULL);
        }
        ally_event("ally_waiting",NULL);LOG("[coop-ally] waiting for guest, turn %u, timeout %u ms\n",ally_round,ally_timeout());
    }
    CoopBattleMove command;uint64_t received;
    if(coop_battle_link_take(&command,&received) && !coop_ally_submit(&ally_wait,&command,received))ally_event("ally_rejected",&command);
    if(link.phase!=COOP_LINK_ACTIVE)ally_wait.deadline_ns=now;
    if(coop_ally_poll(&ally_wait,now)==COOP_ALLY_WAITING){
        // The host has chosen for slot 0 this turn; the guest has not chosen yet.
        if(native_ui() && local_done && !wait_shown && !(wait_shown=show_wait(c,local_ui(controller))))native_failed=true;
        c->x[0]=0;return 1;}
    bool ai;if(!coop_ally_consume(&ally_wait,&command,&ai)){c->x[0]=0;return 1;}
    if(ai){ally_native=ally_publish_ai=true;coop_battle_link_complete(true);ally_event("ally_npc_fallback",NULL);
        return previous_ally_select ? previous_ally_select(c,addr):0;}
    uint64_t output=ptr(controller+0x80);
    // The host builds the command itself: a move (0x7000a50a90) or a switch to the guest's battle-party slot (0x7000a50c50).
    Cpu call=*c;call.x[0]=output;
    if(command.switch_to){call.x[1]=*(uint8_t *)(uintptr_t)(controller+0x270);call.x[2]=command.switch_to-1;rt_call_original(&call,0x7000a50c50ull);}
    else{call.x[1]=command.move;call.x[2]=command.target;call.x[3]=0;rt_call_original(&call,0x7000a50a90ull);}
    *(uint8_t *)(uintptr_t)(controller+0x20c)=1;
    *(uint32_t *)(uintptr_t)state=3;c->x[0]=0;coop_battle_link_complete(false);
    publish(c,controller,2,ally_wait.sequence,2,false,*(uint32_t *)(uintptr_t)output,1);ally_event("ally_injected",&command);
    if(command.switch_to)LOG("[coop-ally] injected guest switch to party slot %u for turn %u\n",command.switch_to-1,command.turn);
    else LOG("[coop-ally] injected guest move %u target %u for turn %u\n",command.move,command.target,command.turn);
    return 1;
}
// Slot 2's switch-in (the NPC handler 0x7000a58000 for requests 4, after a faint, and 5, mid-turn). Host: the guest
// picks one of its own Pokemon within the ally timeout (its party list, native_menu), else the NPC does; the
// NPC's handler always runs once on both clients, and the guest's pick replaces its party slot. Guest: its party
// list while waiting, then the host's decision.
static bool repl_prepared,repl_native;
static RuntimeFnHook previous_ai_switch;
static int ai_switch_hook(Cpu *c,uint64_t addr){
    uint64_t controller=c->x[0];
    if(!supported || !trial_active || !coop_battle_link_enabled() || !guest(controller,0x3d0) || *(uint8_t *)(uintptr_t)(controller+0x1fa)!=2)
        return previous_ai_switch?previous_ai_switch(c,addr):0;
    unsigned kind=*(uint32_t *)(uintptr_t)(controller+0x21c)==5?5:4;
    CoopBattleLinkView link;coop_battle_link_view(&link);uint64_t now=plat_now_ns();
    if(!repl_prepared){
        repl_prepared=true;repl_native=true;ally_round++;have_held=false;
        // Request 4 goes to every controller after any faint: only ask when one of this trainer's positions is
        // listed (0x7000a51c70 lists them, 0x70009d9f00 owns one), as the handlers do.
        Cpu call=*c;uint64_t list=0;unsigned mine=0;
        call.x[0]=ptr(controller+0x120);call.x[1]=(uint64_t)(uintptr_t)&list;rt_call_original(&call,0x7000a51c70ull);
        for(unsigned i=0,n=(uint8_t)call.x[0];i<n && guest(list+i,1);i++){
            call.x[0]=ptr(controller+0x58);call.x[1]=*(uint8_t *)(uintptr_t)(list+i);rt_call_original(&call,0x70009d9f00ull);
            mine+=(uint8_t)call.x[0]==*(uint8_t *)(uintptr_t)(controller+0x1fa);
        }
        uint8_t mask=mine?party_switches(c,controller):0;
        if(!mirrored_guest && mask && link.host && link.phase==COOP_LINK_ACTIVE){
            CoopBattleChoices choices={.kind=kind,.switches=mask};uint16_t none[2]={0};
            memset(&ally_wait,0,sizeof ally_wait);
            if(coop_ally_begin(&ally_wait,link.battle,ally_round,ally_round,&choices,now,ally_timeout())){
                repl_native=false;wait_shown=false;
                if(!coop_battle_link_request(&ally_wait,none,none,none))ally_wait.deadline_ns=now;
                LOG("[coop-ally] guest's Pokemon fainted: waiting for its replacement, slots %02x, timeout %u ms\n",mask,ally_timeout());
            }
        }
        ally_event(mirrored_guest?"mirror_switch_waiting":"ally_switch_waiting",NULL);
        LOG("[coop-battle] slot 2 switch-in, request %u, round %u, party slots %02x\n",kind,ally_round,mask);
    }
    bool ai=true;CoopBattleMove command={0};
    if(mirrored_guest){
        take_held(kind);
        if(native_menu(c,controller,kind)){c->x[0]=0;return 1;}
        if(!have_held && !mirror_aborted){
            CoopBattleMirrorSetup status;bool ready=coop_battle_link_mirror_setup(&status);
            if(ready && !status.aborted && !status.ended){c->x[0]=0;return 1;}
            mirror_abort(&status,ready);
        }
    } else if(!repl_native){
        uint64_t received;CoopBattleMove in;
        if(coop_battle_link_take(&in,&received) && !coop_ally_submit(&ally_wait,&in,received))ally_event("ally_rejected",&in);
        if(link.phase!=COOP_LINK_ACTIVE)ally_wait.deadline_ns=now;
        if(coop_ally_poll(&ally_wait,now)==COOP_ALLY_WAITING || !coop_ally_consume(&ally_wait,&command,&ai)){c->x[0]=0;return 1;}
        coop_battle_link_complete(ai);
    }
    if(!previous_ai_switch || !previous_ai_switch(c,addr))rt_call_original(c,addr);
    uint8_t *ctl=(uint8_t *)(uintptr_t)controller;uint32_t packet;memcpy(&packet,ctl+0x234,4);
    if(mirrored_guest){
        if(have_held && !mirror_aborted){reply(controller,&held.move);ally_event("mirror_switch_injected",&held.move);}
        ally_done_round=ally_round;
    } else {
        if(!ai && command.switch_to && ctl[0x230] && (packet&15)==3 && !(packet&0x400)){
            packet=(packet&~(7u<<7))|(uint32_t)(command.switch_to-1)<<7;memcpy(ctl+0x234,&packet,4);memcpy(ctl+0x288,&packet,4);
            LOG("[coop-ally] guest sent in its party slot %u\n",command.switch_to-1);
        } else ai=true;
        publish(c,controller,2,ally_round,kind,ai,packet,ctl[0x230]);ally_event(ai?"ally_switch_npc":"ally_switch_injected",NULL);
    }
    have_held=repl_prepared=false;c->x[0]=1;return 1;
}
static int trainer_party_hook(Cpu *c,uint64_t addr) {
    uint64_t party=c->x[0];unsigned id=(unsigned)c->x[2];
    if(!previous_trainer_party || !previous_trainer_party(c,addr))rt_call_original(c,addr);
    if(supported && trial_active && guest(party,0x90)) {
        LOG("[coop-battle] trainer party id %u count %u\n",id,*(uint8_t *)(uintptr_t)(party+0x88));
        // Opt-in ownership experiment: replace only the temporary NPC ally's
        // lead. Its party belongs to this battle setup, never to SaveData.
        const char *peer=ENV("LARECOMP_COOP_PARTY_TEST_PEER");
        if((getenv("LARECOMP_COOP_TRAINER_TEST_GUEST") || coop_battle_link_enabled()) && (peer || coop_battle_link_enabled()) &&
            party==ptr(trainer_process+0xe0+0xc0)) {
            CoopParty remote,before,after;Cpu call=*c;uint8_t roundtrip[COOP_PB7_BYTES];
            bool loaded=false;
            if(coop_battle_link_enabled()){uint64_t id;
                loaded=mirrored_guest?coop_battle_link_local_party(&remote):coop_battle_link_guest_party(&remote,&id);}
            else loaded=load_party(peer,&remote);
            if(loaded && coop_battle_capture_party(&call,&before)) {
                // The guest's whole party replaces the trainer's: the ally only ever sends in the guest's own Pokemon.
                bool exact=true;unsigned n=0;
                for(;n<remote.count;n++){
                    call.x[0]=party;call.x[1]=n;rt_call_original(&call,0x7000721b00ull);uint64_t mon=call.x[0];
                    if(!guest(mon,0xb8))break;
                    call.x[0]=mon;call.x[1]=(uint64_t)(uintptr_t)remote.pokemon[n];rt_call_original(&call,IMPORT);
                    serialize(&call,mon,roundtrip);exact&=!memcmp(roundtrip,remote.pokemon[n],sizeof roundtrip);
                }
                // (published once every Pokemon is in, as make_party_clone)
                if(n)*(uint8_t *)(uintptr_t)(party+0x88)=(uint8_t)n;
                exact&=n==remote.count;
                bool unchanged=coop_battle_capture_party(&call,&after) && before.count==after.count &&
                    !memcmp(before.pokemon,after.pokemon,before.count*COOP_PB7_BYTES);
                LOG("[coop-battle] temporary ally guest party: %u of %u, exact %u local party unchanged %u\n",n,remote.count,exact,unchanged);
                journal(exact && unchanged ? "guest_lead_imported" : "guest_lead_failed",renderer_game_frame(),NULL);
            }
        }
    }
    return 1;
}
static int mirror_spirit_hook(Cpu *c,uint64_t addr){
    if(supported && trial_active && player_clone_party){
        // Different parameter accessors can share a saved Pokemon's core.
        // Compare the backing buffer, not the temporary accessor object's id.
        uint64_t buffer=ptr(c->x[0]+0x10);
        if(buffer)for(unsigned i=0;i<6;i++)if(buffer==saved_party_buffers[i]){
            journal("mirror_spirit_discarded",renderer_game_frame(),NULL);return 1;
        }
    }
    return previous_spirit?previous_spirit(c,addr):0;
}
static int mirror_after_battle_hook(Cpu *c,uint64_t addr){
    if(supported && trial_active && player_clone_party && guest(c->x[0],0xf0) &&
        *(uint32_t *)(uintptr_t)(c->x[0]+0x68)==0){
        // This common result phase applies the battle party and starts local
        // evolutions. A mirrored presentation must discard those local effects;
        // let the native terminal state perform its normal completion instead.
        *(uint32_t *)(uintptr_t)(c->x[0]+0x68)=4;
        journal("mirror_results_discarded",renderer_game_frame(),NULL);
    }
    return previous_after_battle?previous_after_battle(c,addr):0;
}
static int battle_party_hook(Cpu *c,uint64_t addr) {
    uint64_t clone_reference=0;
    const char *fixture=ENV("LARECOMP_COOP_BATTLE_CLONE_PARTY");
    if(supported && trial_active && c->x[1]==0 && (fixture || mirrored_guest)){
        if(!player_clone_party){
            CoopParty source;
            bool loaded=mirrored_guest?(source=native_mirror.host_party,true):load_party(fixture,&source);
            if(loaded){remember_saved_party_buffers(c);player_clone_party=make_party_clone(c,&source);}
        }
        if(player_clone_party){
            // The pool retains its own shared reference. Keep our reference
            // until overworld return so QA can inspect the temporary results.
            clone_reference=player_clone_party;c->x[2]=(uint64_t)(uintptr_t)&clone_reference;
            journal("player_party_cloned",renderer_game_frame(),NULL);
        }
    }
    if(supported && trainer_started) {
        uint64_t party=clone_reference?clone_reference:ptr(c->x[2]);
        LOG("[coop-battle] battle slot %u native party %llx count %u first %llx\n",
            (unsigned)c->x[1],(unsigned long long)party,
            guest(party,0x90) ? *(uint8_t *)(uintptr_t)(party+0x88) : 0,
            (unsigned long long)ptr(party+0x58));
    }
    if(!previous_battle_party || !previous_battle_party(c,addr))rt_call_original(c,addr);
    return 1;
}
// Ordinary overworld trainers (docs/COOP-TRAINER-ENCOUNTERS.md): the offer goes out at the trainer's pre-battle
// text (GetTrainerMsgID type 0), the text is held up to 10 s while the partner answers (IsMsgWinEnd_), and an
// accepted, validated lead turns the native call into the verified team-up {1,T,T,T}. Otherwise the battle is solo.
static enum {ENTRY_IDLE,ENTRY_OFFERED,ENTRY_READY} entry;
static unsigned entry_trainer;
static uint64_t entry_deadline;
static bool script_battle,factory_battle,capture_process;
static RuntimeFnHook previous_trainer_msg,previous_msg_end,previous_trainer_core,previous_process_setter;
static void entry_solo(const char *why){
    if(entry==ENTRY_IDLE)return;
    coop_battle_link_end();entry=ENTRY_IDLE;
    LOG("[coop-battle] trainer %u: solo battle (%s)\n",entry_trainer,why);journal(why,renderer_game_frame(),NULL);
}
// Still holding the text: the partner has not answered within the hold.
static bool entry_poll(Cpu *c){
    CoopBattleLinkView v;coop_battle_link_view(&v);CoopParty check;uint64_t id;
    if(v.phase==COOP_LINK_NONE || !v.host){entry_solo("entry_ended");return false;} // declined, expired (B6) or disconnected
    if(coop_battle_link_guest_party(&check,&id)){
        uint16_t species[6];
        if(!coop_battle_check_import(c,&check,species)){entry_solo("entry_bad_lead");return false;}
        entry=ENTRY_READY;journal("entry_accepted",renderer_game_frame(),NULL);return false;
    }
    if(plat_now_ns()>=entry_deadline){entry_solo("entry_timeout");return false;}
    return true;
}
static int trainer_msg_hook(Cpu *c,uint64_t addr){
    uint64_t params=c->x[1];unsigned trainer=(unsigned)ptr(params+8);
    CoopSession s;coop_net_session(&s);CoopTeamUp team;coop_net_teamup(&team);CoopBattleLinkView v;coop_battle_link_view(&v);
    // Pre-battle text only (type 0; an already-beaten trainer shows type 2). Master trainers (452-597) are excluded.
    if(supported && coop_battle_link_enabled() && entry==ENTRY_IDLE && !trial_active && guest(params,24) && ptr(params+16)==0 &&
        trainer>=1 && trainer<=601 && (trainer<452 || trainer>597) && s.connection==COOP_CONNECTED && s.local_slot==s.host_slot &&
        team.state==2 && v.phase==COOP_LINK_NONE && coop_net_remote_near(3000)){
        uint64_t now=plat_now_ns();
        if(coop_battle_link_offer(now,ally_timeout(),trainer)){
            entry=ENTRY_OFFERED;entry_trainer=trainer;entry_deadline=now+10000000000ull;coop_battle_link_offer_deadline(entry_deadline);
            LOG("[coop-battle] trainer %u (%s): team-up offered, holding the text up to 10 s\n",trainer,coop_battle_trainer_name(trainer));journal("entry_offered",renderer_game_frame(),NULL);
        } else LOG("[coop-battle] trainer %u: no team-up offer (Team-up battles off)\n",trainer);
    } else if(supported && coop_battle_link_enabled() && s.connection==COOP_CONNECTED && guest(params,24) && ptr(params+16)==0)
        LOG("[coop-battle] trainer %u: no team-up offer (connection %u host %u tag %u link %u near %u)\n",trainer,s.connection,
            s.local_slot==s.host_slot,team.state,v.phase,coop_net_remote_near(3000));
    return previous_trainer_msg?previous_trainer_msg(c,addr):0;
}
static int msg_end_hook(Cpu *c,uint64_t addr){
    if(supported && entry==ENTRY_OFFERED && entry_poll(c)){c->x[0]=0;return 1;}
    return previous_msg_end?previous_msg_end(c,addr):0;
}
static int process_setter_hook(Cpu *c,uint64_t addr){
    if(capture_process && guest(c->x[0],0x740))trainer_process=c->x[0]; // CallTrainerBattleCore's first setter gets the new process
    return previous_process_setter?previous_process_setter(c,addr):0;
}
// {1,T,T,T} has two copies of the trainer, so the result's prize (0x7000319bc0 adds its +0x504 total here) is
// twice the single battle's. The host keeps the solo prize. shortcut: halves the whole total (Pay Day included).
static RuntimeFnHook previous_money,previous_item;
static int money_hook(Cpu *c,uint64_t addr){
    if(supported && script_battle && c->x[30]==0x7000319e0cull){
        LOG("[coop-battle] team-up prize %u -> %u (one trainer's)\n",(unsigned)c->x[1],(unsigned)c->x[1]/2);
        c->x[1]=(uint32_t)c->x[1]/2;journal("prize_halved",renderer_game_frame(),NULL);
    }
    return previous_money?previous_money(c,addr):0;
}
// The mirrored guest's replay gives no items either (the trainer's Poke Balls are added during the battle, before
// the discarded result phase). The native add reports success so the battle continues as usual.
static int item_hook(Cpu *c,uint64_t addr){
    if(supported && trial_active && mirrored_guest){journal("mirror_item_discarded",renderer_game_frame(),NULL);c->x[0]=1;return 1;}
    return previous_item?previous_item(c,addr):0;
}
// The guest's Bag in its lent menu takes the game's own "can't use items" path: the action dispatcher 0x7000a5a810
// asks 0x70009e3e50(Pokemon, 0x22) before opening the Bag (the call returning to 0x7000a5a918) and, when it holds,
// shows the native refusal and returns to the menu. Its items would come from no one's bag on the host.
static RuntimeFnHook previous_bag_check;
static int bag_check_hook(Cpu *c,uint64_t addr){
    if(supported && trial_active && mirrored_guest && menu_open && c->x[30]==0x7000a5a918ull){
        journal("guest_bag_refused",renderer_game_frame(),NULL);c->x[0]=1;return 1;
    }
    return previous_bag_check?previous_bag_check(c,addr):0;
}
// The host's own items (its Bag, slot 0) are spent from its bag by the battle (0x70009da170 -> bag remove); the
// guest's replay of that turn would spend them from the guest's bag, so the mirrored guest skips the removal.
static RuntimeFnHook previous_item_use;
static int item_use_hook(Cpu *c,uint64_t addr){
    if(supported && trial_active && mirrored_guest){journal("mirror_item_use_discarded",renderer_game_frame(),NULL);c->x[0]=1;return 1;}
    return previous_item_use?previous_item_use(c,addr):0;
}
static int trainer_core_hook(Cpu *c,uint64_t addr){
    uint64_t params=c->x[1];
    if(!supported || entry==ENTRY_IDLE || !guest(params,0x28))return previous_trainer_core?previous_trainer_core(c,addr):0;
    uint64_t *p=(uint64_t *)(uintptr_t)params; // ally, opponent 1, opponent 2, flags
    if(entry==ENTRY_OFFERED && entry_poll(c))entry_solo("entry_timeout");
    // Only script 699's ordinary single trainer passes flags 0x200 (story, double and tag battles stay solo).
    if(entry==ENTRY_READY && (p[1] || p[2]!=entry_trainer || p[3] || p[4]!=0x200))entry_solo("entry_not_ordinary");
    CoopParty host_party;
    if(entry==ENTRY_READY && coop_battle_link_mirror_enabled() && (!coop_battle_capture_party(c,&host_party) || !coop_battle_link_mirror_party(&host_party)))
        entry_solo("entry_mirror_failed");
    if(entry!=ENTRY_READY)return previous_trainer_core?previous_trainer_core(c,addr):0;
    entry=ENTRY_IDLE;
    // The native's own rule picks mode 1 once ally or opponent 2 is set: the verified team-up {1,T,T,T}.
    p[1]=p[3]=entry_trainer;trainer_started=trial_active=script_battle=capture_process=true;trainer_process=0;
    if(!previous_trainer_core || !previous_trainer_core(c,addr))rt_call_original(c,addr);
    capture_process=false;p[1]=p[3]=0;
    if(!trainer_process){trial_active=false;LOG("[coop-battle] trainer %u: no battle process\n",entry_trainer);}
    else{LOG("[coop-battle] ordinary trainer %u launched by its script as a team-up\n",entry_trainer);journal("trainer_started",renderer_game_frame(),NULL);}
    return 1;
}
static void maybe_trainer(Cpu *c) {
    const char *at=ENV("LARECOMP_COOP_TRAINER_TEST_AT");CoopBattleMirrorSetup setup;
    bool mirror_ready=coop_battle_link_mirror_enabled() && coop_battle_link_mirror_setup(&setup) && !setup.host && !setup.aborted;
    if(trainer_started || (!mirror_ready && (!at || renderer_game_frame()<strtoull(at,NULL,10))))return;
    // The factory test is explicit isolated QA only: no script interpreter or story event is run.
    if(!mirror_ready && (!getenv("LARECOMP_SAVE_DIR") || !getenv("LARECOMP_COOP_PARTY_TEST_DIR")))return;
    // The guest's replay starts on the overworld only (it declined while busy; after accepting it waits for it).
    CoopSession session;coop_net_session(&session);if(mirror_ready && session.activity!=COOP_OVERWORLD)return;
    uint64_t world=ptr(ptr(WORLD_GOT)),mgr=ptr(world+0x68),alloc=allocator();
    if(!guest(mgr,8) || !guest(alloc,8))return;
    unsigned trainer=mirror_ready?setup.trainer:26;const char *id=getenv("LARECOMP_COOP_TRAINER_TEST_ID");if(!mirror_ready && id)trainer=(unsigned)strtoul(id,NULL,10);
    if(!trainer || trainer>601)return;
    if(mirror_ready){
        uint16_t species[6];if(!coop_battle_check_import(c,&setup.host_party,species))return;
        native_mirror=setup;mirrored_guest=true;
    } else if(coop_battle_link_enabled()){
        CoopSession session;coop_net_session(&session);
        if(session.connection!=COOP_CONNECTED || session.local_slot!=session.host_slot)return;
        CoopBattleLinkView link;coop_battle_link_view(&link);
        if(link.phase==COOP_LINK_NONE){
            if(trainer_offer_sent || !coop_net_remote_near(3000))return;
            trainer_offer_sent=coop_battle_link_offer(plat_now_ns(),ally_timeout(),trainer);return;
        }
        CoopParty check;uint64_t id;
        if(!coop_battle_link_guest_party(&check,&id))return;
        uint16_t species[6];
        if(!coop_battle_check_import(c,&check,species)){coop_battle_link_end();return;}
        if(coop_battle_link_mirror_enabled()){
            CoopParty host_party;if(!coop_battle_capture_party(c,&host_party) || !coop_battle_link_mirror_party(&host_party))return;
        }
    }
    Cpu call=*c;uint64_t handle[2]={0},process=0;uint32_t cfg[12]={0,0,trainer};
    if(getenv("LARECOMP_COOP_TRAINER_TEST_GUEST") || coop_battle_link_enabled()){cfg[0]=1;cfg[1]=trainer;cfg[3]=trainer;}
    // Native order is ally, opponent 1, opponent 2. Setting the ally
    // field instead leaves opponent slot 1 empty and crashes initialization.
    call.x[0]=(uint64_t)(uintptr_t)&mgr;call.x[1]=alloc;call.x[2]=(uint64_t)(uintptr_t)cfg;
    call.x[8]=(uint64_t)(uintptr_t)handle;
    extern void fps60_scene_begin(void),input_mark(void);fps60_scene_begin();input_mark();
    dump_save("save-before.bin");
    trainer_started=trial_active=factory_battle=true;
    rt_call_original(&call,0x700028bb60ull);
    call.x[0]=(uint64_t)(uintptr_t)handle;call.x[8]=(uint64_t)(uintptr_t)&process;
    rt_call_original(&call,0x700017ef80ull);
    if(guest(process,0x740)) {
        trainer_process=process;
        call.x[0]=process;call.x[1]=0;call.x[2]=0;rt_call_original(&call,0x700028dd20ull);
        call.x[0]=process;call.x[1]=1;rt_call_original(&call,0x700028dd40ull);
        call.x[0]=process;call.x[1]=1;rt_call_original(&call,0x700028dd70ull);
        call.x[0]=process;call.x[1]=1;rt_call_original(&call,0x700028dd80ull);
        call.x[0]=process;call.x[1]=1;call.x[2]=1;rt_call_original(&call,0x700028dd50ull);
        // The native manager owns the running battle; release our temporary
        // process reference after configuring it, as the script wrapper does.
        uint64_t release=ptr(ptr(process+0x48)+0x18);
        if(guest(release,4)){call.x[0]=process+0x48;rt_call_original(&call,release);}
        LOG("[coop-battle] ordinary trainer %u launched\n",trainer);journal("trainer_started",renderer_game_frame(),NULL);
    } else {trial_active=false;LOG("[coop-battle] trainer factory did not resolve a process\n");}
    uint64_t release=ptr(ptr(handle[0]+0x48)+0x18);
    if(guest(release,4)){call.x[0]=handle[0]+0x48;rt_call_original(&call,release);}
}
void coop_battle_field(Cpu *c) {
    // Explicit local QA mode only. No automatic party exchange on social join.
    if(supported && coop_battle_link_enabled() && coop_net_enabled()){
        coop_battle_names_load();
        CoopBattleLinkView view;coop_battle_link_view(&view);
        // Serializing all six native Pokemon on every field tick needlessly
        // decrypts/encrypts the party. Refresh on a new offer and at most once
        // per second while idle; an active battle owns its temporary clone.
        static uint64_t next_party_snapshot,party_offer;
        uint64_t now=plat_now_ns();
        bool new_offer=view.phase==COOP_LINK_OFFER && view.battle!=party_offer;
        if(!view.party_ready || new_offer || (view.phase==COOP_LINK_NONE && now>=next_party_snapshot)){
            CoopParty party;
            if(coop_battle_capture_party(c,&party))coop_battle_link_party(&party);
            next_party_snapshot=now+1000000000ull;party_offer=view.battle;
        }
        battle_link_test_tick();
        if(ENV("LARECOMP_COOP_AUTO_TAG")){ // QA: the host invites as soon as both are near, the guest accepts
            static uint64_t next_tag;CoopTeamUp t;coop_net_teamup(&t);CoopSession s;coop_net_session(&s);
            bool invite=t.state==0 && s.local_slot==s.host_slot && coop_net_remote_near(3000),accept=t.state==1 && t.from_slot!=s.local_slot;
            if(now>=next_tag && (invite || accept) && coop_net_teamup_action(invite?0:1))next_tag=now+2000000000ull;
        }
    }
    // Files only for QA (LARECOMP_COOP_PARTY_TEST_DIR); a team-up in normal play writes none.
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");bool qa=dir && *dir;
    if(!supported)return;
    const char *late=ENV("LARECOMP_COOP_PARTY_SNAPSHOT_AT");
    if(qa && late && !late_exported && renderer_game_frame()>=strtoull(late,NULL,10)){
        CoopParty current;if(coop_battle_capture_party(c,&current))late_exported=save_party(dir,"party-late.bin",&current);
    }
    if(battle_seen){
        world_return_frame=renderer_game_frame();journal("world_return",world_return_frame,NULL);dump_save("save-return.bin");
        if(trial_active)LOG("[coop-battle] team-up battle over: back on the overworld%s\n",mirrored_guest?" (guest replay, results discarded)":"");
        CoopParty after;if(qa && coop_battle_capture_party(c,&after))save_party(dir,"party-after.bin",&after);
        if(player_clone_party){
            if(qa && capture_native_party(c,player_clone_party,&after))save_party(dir,"player-clone-after.bin",&after);
            release_party_clone(c,player_clone_party);player_clone_party=0;
        }
        // The QA FadeIn is for factory-started battles only; a trainer's script runs its own FadeIn_.
        if(!factory_battle)world_return_frame=0,journal("world_fade_restored",renderer_game_frame(),NULL);
        if(script_battle)trainer_started=false;
        trainer_process=0;battle_seen=trial_active=factory_battle=script_battle=false;
        if(mirrored_guest){coop_battle_link_mirror_clear();mirrored_guest=remote_waiting=false;}
        else coop_battle_link_end();
        mirror_aborted=false;menu_open=menu_forced=have_held=native_failed=local_done=wait_shown=false;menu_turn=ally_done_round=0;wait_text[0]=0;
        ally_publish_ai=repl_prepared=repl_native=slot0_have=slot0_pending=false;local_seq=remote_round=ally_round=0;
    }
    if(world_return_frame && renderer_game_frame()>=world_return_frame+30) {
        // The factory-only QA has no story script to perform its final FadeIn.
        Cpu call=*c;call.x[0]=ptr(ptr(0x70015facf8ull));call.x[1]=0;call.x[2]=1;call.x[3]=10;
        rt_call_original(&call,0x700011ece0ull);world_return_frame=0;dump_save("save-after.bin");
        journal("world_fade_restored",renderer_game_frame(),NULL);
    }
    maybe_trainer(c);
    if(!qa || imported)return;
    CoopParty local;
    if(!coop_battle_capture_party(c,&local))return;
    if(!exported) {
        exported=save_party(dir,"party.bin",&local);
        LOG("[coop-battle] local party count %u digest %016llx, export %u\n",local.count,
            (unsigned long long)coop_party_digest(&local),exported);
    }
    const char *peer=ENV("LARECOMP_COOP_PARTY_TEST_PEER");CoopParty remote;
    if(!peer || !load_party(peer,&remote))return;
    uint16_t species[COOP_PARTY_MAX];bool ok=coop_battle_check_import(c,&remote,species);
    bool cloned=ok && check_party_clone(c,&remote);
    CoopParty after;bool unchanged=coop_battle_capture_party(c,&after) &&
        after.count==local.count && !memcmp(after.pokemon,local.pokemon,local.count*COOP_PB7_BYTES);
    char path[1400];snprintf(path,sizeof path,"%s/import.txt",dir);FILE *f=fopen(path,"w");
    if(f){fprintf(f,"native_roundtrip=%u\nnative_party_clone=%u\nlocal_party_unchanged=%u\npeer_count=%u\npeer_digest=%016llx\n",ok,cloned,unchanged,
        remote.count,(unsigned long long)coop_party_digest(&remote));
        for(unsigned i=0;i<remote.count;i++)fprintf(f,"species[%u]=%u\n",i,species[i]);fclose(f);}
    LOG("[coop-battle] temporary guest party: roundtrip %u, party clone %u, local party unchanged %u\n",ok,cloned,unchanged);
    imported=true;
}
// Installed on Eevee 1.0.0 in normal play: each hook passes through unless a team-up (or QA) battle is running.
void coop_battle_install_hooks(void) {
    if(supported) {
        rt_register_fnhook_chain(0x700028be80ull,mirror_frame_hook,&previous_battle_frame);
        rt_register_fnhook_chain(0x7000a5a180ull,remote_player_test_hook,&previous_remote_player);
        rt_register_fnhook_chain(0x7000737490ull,mirror_spirit_hook,&previous_spirit);
        rt_register_fnhook_chain(0x7000319bc0ull,mirror_after_battle_hook,&previous_after_battle);
        rt_register_fnhook_chain(0x70009d2ce0ull,context_update_hook,&previous_context);
        rt_register_fnhook_chain(0x70009e8a50ull,rng_seed_hook,&previous_rng_seed);
        rt_register_fnhook_chain(0x7000a5c9a0ull,npc_commit_hook,&previous_npc_commit);
        rt_register_fnhook_chain(0x7000a577f0ull,ally_select_hook,&previous_ally_select);
        rt_register_fnhook_chain(COMMAND,command_hook,&previous_command);
        rt_register_fnhook_chain(0x7000aa5660ull,move_result_hook,&previous_move_result);
        rt_register_fnhook_chain(0x7000aa5ca0ull,target_result_hook,&previous_target_result);
        rt_register_fnhook_chain(0x7000aca930ull,skill_pick_hook,&previous_skill_pick);
        rt_register_fnhook_chain(HUMAN_SELECT,human_select_hook,&previous_human_select);
        rt_register_fnhook_chain(0x7000a57ff0ull,switch_in_hook,&previous_switch_in[0]);     // human switch-in after a faint
        rt_register_fnhook_chain(0x7000a584f0ull,switch_in_hook,&previous_switch_in[1]);     // human mid-turn switch-in
        rt_register_fnhook_chain(0x7000a58000ull,ai_switch_hook,&previous_ai_switch);        // NPC switch-in (both)
        rt_register_fnhook_chain(0x70001c9490ull,item_use_hook,&previous_item_use);           // remove from the bag
        rt_register_fnhook_chain(0x70009e3e50ull,bag_check_hook,&previous_bag_check);         // Pokemon condition check
        rt_register_fnhook_chain(0x70006786b0ull,wait_text_hook,&previous_msg);
        rt_register_fnhook_chain(0x7000b0ed20ull,trainer_party_hook,&previous_trainer_party);
        rt_register_fnhook_chain(0x70009e0940ull,battle_party_hook,&previous_battle_party);
        rt_register_fnhook_chain(0x700017e9d0ull,trainer_msg_hook,&previous_trainer_msg);     // GetTrainerMsgID
        rt_register_fnhook_chain(0x70001a82e0ull,msg_end_hook,&previous_msg_end);              // IsMsgWinEnd_
        rt_register_fnhook_chain(0x700017e150ull,trainer_core_hook,&previous_trainer_core);    // CallTrainerBattleCore
        rt_register_fnhook_chain(0x700028dd20ull,process_setter_hook,&previous_process_setter);
        rt_register_fnhook_chain(0x70001c6f60ull,money_hook,&previous_money);                  // add money
        rt_register_fnhook_chain(0x70001c8e20ull,item_hook,&previous_item);                    // add to the bag
        extern void coop_battle_model_install(void);coop_battle_model_install();
    }
}
bool coop_battle_trial_model(bool *mirrored){*mirrored=mirrored_guest;return supported && trial_active;}
void coop_battle_configure(int build){supported=build==GAME_BUILD_EEVEE_100;initializing_context=0;exported=imported=late_exported=trainer_started=trial_active=battle_seen=trainer_offer_sent=false;trainer_process=world_return_frame=player_clone_party=0;entry=ENTRY_IDLE;script_battle=factory_battle=capture_process=false;mirrored_guest=remote_waiting=mirror_aborted=false;remote_round=0;memset(&native_mirror,0,sizeof native_mirror);observed_sequence=0;ally_round=0;ally_prepared=ally_native=menu_open=menu_forced=have_held=native_failed=local_done=wait_shown=false;menu_turn=ally_done_round=0;wait_text[0]=0;memset(&ally_wait,0,sizeof ally_wait);ally_publish_ai=repl_prepared=repl_native=slot0_have=slot0_pending=false;local_seq=0;memset(selected_move_slot,255,sizeof selected_move_slot);memset(selected_move_id,0,sizeof selected_move_id);}
RT_ON_RESET(coop_battle_reset){supported=false;initializing_context=0;exported=imported=late_exported=trainer_started=trial_active=battle_seen=trainer_offer_sent=false;trainer_process=world_return_frame=player_clone_party=0;entry=ENTRY_IDLE;script_battle=factory_battle=capture_process=false;mirrored_guest=remote_waiting=mirror_aborted=false;remote_round=0;memset(&native_mirror,0,sizeof native_mirror);observed_sequence=0;ally_round=0;ally_prepared=ally_native=menu_open=menu_forced=have_held=native_failed=local_done=wait_shown=false;menu_turn=ally_done_round=0;wait_text[0]=0;memset(&ally_wait,0,sizeof ally_wait);ally_publish_ai=repl_prepared=repl_native=slot0_have=slot0_pending=false;local_seq=0;memset(selected_move_slot,255,sizeof selected_move_slot);memset(selected_move_id,0,sizeof selected_move_id);}
