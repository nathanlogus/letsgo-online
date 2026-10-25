// Pointer-free native Pokemon serialization for the battle prototype. This is
// not a save block and does not insert received Pokemon into a saved party.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define COOP_PARTY_MAX 6
#define COOP_PB7_BYTES 260
typedef struct {
    uint8_t count;
    uint8_t pokemon[COOP_PARTY_MAX][COOP_PB7_BYTES];
} CoopParty;
bool coop_party_pokemon_valid(const uint8_t data[COOP_PB7_BYTES]);
bool coop_party_valid(const CoopParty *party);
uint64_t coop_party_digest(const CoopParty *party);
