// Social quick actions: R3 / F10 in the overworld. Input belongs only to this
// instance's P1; holding a stick never moves the trainer while the wheel is open.
#include <SDL3/SDL.h>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "emote_wheel.h"
extern "C" {
#include "../coop_net.h"
#include "../paths.h"
#include "../platform.h"
#include "../input_bind.h"
void input_reset(void);
void input_quarantine_pad(void);
bool input_primary_gamepad(uint32_t id);
uint64_t vk_imgui_texture_rgba(const void *, int, int);
}
static std::atomic<bool> opened{false}, available{false};
static std::atomic<int> selected{-1}, axis_x{0}, axis_y{0};
static constexpr float pi = 3.14159265358979323846f;
// IDs 1 / 2 retain the existing Hello / Cheer wire meanings. Protocol 4 has
// always reserved IDs 0..15, so old clients can still join the same room.
static const char *labels[] = {"Hello", "Cheer", "Happy", "Love", "Surprised", "Sad", "Angry", "Sleepy"};
static ImTextureID icons[COOP_EMOTE_COUNT];
static bool loaded;
static void load_icons(void) {
    if (loaded) return;
    loaded = true;
    for (int i=0; i<COOP_EMOTE_COUNT; i++) {
        char rel[160], path[1200]; snprintf(rel,sizeof rel,"assets/emotes/%s-mark.png",coop_emote_name(i+1));
        if (!rt_resource(rel,rel,path,sizeof path)) continue;
        SDL_Surface *src=SDL_LoadPNG(path);
        SDL_Surface *rgba=src ? SDL_ConvertSurface(src,SDL_PIXELFORMAT_RGBA32) : nullptr;
        if (rgba) {
            // The image upload expects tightly packed rows.
            unsigned char *pixels=(unsigned char *)SDL_malloc((size_t)rgba->w*rgba->h*4);
            if (pixels) {
                for(int y=0;y<rgba->h;y++) memcpy(pixels+(size_t)y*rgba->w*4,(char *)rgba->pixels+y*rgba->pitch,(size_t)rgba->w*4);
                icons[i]=(ImTextureID)vk_imgui_texture_rgba(pixels,rgba->w,rgba->h);
                SDL_free(pixels);
            }
            SDL_DestroySurface(rgba);
        }
        if(src) SDL_DestroySurface(src);
    }
    int count=0;for(auto id:icons)if(id)count++;
    fprintf(stderr,"[emote-wheel] loaded %d/%d mark icons\n",count,COOP_EMOTE_COUNT);
}
static void close_wheel(void) { opened=false; input_reset(); input_quarantine_pad(); }
static void send_selected(void) {
    int i=selected.load();
    if(i>=0 && i<8 && available.load()) coop_net_emote((uint8_t)(i+1));
    close_wheel();
}
bool emote_wheel_open(void) { return opened.load(); }
void emote_wheel_available(bool value) {
    available=value;
    if(!value && opened.load()) close_wheel();
}
static int direction(float x,float y) {
    float a=atan2f(x,-y); if(a<0)a+=2*pi;
    return ((int)floorf((a+pi/8)/(pi/4)))%8;
}
bool emote_wheel_event(const SDL_Event *e) {
    bool pad=e->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type==SDL_EVENT_GAMEPAD_BUTTON_UP;
    bool axis=e->type==SDL_EVENT_GAMEPAD_AXIS_MOTION;
    if((pad || axis) && !input_primary_gamepad(pad ? e->gbutton.which : e->gaxis.which)) return false;
    bool key=e->type==SDL_EVENT_KEY_DOWN && !e->key.repeat;
    bool toggle=(key && e->key.scancode==SDL_SCANCODE_F10) ||
        (e->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button==SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    if(toggle && (available.load() || opened.load())) {
        if(opened.load()) close_wheel();
        else {selected=-1;axis_x=axis_y=0;opened=true;input_reset();}
        fprintf(stderr,"[emote-wheel] %s\n",opened.load() ? "opened" : "closed");
        return true;
    }
    if(!opened.load()) return false;
    if(e->type==SDL_EVENT_WINDOW_FOCUS_LOST || e->type==SDL_EVENT_GAMEPAD_REMOVED) {close_wheel();return false;}
    if(key) {
        switch(e->key.scancode) {
        case SDL_SCANCODE_ESCAPE:case SDL_SCANCODE_Q:close_wheel();break;
        case SDL_SCANCODE_RETURN:case SDL_SCANCODE_KP_ENTER:case SDL_SCANCODE_SPACE:send_selected();break;
        case SDL_SCANCODE_UP:selected=0;break;case SDL_SCANCODE_RIGHT:selected=2;break;
        case SDL_SCANCODE_DOWN:selected=4;break;case SDL_SCANCODE_LEFT:selected=6;break;
        default:if(e->key.scancode>=SDL_SCANCODE_1 && e->key.scancode<=SDL_SCANCODE_8) selected=e->key.scancode-SDL_SCANCODE_1;break;
        }
        return true;
    }
    if(pad) {
        if(e->type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            int b=e->gbutton.button;
            int confirm=bind_layout()==BIND_LAYOUT_XBOX ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
            int cancel=bind_layout()==BIND_LAYOUT_XBOX ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
            if(b==confirm)send_selected();else if(b==cancel)close_wheel();
            else if(b==SDL_GAMEPAD_BUTTON_DPAD_UP)selected=0;else if(b==SDL_GAMEPAD_BUTTON_DPAD_RIGHT)selected=2;
            else if(b==SDL_GAMEPAD_BUTTON_DPAD_DOWN)selected=4;else if(b==SDL_GAMEPAD_BUTTON_DPAD_LEFT)selected=6;
        }
        return true;
    }
    if(axis) {
        if(e->gaxis.axis==SDL_GAMEPAD_AXIS_LEFTX || e->gaxis.axis==SDL_GAMEPAD_AXIS_RIGHTX) axis_x=e->gaxis.value;
        if(e->gaxis.axis==SDL_GAMEPAD_AXIS_LEFTY || e->gaxis.axis==SDL_GAMEPAD_AXIS_RIGHTY) axis_y=e->gaxis.value;
        float x=(float)axis_x.load(),y=(float)axis_y.load();
        if(x*x+y*y>12000.f*12000.f)selected=direction(x,y);
        return true;
    }
    return e->type==SDL_EVENT_KEY_UP || e->type==SDL_EVENT_TEXT_INPUT;
}
static void icon(ImDrawList *dl,int i,ImVec2 p,float size,float angle,ImU32 tint=IM_COL32_WHITE) {
    if(!icons[i]) { char l[32];coop_emote_label(i+1,l,sizeof l);ImVec2 t=ImGui::CalcTextSize(l);dl->AddText(ImVec2(p.x-t.x/2,p.y-t.y/2),IM_COL32(61,67,87,255),l);return; }
    float c=cosf(angle)*size/2,s=sinf(angle)*size/2;
    dl->AddImageQuad(icons[i],ImVec2(p.x-c+s,p.y-s-c),ImVec2(p.x+c+s,p.y+s-c),
        ImVec2(p.x+c-s,p.y+s+c),ImVec2(p.x-c-s,p.y-s+c),ImVec2(0,0),ImVec2(1,0),ImVec2(1,1),ImVec2(0,1),tint);
}
void emote_wheel_draw(float scale) {
    if(!opened.load())return;
    load_icons();
    ImGuiIO &io=ImGui::GetIO();ImDrawList *dl=ImGui::GetForegroundDrawList();
    ImVec2 centre(io.DisplaySize.x/2,io.DisplaySize.y/2);
    float radius=fminf(230*scale,io.DisplaySize.y*.29f),node=58*scale;
    dl->AddRectFilled(ImVec2(0,0),io.DisplaySize,IM_COL32(21,34,49,105));
    dl->AddCircleFilled(centre,radius+node+14*scale,IM_COL32(228,239,238,230),96);
    dl->AddCircleFilled(centre,radius-node-13*scale,IM_COL32(61,80,97,235),64);
    ImVec2 mouse=io.MousePos;
    if(io.MouseDelta.x!=0 || io.MouseDelta.y!=0 || ImGui::IsMouseClicked(0)) {
        float x=mouse.x-centre.x,y=mouse.y-centre.y,d=sqrtf(x*x+y*y);
        if(d>radius*.45f && d<radius+node*1.4f) selected=direction(x,y);else selected=-1;
    }
    int choice=selected.load();
    for(int i=0;i<8;i++) {
        float a=-pi/2+i*pi/4;
        ImVec2 at(centre.x+cosf(a)*radius,centre.y+sinf(a)*radius);
        bool active=i==choice;
        dl->AddCircleFilled(ImVec2(at.x,at.y+4*scale),node,IM_COL32(41,57,69,45),48);
        dl->AddCircleFilled(at,node,active ? IM_COL32(255,223,96,255) : IM_COL32(255,255,251,255),48);
        dl->AddCircle(at,node,active ? IM_COL32(255,255,255,255) : IM_COL32(209,218,212,255),48,active ? 4*scale : 1.5f*scale);
        icon(dl,i,at,78*scale,active ? sinf((float)ImGui::GetTime()*12)*.045f : 0);
        ImVec2 t=ImGui::CalcTextSize(labels[i]),p(at.x-t.x/2,at.y+node+5*scale);
        dl->AddRectFilled(ImVec2(p.x-9*scale,p.y-2*scale),ImVec2(p.x+t.x+9*scale,p.y+t.y+2*scale),IM_COL32(255,255,251,250),12*scale);
        dl->AddText(p,IM_COL32(45,58,63,255),labels[i]);
    }
    const char *title=choice>=0 ? labels[choice] : "Emotes";
    ImVec2 t=ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(centre.x-t.x/2,centre.y-t.y/2),IM_COL32_WHITE,title);
    const char *help="Stick / mouse / 1-8: choose     A / Enter: send     B / F10: close";
    t=ImGui::CalcTextSize(help);
    dl->AddText(ImVec2(centre.x-t.x/2,centre.y+radius+node+55*scale),IM_COL32_WHITE,help);
    if(ImGui::IsMouseClicked(0) && choice>=0)send_selected();
    if(ImGui::IsMouseClicked(1))close_wheel();
}
void emote_icon_draw(uint8_t emote,float x,float y,float size) {
    if(!coop_emote_name(emote))return;
    load_icons();
    icon(ImGui::GetForegroundDrawList(),emote-1,ImVec2(x,y),size,0);
}
void emote_bubble_draw(uint8_t emote,uint64_t at,float x,float bottom,float scale) {
    uint64_t now=plat_now_ns();
    if(!coop_emote_name(emote) || !at || now<at || now-at>3000000000ull)return;
    load_icons();
    // Wall-clock timing: pop in with a small overshoot, float gently, then shrink and fade over the last 0.3 s.
    float age=(float)(now-at)/1e9f,in=fminf(age/.35f,1.f),k=in-1,out=fmaxf(0.f,(age-2.7f)/.3f),fade=1-out;
    float pop=1+2.70158f*k*k*k+1.70158f*k*k;
    float size=80*scale*pop*(1-.35f*out),shake=sinf(age*30)*.13f*expf(-age*1.6f);
    float drift=sinf(age*25)*3*scale*expf(-age*1.5f),bob=sinf(age*5)*3.5f*scale*in-out*10*scale;
    ImVec2 p(x+drift,bottom-size/2-11*scale+bob),a(p.x-size/2,p.y-size/2),b(p.x+size/2,p.y+size/2);
    a.x=fmaxf(6*scale,fminf(a.x,ImGui::GetIO().DisplaySize.x-size-6*scale));b.x=a.x+size;p.x=(a.x+b.x)/2;
    if(a.y<6*scale){float d=6*scale-a.y;a.y+=d;b.y+=d;p.y+=d;}
    ImDrawList *dl=ImGui::GetForegroundDrawList();
    ImU32 white=IM_COL32(255,255,251,(int)(255*fade));
    dl->AddRectFilled(ImVec2(a.x+2*scale,a.y+3*scale),ImVec2(b.x+2*scale,b.y+3*scale),IM_COL32(48,60,65,(int)(55*fade)),13*scale);
    dl->AddRectFilled(a,b,white,13*scale);
    dl->AddRect(a,b,IM_COL32(210,217,211,(int)(220*fade)),13*scale,0,1.4f*scale);
    dl->AddTriangleFilled(ImVec2(p.x+8*scale,b.y-2*scale),ImVec2(p.x+23*scale,b.y-2*scale),ImVec2(p.x+12*scale,b.y+11*scale),white);
    icon(dl,emote-1,p,size*.79f,shake,IM_COL32(255,255,255,(int)(255*fade)));
}
