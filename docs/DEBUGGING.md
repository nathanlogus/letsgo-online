# Debugging tools

Everything that was used to diagnose the port. The environment variables apply to `build/lasrecomp.exe` (in Git Bash:
`LARECOMP_X=1 ./build/lasrecomp.exe extracted --play`).

## Running and input
| Variable | Effect |
|---|---|
| `LARECOMP_INPUT="frame:BOTAO:frames,..."` | scripted input (A, B, X, Y, L, R, PLUS, MINUS, directions, STICK_*). Disables the real controller during the test |
| `LARECOMP_NPAD=handheld\|fullkey\|dual\|left\|right` | how the PC controller appears to the game (default: `handheld`) |
| `LARECOMP_FPS=<n>` | forces n fps (default: the game's interval, 30 fps; `0` = no limit) |
| `LARECOMP_NO_AUDIO=1` | no audio (automated tests) |
| `LARECOMP_FONT=<arquivo>` | system font handed to the game instead of the default list (`runtime/hle/pl.c`; the Yu Gothic Regular of some Win10 installs crashes the font engine, issue #1) |
| `LARECOMP_LANG=en\|pt\|ru` | language of the overlay menu (closing the game, name keyboard); without it, the system language |
| `LARECOMP_MENU_KEY=<tecla>` | another key for the settings menu (default `F12`; SDL scancode name: `F10`, `Insert`...) |
| `LARECOMP_SWKBD_UTF8=1` | keyboard returns UTF-8 instead of UTF-16 |
| `LARECOMP_GYRO_TEST=x\|y\|z` | virtual gyroscope oscillating ±15° on the given axis (`z+` / `x-`...: a 10° turn ramped over 1 s after aiming starts); logs when the game reads the sensor. To get to a catch: walk through the grass (`STICK_*`) and press A on "Get ready"; the encounter is random, repeat until the log has `sensor started` |
| `mod` / `game_lang` (larecomp.ini) | active mod (`mods/<nome>/`, the log shows `[mods]` and how many romfs files were replaced) / language requested from the system (`en-US`, `es`...; an existing save keeps its own) |
| `LARECOMP_LOG_FILES=1` | each romfs file read by the game (to find where a text or image is for a mod) |
| `LARECOMP_TOUCH=<de>-<até>[@x,y[,largura]]` | a finger going back and forth around x,y (pixels of the 1920x1080 screen; width 0 = still) between those frames; `@x,y>x2,y2` drags; several segments with `;`. Logs `[touch]`: start and end of the touch, gestures and who reads them. Playing with Eevee: `1440:X:6,1490:UP:6,1530:A:6,1640:A:6,1700:A:6`; petting the forehead `1760-1960@960,375`; berry button `@1807,952,0`. The controller's finger in the script: `RSTICK_*` and `ZR` |
| `SAVE=<savedata.bin ou pasta> tools/repro.sh ...` | runs the test with another save (copied to `build/test-save`; the player's `save/` is not touched). The `LARECOMP_INPUT` timings change from one save to another (tutorials seen or not) |
| `TURBO=0 tools/repro.sh ...` | `repro.sh` runs in turbo by default (`LARECOMP_FPS=0`, ~4x faster: the frames are the same, only idle animations tied to real time change phase); `TURBO=0` goes back to real pace, to measure fps or audio |
| `LARECOMP_DRAW_CHAINS=<quadro>[-<último>]` | records each draw, list recording (`begin`) and list call (`call`) in those frames, with the game's function chain (`[chain]`) |
| `LARECOMP_HOOK_DUMP_FRAMES=<primeiro>-<último>` | limits `LARECOMP_HOOK_DUMP` to those frames |
| `LARECOMP_ELAPSED_LOG="<função>:<passo>,..."` (+ `LARECOMP_TIME_PROBE_FRAMES=<de>-<até>`) | the time step (ns) each function receives from the timer object, with the object and the getter (docs/RESEARCH-60FPS.md, "Time map") |
| `LARECOMP_TIMER_USERS=<de>-<até>` | who reads each time step (calls to the getter `f_70000395f0` by return address), listed in the last frame |
| `tools/teste60.sh <captura\|batalha> <0\|1\|2>` | the same scene at 30 fps and in a 60 mode, with images at the same game time (`build/teste60/<cena>-<modo>/sheet.png`) |
| Warning | `fnhook` keeps **one hook per address**: the last one registered wins; two probes on the same function cancel each other out |
| `LARECOMP_DAYS_AHEAD=<n>` | clock n days ahead (daily things: the partner's hunger, which has to be above zero for it to eat) |
| `LARECOMP_WILDBATTLE=1` (+ `LARECOMP_WILDBATTLE_*`) | wild battles (docs/BATTLE-MODE.md; the launcher enables it the same way). `SPECIES=<n>`/`LEVEL=<n>`: always that Pokémon (default: the one touched); `MOVES=a,b,c,d`: those moves (default: by level); `MAX=<n>`: battles per run (default: all); `FADEIN=0` disables the manual fade in, `FADE_TYPE`/`FADE_DELAY`/`FADE_FRAMES` tune it; `LOGPOKE=1` logs the Pokémon object's fields (`=2` writes `build/poke_<quadro>.bin`); `STATE_LOG=1` the states of the battle process and event (with the result); `DEBUG=1` records the passes of one frame 240 frames after the battle; `USECFG=1` (experimental, crashes the game) launches the builder's configuration. Catching mid-battle (step 6): `AUTOCATCH=<n>` turns the n-th menu choice in each battle into a catch request (tests without touching the button); `FORCE_RATE=<n>` answers with that catch rate (1 to fail, 255 to catch); `HP=<n>` starts the wild Pokémon with n HP (knockout in one move, to test XP); `THEIRTURN=1` makes every catch round
start with the wild Pokémon's turn (normally only after a ball it broke out of); `CATCH_MENU=0` hands over the original menu even in the mode's battles (the trainer battle path); `CATCH_DUMP=<pasta>` writes there the menu files assembled at boot; `CATCHLOG=1` who asks for the catch rate; `HPLOG=1` each battle Pokémon assembled and returned (HP, status, id); `MENULOG=1` the menu (the CATCH button enabled, the choices); `MSGLOG=1` the decoded lines in our battles; `HPSCAN=<A>:<B>[:<C>]` (+ `HPSCAN_HP`) searched for HP in memory |
| `LARECOMP_WILDBATTLE_CLASSIC=0` | Eevee 1.0.0: disables the classic rules and restores battle followed by the GO catch for comparison. Without an override, classic combat follows the wild battles option. `LARECOMP_WILDBATTLE_STATE_LOG=1` also records XP changes (`[wild-classic]`). |
| `LARECOMP_WILDBATTLE_CATCH_ANIMATION=0` | Eevee 1.0.0: disables only the new in-battle catch animation to compare with the original empty callback. By default, throw, impact and waiting for the result stay enabled in classic combat. |
| `LARECOMP_WILDBATTLE_TEST_AT=<quadro>` | isolated test on Eevee 1.0.0: starts a synthetic encounter on the first field update after that frame. Use a copy of the save and `LARECOMP_SAVE_DIR`; it does not depend on random spawns. |
| `LARECOMP_SKIP_CONTROLLER=0/1` | without the controller selection screen at startup (the launcher enables it the same way: `skip_controller`); `runtime/hle/controller_skip.c` |
| `LARECOMP_CUTSCENE_LOG=1` | records the start and end of each cutscene (`[cutscene]`: the script natives `EvCameraStart` / `EvCameraEnd`; `runtime/hle/cutscene.c`): there and on the title screen SSAO goes to half |
| `LARECOMP_IMAGE_DUMP=<arquivo>` | writes the `main` module as loaded and relocated (base `0x7000000000`): to read the executable without the NSO tools (the script natives table, strings) |
| `LARECOMP_INPUT` with `b<N>:<botão>:<len>` / `c<N>:...` | inputs counted from the start of the wild battle / of the catch after it (the encounter frame changes every round); after the marker, absolute inputs stop |
| `LARECOMP_FNPROF=b<de>:<até>` | the function profile window counted from the start of the wild battle |
| `LARECOMP_WATCH_READ=1` | the `LARECOMP_WATCH` watch also catches reads (the page becomes inaccessible) |
| `LARECOMP_WATCH=<addr>[,<addr>...]` | write watch (Windows, `runtime/hle/memwatch.c`): protects the page of those addresses (32-bit words) and, on each write to them, logs the backtrace of the writer (game functions as `f_<endereço>`). Heap addresses **are not the same between runs** |
| `LARECOMP_WATCH_PARTY=<quadro>` + `LARECOMP_WATCH_SPECIES=133:10,16:9,...` | searches the heap for 0x104-byte records in PB7 format (species u16 at `+8`, XP u32 at `+0x10`, level at `+0xEC`) and watches the XP of each one. The heuristic finds many false positives (the in-memory structure may not be raw PB7) |
| `LARECOMP_SNAP=<quadro A>,<quadro B>` | copies the non-null heap pages at A and, every 250 frames from B on (30 times), lists the most common small deltas of the words that grew (3 to 20 hits) with example addresses; a team's XP gain is a shared delta. The heap has many counters changing, so the noise is large |
| `LARECOMP_GYRO_AXES=<yaw><pitch>` | virtual gyroscope axes moved by the mouse (default `zx`) |
| `mouse_sens` (larecomp.ini) | degrees of rotation per mouse pixel (default 0.1) |
| `LARECOMP_LAUNCHER_TAB=<n>` | launcher opened on page n (0 Game, 1 Graphics, 2 Image and sound, 3 Controls, 4 Mods; with `LARECOMP_LAUNCHER_SHOT`) |
| `LARECOMP_LAUNCHER_KEYS=1` / `<slot>` / `<slot>:<tecla>` | launcher on the "Controls" page / waiting for the key of action `slot` (`BIND_*` in runtime/input_bind.h) / pressing that key (with `LARECOMP_LAUNCHER_SHOT`) |

## Hangs and crashes
| Tool | Use |
|---|---|
| Crash backtrace | game functions appear as `f_<endereço>`; `svcBreak` shows the `nn::Result` |
| `LARECOMP_THREAD_DUMP=<s>[,<repete s>]` | lists each thread: the svc it is blocked on, arguments and the guest stack |
| `LARECOMP_TRACE_SVC=1` | logs every system call (very high volume) |
| `LARECOMP_PROF_SVC=1` | every 2 s, per guest thread: system calls (`svc`), how many and how long each one blocked (`[svcprof]`) |
| `LARECOMP_PROF_CONDVAR=1` | every 2 s, the game's condition waits (`WaitProcessWideKeyAtomic`) by duration and the average latency between the signal and the thread running again (`[condvar]`) |
| `LARECOMP_PROF_LOCKS=1` | every 2 s, the game's most contended mutexes (`ArbitrateLock` that had to wait): address, waits, time and the guest stack of the first one (`[locks]`; resolve with `tools/addr2sym.py`) |
| `LARECOMP_GPUPROF_AT=<quadro>[,<quadro>...]` | GPU time per pass, copy and dispatch for those frames (`[gpuprof]`; the header carries the frame number). With `LARECOMP_GPUPROF_DRAWS=1`, a timestamp before each draw (one line per draw: vertex program, hash and address of the fragment shader, indices, instances); with `LARECOMP_GPUPROF_STATS=1` also the pipeline statistics of each draw (`ia`, `vsinv`, `clipprim`, `fsinv`). The timestamps serialize the GPU: they are for comparing draws with each other, and the total runs a little above the real one. Our passes have one line each (`ours: camera motion vectors`, `GI`, `GI trace`, `GI blur, temporal`, `GI composite`, `SSAO`, `contact shadows`, `post`, `FSR 3.1`, `present to the window, menus`); `(submission end ...)` closes each submission. Before, the "SSAO" line measured the motion vectors and the GI one included SSAO, and the last pass of the frame (FSR 3.1, the presentation) did not appear |
| `LARECOMP_EXP_VS=<endereço do vertex program, hex>` + `LARECOMP_EXP_MODE=skip\|scissor\|half` | research: in the draws of that program, does not draw, draws with a 1x1 scissor (no pixels, only vertices) or with half the instances. `LARECOMP_EXP_ALT=1` only on odd frames, to have both versions of the same scene in the same run (see [RESEARCH-GPU.md](RESEARCH-GPU.md): comparing separate runs is misleading) |
| `LARECOMP_EXP_VS_EARLY=<endereço>[:<pc, hex>]` | research: that program's vertex shader returns at the start of `main`, or before the instruction at `pc` (the `pc` only exists in the GLSL with `LARECOMP_SAVE_MSL=1`) |
| `LARECOMP_GLSL_OVERRIDE_DIR=<pasta>` | research: `<pasta>/<endereço do vertex program, hex>_vs.glsl` replaces the generated GLSL (start from `build/glsl/`, written by `LARECOMP_SAVE_MSL=1`) |
| `LARECOMP_NO_ROBUST=1` | research: disables Vulkan's `robustBufferAccess` |
| `LARECOMP_VERTEX_READ=direct\|copy\|auto` | where the GPU reads the draws' vertices and indices from: `direct` = in place, from the imported game memory (over PCIe on a discrete card); `copy` = copy in VRAM; `auto` (default) = copy on a discrete card, in place on an integrated one. Ranges a compute writes always stay in place ([RESEARCH-GPU.md](RESEARCH-GPU.md)). The statistics line shows `vertex ranges N copied, M in place` |
| `LARECOMP_RENDER_THREAD=0\|1` | the Vulkan backend's render thread (enabled by default; `render_thread = 0` in `larecomp.ini` also disables it). `vertex_read = auto\|copy\|direct` in the ini is equivalent to `LARECOMP_VERTEX_READ`. `LARECOMP_HEAL_KEYS=1`: F5 makes every texture recheck memory, F6 reads all vertices in place (research, HISTORY 60) |
| `LARECOMP_INTERP=1` + `LARECOMP_INTERP_MULT=<k>` | research: re-execution interpolation always on, with a fixed k images per present (2..8; without `_MULT`, 2). The F12 menu's frame generation (`frame_gen = 2 / 3 / 4`) uses the same interpolation with k chosen by each frame's pacing. `LARECOMP_WINDOW_SHOTS` writes `mid1`..`mid<k−1>` and `fin` (with k = 2, `mid`). Results: docs/RESEARCH-60FPS.md, "Free multiplier" |
| `LARECOMP_FG_AT=<present>:<k>,...` | changes frame generation to k (0, 1 = Auto, 2, 3, 4) at those presents, like the F12 menu |
| `LARECOMP_FG_DISPLAY_HZ=<hz>` | (research) Auto frame generation picks the multiplier as if the screen had that refresh rate (without it: the Windows compositor's, `[fg] auto: ...` in the log) |
| `LARECOMP_FG_VRR=1` | the screen has a variable refresh rate (G-Sync / FreeSync): scheduled images go out off the refresh grid and Auto picks the largest multiplier up to the refresh rate, without requiring it to divide evenly |
| `LARECOMP_FG_SCALE=<f>` | (research) the in-between images rendered at f times the size of the real frames on each axis (0.25..1; without it, full size - or, in Auto, 75 % / 50 % when the GPU cannot keep up, before lowering the multiplier: `[fg] auto: ...`). A render target that the re-execution reads before drawing is filled from the full-size one (`[vk] reduced in-between images: ...`) |
| `LARECOMP_LO_SHARP=<stops>` + `LARECOMP_LO_HIST=<peso>` | (research) the reduced in-between images without FSR 3.1, redone against image N: the RCAS strength afterwards (stops; negative: no RCAS; default 1 at 75 % and 0.9 at 50 % with SMAA, 1.3 and 1.15 with TAA) and the weight of image N (default 0.9) |
| `LARECOMP_PRESENT_LOG=<arquivo>` | CSV with each window image: the present, when the call returned and when it reached the screen (`VK_KHR_present_wait`, on a separate thread): `id,frame,image,nimg,present_ms,shown_ms`. Shows the frame generation pacing as it is seen |
| `LARECOMP_PENDING_LOG=1` | each scheduled image (frame generation) when shown: the delay relative to its time and the learned lead (`lead`) |
| `LARECOMP_FLOW_MATCH=0` | (comparison) optical flow frame generation (`fg_method = flow`) only with the renderer's motion vectors (camera and objects), without the block matching on luminance that corrects them where they do not match the image (shadows moving over still ground, particles, water, reflections). On the walk through the grass at 720p, 2x: flicker 0.76 → 1.10 % of blocks |
| `LARECOMP_FLOW_RADIUS=<px>` | (research) the optical flow block matching search range in luminance pixels (~640 wide, at any resolution), in steps of 2 around the analytic vector (default 4) |
| `LARECOMP_FLOW_OCCLUSION=0` | (comparison) optical flow without the N-1 depth test: a point found in N is always blended with N-1 where it was, even if something closer covered it in N-1 |
| `LARECOMP_FLOW_SHARP=<k>` | (research) the strength of the sharpening after the optical flow's N-1 and N blend (default 0.2; 0 disables: −9 % detail compared to the real frames, 0.5: +13 %) |
| `LARECOMP_FLOW_DEBUG=1` | the optical flow in-between images tinted: red where block matching replaced the analytic vector, green where only N (the point was covered in N-1), blue where only N-1 (uncovered after t) |
| `LARECOMP_FG_GPULAG=1` | (research) on each present with frame generation, waits for the GPU and records how long after the present it finished the images (`[gpulag]`); the wait ruins the pacing, only for measuring |
| `LARECOMP_FG_DRAW_AT_TIME=1` | (comparison) scheduled images go back to being drawn at their own time, on the main queue, as before the presentation queue |
| `LARECOMP_ONE_QUEUE=1` | (comparison) without the second queue: images drawn ahead are presented from the main queue |
| `LARECOMP_FG_NO_VSYNC_GRID=1` | (comparison) scheduled images off the monitor's refresh grid (`DwmGetCompositionTimingInfo`) |
| `LARECOMP_SIDE_SHARP_STOPS=<stops>` | (research) the RCAS of the in-between images with FSR 3.1: that many stops weaker than FSR's (default 0.6 at full size, 0.2 at 75 %, 0.05 at 50 %) |
| `LARECOMP_INTERP_REJ_LOG=1` | each present without in-between images and the reason (`[interp-rej]`, `[interp-cut]`) and each object kept at frame N (`[interp-obj]`: the fraction of its words that changed a lot; above `LARECOMP_INTERP_OBJ_LIMIT`, default 0.4, it is not blended) |
| `LARECOMP_INTERP_JUMP_ABS=<unidades>` | (research) the absolute term of a word's jump between N-1 and N (default 100; it was 4): above half its size and above that, the word - and the whole buffer - stays at frame N |
| `LARECOMP_INTERP_TFIX=<t>` | (research) all in-between images at that t: with 1 they should be identical to frame N (checks the re-execution), with 0 to frame N-1 (what does not match is what does not blend well) |
| `LARECOMP_INTERP_DROP=<máscara>` | (research) the in-between images leave out the 3D draws that kept frame N's constants for these reasons (2 no pair, 4 object rejected, 8 jump): what disappears shows what they are |
| `LARECOMP_INTERP_KEEP_OBJ=<objeto hex>` | (research) that object's draws stay at frame N in the in-between images |
| `LARECOMP_INTERP_FAR_LOG=<unidades>` | (research) records each blended buffer that ended up farther than that from frame N (`[interp-far]`: the render target width, the word, N-1, N, the blend and the draw): a wrong pair shows up as a big jump where the rest moves little |
| `LARECOMP_INTERP_SIG_LOG=<present>:<tamanho>` + `LARECOMP_INTERP_SIG_ARGS=<texto>` + `LARECOMP_INTERP_SIG_DUMP=<n>` | (research) the identity (program, vertex buffer, object, arguments) of each draw that binds a cbuf of that size - or, with `_ARGS`, of all draws whose identity contains that text - at that present and the next one; with `_DUMP`, the words of the first n |
| `LARECOMP_INTERP_NO_HELD=1` | (comparison) held values that change (sprite frame, expression) go back to being blended instead of switched halfway |
| `LARECOMP_INTERP_ALL3D=1` | (research) all draws are blended as 3D, the interface too |
| `LARECOMP_INTERP_KEPT_LOG=<present>[:<último>]` | (research) in the first in-between image of those presents, each cbuf that stayed at frame N and the reason (`2d`, `rejected`, `no-match`, `jump`, with the word), how much it changed since its pair in N-1 (a draw in N that changed is motion at the game's cadence), the blended 2D buffers and how many words stayed at N, and (with `LARECOMP_INTERP_VTX=1`) each small vertex stream (`[interp-kept]`) |
| `LARECOMP_INTERP_VTX=1` | (research, disabled) the small vertex streams (up to 64 KB) of the blended draws recorded with the frame and blended in the re-execution: the position of a vertex stream, all float attributes of a per-instance stream. It did not pay off: the flowers already come halfway via the cbufs, and grass tufts disappeared in an in-between image (streams of another tuft of the same size) |
| `LARECOMP_INTERP_KEEP_SIG=<texto>[\|<texto>...]` | (research) draws whose identity (as `LARECOMP_INTERP_SIG_LOG` writes it: program, vertex buffer, object, arguments) contains one of these texts keep frame N's constants |
| `LARECOMP_INTERP_KEEP_RT=<largura>` | (research) draws on a render target of that width (the game's: 2048 = the shadow map) stay at frame N |
| `LARECOMP_INTERP_RT_DUMP=<present>:<largura>` + `LARECOMP_INTERP_RT_DUMP_DIR=<pasta>` | (research) each pass on a render target of that width at that present, from the real frame and from the in-between images: the color in `build/frames/rt_<present>_<imagem>_<n>.png` and the depth as PGM (stretched over its range) in the folder (default `build/frames`, which `scenario_shot.sh` does not copy) |
| `LARECOMP_INTERP_OTHER_COPY=0` | (comparison) a draw paired by content with another copy of the same object (a word that changes by more than a quarter of both values and 50 units) goes back to being blended with it: the bush shadows where there is none |
| `LARECOMP_INTERP_ONE_TO_ONE=0` | (comparison) a draw from N-1 can again be the pair of several draws from N (without it, only of the nearest one; the others stay at frame N): the new copy of a repeated object is again drawn in place of another copy |
| `LARECOMP_INTERP_SIBLING=0` | (comparison) the copy of an object that enters the scene without a pair in N-1 goes back to staying entirely at frame N (with N's camera) instead of changing, word by word, like the nearest copy of it that has a pair |
| `LARECOMP_INTERP_SLERP=0` | (comparison) matrices that rotate more than 3 degrees between N-1 and N go back to being blended term by term, instead of along the arc (the rotation with Rodrigues, the scale and the translation in a straight line) |
| `LARECOMP_INTERP_NO_ALT=1` | (comparison) vec4s that return to N-2's value (alternating between two states) go back to being blended instead of staying at N |
| `LARECOMP_INTERP_CURVE=0` | (comparison) in-between images go back to the straight line from N-1 to N, without the curve through N-2, N-1 and N |
| `LARECOMP_INTERP_PLAN=0` / `=2` | (comparison / validation) 0: each in-between image decides each cbuf's blend again, instead of using the one decided in the present's first (`ip_plan_*`); 2: both, compared (`[interp-plan]`: differing words, the largest relative difference, cbufs kept by one and blended by the other) |
| `LARECOMP_INTERP_VTX_MEMO=0` | (comparison) re-executions go back to reading and copying each draw's indices and vertices, instead of using frame N's (`vtx_memo`; `[interp]` records how many were reused and how many copied again) |
| `LARECOMP_INTERP_UI_KEEP=0` | (comparison) in-between images go back to resolving the interface against image N through the scene's motion vectors (with FSR 3.1 or reduced: the duplicated text in battles); without it, where N's interface mask says interface, image N at the same spot (`ip_ui_keep`) |
| `LARECOMP_FSR3_UI_COMPOSITE=0` | (comparison) the interface goes back to coming out of FSR 3.1 as part of the jittered scene (at 4K, one pixel of shake every frame); without it, where the mask says interface, the image given to FSR in Catmull-Rom (`ui_composite.frag`) |
| `LARECOMP_POOL_FIX=0` | goes back to the old texture pool behavior (slot freed on the host object), only counting in the `[nvn] texture pool: ...` log (HISTORY 60) |
| `LARECOMP_INPUT_LOOP=<início>:<período>` | with `LARECOMP_INPUT`, repeats the inputs of `[início, início + período)` forever; after each battle / catch it stays still for 300 frames, and inside the encounter it flees (B, down three times, A). Used by `tools/capture_loop.py run <nome>` / `scan <nome>` (before/after pairs of each encounter in `build/capture_loop/<nome>/`). `LARECOMP_INPUT_LOOP_CATCH=1`: inside the encounter presses A every 40 frames instead of fleeing (gets ready, throws and goes through the catch screens: experience, Pokédex). In `capture_loop.py`, `--leg <quadros>` lengthens each leg of the walk (300 crosses parts of the area; the default 80 stays in the same grass) and `EXE=<caminho>` runs another build |
| `LARECOMP_SHOT_RAW=<arquivo>` + `LARECOMP_SHOT_RAW_LAST=<quadro>` | the `LARECOMP_SCREENSHOT_EVERY/FROM` series goes to a raw file (480x270 RGB per frame, no PNG) and the game exits on the last frame; read by `tools/bateria_scan.py` |
| `LARECOMP_EXIT_AT_PRESENT=<n>` | with `LARECOMP_FRAMETIME_CSV`, exits at present n; used by `tools/perf_ab.py` |
| `LARECOMP_GAME_CPU=v2` | on a CPU with AVX2, the game code is compiled (and only loads) for x86-64-v2, as on a CPU without AVX2: the emulated fused operations (`simd_fp.h` `FFMA`). To measure the cost without an old machine: the start screen recompiles the game code; removing the variable recompiles it again for v3 |
| `EXE=<exe> tools/bateria.sh ...` / variant `B=EXE=<exe>,LARECOMP_GAME_LIB=<dll>` in `tools/perf_ab.py` | runs another runtime build (a reference one built separately, e.g. `git worktree add ../lg-base HEAD` + `make build/lasrecomp.exe` there, copied to `build/base/`); in the battery, Eevee then uses `build/lasgame.dll` through the path. A DLL only loads with the exe of the same `GAMELIB_ID` |
| `LARECOMP_PIPE_STATS=<vertex program, hex>\|all` | what the driver reports for each compiled shader (registers, spills, code size: `[pipestats]`) and the program's vertex attribute layout (`VK_KHR_pipeline_executable_properties`) |
| `LARECOMP_LOG_VCOPY=1` (with `LARECOMP_GPUPROF_AT`) | one line per vertex stream copy in the profiled frames: bytes requested, bound buffer size, stride, maximum index |
| `LARECOMP_VK_KEEP_VERTICES=0` | vertices go back to being copied to VRAM every frame (default: a copy is kept and reused as long as the game does not write to its pages; disabled where the platform does not track writes). With `LARECOMP_VK_PROF`, the line shows the copies read, new, rewritten by the game (they go to the ring) and out of space |
| `LARECOMP_WRITE_WATCH=0` / `=1` | the game heap is reserved without / with Windows' written-page tracking (`MEM_WRITE_WATCH`), which the vertex cache depends on. Without the variable: enabled only when the primary video adapter is NVIDIA (the only one tested) |
| `LARECOMP_VK_ASYNC_COPY=0` | vertex and index copies to VRAM go back to being done by the render thread (default: two helper threads do those of 32 KB or more; disabled with fewer than 4 logical processors) |
| `LARECOMP_VCOPY_SAME=1` (with `LARECOMP_VK_PROF`) | research: how many copied vertex bytes were equal to the previous copy of the same range (one hash per range; costs as much as the copy) |
| `LARECOMP_FRAME_FINISH=1` | with the render thread, the `nvnQueueFinish` of the game's frame loop (which it calls when it measured less than 4 ms of GPU in the previous frame) goes back to waiting for the queue and the GPU; without it, it follows the game's own slow-GPU path (HISTORY, "The Finish of every frame") |
| `LARECOMP_VK_COPY_WAIT=1` | the game's buffer copies (`CopyBufferToBuffer`) go back to always waiting for the GPU (before 0.5.1 it was like that); without it, they only wait when the GPU writes the source or the destination, or reads the destination straight from memory |
| `LARECOMP_LOG_B2B=1` | the first 60 buffer copies: source, destination, size and whether they waited for the GPU (`[b2b]`) |
| `LARECOMP_LOG_GPUWRITES=1` | logs each memory range a compute can write (bound storage buffers) when it enters the set |
| `LARECOMP_HLE_WHO=<endereço falso, hex>[,...]` | at boot, prints the name of the host function behind a `0x70800000xx` address seen in a `LARECOMP_THREAD_DUMP` |
| `LARECOMP_LOCK_SPIN_US=<n>` | how long a thread spins waiting for a contended mutex before sleeping (default 40; 0 = always sleeps, as before; disabled with fewer than 4 logical processors). For comparing before/after on the same binary |
| `tools/addr2sym.py <endereço> [-d <bytes>]` | module, nearest symbol and disassembly of a guest address |
| `tools/addrmap.py <main de> <main para> <endereço>... [d:<dado>]` | the same code (or data) in another build of `main` (Eevee <-> Pikachu, update): compares the instructions with the relative fields masked until it finds a single one. The runtime addresses go in `runtime/fnhook.c` and `tools/patches.py` (parity rule, CLAUDE.md) |
| `nm build/lasrecomp.exe` | runtime symbols for `lasrecomp+0x...` addresses |

## Graphics
| Variable | Effect |
|---|---|
| `LARECOMP_SHOT_AT=<frame>,...` | saves the game frame to `build/frames/frame_N.png` |
| `LARECOMP_MV_DUMP=<frame>,...` | saves the camera motion vectors of that frame to `build/frames/mv_N.png` (RGBA8: 0.5 + 32 × velocity in uv; the image comes out flipped in y like the `frame_N.png` files). `tools/mv_check.py build/frames N` checks them: reprojects the previous frame with them and compares the error |
| `LARECOMP_SSAO_DUMP=<frame>,...` | saves the occlusion (SSAO) as applied in that frame, after the blur and the history, to `build/frames/ao_N.png`. `tools/ssao_temporal_check.py <sem> <com>` compares two runs with the same input (accumulation off / on): ghosting (temporal darker > 2%) and noise; `tools/ssao_temporal_sheet.py` builds enlarged crops with the difference |
| `LARECOMP_GI_DEBUG_EXPOSURE=<x>` | exposure of the advanced menu's "indirect light only" (GI) view; default 16 (the game's HDR scene is dark before its exposure) |
| `LARECOMP_MVO_PROBE=<frame>` | records, for each draw with per-object vectors in that frame, the previous frame's constant blocks: identical, missing (`MISSING`) or rejected for a jump (`REJECTED`, with the word and the values) |
| `LARECOMP_FSR3_MASK_DUMP=<frame>` | saves the interface mask of that frame (the one FSR 3.1 receives, and with frame generation the one of the in-between images; with it, the present number) to `build/frames/uimask_N.png` |
| `LARECOMP_CAM_LOG=1` | the camera position in the world (cm) on each real frame (`[cam] present N pos x y z`) and that of each in-between image (`[cam] present N t T pos x y z`: between the previous real frame and this present's; needs motion vectors: TAA, FSR 3.1, SSAO or GI). The camera follows the player: `tools/walk_speed.sh` uses this to measure walking speed in game time |
| `LARECOMP_FPS60_NOWALKFIX=1` | disables the walking and turning speed correction at 60 / 120 fps (`fps60.c`, "the player's walking") |
| `LARECOMP_STEP_SCALE=<fn>:<fator>,...` | (research) the timers' step 1 multiplied while those functions run (addresses of the build in use); `LARECOMP_STEP_SCALE_LOG=<presente>` records fields +0x494..+0x49c of x0 |
| `LARECOMP_FARG_SCALE=<fn>:<fator>,...` | (research) the first float argument (s0) of those functions multiplied; `LARECOMP_FARG_LOG=<presente>` records the animation player at x0+0x120 |
| `LARECOMP_FIND_MOVER=<presente>` | (research) in the objects passed to `LARECOMP_STEP_SCALE`, the float triples that move 1–20 cm per update (a position); `LARECOMP_FIND_MOVER_DUMP=<arquivo>` writes the 0x1000 bytes of each object |
| `LARECOMP_FNWATCH=<endereço>` + `LARECOMP_FNWATCH_FNS=<fn>,...` | (research) records when the float changes during one of those functions (with several threads, the function may not be the one writing: prefer `LARECOMP_WATCH`) |
| `LARECOMP_WATCH_AT=<presente>`, `LARECOMP_WATCH_MAX=<n>`, `LARECOMP_WATCH_LIKE=<presente>:<endereço>` | `LARECOMP_WATCH` only from that present on; only the first n writes; and all heap words with the value of that address at that present watched (the scan runs at the present, on the game thread). Writes of up to 16 bytes that cover the word (SIMD vectors) now count |
| `LARECOMP_FPS_AT=<present>:<fps>,...` | changes the frame limit (30/60/120) at those presents, like the F12 menu |
| `LARECOMP_SET_AT="<present>:<chave>=<valor>;..."` | applies a `larecomp.ini` line at that present, the way the F12 menu changes an option while the game runs (e.g. `1300:ssao_level=3;1400:aa=taa`; no spaces, so it fits in a variable) |
| `fx_custom = 1` and the parameters in `larecomp.ini` | the effect parameters by hand, like the F12 Advanced tab: `ssao_radius`, `ssao_intensity`, `ssao_samples`, `ssao_temporal`, `contact_shadows`, `contact_steps`, `contact_strength`, `contact_length`, `contact_thickness`, `gi_intensity`, `gi_radius`, `gi_thickness`, `gi_rays`, `gi_steps`, `gi_div`, `gi_temporal` (without `fx_custom` the levels overwrite them; with it the levels only turn SSAO and GI on and off). Also with `LARECOMP_SET_AT` (e.g. `1300:fx_custom=1;1300:gi_div=4`) |
| `LARECOMP_MENU_PAGE=cfg/adv/<n>` | with `LARECOMP_MENU_AT`: opens the F12 settings (instead of the pause menu) on tab n - cfg the first one (Graphics), adv the Advanced one |
| `LARECOMP_MENU_SMALL=1` | the F12 settings already open in small mode (the one from Ctrl or the controller's top button, Y / X, hidden: half size, in the bottom right corner, without darkening the game), for captures |
| `LARECOMP_UI2X_GENTLE=1` | "Make the 2x images" starts with waifu2x's gentle attempt (`-t 32 -j 1:1:1`), the one that comes after a rejected output |
| `LARECOMP_UI2X_GENERATE=1` | the start screen generates the interface images at 2x (`runtime/ui2x.c`, like the button) and exits: status 0 if it finished; the step by step goes to the log (`[ui2x]`) |
| `LARECOMP_TEXKEY_LOG=1` | records the content key of each texture uploaded from the CPU (`[texkey] <chave de 64 bits> <w>x<h> nvn <formato> ...`: CRC-32 and Adler-32 of the deswizzled level 0, `ui2x_key`, the same as in `cache/ui2x.pak`). `tools/ui_texkeys.py <romfs.bin> <saída.tsv> [<log>]` computes the same key for all interface textures in the game files and says which ones from the log match |
| `LARECOMP_FONT_SCALE=<1..4>` | forces the text scale (`runtime/hle/font_hires.c`) instead of the automatic one based on window height; `1` disables it (for comparison) |
| `LARECOMP_FONT_LOG=1` | records the first 400 glyphs the game's font cache draws (code, size, metrics at 1x and at Nx, cell and line) and the size of the cache textures at Nx (`runtime/hle/font_hires.c`) |
| `LARECOMP_MV_YSIGN=-1` | (validation) flips the motion vectors' NDC y convention; the correct one in Let's Go is the default (+1) |
| Statistics every 2 s (`[vk] t=...`) | draws, passes, dispatches, uploads, `compile N (translate, spirv, pipelines, draws waited)` (programs and pipelines ready, the CPU time of each stage and how long the draws waited for them). To measure a first visit, rename `cache/` before the test and restore it afterwards and `[skip: ...]` with the draws skipped by reason |
| `tools/perf_summary.py [log]` | summary of a session: fps range, frames above 50 ms, skipped draws, compilations, audio underruns (default: `logs/jogo.log`) |
| `LARECOMP_FRAMETIME_CSV=<arquivo>` | writes `present,dt_ms` for each game present (except the first), with monotonic time; the menu's extra in-between frame at 60 does not count. Use a path in `build/` |
| `tools/frametime_summary.py <csv> [--from-present N]` | mean, FPS, P95/P99 (nearest rank), maximum and number of presents above 50 ms; filter out the start to separate boot/loading from the world |
| `LARECOMP_VK_VALIDATION=1` + `VK_ADD_LAYER_PATH=C:\msys64\ucrt64\bin` | Vulkan validation layer (it found the video textures overflow) |
| `LARECOMP_VK_DEVLOST_TRACE=1` (+ `LARECOMP_SYNC_SUBMIT=1`) | last GPU operations before a device lost |
| `LARECOMP_SAVE_MSL=1` | saves each translated shader to `build/glsl/<addr>_{vs,fs}.glsl` |
| `LARECOMP_GLSL_CACHE=0` | without the on-disk cache of the GLSL translation (`cache/glsl.pak`, `runtime/nvn/maxwell/translate.c`): each program translated again (`[vk]` shows `glsl cache N read, M stored` when it is used). The pak belongs to a single build (the time at which `translate.c` was compiled): from another build, unreadable or cut off midway, it starts over or is cut at the first bad record (`[glsl]` in the log). To measure a cold translation, delete only `cache/glsl.pak` |
| `LARECOMP_DEBUG_FRAME=<frame>` | lists all draws of that frame (program, attribute formats, render targets and, per texture: size, format, flags, block height, address) |
| `LARECOMP_LOG_SKIN=1` (+ `LARECOMP_DEBUG_VSCB=1`) | details skinned draws: vertices, bone indices/weights, palette statistics |
| `LARECOMP_DUMP_SKIN=<n>` | writes the inputs of the n-th skinned draw to `build/skin/` |
| `LARECOMP_VK_UBO_DIRECT=1` | uniforms read straight from the game memory on the GPU (experiment) |
| `LARECOMP_VK_EXACT_H2F=1` | half precision converted bit by bit in all stages, as before; without it only the vertex shaders do this and fragment/compute use `unpackHalf2x16` (HISTORY 63) |
| `LARECOMP_VK_COPY_VERTICES=1` | forces copying **all** vertices and indices, even those a compute writes (they may become stale); for normal use, `LARECOMP_VERTEX_READ` |
| `LARECOMP_NO_SCALE=1` | disables internal resolution scaling |
| `LARECOMP_SCALE=<f>` | render height = 1080 x f (ignores the launcher). With `LARECOMP_FPS=0` it measures the GPU headroom |
| `LARECOMP_SETTINGS=<arquivo>` | another `larecomp.ini` (e.g. `smaa = 0` to compare without SMAA) |
| `LARECOMP_ANISO=<n>` | forced anisotropic filtering (default 16; 0 or 1 = the game's) |
| `LARECOMP_MIP_BIAS=<deslocamento>` | (research) the mip bias of the game's textures with TAA / FSR 3.1: `log2(render / janela)` + that offset (default −1 with FSR 3.1, −0.5 with TAA; 0 disables it). Only textures loaded by the CPU in 3D draws (`[post] mip bias` in the log) |
| `LARECOMP_DEBAND=0` | (comparison) without the debanding and the one-8-bit-step dither of the last pass (`deband.frag`) |
| `LARECOMP_CONTACT_STRENGTH=<0..1>` | (research) the strength of the contact shadows (default 0.5). With `ssao_debug = 1` in the ini and SSAO off, the debug view shows only them |
| `LARECOMP_CONTACT_FLIP=1` | (research) flips the y of the sun direction in the contact shadows' view space (the correct one is the default: shadow under the feet) |
| `LARECOMP_SHADER_WAIT_MS=<ms>` | how long a frame can wait for shaders / pipelines being prepared before skipping draws (default 20; 0 = never) |
| `LARECOMP_HIDE_FIRST_FRAMES=<n>` | black window for the first n presents (default 2: the blue flash of the opening; 0 shows everything) |
| `LARECOMP_WINDOW_SHOT=<frame>` | writes the **window's** image to `build/window.png` (`LARECOMP_SHOT_AT` writes the game's image, before the window) |
| `LARECOMP_CAPTURE_DIR=<pasta>` | folder for window captures, including in-between images; the local scripts use `tests/runs/` |
| `LARECOMP_LOG_CLEARS=1` | each color clear: frame, target, format, mask and the color |
| `LARECOMP_DUMP_TEX_FMT=<formato NVN>` | writes level 0 of the textures of that format, already untiled, to `build/tex/<addr>_<w>x<h>_f<fmt>_row<bytes>.bin` |
| `LARECOMP_VK_SKIN_ONEBONE=1` | skinned shaders use only the 1st bone (experiment X4; it was the workaround before the fix) |
| `LARECOMP_VK_SKIN_IDENTITY=1` / `LARECOMP_VK_SKIN_IDX_X=1` / `LARECOMP_VK_SKIN_W=<expr>` | other experiments from [PLAN-3D.md](PLAN-3D.md) |
| `LARECOMP_DUMP_SKIN=1` + `LARECOMP_DUMP_SKIN_FRAME=<frame>` | writes the 1st skinned draw from that frame on to `build/skin/` |

### CPU shader harness (`tools/skintest`)
Runs a translated vertex shader on the CPU with the real data of a draw:
```
LARECOMP_SAVE_MSL=1 LARECOMP_DUMP_SKIN=400 ./build/lasrecomp.exe extracted --play
./.venv/Scripts/python tools/skintest/make_harness.py build/glsl/<vs>_vs.glsl build/skintest.cpp
clang++ -O1 -std=c++17 -w -o build/skintest.exe build/skintest.cpp && ./build/skintest.exe build/skin
```
The generated `.cpp` contains the game's shader: it stays in `build/` and never goes into git. If the folder has a `gpu_pos.bin` (exported by
`tools/rdc_draw.py`), each vertex also shows the position the GPU computed.

### RenderDoc (what the GPU actually received)
RenderDoc 1.46 in `C:\Program Files\RenderDoc`. The runtime triggers the capture on its own when it runs inside it:
```
RENDERDOC=1 tools/repro.sh rdc 1310 LARECOMP_RENDERDOC_AT=1290 [LARECOMP_RENDERDOC_FRAMES=2]
```
This writes `build/rdc/capture_frameN.rdc` (4 frames in a row by default, because the game does not draw the whole scene
on every present) and `build/rdc-shaders.txt`, with the hash of each SPIR-V and the shader's address in the game. To analyze,
use `qrenderdoc`'s Python. It hangs on startup if it finds the VS Code Python debugger in the user
profile, so point `USERPROFILE`/`HOME` to an empty folder **outside** `build/rdc` (`repro.sh` deletes that folder):
```
H="$(cygpath -w "$PWD/build/rdc-home")"
RDC_DIR=D:/projects/letsgo-recomp/build/rdc OUT=D:/projects/letsgo-recomp/build/rdc/skin.txt   USERPROFILE="$H" HOME="$H" "/c/Program Files/RenderDoc/qrenderdoc.exe" --python D:/projects/letsgo-recomp/tools/rdc_skin.py
RDC=<captura.rdc> EID=<evento> OUT=<arquivo.txt> USERPROFILE="$H" HOME="$H"   "/c/Program Files/RenderDoc/qrenderdoc.exe" --python D:/projects/letsgo-recomp/tools/rdc_draw.py
```
- `rdc_skin.py`: for each skinned draw, shows the shader, the NDC and `w` range and how many vertices go off screen.
- `rdc_draw.py`: one draw vertex by vertex (`a11`, `a12`, `gl_Position`), separating the vertices with `w` off the
  median. Exports `build/rdc/draw_<eid>/` in the harness format (`verts.bin`, `cbN.bin`, `gpu_pos.bin`).

Do not enable debug info in the shaders for capturing: with thousands of shaders, the replay takes more than 20 minutes just to open.

### Game functions: profiling, comparison and hooks
- `LARECOMP_FNPROF=<de>:<até>` writes to `build/fnprof.txt` (or `LARECOMP_FNPROF_OUT`) the game functions that
  ran in those frames: address, calls, order of first entry and the `lr` of that entry (runtime/fnprof.c).
  `LARECOMP_FNPROF_BT=<addr>[,...]` (+ `LARECOMP_FNPROF_BT_MAX=<n>`) prints the stack (functions `f_<addr>`) on the
  first entries of those functions.
- `tools/fncompare.py --base <perfis sem o evento> --event nome=<perfil>` lists what only the event executed; with two
  events, what is common and what belongs to each one (example: docs/BATTLE-MODE.md).
- **Hooks** (runtime/fnhook.c): every generated function goes through `FN_ENTER`, which calls `rt_fnhook` while there is
  a hook registered (`rt_register_fnhook(addr, fn)`; `fn` returns 1 to handle the call instead of the game;
  `rt_call_original` runs the original function from inside the hook). Changing behavior does not require generating the DLL again.
- `LARECOMP_SCAN="<bytes hex>[@<frame>]"` (+ `LARECOMP_SCAN_BACK=<n>`; `LARECOMP_SCAN_LEN=<bytes>` logs more than 0x40 bytes per hit, `LARECOMP_SCAN_NOPTR=1` skips the pointer search, which is slow): scans the guest's mapped memory for those bytes
  (e.g. a record's hash in little-endian) when the game reaches the frame (default 1300) and logs each hit, the 0x40
  bytes after it, `n` records of 0x40 before it and the pointers to the base (runtime/hle/memscan.c, Windows only).
- `LARECOMP_HOOK_DUMP="<addr>:<args>[;...]"` (full guest address, e.g. `700028ef70`; the `main` offset
  alone does not trigger) records, on the first `LARECOMP_HOOK_DUMP_MAX` (4) entries, x0..x7 and the
  memory of the arguments: `x2:30` = 0x30 bytes at x2; `*x0:400` = 0x400 bytes at the pointer stored in x0.

### Cross-references in the game code (`tools/xrefs.py`)
`cd tools && ../.venv/Scripts/python xrefs.py calls <addr>` (who calls the function containing the address), `imm <valor>`
(instructions with that immediate) or `float <valor>` (where the constant is and who loads it). The disassembly of `main`
is cached in `tests/cache/xrefs-main.pkl`. Calls through a vtable do not show up: for those, the caller chain at run
time (the guest's frame pointers, `x29`) is the way.

### Playing while testing
`build/jogo/lasrecomp.exe` (double click) is the stable copy. `make jogo` (`tools/publicar_jogo.sh`, with the game
closed) copies the current build there after it is validated. That way, rebuilding and testing in `build/` does not run into the executable locked by the open game.

## Video and audio
| Variable | Effect |
|---|---|
| `LARECOMP_AUDOUT_WAV=<prefixo>` | writes each audio output to `<prefixo>.<n>ch.raw` (s16, 48 kHz) |
| `LARECOMP_AUDOUT_MS=<ms>` | minimum audio output queue (default 60) |
| `LARECOMP_MOVIE_STATE=1` | state of the decoder queues every 5 s |
| `LARECOMP_LOG_MOVIE_CALLERS=1` | who reads the video frames (Buffer::Base, GetOutputBuffer) |
| `LARECOMP_MOVIE_MAX=<w>x<h>` | scales down videos larger than that |
| `LARECOMP_MOVIE_TEST_FILL=<byte>` | fills the video frames with a value (test) |
| `LARECOMP_LOG_BUF2TEX=1` | log of the buffer → texture copies of the video planes |

To compare audio with the original:
`ffmpeg -i <video>.webm -vn -f s16le -ar 48000 -ac 2 ref.raw` and correlate with the recording (numpy in the `.venv`).
