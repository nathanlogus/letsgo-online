// Native Communicate -> Play With Others integration for Eevee 1.0.0.
// Reuse the game's three-choice matching app, its native cursor / touch handlers
// and text panes. The Nintendo network state machine only receives Cancel.
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "../hle.h"
#include "../gamesetup.h"
#include "../coop_net.h"

void rt_register_fnhook(uint64_t addr,int (*fn)(Cpu *,uint64_t));
void rt_call_original(Cpu *c,uint64_t addr);
int menu_keyboard_run_labeled(const char *,const char *,int,char *,int);
void menu_copy_text(const char *);
#define NETWORK_FACTORY 0x7000888970ull
#define CHOICE_FACTORY  0x7000888cb0ull
#define CHOICE_OPEN     0x7000890aa0ull
#define CHOICE_UPDATE   0x7000890a20ull
#define UI_GET          0x70001266e0ull
#define UI_TEXT         0x700036cb70ull
#define UI_INPUT        0x7000126670ull
#define UI_REARM        0x70003623c0ull

enum { ROOT, ROOM, MANAGE, PLAYERS, REMOTE, INSPECT, SETTINGS, SAVED, SAVED_DETAIL,
       REMOVE_CONFIRM, LEAVE_CONFIRM, FORGET_CONFIRM, STATUS, TEAM, SOCIAL };
static uint64_t g_app;
static atomic_bool g_visible;
static int g_page, g_stack[16], g_depth, g_saved;
static bool g_show_code, g_inspect_self;
static char g_notice[160], g_last[3][256];
static uint8_t g_emote;
static uint64_t g_emote_until;
static uint8_t g_team_queued=255;
static uint32_t g_team_queued_id;
static uint64_t g_team_until;
static bool guest(uint64_t p,size_t n) { return p>=GUEST_LO && p<GUEST_HI && n<=GUEST_HI-p; }
static uint64_t ptr(uint64_t p) { return guest(p,8) ? *(const uint64_t *)(uintptr_t)p : 0; }
static uint64_t hash(const char *s) { uint64_t h=0xcbf29ce484222645ull; while(*s) h=(h^(unsigned char)*s++)*0x100000001b3ull; return h; }
static const char *activity(CoopActivity a) {
    switch(a) {case COOP_OVERWORLD:return "Exploring";case COOP_MENU:return "Menu / loading";
    case COOP_BATTLE:return "In battle";case COOP_STORY:return "Story scene";default:return "Loading";}
}
static void page(int next) { if(g_depth<16) g_stack[g_depth++]=g_page; g_page=next; g_last[0][0]=g_last[1][0]=g_last[2][0]=0; }
static void back(void) { g_page=g_depth ? g_stack[--g_depth] : ROOT; g_last[0][0]=g_last[1][0]=g_last[2][0]=0; }
static void notice(const char *s) { snprintf(g_notice,sizeof g_notice,"%s",s); page(STATUS); }
static void text(Cpu *c,uint64_t ui,const char *pane,const char *s) {
    uint16_t chars[256]; unsigned n=0;
    // Names come from UTF-8. Preserve them in the game's native UTF-16 text.
    const unsigned char *p=(const unsigned char *)s;
    while(*p && n+2<256) {
        uint32_t cp=*p++; unsigned more=cp>=0xf0 ? 3 : cp>=0xe0 ? 2 : cp>=0xc0 ? 1 : 0;
        if(more) { cp&=0x3f>>more; while(more-- && (*p&0xc0)==0x80) cp=(cp<<6)|(*p++&0x3f); }
        if(cp>=0x10000 && cp<=0x10ffff) {cp-=0x10000;chars[n++]=0xd800|(cp>>10);chars[n++]=0xdc00|(cp&0x3ff);} else chars[n++]=cp<=0xffff ? cp : '?';
    }
    chars[n]=0; uint64_t strbuf[16]={0},key=hash(pane);
    strbuf[0x58/8]=(uint64_t)(uintptr_t)chars;
    c->x[0]=ui;c->x[1]=(uint64_t)(uintptr_t)&key;c->x[2]=(uint64_t)(uintptr_t)strbuf;c->x[3]=10;
    rt_call_original(c,UI_TEXT);
}
static void labels(char out[3][256]) {
    CoopSession s;CoopOptions o;CoopRemote p;
    coop_net_session(&s);coop_net_options(&o);bool peer=coop_net_remote(&p);
    CoopTeamUp team;coop_net_teamup(&team);
    bool connected=s.connection==COOP_CONNECTED;
    const char *state=connected ? "Connected" : s.connection==COOP_ERROR ? "Connection failed" :
        s.connection==COOP_RECONNECTING ? "Reconnecting" : s.connection==COOP_CONNECTING ? "Connecting" : "Playing solo";
    const char *a="Back",*b="Back",*d="Back";
    switch(g_page) {
    case ROOT:
        if(s.connection==COOP_CONNECTING || s.connection==COOP_RECONNECTING){a="Connection status";b="Cancel connection";}
        else {a=connected ? "Players" : "Host a world";b=connected ? "Room controls" : "Join a world";}
        d="Online settings";break;
    case ROOM:
        snprintf(out[0],256,"%s",g_show_code && s.room[0] ? s.room : "Show room code");
        a=NULL;b="Copy room code";d="Manage world";break;
    case MANAGE:
        a=s.local_slot==s.host_slot ? s.locked ? "Unlock world" : "Lock world" : s.locked ? "World is locked" : "World is open";
        b=s.local_slot==s.host_slot ? "Remove player" : "Players";d="Leave world";break;
    case PLAYERS:
        snprintf(out[0],256,"P%u: %.24s",s.local_slot<=1 ? s.local_slot+1 : 1,s.name);
        snprintf(out[1],256,peer ? "P%u: %.24s" : "Waiting for a friend",peer ? p.slot+1 : 2,peer ? p.name : "");
        a=b=NULL;d="Room controls";break;
    case REMOTE:a="Inspect trainer";b="Team up";d="Social actions";break;
    case SOCIAL:a="Say hello";b="Cheer";d="Back";break;
    case TEAM:
        a=g_team_queued<=1 ? "Return to the overworld" : team.state==2 ? "Tagged up for travel" :
            team.state==1 ? team.from_slot==s.local_slot ? "Waiting for your friend" : "Accept invitation" : "Invite to tag up";
        b=g_team_queued<=1 ? "Cancel queued invitation" : team.state==2 ? "End tag-up" : team.state==1 ?
            team.from_slot==s.local_slot ? "Cancel invitation" : "Decline invitation" : "Battles stay individual";
        if(!team.state && team.reason) {
            static const char *why[]={"","Invitation declined","Tag-up ended","Another request is active",
                "Friend is busy or too far","Invitation expired","Friend disconnected","Invitation no longer valid"};
            b=why[team.reason<=7 ? team.reason : 7];
        }
        d="Back";break;
    case INSPECT:
        snprintf(out[0],256,"%.24s",!g_inspect_self && peer ? p.name : s.name);
        snprintf(out[1],256,"%s",!g_inspect_self && peer ? activity(p.activity) : activity(s.activity));
        snprintf(out[2],256,"%s",!g_inspect_self && peer && p.appearance.valid ? p.appearance.gender ? "Female trainer" : "Male trainer" : "Independent save");a=b=d=NULL;break;
    case SETTINGS:
        snprintf(out[0],256,"Trainer: %.24s",o.name);a=NULL;b="Relay server";d="Saved worlds";break;
    case SAVED:
        snprintf(out[0],256,"Saved world %u%s",g_saved+1,o.saved_rooms[g_saved][0] ? "" : " (empty)");
        a=NULL;b="Next saved world";d="Back";break;
    case SAVED_DETAIL:
        a="Join saved world";b=g_show_code ? o.saved_rooms[g_saved] : "Show room code";d="Forget saved world";break;
    case REMOVE_CONFIRM:a="Remove this player";b="Keep playing together";d="Back";break;
    case LEAVE_CONFIRM:a="Leave and play solo";b="Stay in this world";d="Back";break;
    case FORGET_CONFIRM:a="Forget this saved world";b="Keep saved world";d="Back";break;
    case STATUS:
        snprintf(out[0],256,"%s",g_notice[0] ? g_notice : state);
        snprintf(out[1],256,"%s",s.connection==COOP_ERROR && !g_notice[0] ? s.detail : connected && !g_notice[0] ? "Show room code" : !g_notice[0] && (s.connection==COOP_CONNECTING || s.connection==COOP_RECONNECTING) ? "Cancel connection" : "Independent saves");
        a=b=NULL;d=s.connection==COOP_ERROR && !g_notice[0] ? "Retry connection" : "Back";break;
    }
    if(a)snprintf(out[0],256,"%s",a);if(b)snprintf(out[1],256,"%s",b);if(d)snprintf(out[2],256,"%s",d);
}
static void refresh(Cpu *c,uint64_t app) {
    char current[3][256]={{0}};labels(current);
    c->x[0]=app;c->x[1]=0;rt_call_original(c,UI_GET);uint64_t ui=c->x[0];
    if(!guest(ui,8))return;
    for(unsigned i=0;i<3;i++) if(strcmp(g_last[i],current[i])) {
        char pane[80];snprintf(pane,sizeof pane,"pane_L_button_icon_%02u_T_title_00",i);
        text(c,ui,pane,current[i]);memcpy(g_last[i],current[i],256);
    }
}
static void choose(unsigned item) {
    CoopOptions o;CoopSession s;CoopRemote p;coop_net_options(&o);coop_net_session(&s);
    bool peer=coop_net_remote(&p),connected=s.connection==COOP_CONNECTED;
    CoopTeamUp team;coop_net_teamup(&team);
    char entered[256];
    switch(g_page) {
    case ROOT:
        if(item==3){page(SETTINGS);break;}
        if(connected){page(item==1 ? PLAYERS : ROOM);break;}
        if(s.connection==COOP_CONNECTING || s.connection==COOP_RECONNECTING){
            if(item==1){g_notice[0]=0;page(STATUS);}else coop_net_leave();break;
        }
        if(item==1){o.host=true;coop_net_connect(&o);g_notice[0]=0;page(STATUS);}
        else if(menu_keyboard_run_labeled("Enter a room code (LG-XXXXXX)",o.room,9,entered,sizeof entered)) {
            for(char *q=entered;*q;q++)if(*q>='a'&&*q<='z')*q-=32;
            if(strlen(entered)!=9 || strncmp(entered,"LG-",3)){notice("Check the room code");break;}
            snprintf(o.room,sizeof o.room,"%s",entered);o.host=false;coop_net_connect(&o);g_notice[0]=0;page(STATUS);
        }break;
    case ROOM:
        if(item==1)g_show_code=!g_show_code;
        else if(item==2){if(s.room[0]){menu_copy_text(s.room);notice("Room code copied");}else notice("No active room");}
        else page(MANAGE);break;
    case MANAGE:
        if(item==3)page(LEAVE_CONFIRM);
        else if(item==2){if(s.local_slot==s.host_slot){if(peer)page(REMOVE_CONFIRM);else notice("No player to remove");}else page(PLAYERS);}
        else if(s.local_slot==s.host_slot)coop_net_lock(!s.locked);break;
    case PLAYERS:
        if(item==3)page(ROOM);else if(item==2 && peer)page(REMOTE);else if(item==1){g_inspect_self=true;page(INSPECT);}break;
    case REMOTE:
        if(item==1){g_inspect_self=false;page(INSPECT);}
        else page(item==2 ? TEAM : SOCIAL);break;
    case SOCIAL:
        if(item==3){back();break;}
        if(!peer)notice("Your friend has left");
        else {g_emote=item==1 ? 1 : 2;g_emote_until=plat_now_ns()+60000000000ull;notice("Emote ready for the overworld");}break;
    case TEAM:
        if(item==3){back();break;}
        if(item==2 && g_team_queued<=1){g_team_queued=255;break;}
        if(item==2 && team.state){coop_net_teamup_action(team.state==1 && team.from_slot!=s.local_slot ? 2 : 3);break;}
        if(item==1 && (team.state==0 || (team.state==1 && team.from_slot!=s.local_slot))) {
            g_team_queued=team.state==0 ? 0 : 1;g_team_queued_id=team.id;g_team_until=plat_now_ns()+30000000000ull;
        }break;
    case SETTINGS:
        if(item==3){g_saved=0;page(SAVED);break;}
        if(menu_keyboard_run_labeled(item==1 ? "Trainer name" : "Relay address (host:port)",item==1 ? o.name : o.server,item==1 ? 24 : 255,entered,sizeof entered) && entered[0]) {
            if(item==1)snprintf(o.name,sizeof o.name,"%s",entered);else snprintf(o.server,sizeof o.server,"%s",entered);
            coop_net_preferences(&o);
        }break;
    case SAVED:
        if(item==1 && o.saved_rooms[g_saved][0])page(SAVED_DETAIL);
        else if(item==2)g_saved=(g_saved+1)%4;else if(item==3)back();break;
    case SAVED_DETAIL:
        if(item==1){o.host=false;snprintf(o.room,sizeof o.room,"%s",o.saved_rooms[g_saved]);coop_net_connect(&o);g_notice[0]=0;page(STATUS);}
        else if(item==2)g_show_code=!g_show_code;else page(FORGET_CONFIRM);break;
    case REMOVE_CONFIRM:if(item==1){coop_net_remove_peer();back();}else back();break;
    case LEAVE_CONFIRM:if(item==1){coop_net_leave();g_depth=0;g_page=ROOT;}else back();break;
    case FORGET_CONFIRM:if(item==1){coop_net_forget_room(g_saved);back();back();}else back();break;
    case STATUS:
        if(item==3 && !g_notice[0] && s.connection==COOP_ERROR)coop_net_connect(&o);
        else if(item==2 && !g_notice[0] && connected)page(ROOM);
        else if(item==2 && !g_notice[0] && (s.connection==COOP_CONNECTING || s.connection==COOP_RECONNECTING)){coop_net_leave();back();}
        else back();break;
    case INSPECT:back();break;
    }
    g_last[0][0]=g_last[1][0]=g_last[2][0]=0;
    LOG("[coop-menu] native choice %u -> page %d\n",item,g_page);
}
static int factory_hook(Cpu *c,uint64_t addr) {
    (void)addr;rt_call_original(c,CHOICE_FACTORY);return 1;
}
static int open_hook(Cpu *c,uint64_t addr) {
    uint64_t app=c->x[0];rt_call_original(c,addr);Cpu keep=*c;
    g_app=app;atomic_store(&g_visible,true);g_show_code=false;g_page=ROOT;g_depth=0;g_notice[0]=0;memset(g_last,0,sizeof g_last);
    refresh(c,app);*c=keep;LOG("[coop-menu] native Friends opened at %llx\n",(unsigned long long)app);return 1;
}
static int update_hook(Cpu *c,uint64_t addr) {
    uint64_t app=c->x[0];if(app!=g_app || !guest(app,0x280))return 0;
    Cpu keep=*c;int selected=*(const int32_t *)(uintptr_t)(app+0x24c);
    if(selected==0 && !g_depth){g_app=0;atomic_store(&g_visible,false);return 0;}
    if(selected>=0 && selected<=3) {
        *(int32_t *)(uintptr_t)(app+0x24c)=-1;
        if(!selected)back();else choose((unsigned)selected);
        c->x[0]=app;c->x[1]=0;rt_call_original(c,UI_INPUT);rt_call_original(c,UI_REARM);
    }
    refresh(c,app);*c=keep;rt_call_original(c,addr);return 1;
}
void coop_menu_tick(bool overworld) {
    if(g_team_queued<=1) {
        CoopTeamUp team;coop_net_teamup(&team);
        if(plat_now_ns()>g_team_until || !coop_net_enabled() || team.id!=g_team_queued_id)g_team_queued=255;
        else if(overworld && coop_net_social_near(3000)){coop_net_teamup_action(g_team_queued);g_team_queued=255;}
    }
    if(!g_emote)return;
    if(plat_now_ns()>g_emote_until || !coop_net_enabled()){g_emote=0;return;}
    if(overworld && coop_net_social_near(3000)){coop_net_emote(g_emote);g_emote=0;}
}
bool coop_menu_visible(void) { return atomic_load(&g_visible); }
RT_ON_RESET(coop_menu_reset) {g_app=0;atomic_store(&g_visible,false);g_depth=0;g_emote=0;g_team_queued=255;}
void coop_menu_configure(int build) {
    static bool hooked;if(build!=GAME_BUILD_EEVEE_100 || hooked)return;hooked=true;
    rt_register_fnhook(NETWORK_FACTORY,factory_hook);
    rt_register_fnhook(CHOICE_OPEN,open_hook);
    rt_register_fnhook(CHOICE_UPDATE,update_hook);
}
