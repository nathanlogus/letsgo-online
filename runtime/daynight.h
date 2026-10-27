// Day/night cycle: a colour grade from the clock (larecomp.ini daynight = 1; LARECOMP_HOUR=<0..24> fixes the hour).
#pragma once
#include <stdbool.h>
// Hour of the game day, 0..24. daynight_speed 1 = the host's clock, N = N times faster from the launch time.
float daynight_hour(void);
// 0 by day .. 1 at night (lamps and windows, a later phase), smooth through dawn and dusk.
float daynight_night(void);
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
