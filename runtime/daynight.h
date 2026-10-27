// Day/night cycle: a colour grade from the clock (larecomp.ini daynight = 1; LARECOMP_HOUR=<0..24> fixes the hour).
#pragma once
#include <stdbool.h>
// Hour of the game day, 0..24. daynight_speed 1 = the host's clock, N = N times faster from the launch time.
float daynight_hour(void);
// 0 by day .. 1 at night (lamps and windows, a later phase), smooth through dawn and dusk.
float daynight_night(void);
// The atmosphere by hour (exec.c atmo.frag): sky horizon / zenith multipliers, haze colour + density (per cm), stars 0..1
void daynight_atmo(float hor[3], float zen[3], float fog[4], float *stars);
// Rain 0..1 (weather setting: clear / rain / changing - half-hour spells; LARECOMP_WEATHER)
float daynight_rain(void);
// The weather, each 0..1 (the setting: clear / rain / changing / cloudy / storm / snow / fog; LARECOMP_WEATHER=<name>)
typedef struct { float rain, snow, fog, cloud, storm; } Weather;
void daynight_weather(Weather *w);
// The moving sun: the game's light tables turned by the hour (each outdoor frame, exec.c)
void daynight_sun_update(void);
// The scene's multiplier (linear light) when the cycle is on and the world is on screen (not a battle or capture).
bool daynight_tint(float rgb[3]);
// Night lights: a texture (content key, exec.c tex_content_key >> 32) that glows, as the multiplier its draw should end up
// with after the scene tint (1 = unchanged; > 1 = brighter than daylight, so the game's bloom picks it up). False by day
// or for textures that do not glow. LARECOMP_GLOW_TEX=<hex key>,... (research) adds keys with a warm window glow.
bool daynight_glow(unsigned key, float mul[3]);
// Glass that is only a region of a bigger texture: the multiplier for the fragment shader's colour key, and the key's mode
// (0 none, 1 pale cyan glass, 2 the white parts: Cycling Road's lamps).
int daynight_glow_colorkey(unsigned key, float mul[3]);
// Dark glass windows (nearly black textures: Celadon, Saffron): the warm light the pane adds at night, before the scene tint.
bool daynight_glow_emissive(unsigned key, float add[3]);
