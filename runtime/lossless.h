// Lossless Scaling (Steam): started with the game and its scaling turned on for the game window (runtime/lossless.c)
#pragma once
#include <SDL3/SDL.h>

void lossless_tick(SDL_Window *win);  // main thread, each pass of the window's event loop
