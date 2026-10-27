// A Let's Go Mystery Gift card (WB7, 0x310 bytes) as the game's own "Get via internet" reads it (runtime/wb7.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WB7_SIZE 0x310
#define WB7_MAX_ITEMS 6

typedef struct {
    unsigned card_id;    // 0..2047 for a one-time card (the save's received flag); above that the game treats it as received
    bool one_time;
    unsigned date;       // YYYYMMDD
    bool pokemon;        // else items
    unsigned nitems, items[WB7_MAX_ITEMS][2];  // item id, count
    unsigned species, form, level, ball, moves[4];
    int shiny;           // 0 never, 1 random, 2 always
    int nature;          // -1 random, 0..24
    int gender;          // -1 random, 0 male, 1 female
    int ivs[6];          // -1 random; HP Atk Def Spe SpA SpD
} Wb7Gift;

bool wb7_build(const Wb7Gift *g, uint8_t out[WB7_SIZE]);
bool wb7_received(const uint8_t *save, unsigned card_id);  // the received flag in a 1 MB Let's Go save image
