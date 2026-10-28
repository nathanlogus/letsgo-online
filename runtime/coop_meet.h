// Meet-up decisions: which town the requester flies to for the partner's location (runtime/hle/coop_meet.c), kept free
// of game calls for tests/tools/meetup-test.c. Zones are the game's zone names (their hash is FNV-1a 64 of the name, the
// snapshot's area is that hash folded to 32 bits); towns are the town map's fly points (beluga_townmap_data.bin).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define COOP_MEET_TOWNS 11
enum { COOP_MEET_OK, COOP_MEET_NO_PARTNER, COOP_MEET_PARTNER_BUSY, COOP_MEET_PARTNER_INDOORS, COOP_MEET_LOCKED,
       COOP_MEET_SAME_TOWN, COOP_MEET_CANT_FLY_HERE, COOP_MEET_BUSY, COOP_MEET_REASONS };

// town001..town011 in the town map's order; the area and the centre (x, z in cm) of each town's terrain box
// (tests/logs/area-positions.txt).
static const struct { const char *zone, *name; uint8_t area; float x, z; } coop_meet_town[COOP_MEET_TOWNS] = {
    {"town001", "Pallet Town", 2, 1800, 1647},     {"town002", "Viridian City", 2, 2695, -6186},
    {"town003", "Pewter City", 3, 2050, 1228},     {"town004", "Cerulean City", 4, 10862, 897},
    {"town005", "Lavender Town", 8, 9354, 1377},   {"town006", "Vermilion City", 9, 893, 5608},
    {"town007", "Celadon City", 6, 2832, 1550},    {"town008", "Fuchsia City", 2, 16384, 5300},
    {"town009", "Cinnabar Island", 2, 1600, 13376}, {"town010", "Indigo Plateau", 1, 1306, -4068},
    {"town011", "Saffron City", 7, 3146, 3773}};

// Every outdoor zone (a town or a route): its area and the towns next to it, nearest first (1-based town numbers).
// Interiors, caves and the Safari Zone are not listed: the partner's last outdoor zone stands in for them.
static const struct { const char *zone; uint8_t area, towns[3]; } coop_meet_zone[] = {
    {"town001", 2, {1, 2}}, {"town002", 2, {2, 1, 3}}, {"town003", 3, {3, 2, 4}}, {"town004", 4, {4, 3, 11}},
    {"town005", 8, {5, 4, 11}}, {"town006", 9, {6, 11, 5}}, {"town007", 6, {7, 11}}, {"town008", 2, {8, 7, 9}},
    {"town009", 2, {9, 1, 8}}, {"town010", 1, {10, 2}}, {"town011", 7, {11, 7, 4}},
    {"road001", 2, {1, 2}}, {"road002_1", 2, {2, 3}}, {"road002_2", 3, {3, 2}}, {"road003", 3, {3, 4}},
    {"road004_1", 3, {3, 4}}, {"road004_2", 4, {4, 3}}, {"road005", 4, {4, 11}}, {"road006", 9, {6, 11}},
    {"road007", 6, {7, 11}}, {"road008", 8, {5, 11}}, {"road009", 4, {4, 5}}, {"road010_1", 8, {5, 4}},
    {"road010_2", 4, {4, 5}}, {"road011_1", 9, {6, 5}}, {"road011_2", 8, {5, 6}}, {"road012", 8, {5, 8}},
    {"road013", 8, {8, 5}}, {"road014", 8, {8, 5}}, {"road015_1", 8, {8, 5}}, {"road015_2", 2, {8, 5}},
    {"road016_1", 5, {7}}, {"road016_2", 6, {7}}, {"road017", 5, {7, 8}}, {"road018_1", 5, {8, 7}},
    {"road018_2", 2, {8, 7}}, {"road019", 2, {8, 9}}, {"road020", 2, {9, 8}}, {"road020_2", 2, {9, 8}},
    {"road021", 2, {1, 9}}, {"road022", 2, {2, 10}}, {"road023", 1, {10, 2}}, {"road023_2", 1, {10, 2}},
    {"road024", 4, {4}}, {"road025", 4, {4}}, {"t006r0301", 9, {6}}};
#define COOP_MEET_ZONES (int)(sizeof coop_meet_zone / sizeof *coop_meet_zone)

static inline uint64_t coop_meet_hash(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    while (*s) h = (h ^ (unsigned char)*s++) * 0x100000001b3ull;
    return h;
}
static inline uint32_t coop_meet_fold(uint64_t zone) { return (uint32_t)zone ^ (uint32_t)(zone >> 32); }  // coop_actor.c
// The outdoor zone with this snapshot area, or -1 (an interior, a cave, or a zone not listed).
static inline int coop_meet_zone_of(uint32_t area) {
    for (int i = 0; i < COOP_MEET_ZONES; i++) if (coop_meet_fold(coop_meet_hash(coop_meet_zone[i].zone)) == area) return i;
    return -1;
}
static inline int coop_meet_town_of(uint64_t zone) {  // 0-based town for a zone hash, or -1
    for (int t = 0; t < COOP_MEET_TOWNS; t++) if (coop_meet_hash(coop_meet_town[t].zone) == zone) return t;
    return -1;
}

// The town to fly to for a partner last seen outdoors in zone `zone` at (x, z). `usable` bit t: the traveller can fly
// to town t (visited on their own save and a fly point exists). `here`: the traveller's current town or -1. Of the
// zone's towns, a usable one in the zone's own area nearest to the partner wins, then the others in list order.
static inline int coop_meet_plan(int zone, float x, float z, uint16_t usable, int here, int *town, bool *exact) {
    *town = -1; *exact = false;
    if (zone < 0 || zone >= COOP_MEET_ZONES) return COOP_MEET_PARTNER_INDOORS;
    double best = 0;
    for (int i = 0; i < 3 && coop_meet_zone[zone].towns[i]; i++) {
        int t = coop_meet_zone[zone].towns[i] - 1;
        if (!(usable >> t & 1)) continue;
        double dx = x - coop_meet_town[t].x, dz = z - coop_meet_town[t].z;
        double key = coop_meet_town[t].area == coop_meet_zone[zone].area ? dx * dx + dz * dz : 1e18 + i;
        if (*town < 0 || key < best) *town = t, best = key;
    }
    if (*town < 0) return COOP_MEET_LOCKED;
    *exact = coop_meet_hash(coop_meet_zone[zone].zone) == coop_meet_hash(coop_meet_town[*town].zone);
    return *town == here ? COOP_MEET_SAME_TOWN : COOP_MEET_OK;
}

static inline const char *coop_meet_reason(int reason) {
    static const char *const text[COOP_MEET_REASONS] = {"", "No friend in the room", "Your friend is busy",
        "Your friend is indoors", "No town near them visited yet", "You are already in that town",
        "You can't fly from here", "Finish what you are doing"};
    return reason >= 0 && reason < COOP_MEET_REASONS ? text[reason] : "";
}

// The latest plan from the game thread (runtime/hle/coop_meet.c), for the Friends page and Communicate.
typedef struct { int reason, town; bool exact; } CoopMeetPlan;
void coop_meet_view(CoopMeetPlan *out);
bool coop_meet_script_busy(void);                  // a field script ran in the last half second
bool coop_meet_toast(char *out, unsigned size);    // the requester's travel result or the partner's notice, for 5 s
