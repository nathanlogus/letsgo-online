// Social co-op's native TCP client. The relay transports only ephemeral avatar state and emotes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool present;
    uint8_t slot;
    char name[25];
    uint32_t area;
    uint32_t sequence;
    float x, y, z;  // game world units (centimetres)
    float yaw;
    uint8_t motion;
    uint64_t received_ns;
} CoopRemote;

// Call once for each game boot after g_program_id and the executable build are known.
void coop_net_start(void);
void coop_net_reset(void);  // RT_ON_RESET: stop its worker before the next in-place boot
bool coop_net_enabled(void);
void coop_net_publish_position(float x, float y, float z, float yaw, uint32_t area, uint8_t motion);
bool coop_net_remote(CoopRemote *out);
void coop_net_emote(uint8_t emote);
