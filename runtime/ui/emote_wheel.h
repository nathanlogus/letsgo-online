#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef union SDL_Event SDL_Event;
bool emote_wheel_event(const SDL_Event *event);
bool emote_wheel_open(void);
void emote_wheel_available(bool available);
void emote_wheel_draw(float scale);
void emote_bubble_draw(uint8_t emote, uint64_t at, float x, float bottom, float scale);
