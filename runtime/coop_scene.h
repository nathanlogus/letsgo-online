// Shared Adventure (docs/COOP-SHARED-ADVENTURE.md): mapped story scenes a partner can take over after its own consent,
// the relay's Y channel and the per-save ledger <save>.scene. No game calls here (tests/tools/scene-test.c uses it
// directly); the game side is runtime/hle/coop_scene_apply.c, the overlay runtime/ui/coop_scene_ui.mm.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A mapped scene: the event flags its completion sets (by the game's names; the game's FlagGet/FlagSet take FNV-1a 64
// of the name), the flags that must already be set on the receiving save (its story point) and at most one one-time
// gift. The wire carries only the id: a peer can never name flags or items.
typedef struct {
    uint16_t id;
    const char *name, *done[2], *needs[2];
    uint16_t item, count;
    const char *item_name;
} CoopSceneDef;
#define COOP_SCENES 1
extern const CoopSceneDef coop_scenes[COOP_SCENES];

enum { COOP_LEDGER_NONE, COOP_LEDGER_PENDING, COOP_LEDGER_ACCEPTED, COOP_LEDGER_DONE, COOP_LEDGER_DECLINED };
enum { COOP_SCENE_OK, COOP_SCENE_ALREADY, COOP_SCENE_NOT_REACHED, COOP_SCENE_PERSONAL };  // coop_scene_check
// Y2 answers, and (10 + reason) for the relay's Y4 closes in the initiator's result.
enum { COOP_ANSWER_READY = 1, COOP_ANSWER_NOT_YET, COOP_ANSWER_STAY_OUT, COOP_ANSWER_ALREADY, COOP_ANSWER_NOT_SHARING };
enum { COOP_CLOSED_MODE = 1, COOP_CLOSED_NOT_NEAR, COOP_CLOSED_BUSY, COOP_CLOSED_NO_PARTNER, COOP_CLOSED_EXPIRED,
       COOP_CLOSED_GONE, COOP_CLOSED_STALE };
// Y0 modes: 0 Social, 1 Shared (no consent for this session yet), 2 Shared and consent given: the relay offers
// scenes only between two players in mode 2.
enum { COOP_MODE_SOCIAL, COOP_MODE_SHARED, COOP_MODE_ACTIVE };

typedef struct {
    bool shared, nearby, consent;
    uint8_t peer_mode;
    uint8_t ledger[COOP_SCENES];
    bool observed[COOP_SCENES], completed[COOP_SCENES], reached[COOP_SCENES];
    bool asked;  // an incoming offer waits for this player's answer
    int in_scene;
    uint64_t in_id, in_expires_ns;
    int out_phase, out_scene;  // the initiator: 0 none, 1 waiting for the partner, 2 its result (until out_until_ns)
    uint8_t out_result;
    uint64_t out_id, out_expires_ns, out_until_ns;
    char toast[160];
    uint64_t toast_until_ns;
} CoopSceneView;

// Pure helpers.
uint64_t coop_scene_hash(const char *name);
int coop_scene_index(uint16_t id);                 // -1: not a mapped scene
bool coop_scene_personal(const char *flag);        // badges, gyms, the rival, the League, trainers: never shared
bool coop_scene_valid(const CoopSceneDef *def);    // mapped flags only, none personal, at most one gift
// The receiving save's story point (get(ctx, hash) = FlagGet).
int coop_scene_check(const CoopSceneDef *def, bool (*get)(void *ctx, uint64_t hash), void *ctx);

// Settings (coop_net.c: coop.ini) and the UI.
void coop_scene_set_mode(bool shared, bool nearby);
void coop_scene_consent(bool on);                  // this session's consent; coop_net clears it for a new session
void coop_scene_view(CoopSceneView *out);
bool coop_scene_offer(int scene);                  // the initiator offers a scene it completed
bool coop_scene_answer(int answer);                // the partner: READY / NOT_YET / STAY_OUT
bool coop_scene_cancel(bool continue_without);     // the initiator, while it waits
bool coop_scene_catch_up(int scene);               // a pending scene: explicit, Shared mode only
void coop_scene_dismiss(void);

// Relay worker.
void coop_scene_tick(void);
bool coop_scene_pop(uint8_t body[255], size_t *size);
void coop_scene_message(const uint8_t *body, size_t size);
void coop_scene_link_lost(void);

// Game thread: the scene's completion flags (done) and story point (reached) as the running game has them.
void coop_scene_observe(int scene, bool done, bool reached);
bool coop_scene_to_apply(int *scene);              // an accepted scene to apply now (Shared mode only)
// result: a coop_scene_check result (OK: applied now), or -1: no room for the gift in the Bag (retried later).
void coop_scene_applied(int scene, int result);
void coop_scene_serialized(void);                  // the game built a save image
// Durability (fs.c): the save opened (loads <save>.scene); a whole-save write ended.
void coop_scene_recover(const char *save);
void coop_scene_after_replace(bool ok);
void coop_scene_reset(void);
