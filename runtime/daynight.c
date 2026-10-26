// Day/night cycle (docs/DAYNIGHT-PLAN: phase 1): keyframed grade by hour, applied in color.frag (nvn/vulkan/exec.c).
#include "daynight.h"
#include "settings.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>

typedef struct { float h, r, g, b, mul, sat, night; } Key;
// (dawn 6.5, day 8-17, dusk 18.5; night readable: lifted, cool, less saturated)
static const Key keys[] = {  // (night: ssrecomp's ambient 0.40 / 0.45 / 0.70 - tint x mul; full 20:00 - 4:00, dusk from 17:00)
    {0.0f, .50f, .60f, 1.00f, .70f, .85f, 1}, {4.0f, .50f, .60f, 1.00f, .70f, .85f, 1},
    {5.5f, 1.00f, .78f, .65f, .88f, .95f, .4f}, {7.0f, 1, 1, 1, 1, 1, 0},
    {17.0f, 1, 1, 1, 1, 1, 0}, {18.5f, 1.00f, .72f, .50f, .90f, 1.05f, .5f},
    {20.0f, .50f, .60f, 1.00f, .70f, .85f, 1}, {24.0f, .50f, .60f, 1.00f, .70f, .85f, 1},
};

float daynight_hour(void) {
    if (g_settings.daynight_hour >= 0) return (float)g_settings.daynight_hour;  // (the menu's fixed hour wins over LARECOMP_HOUR)
    const char *e = getenv("LARECOMP_HOUR");
    if (e && *e) return fmodf(fabsf((float)atof(e)), 24.0f);
    static time_t t0;
    static float h0;
    if (!t0) {
        t0 = time(NULL);
        struct tm tm;
        localtime_r(&t0, &tm);
        h0 = tm.tm_hour + tm.tm_min / 60.0f;
    }
    float s = g_settings.daynight_speed < 0.1f ? 1.0f : g_settings.daynight_speed;
    return fmodf(h0 + (float)difftime(time(NULL), t0) * s / 3600.0f, 24.0f);
}

static Key sample(void) {
    float h = daynight_hour();
    int i = 0;
    while (i < (int)(sizeof keys / sizeof *keys) - 2 && h >= keys[i + 1].h) i++;
    const Key *a = &keys[i], *b = &keys[i + 1];
    float t = (h - a->h) / (b->h - a->h);
    t = t * t * (3 - 2 * t);
    Key k;
#define L(f) k.f = a->f + (b->f - a->f) * t
    L(r), L(g), L(b), L(mul), L(sat), L(night);
#undef L
    return k;
}

float daynight_night(void) { return g_settings.daynight || getenv("LARECOMP_HOUR") ? sample().night : 0; }

extern bool game_title(void);         // runtime/hle/fps60.c: before the world's first update (logos, title, loading)
extern int fs_env_set(void);          // runtime/services/fs.c: the light set the game loaded last (2 room, 3 dungeon: not the outdoors)

bool daynight_tint(float rgb[3]) {
    static int env = -1;
    if (env < 0) env = getenv("LARECOMP_HOUR") != NULL;
    if (!(g_settings.daynight || env) || game_title()) return false;  // (battles and captures keep the hour's light: they follow the overworld's light set)
    if (fs_env_set() == 2 || fs_env_set() == 3) return false;  // (interiors and caves look the same at any hour)
    Key k = sample();
    // (the grade was set in display values; the scene is linear, the game's tone mapping and encoding follow)
    rgb[0] = powf(k.r * k.mul, 2.2f), rgb[1] = powf(k.g * k.mul, 2.2f), rgb[2] = powf(k.b * k.mul, 2.2f);
    return true;
}

// What glows: textures by content key (exec.c tex_content_key >> 32), named from the game's own files (tests/scripts/bntx_scan.py,
// gen_glow_table.py -> daynight_glow.inc: the windows, glass doors, lamp glass, street lamps and roof signs of every outdoor pack). Each is
// the texture of one glass / sign material, so its draws light up without touching the walls around: the lantern glass is its own draw.
enum { GL_WINDOW, GL_GLASS_DOOR, GL_LAMP_GLASS, GL_LAMP_POST, GL_SIGN, GL_CLASSES };
// the multiplier of the daylight colour at full night, by class
static const float glow_rgb[GL_CLASSES][3] = {  // (kept low so the panes, frames and bulbs keep their detail - Sword / Shield nights: warm orange windows,
    [GL_WINDOW] = {1.7f, 1.05f, 0.40f},            //  not blown-out white; LARECOMP_GLOW_STRENGTH scales (g - 1))
    [GL_GLASS_DOOR] = {1.25f, 1.12f, 0.85f},       // warm-white interior light (blue x cool read as a cyan screen)
    [GL_LAMP_GLASS] = {2.2f, 1.45f, 0.50f},        // the lantern bulb: the game's bloom does the halo
    [GL_LAMP_POST] = {1.25f, 1.10f, 0.85f},        // a whole street lamp is one texture: only a warm lift
    [GL_SIGN] = {1.7f, 1.15f, 0.95f},
};
static const struct { unsigned key; int cls; const char *what; } glow[] = {
#include "daynight_glow.inc"
};

bool daynight_glow(unsigned key, float mul[3]) {
    static unsigned extra[16];
    static int nextra = -1;
    if (nextra < 0) {
        nextra = 0;
        for (const char *p = getenv("LARECOMP_GLOW_TEX"); p && *p && nextra < 16;) {
            extra[nextra++] = (unsigned)strtoul(p, (char **)&p, 16);
            while (*p == ',') p++;
        }
    }
    float n = daynight_night();
    if (n < 0.05f) return false;
    const float *g = NULL;
    if (key == 0xFFFFFFFFu) {  // (the window shader's draws: exec.c glow_draw)
        static const float win[3] = {2.3f, 1.45f, 0.55f};
        for (int k = 0; k < 3; k++) mul[k] = 1 + (win[k] - 1) * n;
        return true;
    }
    static const float warm[3] = {1.9f, 1.5f, 0.8f};
    for (unsigned i = 0; i < sizeof glow / sizeof *glow; i++) if (glow[i].key == key) g = glow_rgb[glow[i].cls];
    for (int i = 0; !g && i < nextra; i++) if (extra[i] == key) g = warm;
    if (!g) return false;
    static float strength = -1;  // LARECOMP_GLOW_STRENGTH: scales how far the glow goes past daylight (default 1)
    if (strength < 0) strength = getenv("LARECOMP_GLOW_STRENGTH") ? (float)atof(getenv("LARECOMP_GLOW_STRENGTH")) : 1.0f;
    for (int k = 0; k < 3; k++) mul[k] = 1 + (g[k] - 1) * n * strength;
    return true;
}

// (bldg_shop_wall01_com: its sliding glass door and window strips; the wall around them stays dark)
static const unsigned colorkey_tex[] = {0};  // (none needed so far: the Mart door turned out to be its own draw)

bool daynight_glow_colorkey(unsigned key, float mul[3]) {
    float n = daynight_night();
    if (n < 0.05f) return false;
    for (unsigned i = 0; i < sizeof colorkey_tex / sizeof *colorkey_tex; i++)
        if (colorkey_tex[i] == key) {
            for (int k = 0; k < 3; k++) mul[k] = 1 + (glow_rgb[GL_GLASS_DOOR][k] - 1) * n;
            return true;
        }
    return false;
}
