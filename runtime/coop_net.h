// Social co-op's native TCP client. The relay transports only ephemeral avatar state and emotes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define COOP_APPEARANCE_BYTES 80
typedef enum { COOP_LOADING, COOP_OVERWORLD, COOP_MENU, COOP_BATTLE, COOP_STORY } CoopActivity;
typedef enum { COOP_OFFLINE, COOP_CONNECTING, COOP_CONNECTED, COOP_RECONNECTING, COOP_ERROR } CoopConnection;
typedef struct {
    char server[256], name[25], room[17];
    bool host;
    char saved_rooms[4][17];
    bool teamup_battles;  // "Team-up battles" (default on): ordinary trainers offer the tagged-up partner a team-up
} CoopOptions;
typedef struct {
    CoopConnection connection;
    char detail[128], room[17], name[25];
    uint8_t local_slot, host_slot, attempt;
    bool locked;
    CoopActivity activity;
    float snapshot_age_ms;
} CoopSession;

void coop_net_options(CoopOptions *out);
void coop_net_preferences(const CoopOptions *options); // save name/server for the next connection
void coop_net_connect(const CoopOptions *options); // queues a connection; safe from the UI
void coop_net_leave(void);
void coop_net_session(CoopSession *out);
void coop_net_lock(bool locked);
void coop_net_remove_peer(void);
void coop_net_forget_room(unsigned index);
void coop_net_activity(CoopActivity activity);
typedef struct {
    uint8_t state, from_slot, reason; // state: 0 independent, 1 invitation, 2 social tag
    uint32_t id;
    uint64_t expires_ns;
} CoopTeamUp;
void coop_net_teamup(CoopTeamUp *out);
// 0 invite, 1 accept, 2 decline, 3 cancel/end. A social tag keeps battles solo.
bool coop_net_teamup_action(uint8_t command);
typedef struct {
    bool valid;
    uint8_t gender, style;
    uint8_t parts[COOP_APPEARANCE_BYTES];  // game resource IDs; no guest pointers
} CoopAppearance;

typedef struct {
    float relay_rtt_ms;  // -1 until the relay has answered a probe
    float snapshot_interval_ms;
    uint64_t snapshots_received;
} CoopNetStats;

typedef struct {
    bool present;
    bool world_ready;
    uint8_t slot;
    uint8_t identity[16];
    CoopActivity activity;
    char name[25];
    uint32_t area;
    uint32_t sequence;
    float x, y, z;  // game world units (centimetres)
    float yaw;
    uint8_t motion;
    int16_t stick_x, stick_y;  // remote overworld movement only; buttons/actions are not relayed
    uint64_t received_ns;
    uint8_t emote;
    uint64_t emote_received_ns;
    CoopAppearance appearance;
} CoopRemote;

// Call once for each game boot after g_program_id and the executable build are known.
void coop_net_start(void);
void coop_net_reset(void);  // RT_ON_RESET: stop its worker before the next in-place boot
bool coop_net_enabled(void);
void coop_net_stats(CoopNetStats *out);
void coop_net_publish_appearance(const CoopAppearance *appearance);
bool coop_net_avatar_ready(void);  // both worlds and appearances available, stable before initial join
void coop_net_set_avatar_head(bool visible, float x, float y, float z);
bool coop_net_avatar_head(float *x, float *y, float *z);
bool coop_net_local_head(float *x, float *y, float *z, char name[25]);
uint8_t coop_net_local_slot(void);  // stable room colour: 0 blue, 1 coral
void coop_net_publish_position(float x, float y, float z, float yaw, uint32_t area, uint8_t motion, int16_t stick_x, int16_t stick_y);
bool coop_net_remote_stick(int16_t *x, int16_t *y);  // false if the peer's last snapshot is stale
bool coop_net_remote(CoopRemote *out);
bool coop_net_remote_near(float radius_cm);
bool coop_net_social_near(float radius_cm); // friends overlay may send an emote while open
void coop_net_emote(uint8_t emote);
// Last local emote and a fresh world anchor, also available while playing solo.
bool coop_net_local_emote(uint8_t *emote, uint64_t *at, float *x, float *y, float *z);
