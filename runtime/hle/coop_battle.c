// Eevee 1.0.0 battle preflight. Native allocation/accessor calls run only on the
// game thread. The temporary Pokemon never enter the host's party or storage.
#include <stdio.h>
#include <string.h>
#include "../hle.h"
#include "../gamesetup.h"
#include "../fnhook.h"
#include "coop_battle.h"
void rt_call_original(Cpu *,uint64_t);
uint64_t renderer_game_frame(void);
static bool supported,exported,imported;
static bool trainer_started,battle_seen;
static uint64_t trainer_process,world_return_frame;
static RuntimeFnHook previous_command,previous_trainer_party,previous_battle_party;
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
static bool guest(uint64_t p,size_t n) {return p>=GUEST_LO && p<GUEST_HI && n<=GUEST_HI-p;}
static uint64_t ptr(uint64_t p){return guest(p,8) ? *(const uint64_t *)(uintptr_t)p : 0;}
static uint64_t manager(void){return ptr(ptr(ptr(WORLD_GOT))+0x98);}
static uint64_t allocator(void){return ptr(ptr(ptr(ALLOC_GOT))+0x68);}
static void serialize(Cpu *c,uint64_t mon,uint8_t data[COOP_PB7_BYTES]) {
    c->x[0]=mon;rt_call_original(c,BEGIN);uint64_t changed=c->x[0]&1;
    c->x[0]=mon;c->x[1]=(uint64_t)(uintptr_t)data;rt_call_original(c,SERIALIZE);
    c->x[0]=mon;c->x[1]=changed;rt_call_original(c,END);
}
bool coop_battle_capture_party(Cpu *c,CoopParty *out) {
    if(!supported || !c || !out)return false;
    memset(out,0,sizeof *out);Cpu call=*c;
    uint64_t party=ptr(manager()+0x60),vtable=ptr(party),at=ptr(vtable+0x58);
    if(!guest(party,0x90) || !guest(at,4))return false;
    call.x[0]=party;rt_call_original(&call,COUNT);unsigned count=(unsigned)call.x[0];
    if(!count || count>COOP_PARTY_MAX)return false;
    for(unsigned i=0;i<count;i++) {
        call.x[0]=party;call.x[1]=i;rt_call_original(&call,at);uint64_t mon=call.x[0];
        if(!guest(mon,0xb8))return false;
        serialize(&call,mon,out->pokemon[i]);
    }
    out->count=count;return coop_party_valid(out);
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
static void journal(const char *event,uint64_t frame,const uint32_t *choice) {
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");if(!dir || !*dir)return;
    char path[1400];snprintf(path,sizeof path,"%s/battle.jsonl",dir);FILE *f=fopen(path,"a");if(!f)return;
    fprintf(f,"{\"event\":\"%s\",\"frame\":%llu",event,(unsigned long long)frame);
    if(choice)fprintf(f,",\"menu_choice\":%u",*choice);
    fprintf(f,"}\n");fclose(f);
}
static int command_hook(Cpu *c,uint64_t addr) {
    if(!supported)return previous_command ? previous_command(c,addr) : 0;
    uint64_t frame=c->x[0];
    if(!previous_command || !previous_command(c,addr))rt_call_original(c,addr);
    Cpu keep=*c;c->x[0]=frame;rt_call_original(c,OWNER);uint64_t owner=c->x[0];*c=keep;
    // This UI action can be destroyed when the Fight submenu opens. Copy
    // its value now; retaining its pointer across presents reads recycled memory.
    uint64_t choice=ptr(owner+0x78);uint32_t value=0;
    if(guest(choice,0x14))memcpy(&value,(const void *)(uintptr_t)(choice+0x10),sizeof value);
    battle_seen=true;journal("top_level_choice",renderer_game_frame(),&value);return 1;
}
static int trainer_party_hook(Cpu *c,uint64_t addr) {
    uint64_t party=c->x[0];unsigned id=(unsigned)c->x[2];
    if(!previous_trainer_party || !previous_trainer_party(c,addr))rt_call_original(c,addr);
    if(supported && trainer_started && guest(party,0x90)) {
        LOG("[coop-battle] trainer party id %u count %u\n",id,*(uint8_t *)(uintptr_t)(party+0x88));
        // Opt-in ownership experiment: replace only the temporary NPC ally's
        // lead. Its party belongs to this battle setup, never to SaveData.
        const char *peer=ENV("LARECOMP_COOP_PARTY_TEST_PEER");
        if(getenv("LARECOMP_COOP_TRAINER_TEST_GUEST") && peer &&
            party==ptr(trainer_process+0xe0+0xc0)) {
            CoopParty remote,before,after;Cpu call=*c;uint8_t roundtrip[COOP_PB7_BYTES];
            if(load_party(peer,&remote) && coop_battle_capture_party(&call,&before)) {
                call.x[0]=party;call.x[1]=0;rt_call_original(&call,ptr(ptr(party)+0x58));
                uint64_t mon=call.x[0];
                if(guest(mon,0xb8)) {
                    call.x[0]=mon;call.x[1]=(uint64_t)(uintptr_t)remote.pokemon[0];rt_call_original(&call,IMPORT);
                    serialize(&call,mon,roundtrip);
                    bool unchanged=coop_battle_capture_party(&call,&after) && before.count==after.count &&
                        !memcmp(before.pokemon,after.pokemon,before.count*COOP_PB7_BYTES);
                    bool exact=!memcmp(roundtrip,remote.pokemon[0],sizeof roundtrip);
                    LOG("[coop-battle] temporary ally guest lead: exact %u local party unchanged %u\n",exact,unchanged);
                    journal(exact && unchanged ? "guest_lead_imported" : "guest_lead_failed",renderer_game_frame(),NULL);
                }
            }
        }
    }
    return 1;
}
static int battle_party_hook(Cpu *c,uint64_t addr) {
    if(supported && trainer_started) {
        uint64_t party=ptr(c->x[2]);
        LOG("[coop-battle] battle slot %u native party %llx count %u first %llx\n",
            (unsigned)c->x[1],(unsigned long long)party,
            guest(party,0x90) ? *(uint8_t *)(uintptr_t)(party+0x88) : 0,
            (unsigned long long)ptr(party+0x58));
    }
    if(!previous_battle_party || !previous_battle_party(c,addr))rt_call_original(c,addr);
    return 1;
}
static void maybe_trainer(Cpu *c) {
    const char *at=ENV("LARECOMP_COOP_TRAINER_TEST_AT");
    if(!at || trainer_started || renderer_game_frame()<strtoull(at,NULL,10))return;
    // Explicit isolated QA only: no script interpreter or story event is run.
    if(!getenv("LARECOMP_SAVE_DIR") || !getenv("LARECOMP_COOP_PARTY_TEST_DIR"))return;
    uint64_t world=ptr(ptr(WORLD_GOT)),mgr=ptr(world+0x68),alloc=allocator();
    if(!guest(mgr,8) || !guest(alloc,8))return;
    unsigned trainer=26;const char *id=getenv("LARECOMP_COOP_TRAINER_TEST_ID");if(id)trainer=(unsigned)strtoul(id,NULL,10);
    if(!trainer || trainer>601)return;
    Cpu call=*c;uint64_t handle[2]={0},process=0;uint32_t cfg[12]={0,0,trainer};
    if(getenv("LARECOMP_COOP_TRAINER_TEST_GUEST")){cfg[0]=1;cfg[1]=trainer;cfg[3]=trainer;}
    // Native order is ally, opponent 1, opponent 2. Setting the ally
    // field instead leaves opponent slot 1 empty and crashes initialization.
    call.x[0]=(uint64_t)(uintptr_t)&mgr;call.x[1]=alloc;call.x[2]=(uint64_t)(uintptr_t)cfg;
    call.x[8]=(uint64_t)(uintptr_t)handle;
    extern void fps60_scene_begin(void),input_mark(void);fps60_scene_begin();input_mark();
    trainer_started=true;
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
    } else LOG("[coop-battle] trainer factory did not resolve a process\n");
    uint64_t release=ptr(ptr(handle[0]+0x48)+0x18);
    if(guest(release,4)){call.x[0]=handle[0]+0x48;rt_call_original(&call,release);}
}
void coop_battle_field(Cpu *c) {
    // Explicit local QA mode only. No automatic party exchange on social join.
    const char *dir=ENV("LARECOMP_COOP_PARTY_TEST_DIR");
    if(!supported || !dir || !*dir)return;
    if(battle_seen){
        world_return_frame=renderer_game_frame();journal("world_return",world_return_frame,NULL);
        CoopParty after;if(coop_battle_capture_party(c,&after))save_party(dir,"party-after.bin",&after);
        trainer_process=0;battle_seen=false;
    }
    if(world_return_frame && renderer_game_frame()>=world_return_frame+30) {
        // The factory-only QA has no story script to perform its final FadeIn.
        Cpu call=*c;call.x[0]=ptr(ptr(0x70015facf8ull));call.x[1]=0;call.x[2]=1;call.x[3]=10;
        rt_call_original(&call,0x700011ece0ull);world_return_frame=0;
        journal("world_fade_restored",renderer_game_frame(),NULL);
    }
    maybe_trainer(c);
    if(imported)return;
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
    CoopParty after;bool unchanged=coop_battle_capture_party(c,&after) &&
        after.count==local.count && !memcmp(after.pokemon,local.pokemon,local.count*COOP_PB7_BYTES);
    char path[1400];snprintf(path,sizeof path,"%s/import.txt",dir);FILE *f=fopen(path,"w");
    if(f){fprintf(f,"native_roundtrip=%u\nlocal_party_unchanged=%u\npeer_count=%u\npeer_digest=%016llx\n",ok,unchanged,
        remote.count,(unsigned long long)coop_party_digest(&remote));
        for(unsigned i=0;i<remote.count;i++)fprintf(f,"species[%u]=%u\n",i,species[i]);fclose(f);}
    LOG("[coop-battle] temporary guest party: roundtrip %u, local party unchanged %u\n",ok,unchanged);
    imported=true;
}
void coop_battle_install_hooks(void) {
    if(supported && getenv("LARECOMP_COOP_PARTY_TEST_DIR")) {
        rt_register_fnhook_chain(COMMAND,command_hook,&previous_command);
        rt_register_fnhook_chain(0x7000b0ed20ull,trainer_party_hook,&previous_trainer_party);
        rt_register_fnhook_chain(0x70009e0940ull,battle_party_hook,&previous_battle_party);
    }
}
void coop_battle_configure(int build){supported=build==GAME_BUILD_EEVEE_100;exported=imported=trainer_started=battle_seen=false;trainer_process=world_return_frame=0;}
RT_ON_RESET(coop_battle_reset){supported=false;exported=imported=trainer_started=battle_seen=false;trainer_process=world_return_frame=0;}
