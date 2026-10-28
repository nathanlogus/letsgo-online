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
// Partner rewards (guest): EXP points per own Pokemon (by encryption constant), the battle's result and prize, the
// end-of-battle digest round; battle IDs already rewarded.
typedef struct {uint32_t ec,base,exp;uint16_t species;uint8_t form,level;} PartnerGain;
static PartnerGain gains[6];
static unsigned gain_count;
static bool battle_won,result_seen;
static uint32_t battle_prize,final_round;
static uint64_t ally_controller,hold_since,rewarded[8];
static unsigned rewarded_next;
static bool finishing;
static void rewards_reset(void){finishing=false;memset(gains,0,sizeof gains);gain_count=0;battle_won=result_seen=false;battle_prize=final_round=0;ally_controller=hold_since=0;}
// The EXP formula's "own Pokemon" (0x70001c9d90: MyStatus ID, gender and name against the Pokemon's OT) and the
// player's language (0x70001c9d40, MyStatus +0x8d: another language's Pokemon gets x1.7 instead of x1.5) answer for
// the local player. The partner's replay plays trainer 0 as the host: for the host's party it must answer as the
// host did (B8), or every EXP gain of a host Pokemon is x1.5 there (teamup-wild2/host-out: Lv 10 instead of 8).
static uint64_t my_status(void){return ptr(ptr(manager()+0x58)+0x78);}
static void host_identity(Cpu *cpu,uint8_t *own,uint8_t *language){
    uint64_t status=my_status(),party=ptr(manager()+0x60),at=ptr(ptr(party)+0x58);*own=0;
    *language=guest(status,0x8e)?*(uint8_t *)(uintptr_t)(status+0x8d):0;
    if(!guest(status,0x100) || !guest(party,0x90) || !guest(at,4))return;
    Cpu call=*cpu;call.x[0]=party;rt_call_original(&call,COUNT);unsigned count=(unsigned)call.x[0],n=0;
    for(unsigned i=0;i<count && n<6;i++){ // (the order of coop_battle_capture_party: occupied slots only)
        call.x[0]=party;call.x[1]=i;rt_call_original(&call,at);uint64_t mon=call.x[0];if(!guest(mon,0xb8))continue;
        call.x[0]=mon;rt_call_original(&call,SPECIES);if(!call.x[0])continue;
        call.x[0]=status;call.x[1]=mon;rt_call_original(&call,0x70001c9d90ull);if(call.x[0]&1)*own|=1u<<n;
        n++;
    }
}
static bool replay_exp; // the partner's replay is in the native EXP step (for trainer 0, the host's party)
static RuntimeFnHook previous_own,previous_language;
static int own_hook(Cpu *c,uint64_t addr){
    if(replay_exp){
        uint64_t data=ptr(ptr(c->x[1]+0x98)+0x10);uint32_t ec=guest(data,4)?*(uint32_t *)(uintptr_t)data:0;
        for(unsigned i=0;i<native_mirror.host_party.count;i++)if(!memcmp(native_mirror.host_party.pokemon[i],&ec,4)){
            c->x[0]=(native_mirror.host_own>>i)&1;return 1;}
    }
    return previous_own?previous_own(c,addr):0;
}
static int language_hook(Cpu *c,uint64_t addr){
    if(replay_exp){c->x[0]=native_mirror.host_language;return 1;}
    return previous_language?previous_language(c,addr):0;
}
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
        if(!mirrored_guest){uint8_t own,language;host_identity(c,&own,&language);coop_battle_link_mirror_seed(ptr(object+0x18),own,language);}
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
// Battle Pokemon bytes the game never initialises, so they keep whatever that heap memory held before (it differs
// per client: teamup-desync2 r1/r3/r9 differed at round 1 only there, in +0x215/+0x216 or +0x218 of an opponent
// whose count was 0): the padding of the setup
// 0x70009e2490 (+0x93..+0x97, +0x22b, +0x22f, +0x239..+0x23f, +0x25a..+0x25f, +0x27b) and the slots past the count
// of the byte set at +0x212/+0x213 (24 slots; 0x70009e3d10 adds, 0x70009e7a50 searches only below the count). Also
// bits 1-7 of +0x92: the halfword at +0x91 is a bit field whose bits 9-15 nothing sets or reads (teamup-midturn1
// host-uturn: a reserve sent in by U-turn held 0xd0 on one client, 0xd2 on the other).
static bool raw_unused(unsigned o,unsigned set){
    return (o>=0x93 && o<0x98) || (o>=0x213+set && o<0x22c) || o==0x22f || (o>=0x239 && o<0x240) || (o>=0x25a && o<0x260) || o==0x27b;
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
                // LARECOMP_COOP_TURN_RAW=1 writes turn-raw.txt in the QA directory; =<file> writes there (no QA directory needed)
                const char *rawp=ENV("LARECOMP_COOP_TURN_RAW");char rp[1400]={0};
                if(rawp && dir && *dir)snprintf(rp,sizeof rp,"%s/turn-raw.txt",dir);else if(rawp && strlen(rawp)>1)snprintf(rp,sizeof rp,"%s",rawp);
                FILE *r=rp[0]?fopen(rp,"a"):NULL;
                uint8_t set=*(uint8_t *)(uintptr_t)(mon+0x212);if(set>24)set=24;
                for(unsigned o=0x78;o<0x2e0;o+=8){uint64_t v=*(uint64_t *)(uintptr_t)(mon+o),all=v;
                    for(unsigned i=0;i<8;i++)if(raw_unused(o+i,set))v&=~(0xffull<<8*i);
                    if(o<=0x92 && 0x92<o+8)v&=~(0xfeull<<8*(0x92-o)); // (+0x92 bits 1-7, see raw_unused)
                    if(!guest(v,1))hash_field(&raw,v,8);
                    // the skipped bytes are dumped too (" unhashed <value>"), so each run can check the skip list
                    if(r)fprintf(r,"%u %u %03x %016llx%s",ally_round,trainer,o,(unsigned long long)(guest(v,1)?0:v),guest(v,1)?" ptr":"");
                    if(r)fprintf(r,v!=all && !guest(v,1)?" unhashed %016llx\n":"\n",(unsigned long long)all);}
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
// A move's target type (0x700073d500, as the battle UI 0x7000acef00 and the default-target rule 0x70009d0630 read it):
// 0 any adjacent Pokemon, 1 the user or its ally, 2 its ally, 3 one opponent, 7 the user; the rest (4 all adjacent,
// 5 all opponents, 6 all allies, 8 everyone, 9 a random opponent, 10-13 a side, the field...) choose no one. Curse
// (174) is 7, or 0 for a Ghost type (0x70009d05c0).
static unsigned move_target_type(Cpu *c,uint64_t mon,unsigned move){
    Cpu call=*c;call.x[0]=move;rt_call_original(&call,0x700073d500ull);unsigned type=(unsigned)call.x[0];
    if(move==174){call.x[0]=mon;rt_call_original(&call,0x70009d05c0ull);type=(unsigned)call.x[0];}
    return type;
}
// Positions a move may name (bit per battlefield position): the guest's native menu sends what it shows. foes: the
// living opponents' positions; own/ally: the user's and its living partner's (bit 4: no one, as the NPC sends).
static uint32_t move_targets(unsigned type,uint32_t foes,uint32_t own,uint32_t ally){
    switch(type){
    case 0:return foes|ally;
    case 1:return own|ally;
    case 2:return ally?ally:own;
    case 3:return foes;
    case 7:return own;
    default:return foes|own|ally|1u<<4;
    }
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
    // The user's position and its partner's (trainer 0's active Pokemon: the host's) when that one still stands.
    uint32_t own=0,ally=0;
    call.x[0]=ctx;call.x[1]=pool;call.x[2]=*(uint8_t *)(uintptr_t)(mon+0x87);rt_call_original(&call,0x70009d9d50ull);
    if((unsigned)call.x[0]<4)own=1u<<(unsigned)call.x[0];
    call.x[0]=pool;call.x[1]=0;call.x[2]=0;rt_call_original(&call,0x70009e14c0ull);uint64_t partner=call.x[0];
    if(guest(partner,0x2e1) && partner!=mon){
        call.x[0]=partner;rt_call_original(&call,0x70009e3a20ull);
        if(!(call.x[0]&1)){call.x[0]=ctx;call.x[1]=pool;call.x[2]=*(uint8_t *)(uintptr_t)(partner+0x87);rt_call_original(&call,0x70009d9d50ull);
            if((unsigned)call.x[0]<4)ally=1u<<(unsigned)call.x[0];}
    }
    out->kind=2;out->switches=party_switches(cpu,controller);
    // A trapped Pokemon (Mean Look, Wrap...) can't be switched out: the party list's own check (0x7000a5c200, 5 = free).
    uint8_t trapper=0;uint16_t cause=0;
    call.x[0]=controller;call.x[1]=mon;call.x[2]=(uint64_t)(uintptr_t)&trapper;call.x[3]=(uint64_t)(uintptr_t)&cause;
    if(out->switches){rt_call_original(&call,0x7000a5c200ull);
        if((uint32_t)call.x[0]!=5){LOG("[coop-ally] slot 2 is trapped (%u, move/ability %u): no switch offered\n",(unsigned)call.x[0],cause);out->switches=0;}}
    bool any=out->switches!=0;
    for(unsigned i=0;i<4;i++){
        call.x[0]=mon;call.x[1]=i;rt_call_original(&call,0x70009e4950ull);out->moves[i]=(uint16_t)call.x[0];
        if(!out->moves[i])continue;
        call.x[0]=mon;call.x[1]=i;rt_call_original(&call,0x70009e4a80ull);out->pp[i]=(uint8_t)call.x[0];
        call.x[0]=controller;call.x[1]=mon;call.x[2]=out->moves[i];call.x[3]=0;call.x[4]=0;rt_call_original(&call,0x7000a5bda0ull);
        if(call.x[0]&1)out->pp[i]=0;
        out->targets[i]=move_targets(move_target_type(cpu,mon,out->moves[i]),targets,own,ally);any|=out->pp[i] && out->targets[i];
    }
    return any;
}
static bool native_command_valid(Cpu *c,uint64_t controller,const CoopBattleMove *move){
    uint64_t mon=ptr(controller+0x78);if(!guest(mon,0x2e1) || move->move_index>=4 || move->target>4)return false;
    Cpu call=*c;call.x[0]=mon;call.x[1]=move->move_index;rt_call_original(&call,0x70009e4950ull);
    if(call.x[0]!=move->move)return false;
    call.x[0]=mon;call.x[1]=move->move_index;rt_call_original(&call,0x70009e4a80ull);if(!call.x[0])return false;
    CoopBattleChoices choices;uint16_t species[2],hp[2],maxhp[2];
    return ally_choices(c,controller,&choices,species,hp,maxhp) && (choices.targets[move->move_index]&(1u<<move->target));
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
    if(first){local_done=false;if(!mirrored_guest)LOG("[coop-battle] slot 0 action menu\n");}
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
    // After an abort the guest finishes its copy alone: the host's replacement is the NPC handler's pick, no party list.
    if(slot && mirrored_guest){rt_call_original(c,0x7000a58000ull);ally_event("mirror_aborted_npc_switch",NULL);return 1;}
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
                for(unsigned i=0;i<4;i++)if(choices.moves[i]==((packet>>8)&65535) && (choices.targets[i]&0xa)){
                    unsigned target=(packet>>4)&15,type=move_target_type(c,ptr(controller+0x78),choices.moves[i]),foes=choices.targets[i]&0xa;
                    if((type==0 || type==3) && !(foes&(1u<<target))){
                        for(target=0;target<16 && !(foes&(1u<<target));target++){}
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
        LOG("[coop-ally] native menu open, request %u, turn %u\n",kind,v.turn);
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
        unsigned mask=index<4?v.choices.targets[index]:0,foes=mask&0xa;
        if((target&1) && !(mask&(1u<<target)) && foes && !(foes&(foes-1)))for(target=0;!(foes&(1u<<target));target++){}
        sent=coop_battle_link_choose(index,target);
    }
    lend_ui(c,controller,0);
    memcpy(ctl+0x1e0,menu_saved,sizeof menu_saved);memcpy(ctl+0x288,menu_saved_commands,sizeof menu_saved_commands);
    memcpy(ctl+0x3b8,menu_saved_steps,sizeof menu_saved_steps);memcpy(ctl+0x418,menu_saved_list,sizeof menu_saved_list);menu_open=false;
    CoopBattleMove m={.move=move,.target=target,.packet=packet};ally_event(sent?"native_menu_choice":menu_forced?"native_menu_closed":"native_menu_retry",&m);
    // An item that still gets here (the Bag is refused, bag_check_hook) or an unsupported move: the menu again.
    if(!sent && !menu_forced && kind==2){LOG("[coop-ally] native menu command %08x not offered: the menu again\n",packet);menu_turn=0;return true;}
    if(sent)LOG("[coop-ally] native menu chose: command %08x\n",packet);
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
// Host: the guest's replay has stopped (its B10). The host keeps its battle; logged once.
static bool host_replay_aborted(void){
    CoopBattleMirrorSetup status;
    if(!coop_battle_link_mirror_setup(&status) || !status.aborted)return false;
    if(!mirror_aborted){mirror_aborted=true;LOG("[coop-battle] guest replay reported aborted, reason %u; the NPC chooses for the guest\n",status.reason);ally_event("mirror_aborted_host",NULL);}
    return true;
}
static int ally_select_hook(Cpu *c,uint64_t addr){
    uint64_t controller=c->x[0],state=c->x[1];
    if(!supported || !trial_active || !coop_battle_link_enabled() || !guest(controller,0x3d0) || !guest(state,4) ||
        *(uint8_t *)(uintptr_t)(controller+0x1fa)!=2)return previous_ally_select ? previous_ally_select(c,addr):0;
    unsigned phase=*(uint32_t *)(uintptr_t)state;ally_controller=controller;
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
        // Once the guest's replay has stopped it can no longer choose: the NPC decides at once, every turn.
        if(host_replay_aborted() || !ally_choices(c,controller,&choices,species,hp,maxhp)){ally_native=ally_publish_ai=true;return previous_ally_select ? previous_ally_select(c,addr):0;}
        if(!coop_ally_begin(&ally_wait,link.battle,ally_round,ally_round,&choices,now,ally_timeout())){ally_native=ally_publish_ai=true;return 0;}
        ally_prepared=true;wait_shown=local_done=false;
        if(!coop_battle_link_request(&ally_wait,species,hp,maxhp))ally_wait.deadline_ns=now;
        ally_event("ally_waiting",NULL);LOG("[coop-ally] waiting for guest, turn %u, timeout %u ms\n",ally_round,ally_timeout());
    }
    CoopBattleMove command;uint64_t received;
    if(coop_battle_link_take(&command,&received) && !coop_ally_submit(&ally_wait,&command,received))ally_event("ally_rejected",&command);
    if(link.phase!=COOP_LINK_ACTIVE || host_replay_aborted())ally_wait.deadline_ns=now;
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
        if(!mirrored_guest && mask && link.host && link.phase==COOP_LINK_ACTIVE && !host_replay_aborted()){
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
        if(link.phase!=COOP_LINK_ACTIVE || host_replay_aborted())ally_wait.deadline_ns=now;
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
// The guest's whole party replaces the ally trainer's battle party: the ally only ever sends in the guest's own Pokemon.
static void import_ally_party(Cpu *c,uint64_t party,const CoopParty *remote){
    CoopParty before,after;Cpu call=*c;uint8_t roundtrip[COOP_PB7_BYTES];
    if(!coop_battle_capture_party(&call,&before))return;
    bool exact=true;unsigned n=0;
    for(;n<remote->count;n++){
        call.x[0]=party;call.x[1]=n;rt_call_original(&call,0x7000721b00ull);uint64_t mon=call.x[0];
        if(!guest(mon,0xb8))break;
        call.x[0]=mon;call.x[1]=(uint64_t)(uintptr_t)remote->pokemon[n];rt_call_original(&call,IMPORT);
        serialize(&call,mon,roundtrip);exact&=!memcmp(roundtrip,remote->pokemon[n],sizeof roundtrip);
    }
    // (published once every Pokemon is in, as make_party_clone)
    if(n)*(uint8_t *)(uintptr_t)(party+0x88)=(uint8_t)n;
    exact&=n==remote->count;
    bool unchanged=coop_battle_capture_party(&call,&after) && before.count==after.count &&
        !memcmp(before.pokemon,after.pokemon,before.count*COOP_PB7_BYTES);
    LOG("[coop-battle] temporary ally guest party: %u of %u, exact %u local party unchanged %u\n",n,remote->count,exact,unchanged);
    journal(exact && unchanged ? "guest_lead_imported" : "guest_lead_failed",renderer_game_frame(),NULL);
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
            CoopParty remote;bool loaded=false;
            if(coop_battle_link_enabled()){uint64_t id;
                loaded=mirrored_guest?coop_battle_link_local_party(&remote):coop_battle_link_guest_party(&remote,&id);}
            else loaded=load_party(peer,&remote);
            if(loaded)import_ally_party(c,party,&remote);
        }
    }
    return 1;
}
// A native object as the setup builders make them (0x70009cd7e0): allocate, construct, allocator metadata, one reference.
static uint64_t new_native(Cpu *c,uint64_t size,uint64_t ctor){
    uint64_t alloc=allocator(),allocate=ptr(ptr(alloc)+0x10);if(!guest(alloc,8) || !guest(allocate,4))return 0;
    Cpu call=*c;call.x[0]=alloc;call.x[1]=size;call.x[2]=8;rt_call_original(&call,allocate);
    uint64_t o=call.x[0];if(!guest(o,size))return 0;
    call.x[0]=o;call.x[1]=alloc;rt_call_original(&call,ctor);
    uint64_t *m=(uint64_t *)(uintptr_t)(o+8);m[0]=alloc;m[1]=o;m[2]=size;m[3]=8;
    call.x[0]=o+0x48;rt_call_original(&call,ptr(ptr(o+0x48)+0x10));
    return o;
}
// Wild team-up: the wild setup (0x70009ccda0) gets an ally trainer as the trainer multi builder 0x70009cd7e0 sets one
// up (party +0xc0, trainer data +0x448, trainer info +0x270), with the guest's party. The battle is Let's Go's 2 vs 1
// (rule 3), whose second player position (2) is natively a second Pokemon of trainer 0 (local P2 borrows one of the
// player's own); during a wild team-up it is the ally's (rule3_ally_*). A multi battle (rule 1) needs an opponent 2:
// its empty party crashes the battle party setup (teamup-wild1/e1).
static bool wild_rule,wild_battle,wild_replay; // (wild_battle: a wild team-up or its replay runs)
static bool wild_add_ally(Cpu *c,uint64_t setup,unsigned trainer,const CoopParty *party){
    uint64_t ally=new_native(c,0x90,0x7000720f10ull),data=new_native(c,0xb0,0x70009ce5d0ull);
    if(!ally || !data)return false;
    *(uint64_t *)(uintptr_t)(setup+0xc0)=ally;*(uint64_t *)(uintptr_t)(setup+0x448)=data;
    Cpu call=*c;uint64_t alloc=allocator();
    call.x[0]=data;call.x[1]=alloc;call.x[2]=trainer;rt_call_original(&call,0x7000b0ec00ull);
    call=*c;call.x[0]=ally;call.x[1]=alloc;call.x[2]=trainer;rt_call_original(&call,0x7000b0ed20ull);
    call=*c;call.x[0]=setup+0x270;call.x[1]=alloc;call.x[2]=trainer;rt_call_original(&call,0x7000b0f3d0ull);
    import_ally_party(c,ally,party);
    *(uint32_t *)(uintptr_t)(setup+4)=3;wild_rule=true;
    return true;
}
// Rule 3's position -> trainer (0x70009e1bf0: {0,1,0,-}), positions per trainer (0x70009e1a10: trainer 0 has two) and
// which non-local trainers get an NPC controller (0x7000a54ce0: only trainer 1).
static RuntimeFnHook previous_rule_trainer,previous_rule_count,previous_rule_npc,previous_main_init;
static int rule3_ally_trainer_hook(Cpu *c,uint64_t addr){
    if(wild_rule && (uint32_t)c->x[0]==3 && (uint8_t)c->x[2]==2){c->x[0]=2;return 1;}
    return previous_rule_trainer?previous_rule_trainer(c,addr):0;
}
static int rule3_ally_count_hook(Cpu *c,uint64_t addr){
    if(wild_rule && (uint32_t)c->x[0]==3 && ((uint32_t)c->x[2]==0 || (uint32_t)c->x[2]==2)){c->x[0]=1;return 1;}
    return previous_rule_count?previous_rule_count(c,addr):0;
}
static int rule3_ally_npc_hook(Cpu *c,uint64_t addr){
    if(wild_rule && (uint32_t)c->x[0]==3 && !(uint8_t)c->x[1] && (uint32_t)c->x[3]==2){c->x[0]=1;return 1;}
    return previous_rule_npc?previous_rule_npc(c,addr):0;
}
// The battle main module's init for every mode but a multi (0x70009d4490, step word x1) builds the server's (+0x140) and
// the client's (+0x148) battle parties of trainers 0 and 1 only (steps 2-5): the ally's (2) follow theirs. The multi init
// (0x70009d4970) would build them by presence but sets up the opponent as a trainer. The ally's trainer data is
// loaded by step 1 (0x70009d4d60, setup +0x448 when present).
static int main_init_hook(Cpu *c,uint64_t addr){
    uint64_t main=c->x[0],step=c->x[1];unsigned at=guest(step,4)?*(uint32_t *)(uintptr_t)step:99;
    if(!previous_main_init || !previous_main_init(c,addr))rt_call_original(c,addr);
    if(wild_rule && at==5)for(unsigned client=0;client<2;client++){
        Cpu call=*c;call.x[0]=main;call.x[1]=ptr(main+0x140+8*client);call.x[2]=2;call.x[3]=client;rt_call_original(&call,0x70009d5d50ull);
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
// Partner rewards (docs/COOP-ONLINE.md#team-up-trainer-battles). The guest's engine is the host's battle replayed,
// so its own native result stands in for the host's once the replay is proven in sync to the end; nothing is
// recomputed from messages. EXP: at each knock-out the battle's EXP step (0x70009f2970) gives points to the local
// player's party only (0x70009da090 -> trainer 0: on the guest, the host's copy). For the guest's own party
// (trainer 2) the same native calculation (0x7000a11a30: participation, the share of the rest of the party, Let's Go's
// boosts) runs on each opponent knocked out, before the step marks it done. Nothing is written to the battle (it
// must stay the host's): the points are kept, with the level they raise, which the next knock-out's calculation reads.
static uint32_t mon_ec(uint64_t param){uint64_t data=ptr(ptr(param+0x98)+0x10);return guest(data,4)?*(uint32_t *)(uintptr_t)data:0;}
static RuntimeFnHook previous_exp_step,previous_result,previous_affection;
// The affection bonus (0x70009dadc0: x1.2 from 100 friendship) goes to trainer 0's Pokemon only, as an NPC partner's
// are not the player's. The partner's own are (Let's Go's local co-op keeps P2's in trainer 0 too): during the
// partner's calculation the same rule runs without that one check, on the Pokemon's own friendship (0x7000727fd0;
// the battle copies it to +0x2e2 for the player's Pokemon only, an NPC partner's hold 0).
static bool partner_calc;
static int affection_hook(Cpu *c,uint64_t addr){
    if(!partner_calc)return previous_affection?previous_affection(c,addr):0;
    uint64_t main=c->x[0],mon=c->x[1];Cpu call=*c;bool none=*(uint32_t *)(uintptr_t)ptr(main+0x80)>1;
    static const uint64_t states[3]={0x70009e7c50ull,0x70009e6860ull,0x70009e6870ull};
    for(unsigned i=0;i<3 && !none;i++){call.x[0]=mon;rt_call_original(&call,states[i]);none=call.x[0]&1;}
    if(!none){call.x[0]=ptr(mon+0x60);rt_call_original(&call,0x7000727fd0ull);}
    c->x[0]=none?0:(uint8_t)call.x[0];return 1;
}
static void partner_exp(Cpu *cpu,uint64_t server){
    uint64_t main=ptr(server+0x60),pool=ptr(server+0x68),list=server+0x1354;Cpu call=*cpu;
    call.x[0]=main;rt_call_original(&call,0x70009d8ae0ull);if(!(call.x[0]&1))return; // this battle gives EXP
    call.x[0]=pool;call.x[1]=2;rt_call_original(&call,0x70009e0e10ull);uint64_t party=call.x[0];if(!guest(party,8))return;
    call.x[0]=main;rt_call_original(&call,0x70009d71c0ull);uint64_t mode=call.x[0];
    call.x[0]=list;call.x[1]=0;rt_call_original(&call,0x7000a66000ull);unsigned dead=(uint8_t)call.x[0];
    for(unsigned i=0;i<dead;i++){
        call.x[0]=list;call.x[1]=0;call.x[2]=i;rt_call_original(&call,0x7000a66080ull);if(call.x[0]&1)continue; // done
        call.x[0]=list;call.x[1]=0;call.x[2]=i;rt_call_original(&call,0x7000a66030ull);uint32_t id=(uint32_t)call.x[0];
        call.x[0]=main;call.x[1]=id;rt_call_original(&call,0x70009dab70ull);if((uint8_t)call.x[0]!=1)continue; // an opponent
        call.x[0]=pool;call.x[1]=id;rt_call_original(&call,0x70009e0e90ull);uint64_t foe=call.x[0];if(!guest(foe,0x2e2))continue;
        call.x[0]=party;rt_call_original(&call,0x70009df300ull);unsigned n=(uint8_t)call.x[0];if(n>6)n=6;
        uint64_t mon[6]={0};uint8_t level[6];unsigned slot[6];
        for(unsigned j=0;j<n;j++){
            call.x[0]=party;call.x[1]=j;rt_call_original(&call,0x70009df450ull);if(!guest(call.x[0],0x2e2))continue;
            uint64_t m=call.x[0];uint32_t ec=mon_ec(ptr(m+0x60));unsigned g=0;
            while(g<gain_count && gains[g].ec!=ec)g++;
            if(g==gain_count){if(g==6)continue;gain_count++;
                gains[g]=(PartnerGain){.ec=ec,.base=*(uint32_t *)(uintptr_t)(m+0x74),.species=*(uint16_t *)(uintptr_t)(m+0x78),
                    .form=*(uint8_t *)(uintptr_t)(m+0x2e1),.level=*(uint8_t *)(uintptr_t)(m+0x86)};}
            mon[j]=m;slot[j]=g;level[j]=*(uint8_t *)(uintptr_t)(m+0x86);*(uint8_t *)(uintptr_t)(m+0x86)=gains[g].level;
        }
        int32_t out[24]={0};
        call.x[0]=server+0x2714;call.x[1]=mode;call.x[2]=server;call.x[3]=party;call.x[4]=foe;call.x[5]=(uint64_t)(uintptr_t)out;
        partner_calc=true;rt_call_original(&call,0x7000a11a30ull);partner_calc=false;
        char text[200];int at=snprintf(text,sizeof text,"opponent %u (species %u):",id,*(uint16_t *)(uintptr_t)(foe+0x78));
        for(unsigned j=0;j<n;j++){
            if(!mon[j])continue;*(uint8_t *)(uintptr_t)(mon[j]+0x86)=level[j];
            PartnerGain *g=&gains[slot[j]];
            // (as the step: points for a Pokemon still standing, below level 100)
            call.x[0]=mon[j];rt_call_original(&call,0x70009e3f10ull);
            if(out[4*j]<=0 || !(call.x[0]&1) || g->level>=100)continue;
            g->exp+=(uint32_t)out[4*j];
            for(;g->level<100;g->level++){
                call.x[0]=g->species;call.x[1]=g->form;call.x[2]=g->level+1u;rt_call_original(&call,0x70009d00e0ull);
                if(g->base+g->exp<(uint32_t)call.x[0])break;
            }
            if(at<(int)sizeof text)at+=snprintf(text+at,sizeof text-at," slot %u +%d (lv %u)",j,out[4*j],g->level);
        }
        LOG("[coop-battle] partner EXP, %s\n",text);
        if(dir_set()){char e[256];snprintf(e,sizeof e,"partner_exp %s",text);journal(e,renderer_game_frame(),NULL);}
    }
}
static int exp_step_hook(Cpu *c,uint64_t addr){
    if(!supported || !trial_active || !mirrored_guest)return previous_exp_step?previous_exp_step(c,addr):0;
    if(!mirror_aborted && guest(c->x[0],0x2ac0))partner_exp(c,c->x[0]);
    replay_exp=true;if(!previous_exp_step || !previous_exp_step(c,addr))rt_call_original(c,addr);replay_exp=false;
    return 1;
}
// The battle's result (0x70009d3870 sets setup +0x508, 1 = won, and the prize, +0x16c here): on both sides a last
// digest, so the guest's rewards come only from a replay that ended exactly as the host's battle did.
static int result_hook(Cpu *c,uint64_t addr){
    uint64_t main=c->x[0];
    if(!previous_result || !previous_result(c,addr))rt_call_original(c,addr);
    if(supported && trial_active && !final_round && guest(main,0x200)){
        uint64_t setup=ptr(main+0x80);
        result_seen=true;battle_won=guest(setup,0x50c) && *(uint32_t *)(uintptr_t)(setup+0x508)==1;
        // (a wild team-up caught by the host's ball, result 5: the catch is the host's; on the partner's replay it ends
        // like a run, so wildbattle_classic.c's capture registration never puts it in the partner's save)
        if(mirrored_guest && wild_battle && guest(setup,0x50c) && *(uint32_t *)(uintptr_t)(setup+0x508)==5){
            *(uint32_t *)(uintptr_t)(setup+0x508)=3;LOG("[coop-battle] the host caught the wild Pokemon: the catch is the host's\n");}
        battle_prize=battle_won?*(uint32_t *)(uintptr_t)(main+0x16c):0;
        if(ally_controller && coop_battle_link_mirror_enabled()){final_round=++ally_round;coop_battle_link_mirror_digest(final_round,journal_turn_state(c,ally_controller));}
        LOG("[coop-battle] battle result %u, prize %u, final round %u, kind %u\n",guest(setup,0x50c)?*(uint32_t *)(uintptr_t)(setup+0x508):0,battle_prize,
            final_round,guest(setup,4)?*(uint32_t *)(uintptr_t)setup:0);
    }
    return 1;
}
bool coop_battle_partner_finishing(void){return finishing;}
// The partner's replay, before its result: the host's battle, whose records (a catch: total captured...) are the host's.
bool coop_battle_replay_in_battle(void){return supported && trial_active && mirrored_guest && !result_seen;}
// Guest, at the result phase: -1 while the host's end is not in yet, 0 no rewards, 1 applied. Rewards need the host's
// finished end (every command received), the end-of-battle digest matched and a won battle; an abort, a lost link or a
// loss gives none. Once per battle ID; they go to the in-memory save only, saved when the player saves.
static int partner_rewards(Cpu *c,uint64_t proc){
    CoopBattleMirrorSetup s;bool ready=coop_battle_link_mirror_setup(&s);uint64_t now=plat_now_ns();const char *why=NULL;finishing=false;
    if(mirror_aborted || s.aborted)why="replay aborted";
    else if(!battle_won)why="battle not won";
    else if(!s.ended || !coop_battle_link_mirror_matched(final_round)){
        // The replay presses through its messages and usually ends first; the host reads its own. The wait ends with
        // the host's end, a lost link (an abort) or 2 minutes; meanwhile a notice says what the black screen is.
        if(!hold_since)hold_since=now;
        if((finishing=ready && now-hold_since<120000000000ull))return -1;
        why=s.ended?"final state not confirmed":"no finished end from the host";
    }
    else for(unsigned i=0;i<8;i++)if(rewarded[i]==s.battle)why="already rewarded";
    if(why){LOG("[coop-battle] partner rewards: none (%s)\n",why);journal("partner_rewards_none",renderer_game_frame(),NULL);return 0;}
    rewarded[rewarded_next++%8]=s.battle;
    // (from here the result phase writes the guest's own party: the replay's guard against partner writes is lifted)
    memset(saved_party_buffers,0,sizeof saved_party_buffers);
    uint64_t mgr=manager(),party=ptr(mgr+0x60),at=ptr(ptr(party)+0x58);uint8_t *p=(uint8_t *)(uintptr_t)proc;bool evolve=false;
    Cpu call=*c;call.x[0]=party;rt_call_original(&call,COUNT);unsigned n=(unsigned)call.x[0];
    for(unsigned i=0;i<n && i<6 && guest(at,4);i++){
        p[i+0xd8]=p[i+0xde]=0;
        call.x[0]=party;call.x[1]=i;rt_call_original(&call,at);uint64_t mon=call.x[0];if(!guest(mon,0xb8))continue;
        uint32_t ec=mon_ec(mon);unsigned g=0;while(g<gain_count && (gains[g].ec!=ec || !gains[g].exp))g++;
        if(g==gain_count)continue;
        call.x[0]=mon;rt_call_original(&call,0x7000726710ull);unsigned before=(uint8_t)call.x[0];
        // The game's own level-up outside battle (Exp. Candy, 0x70008319c0): one level per call, then the rest.
        uint32_t left=gains[g].exp;uint8_t info[0x20];
        for(unsigned k=0;k<100;k++){call.x[0]=0;call.x[1]=mon;call.x[2]=(uint64_t)(uintptr_t)&left;call.x[3]=(uint64_t)(uintptr_t)info;
            rt_call_original(&call,0x70008319c0ull);if(!(call.x[0]&1))break;}
        call.x[0]=mon;rt_call_original(&call,0x7000726710ull);unsigned after=(uint8_t)call.x[0];
        p[i+0xd8]=after>before;evolve|=after>before; // the native evolution check for a Pokemon that levelled up (state 1)
        LOG("[coop-battle] partner reward: party slot %u +%u EXP, level %u -> %u\n",i,gains[g].exp,before,after);
    }
    uint64_t wallet=ptr(ptr(mgr+0x58)+0x90);
    if(battle_prize && guest(wallet,8)){call.x[0]=wallet;call.x[1]=battle_prize;rt_call_original(&call,0x70001c6f60ull);}
    LOG("[coop-battle] partner reward: +%u money (battle %016llx)\n",battle_prize,(unsigned long long)s.battle);
    journal("partner_rewards_applied",renderer_game_frame(),NULL);
    *(uint32_t *)(uintptr_t)(proc+0xe4)=0;*(uint32_t *)(uintptr_t)(proc+0x68)=evolve?1:4;
    return 1;
}
static int mirror_after_battle_hook(Cpu *c,uint64_t addr){
    uint64_t proc=c->x[0];bool start=supported && trial_active && guest(proc,0xf0) && *(uint32_t *)(uintptr_t)(proc+0x68)==0;
    if(start && player_clone_party){
        // This common result phase applies the battle party (here the host's copy) and its money, and starts local
        // evolutions: all discarded. The partner's own rewards replace them, else the terminal state completes.
        int r=partner_rewards(c,proc);
        if(r<0){c->x[0]=0;return 1;}
        if(!r){*(uint32_t *)(uintptr_t)(proc+0x68)=4;journal("mirror_results_discarded",renderer_game_frame(),NULL);}
    }
    // Host: its battle is over; the guest's rewards wait for this finished end.
    else if(start && !mirrored_guest)coop_battle_link_end();
    return previous_after_battle?previous_after_battle(c,addr):0;
}
// The battle builds trainer 0's party twice, for the server's pool and the client's (0x70009d5d50: setup copies at
// main +0xe0 / +0x100), and a level-up is applied to each with its own party's Pokemon. One clone shared by both
// pools gets the second level-up on top of the first (teamup-wild2/host-out4: the replay's HP 3 above the host's).
static uint64_t client_clone_party,clone_pool;
static int battle_party_hook(Cpu *c,uint64_t addr) {
    uint64_t clone_reference=0;
    const char *fixture=ENV("LARECOMP_COOP_BATTLE_CLONE_PARTY");
    if(supported && trial_active && c->x[1]==0 && (fixture || mirrored_guest)){
        CoopParty source;
        bool loaded=mirrored_guest?(source=native_mirror.host_party,true):load_party(fixture,&source);
        if(!player_clone_party){
            if(loaded){remember_saved_party_buffers(c);player_clone_party=make_party_clone(c,&source);clone_pool=c->x[0];}
        } else if(c->x[0]!=clone_pool && !client_clone_party && loaded)client_clone_party=make_party_clone(c,&source);
        uint64_t clone=c->x[0]==clone_pool?player_clone_party:client_clone_party;
        if(clone){
            // The pool retains its own shared reference. Keep our reference
            // until overworld return so QA can inspect the temporary results.
            clone_reference=clone;c->x[2]=(uint64_t)(uintptr_t)&clone_reference;
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
    LOG("[coop-battle] %s %u: solo battle (%s)\n",coop_battle_wild(entry_trainer)?"wild":"trainer",entry_trainer&0x7fff,why);journal(why,renderer_game_frame(),NULL);
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
// The room's host, tagged up with a partner nearby, no team-up or link running: a battle may be offered.
static bool teamup_ready(const char *what,unsigned id){
    CoopSession s;coop_net_session(&s);CoopTeamUp team;coop_net_teamup(&team);CoopBattleLinkView v;coop_battle_link_view(&v);
    bool ok=supported && coop_battle_link_enabled() && entry==ENTRY_IDLE && !trial_active && s.connection==COOP_CONNECTED &&
        s.local_slot==s.host_slot && team.state==2 && v.phase==COOP_LINK_NONE && coop_net_remote_near(3000);
    if(!ok && supported && coop_battle_link_enabled() && s.connection==COOP_CONNECTED)
        LOG("[coop-battle] %s %u: no team-up offer (connection %u host %u tag %u link %u near %u)\n",what,id,s.connection,
            s.local_slot==s.host_slot,team.state,v.phase,coop_net_remote_near(3000));
    return ok;
}
// Offer, then hold the battle (the trainer's text, a wild battle's start) up to 10 s for the partner's answer.
static void entry_offer(const char *what,unsigned trainer,const char *name){
    uint64_t now=plat_now_ns();
    if(coop_battle_link_offer(now,ally_timeout(),trainer,10000)){
        entry=ENTRY_OFFERED;entry_trainer=trainer;entry_deadline=now+10000000000ull;
        LOG("[coop-battle] %s %u (%s): team-up offered, holding the battle up to 10 s\n",what,trainer&0x7fff,name);journal("entry_offered",renderer_game_frame(),NULL);
    } else LOG("[coop-battle] %s %u: no team-up offer (Team-up battles off)\n",what,trainer&0x7fff);
}
static int trainer_msg_hook(Cpu *c,uint64_t addr){
    uint64_t params=c->x[1];unsigned trainer=(unsigned)ptr(params+8);
    // Pre-battle text only (type 0; an already-beaten trainer shows type 2). Master trainers (452-597) are excluded.
    if(guest(params,24) && ptr(params+16)==0 && trainer>=1 && trainer<=601 && (trainer<452 || trainer>597) && teamup_ready("trainer",trainer))
        entry_offer("trainer",trainer,coop_battle_trainer_name(trainer));
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
// {1,T,T,T} has two copies of the trainer: the prize (0x70009d08d0, both opponents' base x level x 4, kept at the
// battle's +0x16c when it starts) is twice the solo one. Halved, each player gets one trainer's prize; Pay Day is
// counted apart (+0x170) and stays whole.
static RuntimeFnHook previous_prize,previous_item;
static int prize_hook(Cpu *c,uint64_t addr){
    if(!previous_prize || !previous_prize(c,addr))rt_call_original(c,addr);
    if(supported && trial_active){
        LOG("[coop-battle] team-up prize %u -> %u (one trainer's)\n",(unsigned)c->x[0],(unsigned)c->x[0]/2);
        c->x[0]=(uint32_t)c->x[0]/2;journal("prize_halved",renderer_game_frame(),NULL);
    }
    return 1;
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
// Wild team-ups (docs/COOP-TRAINER-ENCOUNTERS.md section 11). A classic wild battle (wildbattle.c's synthetic
// WILD_HASH encounters with the battle Bag; never a story static, whose script, flags and GO capture stay the host's) is
// offered at the touch, while both players are still on the overworld (the relay's tag check), and its battle event
// (0x700028be80, kind 2) is held at state 0, before the setup, up to 10 s. An accepted, validated party turns the wild
// setup (wild_setup_hook) into the 2 vs 1 with the guest's party as the ally. The partner replays it from the host's own
// wild Pokemon (B7 chunks 12-13), started like the host's.
#define WILD_HASH 0x57494c4442415454ull
#define WILD_ALLY 54 // the ally's trainer data (an ordinary Bug Catcher): its NPC chooses at a deadline; drawn as the partner
extern bool wildbattle_classic_enabled(void);
extern bool wildbattle_replay(Cpu *c,unsigned species,unsigned level);
static RuntimeFnHook previous_wild_event,previous_wild_setup;
void coop_battle_wild_touch(unsigned species){
    if(species>=1 && species<=809 && wildbattle_classic_enabled() && teamup_ready("wild",species))
        entry_offer("wild",COOP_BATTLE_WILD|species,coop_battle_species_name(species));
}
static int wild_event_hook(Cpu *c,uint64_t addr){
    uint64_t ev=c->x[0],cfg=guest(ev,0x740)?ptr(ev+0xc0):0;
    if(supported && entry==ENTRY_OFFERED && coop_battle_wild(entry_trainer) && guest(cfg,0x2b8) && *(uint32_t *)(uintptr_t)(ev+0x68)==0 &&
        *(uint32_t *)(uintptr_t)(ev+0xd0)==2 && ptr(cfg+0x2b0)==WILD_HASH && entry_poll(c)){c->x[0]=0;return 1;}
    return previous_wild_event?previous_wild_event(c,addr):0;
}
// The native Pokemon at party slot i (0x7000721b00).
static uint64_t party_mon(Cpu *c,uint64_t party,unsigned i){
    if(!guest(party,0x90))return 0;Cpu call=*c;call.x[0]=party;call.x[1]=i;rt_call_original(&call,0x7000721b00ull);return guest(call.x[0],0xb8)?call.x[0]:0;
}
static int wild_setup_hook(Cpu *c,uint64_t addr){
    uint64_t setup=c->x[0];
    if(!previous_wild_setup || !previous_wild_setup(c,addr))rt_call_original(c,addr);
    bool host=entry==ENTRY_READY && coop_battle_wild(entry_trainer);
    wild_rule=false;
    if(!supported || !guest(setup,0x640))return 1;
    uint64_t wild=party_mon(c,ptr(setup+0xb8),0);CoopParty party,host_party;uint64_t id;uint8_t pb7[COOP_PB7_BYTES];Cpu call=*c;
    if(host){
        entry=ENTRY_IDLE;
        // The partner's replay builds its own wild Pokemon from the record; this one (PID, nature, IVs...) replaces it.
        if(!wild || !coop_battle_link_guest_party(&party,&id) || !coop_battle_capture_party(c,&host_party) ||
            !coop_battle_link_mirror_party(&host_party) || (serialize(&call,wild,pb7),!coop_battle_link_mirror_wild(pb7)) ||
            !wild_add_ally(c,setup,WILD_ALLY,&party)){entry=ENTRY_READY;entry_solo("entry_mirror_failed");return 1;}
        trainer_started=trial_active=script_battle=wild_battle=true;
        LOG("[coop-battle] wild %u launched as a team-up\n",entry_trainer&0x7fff);journal("trainer_started",renderer_game_frame(),NULL);
    } else if(wild_replay){
        wild_replay=false;
        if(wild && coop_battle_link_local_party(&party)){
            call.x[0]=wild;call.x[1]=(uint64_t)(uintptr_t)native_mirror.wild;rt_call_original(&call,IMPORT);
            wild_battle=wild_add_ally(c,setup,WILD_ALLY,&party);
        }
        LOG("[coop-battle] wild team-up replay set up: %u\n",wild_battle);
        if(!wild_battle){CoopBattleMirrorSetup bad={.aborted=true,.reason=COOP_MIRROR_BAD_PARTY};mirror_abort(&bad,true);}
    } else {
        // QA, one offline game: LARECOMP_COOP_WILD_TEST_ALLY=<trainer> + LARECOMP_COOP_PARTY_TEST_PEER (the ally's party).
        const char *qa=ENV("LARECOMP_COOP_WILD_TEST_ALLY"),*peer=ENV("LARECOMP_COOP_PARTY_TEST_PEER");
        if(qa && peer && load_party(peer,&party) && wild_add_ally(c,setup,(unsigned)strtoul(qa,NULL,10),&party)){
            trial_active=true;LOG("[coop-battle] QA wild battle with an ally (trainer %s)\n",qa);
        }
    }
    return 1;
}
// Guest: once accepted, no wild Pokemon starts the partner's own battle until the replay is over (the host's battle
// would wait for it each turn); runtime/hle/wildbattle.c swallows the touch, in both wild modes.
// The 2 vs 1's second position reads P2's controller (runtime/hle/input.c): the partner's own menu there is theirs.
bool coop_battle_menu_on_p2(void){return supported && menu_open && wild_battle && mirrored_guest;}
bool coop_battle_encounter_blocked(void){
    CoopBattleLinkView v;coop_battle_link_view(&v);
    return supported && (mirrored_guest || (!v.host && v.phase==COOP_LINK_ACTIVE));
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
    if(mirror_ready && coop_battle_wild(trainer)){
        // The same synthetic wild battle as the host's; its setup takes the host's wild Pokemon (wild_setup_hook).
        uint16_t species[6];Cpu call=*c;uint64_t mon[0xb8/8]={0},m=(uint64_t)(uintptr_t)mon;
        if(!coop_battle_check_import(c,&setup.host_party,species) || !guest(allocator(),8))return;
        call.x[0]=m;call.x[1]=allocator();rt_call_original(&call,CONSTRUCT);
        call.x[0]=m;call.x[1]=(uint64_t)(uintptr_t)setup.wild;rt_call_original(&call,IMPORT);
        call.x[0]=m;rt_call_original(&call,0x7000726710ull);unsigned level=(uint8_t)call.x[0];
        call.x[0]=m;rt_call_original(&call,DESTROY);
        native_mirror=setup;mirrored_guest=trainer_started=trial_active=script_battle=wild_replay=true;
        if(!wildbattle_replay(c,trainer&0x7fff,level)){mirrored_guest=trainer_started=trial_active=script_battle=wild_replay=false;return;}
        LOG("[coop-battle] replaying the host's wild team-up (species %u, level %u)\n",trainer&0x7fff,level);journal("trainer_started",renderer_game_frame(),NULL);
        return;
    }
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
            trainer_offer_sent=coop_battle_link_offer(plat_now_ns(),ally_timeout(),trainer,0);return;
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
        coop_battle_names_load(*(const uint32_t *)(uintptr_t)0x7001614b34ull); // the game's message language
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
            if(client_clone_party)release_party_clone(c,client_clone_party);client_clone_party=clone_pool=0;
        }
        // The QA FadeIn is for factory-started battles only; a trainer's script runs its own FadeIn_.
        if(!factory_battle)world_return_frame=0,journal("world_fade_restored",renderer_game_frame(),NULL);
        if(script_battle)trainer_started=false;
        wild_battle=wild_rule=false;
        trainer_process=0;battle_seen=trial_active=factory_battle=script_battle=false;
        if(mirrored_guest){coop_battle_link_mirror_clear();mirrored_guest=remote_waiting=false;}
        else coop_battle_link_end();
        mirror_aborted=false;menu_open=menu_forced=have_held=native_failed=local_done=wait_shown=false;menu_turn=ally_done_round=0;wait_text[0]=0;
        ally_publish_ai=repl_prepared=repl_native=slot0_have=slot0_pending=false;local_seq=remote_round=ally_round=0;rewards_reset();
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
        rt_register_fnhook_chain(0x70009d08d0ull,prize_hook,&previous_prize);                  // a trainer battle's prize
        rt_register_fnhook_chain(0x70009f2970ull,exp_step_hook,&previous_exp_step);            // EXP at a knock-out
        rt_register_fnhook_chain(0x70009d3870ull,result_hook,&previous_result);                // the battle's result
        rt_register_fnhook_chain(0x70009dadc0ull,affection_hook,&previous_affection);          // the affection EXP bonus
        rt_register_fnhook_chain(0x70001c8e20ull,item_hook,&previous_item);                    // add to the bag
        rt_register_fnhook_chain(0x70001c9d90ull,own_hook,&previous_own);                      // "own Pokemon" (EXP)
        rt_register_fnhook_chain(0x70001c9d40ull,language_hook,&previous_language);            // player's language
        rt_register_fnhook_chain(0x700028be80ull,wild_event_hook,&previous_wild_event);        // battle event (the wild hold)
        rt_register_fnhook_chain(0x70009ccda0ull,wild_setup_hook,&previous_wild_setup);        // a wild battle's setup
        rt_register_fnhook_chain(0x70009e1bf0ull,rule3_ally_trainer_hook,&previous_rule_trainer); // position -> trainer
        rt_register_fnhook_chain(0x70009e1a10ull,rule3_ally_count_hook,&previous_rule_count);     // positions per trainer
        rt_register_fnhook_chain(0x70009d4490ull,main_init_hook,&previous_main_init);          // battle main module init
        rt_register_fnhook_chain(0x7000a54ce0ull,rule3_ally_npc_hook,&previous_rule_npc);       // NPC controllers
        extern void coop_battle_model_install(void);coop_battle_model_install();
    }
}
bool coop_battle_trial_model(bool *mirrored){*mirrored=mirrored_guest;return supported && trial_active;}
void coop_battle_configure(int build){supported=build==GAME_BUILD_EEVEE_100;initializing_context=0;exported=imported=late_exported=trainer_started=trial_active=battle_seen=trainer_offer_sent=false;trainer_process=world_return_frame=player_clone_party=0;entry=ENTRY_IDLE;script_battle=factory_battle=capture_process=wild_battle=wild_replay=wild_rule=false;mirrored_guest=remote_waiting=mirror_aborted=false;remote_round=0;memset(&native_mirror,0,sizeof native_mirror);observed_sequence=0;ally_round=0;ally_prepared=ally_native=menu_open=menu_forced=have_held=native_failed=local_done=wait_shown=false;menu_turn=ally_done_round=0;wait_text[0]=0;memset(&ally_wait,0,sizeof ally_wait);ally_publish_ai=repl_prepared=repl_native=slot0_have=slot0_pending=false;local_seq=0;memset(selected_move_slot,255,sizeof selected_move_slot);memset(selected_move_id,0,sizeof selected_move_id);rewards_reset();}
RT_ON_RESET(coop_battle_reset){supported=false;initializing_context=0;exported=imported=late_exported=trainer_started=trial_active=battle_seen=trainer_offer_sent=false;trainer_process=world_return_frame=player_clone_party=0;entry=ENTRY_IDLE;script_battle=factory_battle=capture_process=wild_battle=wild_replay=wild_rule=false;mirrored_guest=remote_waiting=mirror_aborted=false;remote_round=0;memset(&native_mirror,0,sizeof native_mirror);observed_sequence=0;ally_round=0;ally_prepared=ally_native=menu_open=menu_forced=have_held=native_failed=local_done=wait_shown=false;menu_turn=ally_done_round=0;wait_text[0]=0;memset(&ally_wait,0,sizeof ally_wait);ally_publish_ai=repl_prepared=repl_native=slot0_have=slot0_pending=false;local_seq=0;memset(selected_move_slot,255,sizeof selected_move_slot);memset(selected_move_id,0,sizeof selected_move_id);rewards_reset();}
