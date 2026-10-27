// Day/night cycle (docs/DAYNIGHT-PLAN: phase 1): keyframed grade by hour, applied in color.frag (nvn/vulkan/exec.c).
#include "daynight.h"
#include "settings.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
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

// The atmosphere by hour (exec.c atmo pass): the sky multiplied toward its horizon / zenith colours (after the tint), the stars'
// strength, and the distance haze (colour after the tint, density per cm). Day: the game's own sky and haze, nearly untouched.
typedef struct { float h, hor[3], zen[3], fog[3], dens, stars; } AtmoKey;
static const AtmoKey akeys[] = {
    {0.0f, {0.55f, 0.6f, 0.85f}, {0.3f, 0.35f, 0.65f}, {0.025f, 0.03f, 0.06f}, 5e-5f, 1},
    {4.0f, {0.55f, 0.6f, 0.85f}, {0.3f, 0.35f, 0.65f}, {0.025f, 0.03f, 0.06f}, 5e-5f, 1},
    {5.5f, {1.45f, 0.95f, 0.7f}, {0.75f, 0.7f, 0.95f}, {0.55f, 0.45f, 0.42f}, 9e-5f, 0.2f},  // dawn: pink-gold horizon, mist
    {7.0f, {1, 1, 1}, {1, 1, 1}, {0.6f, 0.7f, 0.85f}, 1.5e-5f, 0},
    {17.0f, {1, 1, 1}, {1, 1, 1}, {0.6f, 0.7f, 0.85f}, 1.5e-5f, 0},
    {18.5f, {1.6f, 0.75f, 0.45f}, {0.7f, 0.55f, 0.85f}, {0.6f, 0.38f, 0.25f}, 6e-5f, 0.15f},  // dusk: orange horizon, warm haze
    {20.0f, {0.55f, 0.6f, 0.85f}, {0.3f, 0.35f, 0.65f}, {0.025f, 0.03f, 0.06f}, 5e-5f, 1},
    {24.0f, {0.55f, 0.6f, 0.85f}, {0.3f, 0.35f, 0.65f}, {0.025f, 0.03f, 0.06f}, 5e-5f, 1},
};
void daynight_atmo(float hor[3], float zen[3], float fog[4], float *stars) {
    float h = daynight_hour();
    int i = 0;
    while (i < (int)(sizeof akeys / sizeof *akeys) - 2 && h >= akeys[i + 1].h) i++;
    const AtmoKey *a = &akeys[i], *b = &akeys[i + 1];
    float t = (h - a->h) / (b->h - a->h);
    t = t * t * (3 - 2 * t);
    for (int k = 0; k < 3; k++) {
        hor[k] = a->hor[k] + (b->hor[k] - a->hor[k]) * t;
        zen[k] = a->zen[k] + (b->zen[k] - a->zen[k]) * t;
        fog[k] = a->fog[k] + (b->fog[k] - a->fog[k]) * t;
    }
    fog[3] = a->dens + (b->dens - a->dens) * t;
    *stars = a->stars + (b->stars - a->stars) * t;
}

// Rain, 0..1 (exec.c rain.frag, the clouds' cover, the wet ground's reflections). g_settings.weather: 0 clear, 1 rain, 2 changing -
// half-hour spells of the real clock, a quarter of them rainy (hashed), fading in and out over a minute. LARECOMP_WEATHER=clear|rain|changing.
// The weather (exec.c: rain.frag's rain / snow / lightning, the clouds' cover, atmo.frag's fog). g_settings.weather / LARECOMP_WEATHER:
// 0 clear, 1 rain, 2 changing (half-hour spells of the real clock, each a hashed type, a minute's fade at both ends), 3 cloudy,
// 4 thunderstorm, 5 snow, 6 fog - after weather-canvas's set (MIT; its types, not its code).
enum { WX_CLEAR, WX_RAIN, WX_CHANGING, WX_CLOUDY, WX_STORM, WX_SNOW, WX_FOG };
static void wx_of(int type, float k, Weather *w) {
    switch (type) {
    case WX_RAIN: w->rain = k, w->cloud = 0.7f * k; break;
    case WX_CLOUDY: w->cloud = 0.6f * k; break;
    case WX_STORM: w->rain = k, w->cloud = k, w->storm = k; break;
    case WX_SNOW: w->snow = k, w->cloud = 0.6f * k; break;
    case WX_FOG: w->fog = k, w->cloud = 0.3f * k; break;
    default: break;
    }
}
void daynight_weather(Weather *w) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_WEATHER");
        static const char *const names[] = {"clear", "rain", "changing", "cloudy", "storm", "snow", "fog"};
        env = -1;
        for (int i = 0; e && i < 7; i++) if (!strcmp(e, names[i])) env = i;
    }
    int t = env >= 0 ? env : g_settings.weather;
    memset(w, 0, sizeof *w);
    if (t != WX_CHANGING) { wx_of(t, 1, w); return; }
    double now = (double)time(NULL), block = 1800.0;
    long b = (long)(now / block);
    float in = (float)fmod(now, block);
    unsigned h = (unsigned)b * 2654435761u;
    h ^= h >> 15, h *= 2246822519u, h ^= h >> 13;
    unsigned r = h & 1023u;  // (about: clear 45 %, cloudy 20 %, rain 15 %, fog 8 %, storm 7 %, snow 5 %)
    int type = r < 460 ? WX_CLEAR : r < 665 ? WX_CLOUDY : r < 820 ? WX_RAIN : r < 902 ? WX_FOG : r < 974 ? WX_STORM : WX_SNOW;
    float k = fminf(fminf(in / 60.0f, (float)(block - in) / 60.0f), 1.0f);
    wx_of(type, k, w);
}
float daynight_rain(void) { Weather w; daynight_weather(&w); return w.rain; }

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
    [GL_GLASS_DOOR] = {1.12f, 1.0f, 0.72f},        // a warm interior light, kept low (1.25 / 1.12 / 0.85: the Center's blue door read cyan
                                                   //  and the Mart's white one blew out - both bright enough to be dynamic-light sources)
    [GL_LAMP_GLASS] = {2.2f, 1.45f, 0.50f},        // the lantern bulb: the game's bloom does the halo
    [GL_LAMP_POST] = {1.25f, 1.10f, 0.85f},        // a whole street lamp is one texture: only a warm lift
    [GL_SIGN] = {1.7f, 1.15f, 0.95f},              // (the Center's sign only: the Center / Mart roof textures are whole roofs)
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

// Dark glass windows (Celadon and Saffron, area07: their window textures are nearly black, so a multiplier keeps them dark): a warm
// lit-interior light added in the draw's own shader (exec.c, ShaderKey.glowck 3), as the colour the pane should add before the tint.
static const unsigned emissive_tex[] = {0x4d6065ff /* window02_01_area07 */, 0x2abecc1e /* window03_01_area07 */, 0x8cc01a3f /* window01_com */,
                                       0xa45b4798 /* Celadon's blue windows (not in the name index; found with bisect_tex.py) */};
bool daynight_glow_emissive(unsigned key, float add[3]) {
    float n = daynight_night();
    if (n < 0.05f) return false;
    static const float warm[3] = {0.6f, 0.3f, 0.08f};  // (linear: the warm orange of the lit windows elsewhere; 0.9 / 0.5 / 0.16 read pinkish white)
    for (unsigned i = 0; i < sizeof emissive_tex / sizeof *emissive_tex; i++)
        if (emissive_tex[i] == key) {
            for (int k = 0; k < 3; k++) add[k] = warm[k] * n;
            return true;
        }
    return false;
}

// Glass that is a region of a bigger texture, by the key's mode: 1 the pale cyan glass, 2 the white parts (a lamp drawn as one texture)
static const struct { unsigned key; int mode, cls; } colorkey_tex[] = {
    {0xe2c34422, 2, GL_LAMP_GLASS},  // roadlight01_area05: Cycling Road's lamps, the white half of the ball-shaped head
};

int daynight_glow_colorkey(unsigned key, float mul[3]) {
    float n = daynight_night();
    if (n < 0.05f) return 0;
    for (unsigned i = 0; i < sizeof colorkey_tex / sizeof *colorkey_tex; i++)
        if (colorkey_tex[i].key == key) {
            for (int k = 0; k < 3; k++) mul[k] = 1 + (glow_rgb[colorkey_tex[i].cls][k] - 1) * n;
            return colorkey_tex[i].mode;
        }
    return 0;
}

// The moving sun (runtime/services/fs.c fs_sun_apply: the game's light tables, read each frame): from 6:00 to 18:00 its azimuth sweeps
// 140 degrees around the game's own (east to west), its height from 40 % of the game's at the ends of the day to all of it at noon;
// the night holds the evening's. Setting moving_sun; LARECOMP_MOVING_SUN=0 / 1.
void fs_sun_apply(float daz, float ek);
void daynight_sun_update(void) {
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_MOVING_SUN") ? atoi(getenv("LARECOMP_MOVING_SUN")) != 0 : -1;
    bool on = (env >= 0 ? env : g_settings.moving_sun) && (g_settings.daynight || getenv("LARECOMP_HOUR"));  // (day / night off: the game's sun)
    float h = daynight_hour(), t = 0;
    if (on) t = fminf(fmaxf((h - 12.0f) / 6.0f, -1.0f), 1.0f);
    fs_sun_apply(t * 1.2217f, 1.0f - 0.6f * t * t);  // (off: the game's own, written back)
    {   static int n; extern void fs_sun_scan(void);  // (research: LARECOMP_SUN_SCAN=1 - once, 200 frames into the world)
        if (getenv("LARECOMP_SUN_SCAN") && ++n == 200) fs_sun_scan(); }
}
