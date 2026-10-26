// Cutscenes: the game's event scripts (AMX, docs/BATTLE-MODE.md) take the camera with the native EvCameraStart and give
// it back with EvCameraEnd - a talk with someone doesn't, a scene with the camera moving around does. Between the two
// game_cutscene() is true (runtime/nvn/vulkan/exec.c: SSAO at half its intensity there and on the title screen).
// Natives found by name in the image (the table of names and functions: LARECOMP_IMAGE_DUMP and a scan); Eevee 1.0.0
// addresses, Pikachu's in runtime/fnhook.c's table. LARECOMP_CUTSCENE_LOG=1: each start and end.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"

void rt_register_fnhook(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
uint64_t rt_addr(uint64_t eevee);  // runtime/fnhook.c: an Eevee 1.0.0 address in the running build (0: unknown build)
uint64_t renderer_game_frame(void);

#define EV_CAMERA_START 0x7000180a20ull  // native EvCameraStart (Pikachu 1.0.0: 0x70001809f0)
#define EV_CAMERA_END 0x7000180a70ull    // native EvCameraEnd (Pikachu 1.0.0: 0x7000180a40)

static volatile bool g_cut;
static int g_log = -1;
RT_ON_RESET(cutscene_reset) { g_cut = false; }

bool game_cutscene(void) { return g_cut; }

static int start_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    if (!g_cut && g_log) LOG("[cutscene] start at present %llu\n", (unsigned long long)renderer_game_frame());
    g_cut = true;
    return 0;  // (the native runs as usual)
}
static int end_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    if (g_cut && g_log) LOG("[cutscene] end at present %llu\n", (unsigned long long)renderer_game_frame());
    g_cut = false;
    return 0;
}

// Before each boot (runtime/main.c)
void cutscene_configure(void) {
    static bool hooked;
    g_cut = false;
    if (g_log < 0) g_log = getenv("LARECOMP_CUTSCENE_LOG") != NULL;
    if (hooked || !rt_addr(EV_CAMERA_START) || !rt_addr(EV_CAMERA_END)) return;
    hooked = true;
    rt_register_fnhook(EV_CAMERA_START, start_hook);
    rt_register_fnhook(EV_CAMERA_END, end_hook);
}
