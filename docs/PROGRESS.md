# Progress

Current state of the port. The full history, with each problem and how it was solved, is in
[HISTORY.md](HISTORY.md). The diagnostic tools are in [DEBUGGING.md](DEBUGGING.md).
The prioritized backlog for a mature version is in [ROADMAP.md](ROADMAP.md).

_Updated on 2026-10-17._

## What works

**2026-10-24 — PC settings in the native Options:** Eevee 1.0.0 has 21 extra options
in the original list: graphics, image, audio, debugging and wild battles.
They are native rows, with values, arrows, descriptions and reset buttons; left/right
apply and save the changes immediately. They do not open the overlay. F12 is still available.
Pikachu and updates still use F12: the new addresses
and native layouts have not been validated on those versions.

**2026-10-25 — Classic combat in Eevee 1.0.0:** capture from the Bag and XP
on defeat use the native routines. Capture, consumption of balls/medicine, shared
XP and persistence were verified on a copy of the save. The partner
Eevee enters with the regular model, keeping its form and stats; healing and capture
with it in the lead were also verified.

Capture in combat now reuses the native trainer and ball model
for a visible throw to the target. The scene shows the impact, fall and shake,
restores the Pokémon if it escapes and waits for the animation before the result.
The return to the field after capture was fixed: the sequence uses the native
resource cleanup, including on the initial Route 1 save and after leveling up.

| Area | Status |
|---|---|
| Boot | complete: 4 modules, SDK, constructors, threads, system services |
| Games | *Let's Go, Eevee!* and, since 0.2.1, *Pikachu!* (program id in `main.npdm`), in any version. Parity: every feature applies to both (CLAUDE.md); the fixed addresses (60 fps, wild battles, generator patches) have a per-build table for 1.0.0 of both (`runtime/fnhook.c`, `tools/patches.py`, mapped with `tools/addrmap.py`); updates run without those options. Pikachu 1.0.0 tested: setup, new game up to the name, overworld, menus, 60 fps, wild battle and capture (save copied from Eevee), PT-BR translation, GO Park. Missing: Pikachu new game beyond the name, Play with Pikachu, exclusive Pokémon |
| Controller | PC gamepad as handheld mode (controller selection accepted) |
| Translations / mods | `mods/<nome>/` folder in the Atmosphere format (LayeredFS): romfs files replaced or added, chosen in the launcher ("Translation"). Tested with the PT-BR fan translation (Pokémon Switch Brasil, v3.1): texts, menus and images in Portuguese. To install: the launcher's "Install translation..." button with the downloaded file (.zip/.rar/.7z), or copy Eevee's `romfs` into `mods/<nome>/romfs` |
| Release | `LetsGoRecomp.exe` in a zip (`make dist`): the player chooses the game folder in the launcher, which checks for v1.0.0, copies the files and recompiles the game code on their PC (frozen generator + Zig); launcher in English or Portuguese |
| Touch screen (Play with your partner) | mouse: holding the left button is the finger; controller: the right stick moves a finger (circle on the screen) and holding ZR touches. Petting, opening the berry tray and bringing the berry to the mouth work (tested only with a controller: Eevee ate). Works at any window size and aspect ratio |
| Keyboard and mouse | WASD walks / aims, Space or Enter = A, Q or Backspace = B, Tab = X (menu), R = Y, Z / C = L / R, `=` / `-` = Plus / Minus, arrows = d-pad. Mouse: while aiming the Poké Ball (after "Get ready") it is captured by the window: movement = aim (virtual gyroscope), left = throw; outside of that it is free and the left button is a finger on the touch screen (Play with Eevee: petting); right = B always. The keys and the mouse sensitivity are changed in the launcher ("Controls" page) |
| Opening video | correct image and audio, 1080p, ends and continues into the game |
| Audio | music and effects (Opus via `hwopus`), video audio without dropouts |
| Pacing | locked 30 fps (the game's own) by default; experimental 60 fps option in the overworld (item 5) |
| Introduction | language, Professor Oak, character choice, name typed correctly |
| 3D | correct scenery (bedroom, Pallet Town, sky), depth/shadow passes |
| Characters and Pokémon | correct skinned models, with animation (fix for `LDC.64` in the shader translator) |
| Gameplay | walking, meeting the Professor, catching Pikachu (centered aim) |
| Save | saving and loading work (`save/save_0/savedata.bin`) |
| Wild battles (experimental, on by default; `wild_battle = 1`) | Eevee 1.0.0: classic combat without a timer, XP on defeat and capture during the battle from the Bag, with real quantities of balls and medicine. Captures receive the trainer's data and go to the party or storage; the post-battle does not open the GO capture. Pikachu keeps battle followed by capture. The option can be changed in Eevee's native Options during play. More areas, nature/shiny and parity with Pikachu still need validation ([BATTLE-MODE.md](BATTLE-MODE.md)) |

## What is missing
1. **Validate the 3D in more scenes** (battles, large Pokémon, cities): the fix for skinned models was
   validated in Pallet Town. History in [HISTORY.md, part 6](HISTORY.md#part-6-exploded-characters-fixed).
2. **Motion aiming**: mouse (virtual gyroscope) and the controller's real gyroscope (SDL, written on 2026-10-16 and kept outside `main`
   in the `git stash` "giroscopio do controle"; **not tested with a real controller**: axis direction to be confirmed; `LARECOMP_GYRO_PAD_FLIP=1|2|3` inverts
   yaw/pitch). Sensitivities in the launcher. Missing: pitch drift correction from the accelerometer.
3. **Online features** (Pokémon GO, trades, Poké Ball Plus): turned off on purpose (offline services).
4. **Performance** (measured on 2026-10-15, `tools/perf_summary.py` over 4 min walking along Route 1): stable 30 fps
   (minimum 28.6 per 2 s window, no frame above 50 ms apart from the test's screenshot, no audio underrun) on an
   RTX 3080. When entering an area, ~0.3 s of objects popping in while the new shaders become ready (with cache: ~200 ms
   of translation and ~90 ms of SPIR-V for ~380 programs); the first time without cache, a few seconds. The pipeline cache
   is now saved during play, not only on exit. In gameplay, a draw waits for its shader (up to 20 ms per frame) instead of
   disappearing: zero skipped draws after loading. Shader translation runs in parallel: even on the first visit
   (empty cache) the area appears complete after loading. Quality: SMAA on, 16x anisotropic filtering and well
   downscaled supersampling (1440p / 4K in the launcher on a smaller screen; 4K runs at ~78 fps uncapped on an RTX 3080). Details in
   [HISTORY.md, parte 8](HISTORY.md).
   10/17 audit: dynamic resolution treated a steady 30 fps as 60 Hz slowness; the budget was fixed
   to 30/60 depending on the scene. Isolated test and scripted Eevee at 30/60 (redone with an LF ini: before it dropped to 0.60, now it stays at 0.95-1.00) passed; the option only exists in the ini (`resolution = dynamic`); optional frametime collection gives
   the average and P95/P99 ([performance-analysis.md](performance-analysis.md)). Pikachu, transitions and CPU/GPU profiling are still missing.
   10/17-18 profile (Eevee, RTX 3080, Route 1 overworld): uncapped, the overworld ran at ~60 fps (the ~160 fps note
   above did not repeat on this route); two causes found and fixed: waiting on a mutex in the emulated kernel (HISTORY 57: the
   paced 60 fps went from 58.8-59.3 to 60.0 fps) and the GPU reading vertices over PCIe (HISTORY 58: GPU from ~10 to
   ~1.2 ms per frame, uncapped fps ~75 -> ~90). The limit is now the CPU chain between the game's 3 threads
   ([RESEARCH-GPU.md](RESEARCH-GPU.md)). Not tested on a weaker GPU or an integrated card.
5. **60 fps (experimental, "60 fps (experimental)" option in the launcher, off by default; `fps = 60` in larecomp.ini or `LARECOMP_FPS60=1`)**: overworld at native 60, menus and Pokédex at 30 with an intermediate frame, battles and captures at 30 (HISTORY 43-49). History of the investigation: the game logic advances one step per frame. With `LARECOMP_FPS=60` it reaches the same state
   at the same frame number in half the time, that is, it runs at double speed (measured on 2026-10-15 with
   `tools/repro.sh`). 60 fps would require changing the game's own time step. What is already known:
   - The engine chooses the present interval at the end of the frame (`0x7000029e80`, function `0x7000029d30`):
     `clamp(config[+8] / (G − 1), 1, 5)`, with `G` = 16,666,667 ns (one 60 Hz vsync, pointer at `0x70015fb438`). The
     result is 2 and is only applied once (`SetPresentInterval 2`, via `0x70005e79c0`); even when paced at 60, the game does not
     request interval 1, so `config[+8]` is fixed (~33.3 ms), not a measurement.
   - The only value near 33.3 ms found in `main`'s data and in the heap (`33.412.440` at `0x727eeccb68`) is not it:
     changing it to 16,666,667 changes nothing.
   - The float 1/30 appears once (`0x7000f47428`), used only in an interpolation function (`0x7000611440`), not in the loop.
   - The application loop (`0x70000231a0`) calls the update (`0x700001d1c0`) and then the end of frame through a vtable.
   - ~~There is no global time step~~ (**corrected on 2026-10-16**: the scan was looking for floats and did not find it because the
     step is an integer in nanoseconds, on a stack). There is a time reference: the duration table `G`
     (`main+0xf7eeb8`: 16.7 / 33.3 / 50 / 66.7 / 83.3 ms, pointer at `0x70015fb438`) and a timer object on the
     main thread's stack with two steps. Step 1 is rewritten on every overworld update by the accessor
     `f_7000039600` with `G[1]`; step 0 stays at 33.3 ms. The systems that use the step's **value** (animations
     `f_7000663660`/`f_7000664cd0`, `f_700067be50`, `f_70005ed1e8`/`f_70005ed908` and others) read step 1 and
     convert time into frames by dividing by `G[1]`; that is why changing `G[1]` in the accessor keeps the overworld correct at
     60. What does not go through there: integer frame counters (the capture and battle timelines) and
     step 0, which only decides whether an object updates or not. Details and measurements: [RESEARCH-60FPS.md](RESEARCH-60FPS.md),
     "Time map".
   - Conclusion: 60 fps at the right speed does not come from a patch. The paths would be generating intermediate frames
     (interpolation in the runtime, without the game's motion vectors: artifacts, mainly in the interface) or an
     external frame generation program. Research into the techniques used by other ports: [RESEARCH-60FPS.md](RESEARCH-60FPS.md).
   - **Interpolation prototype removed** (2026-10-15): re-executing each frame with blended uniforms worked
     for the camera, but without markers from the game itself (as RT64 has) it left visible errors - repeated objects
     swapped (ghosts), blank menus, stretched models, flashes in effects. The game stays at the original 30 fps.
6. **The aura of special Pokémon flickers** (also in the release, at 30 fps): in the player's video (2026-10-17) the aura disappears for
   exactly 1 game frame, at irregular intervals, with the Pokémon in place. Ruled out by measurement: GPU
   timestamps (`LARECOMP_VK_COUNTER`), sync waits, texture copy to memory, interpolation, and a system clock
   pinned per frame (tested in-game by the player: same). Tools to continue: `LARECOMP_COUNT_TEX=1024x256`
   and `LARECOMP_COUNT_FS=<hash>` (draws per frame), `LARECOMP_TRACE_COUNT` (lists that contain a draw and who
   calls them), `LARECOMP_CALLCOUNT`, `tools/blink_scan.py`. The test route has no Pokémon with an aura; F9 recording changes the
   result itself (the first recorded frame loses the effects), so it is useful for structure, not for finding the flicker.

## Runtime fixes (summary)
| Area | Fix | File |
|---|---|---|
| Kernel | exception context (`svcGetInfo` 20), Title ID from `main.npdm` | `runtime/kernel.c`, `main.c` |
| Kernel | condvar does not reacquire the mutex on timeout; does not overwrite a "woken" with a timeout | `runtime/kernel.c` |
| Loader | 4 modules (`main`, `sdk`, `subsdk0` glslc, `subsdk1` multimedia) | `runtime/loader.c`, `dispatch.c`, `gamelib.*` |
| Services | offline Bluetooth (`btm:u`, `bt`), `csrng`, `hwopus` | `runtime/services/offline.c`, `hwopus.c` |
| Video | game buffers in `GetOutputBuffer`, rotating slots, `FindInt64`/`SetCoreMask`, end of stream `0x2000`, game allocator | `runtime/hle/movie.c` |
| `vi` | `SetLayerScalingMode` | `runtime/hle/vi.c` |
| Controller | handheld mode, styles supported by the game, keyboard / mouse, virtual gyroscope driven by the mouse | `runtime/hle/input.c`, `runtime/hle/bindings.c`, `services/hid_set.c` |
| Keyboard | name in UTF-16 | `runtime/hle/swkbd.c` |
| NVN | per-stage programs, vertex-shader-only programs, pacing by the present interval | `runtime/nvn/cmdbuf.c`, `objects.c`, `vulkan/exec.c` |
| Shader translator | `LDC.64` whose address register is also the destination (skinned models) | `runtime/nvn/maxwell/translate.c` |
| Textures | `RGB5A1`/`RGBA4` converted to RGBA8 (Pokémon icons) | `runtime/nvn/vulkan/textures.c` |
| Textures | no scaling for CPU-filled textures; invalidation on host writes | `runtime/nvn/vulkan/textures.c` |
| Audio | minimum queue in `audout` + underrun count | `runtime/services/audio.c` |
