# Project history: what went wrong and how it was fixed

This document tells, in order, how the port of **Pokémon Let's Go Eevee** went from nothing to a playable game
(start a new game, catch Pikachu, save and load). Each problem comes with the **symptom** (what was seen),
the **diagnosis** (how the cause was found), the **cause** and the **fix**, with the files changed.

Period: 14 and 15/10/2026. Current state and open items: [PROGRESS.md](PROGRESS.md). Diagnostic tools:
[DEBUGGING.md](DEBUGGING.md).

---

## 25/10/2026 — Black screen after a catch in battle

- **Symptom:** after a catch on Route 1, the screen went dark and the game did not
  return to the field. Reproduced with a copy of an early-game save.
- **Cause:** the new throw sequence passed `true` to the native player
  (`a81690`). That mode skips the resource and map cleanup in `a7d344`;
  the battle teardown kept waiting for the sequence's resources.
- **Fix:** use `false`, like the native send-out with `ee420`, and release the
  descriptors in the same order as the game. The ball objects keep their native
  lifetime jobs; hiding a ball does not schedule another destruction.
- **Validation:** comparison with the animation off, a Master Ball catch
  on a copy of the starting save, a catch with a level-up and several real catches
  in a row on Route 1. The Pokémon is registered, the battle process ends
  and the field fade-in runs again.

## 25/10/2026 — Visible throw when catching in battle

- **Symptom:** using a ball from the Bag switched the camera to the trainer and
  showed the result without a visible throw.
- **Diagnosis and cause:** the battle scene's catch callback and the ball
  creation command are stubs in Let's Go. The creation code still
  exists in the executable, alongside the Pokémon send-out resources.
- **Fix:** the port's classic encounters reuse the trainer's native sequence,
  camera, ball models, effects and sounds. The command is wired back
  to the native creation; send-out actions and the "Go!" message are filtered out.
  An arced trajectory goes from the ball in hand to the wild model's point, followed
  by the drop and the wobble. The target's visibility follows success or escape.
  The battle waits for the sequence's real process before showing the result,
  keeping the catch, inventory and save-registration rules.
  No game file is changed or distributed; the hooks are limited
  to the encounters created by the port on Eevee 1.0.0.
  The effects player also handles a missing optional instance: the
  native failure path called `Play` with a null pointer after an escape.
- **Validation:** Master Ball visible in the hand and along the arc to Rattata, impact and
  success result; after saving, the ball was used up and the Pokémon went into
  the sixth slot with the right trainer, ball and checksum. The partner Eevee kept
  its form and stats. A Poké Ball against a synthetic Snorlax also showed an escape,
  the target being restored and the battle's next turn, with no crash.
- **Visual open item:** during the arc a small second
  copy of the ball still shows near the player. The throw to the target and the catch
  resolution work; removing the held object through the native sequence did not
  get rid of that copy in the validation screenshots.

## 25/10/2026 — Classic wild battle by default

- **Symptom:** the static encounter process inherited the legendary birds' rules:
  timer, no XP, and a GO-style catch after winning. A catch resolved
  by the battle engine also did not register the Pokémon in the save.
- **Diagnosis and cause:** those rules are copied when the context is built;
  the field process leaves ownership and storage to the GO screen.
- **Fix:** `runtime/hle/wildbattle_classic.c` removes those rules only
  from encounters created by the port, leaves story encounters untouched and
  offers balls with their real quantities in the native battle Bag, next to the medicines.
  The catch result uses the native routines for registration, trainer, place/date,
  ball used and insertion into the party or storage. The post-battle keeps the
  party updates and evolutions, without opening a second catch screen.
  The mode is on by default on Eevee 1.0.0; `LARECOMP_WILDBATTLE_CLASSIC=0` restores the earlier
  experimental flow for diagnosis. Pikachu keeps the earlier flow.
- **Validation:** a failed catch returns to the turn; a Master Ball catch
  uses up the ball, grants XP and adds a Rattata to storage, with valid checksum,
  trainer and encounter data after saving. Defeating Rattata gave
  24 XP to the lead and 12 XP to the other party members.
- **Partner:** the actor's preload and creation descriptors use the common
  Eevee/Pikachu model and the normal send-out animation. This avoids transferring
  a field model that is missing in the static encounter, without changing the form, the stats
  or the saved Pokémon. Validated with the partner Eevee in the lead: entry, healing,
  reopening the Bag, catch and save; form 1 and stats kept.
  Another catch with five members filled the sixth party slot and persisted
  correctly after saving.

## 24/10/2026 — PC settings in the native Options

- **Symptom:** the extra options required opening the launcher or the F12 overlay.
- **Diagnosis and cause:** the original list uses dynamic eight-byte entries,
  but the text and description tables only accept the original types 0 to 10.
  The arrows also need to re-arm input after each edit.
- **Fix:** `runtime/hle/pc_settings.c` adds 21 rows to the list itself,
  with values, arrows, two-line descriptions and native restore actions.
  Left/right and confirm apply and save the values immediately.
  Anti-aliasing changes update the visible dependent rows; unavailable
  options show `--` and cannot be edited. The overlay is not opened.
  The hooks are limited to Eevee 1.0.0; other builds keep F12.
  The wild battles option is now checked on every encounter, so it can be
  turned on and off during play without restarting. Test overrides remain
  available. Neither the save format nor the game files change.
- **Validation:** Windows build and the real game with isolated copies of the save and INI;
  scrolling to the end, restore, three consecutive frame rate changes,
  switching SMAA/TAA/FSR 3.1, editing the FSR mode after enabling it, and two-line
  descriptions. Captures and logs in `tests/runs/pc-settings/` (outside Git).

## Part 1: choosing the approach

### The idea
Run the Switch game **natively on PC**, with no emulator, using **static recompilation**: the game's ARM64 code
is translated to C once, compiled as a Windows program, and a *runtime* replaces the Switch system
(kernel, services, GPU, audio, controllers).

### Why LASRecomp as the base
- [LASRecomp](https://github.com/peiikai/LASRecomp) (Link's Awakening, Switch, MIT license) had already proven that
  the approach works on the Switch: ARM64 → C translator, Horizon kernel/services and the **NVN** graphics API translated
  to Vulkan.
- The analysis ([LASRECOMP-ANALYSIS.md](LASRECOMP-ANALYSIS.md)) showed that the core is generic and that the
  Zelda-specific parts live in separable layers (address patches, randomizer, interface).

### Alternatives considered
| Alternative | Decision | Reason |
|---|---|---|
| Start a recompiler from scratch | no | LASRecomp already solves the generic part |
| HWDER (Hyrule Warriors) | plan B for graphics | recompiles the NVN driver and emulates Maxwell commands; no declared license, study only |
| ssbu-recomp (Smash) | study only | GPL-3, incompatible with copying code into an MIT project |
| mk8-recomp | discarded | depends on suyu running |
| Restructure with CMake (outside suggestion) | no | LASRecomp's Makefile already works; reorganizing does not get closer to the goal |

### Extracting the game (rules)
- The game comes from the **user's own cartridge** (XCI). Extraction with `hactool` using the `prod.keys` **generated on
  the user's console** (Lockpick_RCM). Keys downloaded from websites were refused: the hash check showed that
  the first keys offered were identical to the ProdKeys.NET package; the real dump had a different hash and
  console-unique keys (`device_key_4x`, `sd_seed`, ...).
- Result in `extracted/` (outside git): `exefs/` (code), `romfs/` (assets) and `romfs.bin` (raw RomFS image,
  produced by `hactool --romfs`).
- **Nothing from the game goes into the repository:** game files, keys, generated C code, logs, saves and caches are in
  `.gitignore`. The shader harness (`tools/skintest`) generates the game-derived code only locally.

---

## Part 2: diagnosing the binary

Details in [DIAGNOSIS.md](DIAGNOSIS.md). In short:

- 4 modules: `main` (the game), `sdk` (Nintendo libraries), `subsdk0` (**glslc**, shader compiler) and
  `subsdk1` (**multimedia**, video). Zelda only has `main` + `sdk`.
- The LASRecomp translator converted **100% of the instructions** of the 4 modules (0 unsupported).
- Title ID confirmed in `main.npdm`: `0100187003A36000`.

---

## Part 3: setting up the fork

1. **Environment:** MSYS2 UCRT64 (clang, SDL3, FFmpeg, Vulkan, shaderc) installed via winget/pacman; Python with
   capstone/unicorn/lz4 in `.venv`.
2. **Removing the Zelda layer:** 16 gameplay/randomizer HLE files and 11 interface screens removed.
   What the rest of the runtime still called became a neutral stub in `runtime/hle/zelda_stubs.c`.
3. **4 modules instead of 2:** loader, dispatch table, game library (`lasgame.dll`, ABI 2) and Makefile
   now handle `main`, `sdk`, `subsdk0` and `subsdk1` (`runtime/loader.c`, `dispatch.c`, `gamelib.*`,
   `tools/gamelib_entry.c`, `gamebuild.c`, `rt.h`).
4. **Generator without the Zelda patches:** `tools/patches.py` emptied (the patches were addresses in the Zelda executable).

First build: runtime in seconds; game code (~550 MB of C) in ~22 minutes.

---

## Part 4: from boot to the title screen

| # | Symptom | Diagnosis | Cause | Fix | File |
|---|---|---|---|---|---|
| 1 | Crash in the `main` constructors (access to `0x1c0`) | disassembly: `nn::os::SetDyingMessageRegion` writes to `[GetInfo(20)] + 0x1c0` | `svcGetInfo` id 20 (user exception context) returned 0 | allocate an area and return its address | `runtime/kernel.c` |
| 2 | — (found along the way) | `svcGetInfo` code | Zelda's Title ID hard-coded in the kernel | read from the ACI0 of `main.npdm` | `runtime/main.c`, `kernel.c` |
| 3 | Crash in `nn::vi::GetService` | addr2sym on the backtrace | `nn::vi::SetLayerScalingMode` had no HLE (Zelda does not use it) | HLE of the function | `runtime/hle/vi.c` |
| 4 | Game aborts (`svcBreak`) | named backtrace: `nn::btm::user::InitializeBtmUserInterface` | the `btm:u`/`bt` services were generic stubs with no objects/events | "offline" services (Bluetooth with no devices) + `csrng` with random numbers | `runtime/services/offline.c` |
| 5 | Game stalled, no frames | new thread dump (`LARECOMP_THREAD_DUMP`) | mutex deadlock: on a condvar timeout the kernel re-acquired the mutex, and so did the SDK (`TimedWait` → `Enter` at `0xea01`) | the kernel does not re-acquire on timeout | `runtime/kernel.c` |
| 6 | Crash in `nvnCommandBufferBindProgram` | runtime symbol via `nm` | the game binds a NULL program at startup | (later replaced by item 22) | `runtime/nvn/cmdbuf.c` |
| 7 | "Press a button on the controller you want to use" without accepting the controller | frame capture (`LARECOMP_SHOT_AT`) + `SetSupportedNpadStyleSet 0x5a` | the runtime presented a **Pro Controller**, which Let's Go does not accept | controller as **handheld mode** (configurable through `LARECOMP_NPAD`) | `runtime/hle/input.c`, `services/hid_set.c` |
| 8 | Crash when opening the title video | backtrace: `movie::MediaData::FindInt64` | 2 of the 28 video functions had no HLE (already foreseen in the diagnosis) | `FindInt64` and `SetCoreMask` | `runtime/hle/movie.c` |
| 9 | ~240 fps, choppy audio | renderer statistics | the runtime ignored the game's *present interval* | wait for the game's interval (2 = **30 fps**), `LARECOMP_FPS` to change it | `runtime/nvn/objects.c` |

### The title video (the longest road)
| # | Symptom | Diagnosis | Cause | Fix |
|---|---|---|---|---|
| 10 | GPU "device lost" (write to address 0) | Vulkan validation layer | the video textures were shrunk by the **internal resolution scale** and the full-size copy overflowed | CPU-filled textures stay full size (`vk_tex_unscale`) |
| 11 | Green screen (NV12 with Y=UV=0) | tests by elimination: no scaling, buffers in the game heap, fill with gray → still green | — (hypotheses ruled out, but they became improvements: `backend_pool_written`, the game allocator) | see item 12 |
| 12 | Green screen | log of who reads the frames: the game never called `Buffer::Base` on the video; `GetOutputBuffer` received a **buffer belonging to the game** (1920×1080×1,5) | the runtime swapped the game's buffer for its own; the game kept reading its own, empty | the frame is copied into the game's buffer, in packed NV12 |
| 13 | "Robotic" video audio | WAV recording of the output + comparison with the original file (70% of samples exact) + user's listening test | the decoder immediately reused the just-freed output buffer while the game was still copying from it | rotating output slots |
| 14 | Music and effects silent (6-channel output all zeros) | per-output WAV recording | `hwopus` (Opus decoder) was a stub: the game gave up | `hwopus` service implemented with FFmpeg's Opus |
| 15 | Video froze on a random frame | thread dump: video thread waiting on a mutex **it owned** | race in the kernel: a signal arrived together with the timeout, the kernel handed over the mutex and still answered "timeout" | keep the result from whoever woke the thread |
| 16 | The game never left the video (white screen at the end) | decoder state + packet count of the file (3.458) | at end of file the demuxer returned `1` (generic error, "try again") | return `0x2000`, the real "end of stream", found in the game's own library (`NnMediaCommon::AndroidErrorToNnError`) |

Files: `runtime/hle/movie.c`, `runtime/nvn/vulkan/textures.c`, `exec.c`, `runtime/services/hwopus.c`,
`runtime/services/audio.c`, `runtime/kernel.c`.

---

## Part 5: intro, 3D and gameplay

| # | Symptom | Diagnosis | Cause | Fix | File |
|---|---|---|---|---|---|
| 17 | Name "RRS" showed as "?S" | bytes: `52 52 53` read as UTF-16 = `U+5252` + `S` | the keyboard returned UTF-8; Let's Go reads UTF-16 | reply in UTF-16LE | `runtime/hle/swkbd.c` |
| 18 | No 3D (black screen with sound in the bedroom) | count of skipped draws by reason: all "noobj" | the game binds **separate programs per stage** (vertex in one, fragment in another, NULL to turn off); the runtime replaced the whole program on every bind | program combined per stage (`bind_program_stages`) | `runtime/nvn/cmdbuf.c`, `vulkan/exec.c` |
| 19 | ~4.000 draws/2 s still skipped | stage log: `0x1` | vertex-shader-only programs (depth/shadow passes) | empty fragment shader synthesized | `runtime/nvn/vulkan/exec.c` |
| 20 | Poké Ball aim went up to the sky | — (user report) | motion sensor (`SixAxisSensor`) not implemented: the SDK read garbage | neutral sensor (still, level, pointing forward) | `runtime/hle/input.c` |

Result: **correct 3D scenery** (bedroom, Pallet Town, sky), catching Pikachu, **save and load working**
(`save/save_0/savedata.bin`).

---

## Part 6: "exploded" characters (fixed)

Skinned models (characters, Pokémon) appeared stretched into "spikes" converging on one point.
The static scenery was correct, so the camera and ordinary geometry worked. Full plan and experiments in
[PLAN-3D.md](PLAN-3D.md).

| # | Symptom | Diagnosis | Cause | Fix | File |
|---|---|---|---|---|---|
| 21 | Characters and Pokémon as spikes | RenderDoc capture + the shader run on the CPU with the same data as the GPU: identical outputs, and the output `w` proportional to the weight of the **1st** bone | `LDC.64 R4, c[4][R4+0x38]` (loads R4 and R5 using R4 as the address) became `r4 = cb4[r4+56]; r5 = cb4[r4+60]`: the second read used the **already overwritten** R4. The `w` of the 2nd bone's translation (which should be 1,0) came out zero or garbage, and the vertex went close to the camera plane | the translator copies the address register to a temporary when it is also a destination | `runtime/nvn/maxwell/translate.c` |

### The road to the cause
1. **Ruled out with data** (before the plan): attribute format, indices and weights in memory, palette matrices,
   timing of the uniform copy, vertex copy.
2. **Phases 0 and 1** of the plan: deterministic reproduction with the save (`tools/repro.sh`) and binary cuts on the GPU. A
   100% weight on the 1st bone left the models whole; the 2nd bone was wrong. The conclusion at the time, "the value of `a11.y`
   arrives wrong on the GPU", was **wrong**: the shader test on the CPU (X5) had passed by coincidence, because the
   vertex used did not exercise the faulty read.
3. **Phase 3, RenderDoc**: the vertex bytes the GPU reads (including `a11 = [7, 6]` and the weights) were identical to
   those in memory. Running the translated shader on the CPU with the **exact** GPU data (vertices and `cb3`/`cb4` exported from the
   capture) reproduced the GPU output number for number. So the defect was in the translated shader itself. The log
   of the `cb4` reads in the harness and reading the GLSL pointed to the two lines above.

While the fix did not exist, `jogar.bat` used a workaround (`LARECOMP_VK_SKIN_ONEBONE=1`, 100% weight on the 1st
bone: whole models, stiffer animation).

---

## Part 7: interface

| # | Symptom | Diagnosis | Cause | Fix | File |
|---|---|---|---|---|---|
| 22 | Pokémon icons "chopped up" (party, experience screen after a catch, Pokédex) | log of the draw's textures (`68x56`, format `0x3B`) and decoding of the dumped texture with 4 bit layouts: only "R in the low bits, alpha in bit 15" gives the right icon | NVN `RGB5A1` (and `RGBA4`) has red in the low bits; Vulkan `R5G5B5A1_UNORM_PACK16` has red in the high bits and alpha in bit 0: colors swapped and alpha taken from a color bit | converted to RGBA8 on upload, like RGB8 | `runtime/nvn/vulkan/textures.c` |
| 23 | Blue flash and then black when opening the game | screenshots of the first frames and a log of the clears (`LARECOMP_LOG_CLEARS`, now with the color): the first 2 presents are a full-screen blue 2D quad drawn by the game itself, with no clear; `nn::oe::FinishStartupLogo` is already called on frame 0, so the system logo would not hide them | not explained (it is not known whether the console shows the same) | the window stays black for the first 2 presents (`LARECOMP_HIDE_FIRST_FRAMES`, 0 turns it off) | `runtime/nvn/vulkan/device.c` |
| 24 | Some objects never appeared (1247 draws skipped as `failed` in 4 min of play) | `tools/perf_summary.py` on the log of a long session; the log had `FS translate failed: unknown instruction df5800812ff70400` | the shader translator did not know `TMML` (query of the mipmap level a sample would use): the whole shader failed and every draw using it was skipped. The game's shader uses the level to clamp the sample (`min(lod, 2)`) | `TMML` translated with `textureQueryLod` (8.8 fixed point, components in the reverse order of GLSL, as in Ryujinx) | `runtime/nvn/maxwell/translate.c` |
| 25 | Objects popping in again the first time in each area after a crash | date of `cache/vk_pipelines.bin` | the pipeline cache was only written when the game closed normally | written on a background thread ~6 s after a batch of new pipelines finishes (~90 ms, without a frame hitch) | `runtime/nvn/vulkan/exec.c` |


## Part 8: image quality and pop-in (15/10/2026)

Audit of a Route 1 frame (`LARECOMP_DEBUG_FRAME`, images of each pass) and of the Vulkan validation layer:
the frame is a 2048² shadow map → 1080p HDR scene → bloom chain (960x540 down to 10x6) → composition → interface, with no
missing draw. Validation only flags a 16x16 RGBA8 texture (the game's own "no shadow") read by a shadow
sampler: outside the specification, but the NVIDIA GPU compares the red channel like the Switch's Tegra.

| # | What | Measurement | Change | File |
|---|---|---|---|---|
| 26 | Aliasing: the game has no anti-aliasing | window screenshots with and without | the SMAA 1x inherited from LASRecomp (an option in Zelda's Esc menu, removed) comes back on by default, with a "Smooth jagged edges" box in the launcher (new `smaa` key in the ini: the old `antialias = off` was just the saved default) | `runtime/settings.c`, `runtime/ui/launcher_lg.mm` |
| 27 | Supersampling (1440p / 4K on a smaller screen) downscaled with a single bilinear tap, which skips texels | with no fps limit, gameplay at ~160 fps at 1080p, ~115 at 1440p and ~78 at 4K (RTX 3080): 4K on a 1080p screen has headroom for 30 fps | presentation uses 4 bilinear taps spread over the area of each window pixel (at exactly 2x, the 2x2 average) | `runtime/nvn/vulkan/shaders.c` |
| 28 | Blurry textures at grazing angles (paths, grass in the distance) | the game asks for little or no anisotropic filtering | 16x on samplers with mipmaps and linear filtering (`LARECOMP_ANISO`) | `runtime/nvn/vulkan/textures.c` |
| 29 | Objects appearing a few frames after each batch of new shaders | ~20-60 draws skipped per batch in gameplay; with the cache, ~1 ms per program | the draw waits for the program / pipeline to be ready, up to 20 ms per frame (at 30 fps there are ~27 ms to spare), instead of being skipped: zero draws skipped in gameplay, same fps; the ones left fall on the loading screen (`LARECOMP_SHADER_WAIT_MS`) | `runtime/nvn/vulkan/exec.c` |
| 30 | First visit to an area: shaders ready slowly | empty cache (`cache/` folder moved away): translation ~240 ms on a single thread, while the 16 compile threads waited; after that, the cost is the driver creating pipelines (~30 ms each) | translation runs on the worker threads (the translator's `RSC2` string buffer became per-thread) and compiles right after on the same thread. Even with an empty cache, all the preparation falls on the black loading frames: the area appears complete (screenshots of the transition) | `runtime/nvn/maxwell/translate.c`, `runtime/nvn/vulkan/exec.c` |
| 31 | Mouse does nothing; keyboard with the Zelda layout (K/J/L/I) | encounter found by trial (`LARECOMP_GYRO_TEST`, the game only turns on the sensor after "Get ready") | keyboard with the Let's Go layout; mouse captured by the window with a click: buttons = A / B, movement = virtual gyroscope (later, #34: captured on its own only while aiming; outside of that it is the finger on the touch screen) | `runtime/hle/bindings.c`, `runtime/hle/input.c` |
| 32 | Poké Ball aim stuck in handheld mode | the game read the sensor only 2 times per catch: `IsSixAxisSensorAtRest` always answered "at rest" and the game skips reading a sensor at rest | never "at rest" (a console held in the hand never is); consistent orientation, angle, angular velocity and gravity; z axis = horizontal aim, x = vertical. The game measures rotation from the start of aiming and ignores sudden jumps (noise): only continuous movement, like the mouse's, moves the camera | `runtime/hle/input.c` |
| 33 | No way to play translated (the game has no Portuguese) | the runtime already had a complete LayeredFS (replace and add romfs files) used by the Zelda randomizer seeds, turned off in Let's Go; the fan PT-BR translation comes as an Atmosphere folder and replaces the `English` and `Spanish` texts, the interface (`bin/app`) and images | `runtime/mods.c`: the `mods/<nome>/` folder chosen in the launcher feeds the same mechanism (without redirecting the saves, as the seed did). The language requested from the system became configurable (`game_lang`), but the game follows the language stored in the save: changing only that does not change the texts of an existing save | `runtime/mods.c`, `runtime/services/fs.c`, `runtime/ui/launcher_lg.mm` |
| 34 | "Play with Eevee" did not respond | handheld mode pets through the touch screen; the game imports `GetTouchScreenStates<16>` and `GetGestureStates`, neither implemented (it read empty memory) | `GetTouchScreenStates` returns one finger: the free mouse over the game image (converted to the Switch's 1280x720 screen; the right scale is the TV's, 1920x1080: #36), with Start / End at the beginning and end of the touch; tested by petting the forehead: Eevee closes its eyes, musical notes and sparkles. Gestures: the game queries them, but petting does not depend on them (returns none) | `runtime/hle/input.c`, `runtime/nvn/vulkan/device.c` |
| 35 | Real mouse and keyboard interfering with automated tests | the user's mouse position showing up in a test log | the `input_user_blocked` filter only existed in the Metal backend; the Vulkan event loop did not call it | `runtime/nvn/vulkan/device.c` |
| 36 | Berry button in "Play with Eevee" did not open | taps on the icon in several forms (quick, held, dragged) and the day moved forward; disassembly of the game's touch reader (`0x70007c1ac4`) showed that it checks the position of every sample, including the release one; with the coordinate in 1920x1080 scale the button opened | the runtime tells the game it is on the TV (1920x1080) and the game uses touch in that scale, not the handheld's 1280x720: the icon was at 2/3 of its place. Touches are now in pixels of the reported screen (`LARECOMP_DISPLAY_W`), and the release sample repeats the last position (it used to go to the default / mouse position) | `runtime/hle/input.c` |
| 37 | Play with Eevee impossible with only a controller | handheld mode depends on the touch screen | virtual finger: right stick moves, holding ZR touches (ZR does not reach the game, where it only repeats A), circle drawn by the overlay (`runtime/ui/menu.mm`). Whole flow tested: open the berries, close the tutorial with A, take the Razz Berry, bring it to the mouth: ×23 → ×22 | `runtime/hle/input.c`, `runtime/ui/menu.mm` |
| 38 | Leftovers from Link's Awakening in the runtime | survey of each function neutralized in `hle/zelda_stubs.c` and of who called it | the fork had turned off the Zelda layer with stubs, but the calls and the screens were still in the code | removed: the stubs and the calls (warp, pets, doors, golden pots, bottle, hearts, widescreen dungeons, save editor, level switch by name), the randomizer, Zelda's texture-based button-icon swapper (`texpatch.c`), the old tabs of the Esc menu (the menu now only has the quit question, name entry, fps and the controller finger), the old (Mac) launcher, building the game code on the player's machine (`gamebuild.c`, zig) and the Mac Metal backend, which was never compiled in this project. The Zelda options left `larecomp.ini` (old ones are ignored) | `runtime/` |
| 39 | Playing required MSYS2, `make gen`, `make` and a `jogar.bat` | comparison with other native ports (Zelda64Recomp, UnleashedRecomp, LAS Recomp itself): a zip with the exe, the game folder chosen on the first screen, nothing to install | the LAS Recomp release flow (frozen generator, Zig, `gamelib-src/`) had been removed together with the rest of Zelda | `runtime/gamesetup.c` (the folder chosen in the launcher: looks for `exefs` and `romfs` / `romfs.bin`, checks the v1.0.0 build id, copies to `extracted/` and recompiles the game code with `runtime/gamebuild.c`, the 4 modules); launcher in English or Portuguese (Windows language), with progress and explained errors; "Install translation..." (`runtime/mods.c`: opens .zip/.rar/.7z with Windows' `tar` and finds the Eevee romfs); `make dist` (LGPL-only FFmpeg, frozen generator, slim Zig) → `LetsGoRecomp.exe`. Test pitfall: MSYS2's `timeout` does not pass environment variables to native exes |
| 40 | In release 0.1.0, the game closed right after the controller screen (`svcBreak` 2111-0001) | test of the release downloaded from GitHub in a new folder, without MSYS2: the log showed `[hwopus] FFmpeg has no Opus decoder` just before; `config_components.h` of the release's FFmpeg: `CONFIG_OPUS_DECODER 0` | the dependency script inherited from LAS Recomp (`--disable-swresample`) silently turned off the Opus decoder, which depends on swresample; Zelda does not use Opus, Let's Go uses it for all music and effects | swresample on and the script fails if any decoder the game uses is missing (`tools/build_deps_windows.sh`); without Opus the runtime stops with a clear message (`runtime/services/hwopus.c`). Release 0.1.1. Lesson: test the release like a player, past the controller screen, before publishing |
| 41 | Game build progress bar at 100% minutes before the end | capture of the launcher window during compilation | the generator's progress added 0,5 per module: it reached 1 with Zelda's 2 modules, but reached 2 with Let's Go's 4 | each module is worth 1/4 (`runtime/gamebuild.c`) |
| 42 | In release 0.1.1, a new game (no save) closed at the title screen (`exception c0000005 accessing 0x39`) | two-phase visitor test (build through the launcher; then a new game with scripted input): `[movie] cannot open extracted/romfs/bin/movies/EEVEE_GO/title_male.webm` just before | the video player (HLE) opened the videos straight from the `extracted/romfs/` folder, which only exists in the development environment; the release build only creates the `romfs.bin` image. With a save, the title video does not play, which is why the earlier test passed | the videos are read from inside `romfs.bin` by a custom FFmpeg reader over the `fs.c` index (`romfs_image_file`); order: a mod's file, `romfs/` folder, image (`runtime/hle/movie.c`, `runtime/services/fs.c`). Release 0.1.2 |
| 43 | The documentation claimed the game has no global time step, and 60 fps in the world worked with no explanation | survey of all reads of the `G` table in `main` (`adrp`+`ldr`, 9 instructions), probe of the received step (`LARECOMP_ELAPSED_LOG`) and count of who reads each step (`LARECOMP_TIMER_USERS`) | the old scan looked for floats near 1/30; the step is an integer in ns in a timer object on the main thread's stack: step 1 comes from `G[1]` through the accessor `f_7000039600` on each world update, step 0 is fixed. Only the systems that convert time to frames through step 1 follow the clock; frame counters (catch, battle) do not | PROGRESS and RESEARCH-60FPS corrected ("Time map"); mode 2 (divided counters) proved insufficient for battle (`tools/teste60.sh`): battle and catch keep their logic at 30 |
| 44 | 60 fps (mode 1): in catch and battle the intermediate frame shook, jumped and mixed up the interface (overlapping titles, menu panels out of place, white rectangles) | intermediate/final pairs recorded at the same scene time (`LARECOMP_WINDOW_SHOTS_REL`), per-buffer diagnosis and rejection reason (`LARECOMP_INTERP_DIAG`), invented-pixel metric (`tools/interp_metrics.py`) | three causes: (1) the hooks that tag each draw's object were only installed if interpolation was on at boot, and in mode 1 it only turns on inside the scenes: everything was paired by order (ghosts, mixed interface); (2) interface draws also fall inside objects, so "has an object" did not separate 3D from 2D; (3) blending was word by word, and a matrix row with one component blended and another kept (crossing zero, or an integer in place of a float) deforms the model; in addition the interval to the final image inherited the world's 16,7 ms at the start of each scene | hooks installed whenever 60 mode is on (`cmdbuf.c`); only draws with an object, a target with depth and depth test on are blended (the interface keeps the current frame); blending per vec4, all or nothing, accepting 0 as a float; fixed period of 33,3 ms in 60 mode. Measured: battle 42 of 47 frames without invented pixels (the 5 remaining are cuts and large camera moves, coherent in the image), catch 0 of 46 |
| 45 | 60 fps (mode 1): in battle, stretched characters and deformed Pokémon in the intermediate frame (catch was already fine) | scripted battles with intermediate/final pairs every 4 frames during attacks (`tools/teste60.sh batalha 1`), invented-pixel metric and enlarged images of the flagged frames | (1) camera cut (Eevee → close-up of the trainer): the trainer's far and near bones were blended (the "jump" threshold accepted changes of up to ~100%); (2) blending per vec4 left part of a bone palette at N and part in between; (3) shadows: the shadow map was redrawn with the blended matrices, but the ground (no game object) used N's light matrix and the shadow slid | before the redraw, each object measures how much its buffers changed: above 20% of words with a large step it keeps the pose of N; if the rejected ones hold most of the change (a cut), there is no intermediate; a jump in any word leaves the whole buffer at N; every 3D draw (target with depth and test on) is blended, with or without an object (those without an object form a group). Measured in two battles: 1 of 201 frames flagged, coherent in the image (fast camera zoom) |
| 46 | Even with fixes 44 and 45, interpolation in battles and catches was still bad in the in-game test | the player's assessment at real speed | the automated measurements (invented pixels) did not capture what is bothersome in motion | interpolation removed from 60 mode (`exec.c`: only with `LARECOMP_INTERP=1`, research); battles and catches stay at 30 with no intermediate frame; world, menus and Pokédex stay at native 60 |
| 47 | 60 fps (mode 1): menus and Pokédex sped up | menu opened with `X` at 30 and in mode 1, every frame recorded (`LARECOMP_SCREENSHOT_EVERY=1`): the menu ball appeared in 4 game frames instead of 8 | since the "menus at 60" commit, every frame outside battle/catch went to 60; the menu stops the world and counts its own frames | only the frame in which the world update (`f_70002f8bb0`) ran goes to 60; menus, Pokédex and other screens that stop the world stay at 30 (measured: identical to 30, frame by frame) |
| 48 | After 47, menus and Pokédex were no longer sped up, but stuck at 30 (the player remembered a version in which they were perfect at 60) | comparison with the tested versions: the "perfect" one was the one with an intermediate frame on those screens | menus and Pokédex are only smooth and at the right speed with logic at 30 and the intermediate frame; at native 60 they speed up | interpolation on only in menus (`fps60_menu`: world stopped, no battle or catch), fixed period of 33,3 ms; battle and catch stay without it. Measured with the menu opened with `X`: intermediates without invented pixels |
| 49 | Pokédex stuck at 30 even with the intermediate frame in menus; on opening, the counter swung between 30 and 60 | list scrolling with every intermediate/final pair recorded; rejection diagnosis (`LARECOMP_INTERP_DIAG`) | (1) the Pokédex is 2D, and 2D was left out of blending; (2) the battle cut detector rejected every menu frame (almost every screen change is large); (3) with 2D on, recycled rows of the list crossed the screen halfway; (4) on opening, the intermediate turned on and off (screens without a world, loading) | in menus 2D is blended too, without the cut detector, with a strict jump test for 2D (change above 30%: the draw stays at N); menu intermediate only after the world has run for the first time. Measured: Pokédex scrolling 25 of 25 frames interpolated without invented pixels; main menu without invented pixels |
| 50 | In the catch aim, with the stick released, the aim kept moving; any touch already moved it | reading of the stick in `runtime/hle/input.c` | the raw SDL value went straight to the game, with no dead zone (a controller at rest reads a few hundred, and the game treats any value as a push) | radial dead zone with rescaling (`stick_deadzone`, default 0,15, adjustable in the launcher under "Keyboard and mouse": "Stick dead zone"), also in the simulated deflection of the tests |
| 51 | 60 fps: ugly flash when opening the menu while walking (when standing still it barely happened) | menu opened twice while walking, intermediate/final pairs recorded on the second opening | with interpolation off (world), the two frames recorded the last time it was on remained marked as valid: the first intermediate of the next menu blended the current scene with the previous menu's | the recorded frames are discarded whenever interpolation turns off; measured: second opening while walking without invented pixels |
| 52 | Issue #1: Let's Go Pikachu 1.0.2 on an AMD with Win10 closed at boot (`c0000005` at `0x38`, font), aborted in the GO Park settings and the launcher refused Pikachu; the submitted patch made setup accept "any version" | reading the patch and the user's log; search for hard-coded addresses in the runtime and the generator; test with the user's own Pikachu 1.0.0 (setup through the launcher, scripted new game up to the name screen) | (1) the Yu Gothic Regular of some Win10 installs breaks the game's font engine; (2) `btm:u` did not have `AcquireBlePairingEvent` (cmd 21); (3) setup checked the Eevee 1.0.0 build id. Accepting another version the way the patch did would break 0.2.0: the generator replaces 3 instructions at Eevee 1.0.0 addresses (`tools/patches.py`, always) and the 60 fps, wild battles and object hooks are addresses in that `main` | setup accepts both games by program id (`main.npdm`); parity (project rule): the Eevee 1.0.0 addresses were found in Pikachu 1.0.0's `main` by comparing instruction by instruction, with relative fields masked (`tools/addrmap.py`: shifts from -0x30 to +0x10, identical struct offsets, the `G` table and slot `0x15fab08` in the same place); hooks are still written with the Eevee address and `fnhook` translates per build (`rt_fnhook_set_build`, `rt_addr`); the generator moves the 3 patches (`BUILD_ADDRS`, `tools/patches.py`); other builds (updates) go without them and the launcher disables the two options; real version reported to the game (`game_version`); wait ignores an empty handle slot; flat dump; Pikachu save in `save-pikachu/`; translation by the title id of the built game; Yu Gothic Medium and MS Gothic before Regular; cmd 21; overlay menu in EN/PT/RU; last host calls in the crash report and `lasrecomp-prev.log`. Release 0.2.1 |
| 53 | With 60 fps and wild battles on (0.2.0, in both games), the wild battle got an intermediate frame and looked "ridiculous" (the player saw it on Pikachu; the 0.2.0 test only checked that the battle started) | log of the player's session: after `encounter intercepted` no `scene start`; the same in the log of the 0.2.0 test with Eevee | the wild battle is built by the static battles process (`wildbattle.c`), which never calls `BATTLE_START`: without `g_special`, the stopped world counted as a menu (`fps60_menu`) and went to interpolation | the wild battles mode notifies the 60 fps code when building the battle (`fps60_scene_begin`); the 60 fps log separates the three states (world / menu with intermediate / battle and catch). Checked by script on Eevee (touched on Route 1: `battle / capture: 30 fps`) and by playing both games. Lesson: test the options together, not just each one |
| 54 | Issue #1 (second message, which I only saw after answering the first): local trade, Mystery Gift and online co-op closed the game | the user's patch v2; `service_lookup` (`runtime/ipc.c`): a service with no implementation becomes a stub that answers ok without returning an object | `nifm:u`, `ssl`, `bsd:u` and `ldn:u` did not exist: `CreateGeneralService` / `CreateUserLocalCommunicationService` came back without a session and the game used a null pointer | `runtime/services/network.c`: offline console (nifm with no internet 2110-0300, events already signaled, socket with ENETDOWN, scan with 0 networks); the remaining `btm:u` commands with no data. Tested by script in both games: local trade searches and B goes back, Mystery Gift stays on "Connecting..." (the game has no offline exit there), GO Park searches and B goes back. Lesson: read the whole issue before answering. Release 0.2.2 |

What remains on a first time: an object never seen before (a new Pokémon or effect) in the middle of gameplay can appear 1-2
frames late, because the driver takes ~30 ms to create the pipeline and the per-frame wait is 20 ms. From the second time
on, the cache solves it.

Shadows: at 4K the edges already come out soft (the game filters the shadows); a map larger than 2048² would bring little and would touch the
scaling of targets that are not 16:9 - left out. Physics and logic run the game's own code, recompiled: the
behavior is the Switch's.

---

## Dynamic resolution audit (17/10/2026)

| # | Symptom | Diagnosis | Cause | Fix | File |
|---|---|---|---|---|---|
| 55 | Dynamic resolution could drop even at a stable default 30 fps | `tools/test_output_scale.c` ran the real function for 180 presents at 33,33 ms: scale 0,70 before, 1,00 after; at 45 ms, after, scale 0,70 | `dyn_update` compared the time between presents with fixed 60 Hz thresholds (17,4/14,5 ms), while the default pacing is 30 Hz (~33,3 ms) | thresholds proportional to the 30/60 Hz pacing and the sample window discarded when the pacing changes; validated in game afterwards, with an LF ini (the audit's was CRLF and did not turn the mode on, see 56): Eevee at 30 fps, old code 1,00 → 0,60, new 0,95-1,00; 60 mode in the world at 60,49 fps. Priority P3: the option only exists in the ini | `runtime/nvn/output_scale.c`, `tools/test_output_scale.c` |
| 56 | Values of a hand-edited `larecomp.ini` with CRLF were silently ignored (`resolution = dynamic`, `upscaler = bilinear`, `75%`...) | while validating the audit: the game received `res_mode 0` with `resolution = dynamic` in the ini (temporary log in `output_scale`); even forced to 20 fps, no `[dynres]` | `fopen` reads in binary mode, so the CR stayed at the end of the value; only `mod` and `game_lang` were cleaned | every line loses trailing CR/LF/spaces before the `sscanf`; the same CRLF ini at 20 fps now lowers the scale (1,00 → 0,70) | `runtime/settings.c` |

---

## Kernel locks (17/10/2026)

| # | Symptom | Diagnosis | Cause | Fix | File |
|---|---|---|---|---|---|
| 57 | The Route 1 world (~600 draws per frame) ran at only ~60 fps with no fps limit, and at paced 60 fps stayed at 58,8-59,3 fps (P99 ~19,6 ms), with no thread above ~41% CPU (total 1,09 cores). The cost of the Vulkan backend was small (~3 ms per frame; descriptor sets 0,45 ms) | `LARECOMP_VK_PROF` (backend CPU and GPU per frame), per-thread CPU sampled from outside, `LARECOMP_PROF_SVC`: 3 game threads spent ~750 ms every 2 s in `ArbitrateLock` (~62 thousand calls each, ~12 µs per call). `LARECOMP_PROF_LOCKS`: a single game mutex accounted for ~126 thousand waits in 2 s (`nn::os::LockMutex` called from `main+0x2e704`; `main` has no symbols, the name `addr2sym` shows there is just the closest one). Disassembly of the SDK (`InternalCriticalSectionImplByHorizon::Enter` and `Leave`): after the svc, `Enter` re-reads the mutex word, checks whether it is the owner and repeats the loop, and `Leave` with waiters only calls `ArbitrateUnlock` | Each wait on a contended mutex slept on a kernel condvar (under the global `g_klock`) and ownership was handed to a sleeping thread: ~12 µs of latency to wake a thread on Windows, about a thousand times per frame per thread | Before sleeping, the thread spins for up to 40 µs on its own wait state (`LARECOMP_LOCK_SPIN_US`; 0 restores the old behavior; off with fewer than 4 logical processors). Ownership is still handed over by `mutex_release`, so the kernel semantics did not change. `LARECOMP_PROF_LOCKS=1` lists the contended mutexes every 2 s. Measured (Eevee, RTX 3080, 1280x720 window, same binary, interleaved A/B): cost per wait of the hot mutex 12 → 1,2-1,4 µs; its waits ~130 thousand → 41-51 thousand per 2 s; world with no fps limit ~60 (59,1 and 61,0) → 73,7 and 65,1 fps; paced 60 fps (Route 1, 2 runs each) 58,8 and 59,3 → 60,0 and 60,0 fps, P99 19,6 → 17,3 ms. Pewter City at paced 60 fps: 60,0 fps in both games, already before. Battery (`tools/bateria.sh`) in both games, 3 routes: 0 crashes, 0 validation errors, 0 unimplemented calls, equivalent flicker with and without the spin. Limits: one machine and one GPU; no AMD/Intel, no 2-core machine (the spin stays off there); new game tested for 6.400 frames with audio only up to the intro. The next bottleneck of the world is the GPU (~9-10 ms per frame at 1280x720) plus the rest of the chain between the game threads. The old documentation said ~160 fps with no limit in the world; on this route I measured ~60 before and ~65-84 after | `runtime/kernel.c` |

| 58 | With the kernel locks fixed (57), the Route 1 world still cost ~9-10 ms of GPU per frame on an RTX 3080 (84% in a single pass), almost nothing in the cropped region | `LARECOMP_GPUPROF_AT` with per-draw timestamps and pipeline statistics (`LARECOMP_GPUPROF_DRAWS`/`_STATS`); single-program experiments (skip, 1x1 scissor, half the instances, return at the start of the shader, only fetch attributes), always alternating within the same run; statistics of the shader compiled by the driver. A group of 4 instanced draws cost ~4,9 ms (a third of the GPU), vertex only (0 fragments); the compiled shader is small (48 registers, no spill) and a shader that only reads the attributes cost the same as the full one; the cost disappeared without the fetch. Several hypotheses fell by the wayside as noise: the GPU varies 10-18 ms between runs of the same code | `guest_direct` bound the game's imported heap (system memory) as the vertex and index buffer, "in place" as on the Switch: the GPU re-read over PCIe the 8 instance streams (16 bytes each) for each of the ~71 thousand vertex shader invocations of a draw (~9 MB, ~0,8 ms at ~11 GB/s) | VRAM ring for vertices and indices on discrete cards: the CPU writes to a staging area in system memory and each submission starts by copying what was written (upload buffer, with a barrier); `LARECOMP_VERTEX_READ=direct\|copy\|auto`. The game uses compute to generate vertex data (16-byte buffers read as streams), so compute storage buffers (and those of vertex or fragment shaders with `STG`) go into a set of ranges and those ranges keep being read in place; a shader that writes to global memory without a storage buffer bound turns the copies off (warning in the log). Copy by start address and whole bound buffer (37,3 -> 16,2 MB per frame); index scan in AVX2. Measured (Eevee, RTX 3080, interleaved runs): GPU 10,2 -> 1,2 ms (-88%), uncapped fps in the world 75,1 -> 89,6 (median; the CPU pays ~1,8 ms for the copies), paced 60 fps unchanged (60,0 fps, P99 17,3-18,6 ms). Battery in both games, 3 routes, with `auto` mode: 0 crashes, 0 validation errors, 0 unimplemented calls, 0 skipped draws, flicker in the same range; Eevee world with the Vulkan validation layers on: 0 errors; frames within the noise between runs (PSNR ~33 dB against ~34 dB for the same mode). Limits: one machine and one GPU; integrated card and weak GPU untested (`auto` mode keeps the old behavior on integrated); the image equality is not pixel for pixel (the scene does not repeat). Full research and what was discarded (write watch, flattened shader, stacks as arrays): [RESEARCH-GPU.md](RESEARCH-GPU.md) | `runtime/nvn/vulkan/exec.c`, `maxwell/shader.h`, `maxwell/translate.c` |
| 59 | With the mutex (57) and PCIe (58) fixed, the next limit was the CPU chain between the 3 game threads: the Vulkan backend ran on the game thread and graphics (lists, presentation, textures) delayed the logic | interleaved A/B with `tools/perf_ab.py` (alternating turbo runs, ratio with a 95% bootstrap interval) and a fast parallel battery (`tools/bateria.sh` + `tools/bateria_scan.py`, ~100 s for 6 routes) | The backend's work (~3 ms per frame) was in the chain: without it the chain runs at ~159 fps | Render thread (`runtime/nvn/vulkan/renderthread.c`): ordered queue with fence tickets, written-page stamp per list, texture destruction and `backend_represent` through `rt_call`; `renderer_game_frame` counter (game) separate from `renderer_present_count` (backend). **On by default** (`render_thread = 0` in `larecomp.ini` or `LARECOMP_RENDER_THREAD=0` turns it off; `vertex_read = auto\|copy\|direct` also in the ini). The broken map after catches that appeared with it came from the texture pool (item 60). Measured (RTX 3080, interleaved runs, no fps limit): Eevee 126,9 -> 185,5 fps (ratio 1,462, CI 1,40-1,53; frame time P99 13,3 -> 9,9 ms), Pikachu 132,1 -> 191,4 (1,449, CI 1,31-1,54); paced 60 fps unchanged (16,63 ms in both modes). Battery of both games, 3 routes, 3 repetitions per mode: frame differences within the variation between runs of the same mode, flicker in the same range, 0 Vulkan validation errors, 0 unimplemented calls, 0 crashes; closing the window exits normally. Limits: one machine and one GPU; the debug combination validation layers + thread hangs on exit after `_exit` | `runtime/nvn/vulkan/renderthread.c`, `device.c`, `exec.c`, `textures.c`, `runtime/settings.c` |
| 60 | In 0.3.0 (Eevee, `fps = 60`), after entering and leaving a catch the world could stay broken until changing area: fences, sign and ferns with flat or dark color, gray patches on the ground | Variants played by hand with a save copy: it broke with the render thread or with the vertex copy, without both it did not appear; intermittent. F5 (textures re-check memory) and F6 (vertices in place) do not fix it; `F9` in the good and broken states: same vertex counts per shader, only the look of the objects changes. So: wrong texture, not stale data. Reading the texture pool path: `nvnTexturePoolRegisterTexture` stored the object's **host pointer**; with counters in the log (`LARECOMP_POOL_FIX=0`) and the automated catch loop (`tools/capture_loop.py`), hundreds of slots stayed on finalized textures, and slots of reinitialized textures pointed to another object (16 already at boot); the rebinds happen right after each catch | The game finalizes a texture and initializes it again in the same NVNtexture object (streaming) without registering the slot again. On the console the descriptor stays valid; here the slot stayed on the freed host object and showed whatever `malloc` put in its place: right by luck when the same chunk came back, another texture when not. Any change in the allocation pattern (render thread, vertex copies) changed the luck, which is why each option "caused" the bug | `runtime/nvn/objects.c`: the slot also stores the game's object (`NvnTexEntry.guest`); `nvnTextureFinalize` clears the texture's slots and `nvnTextureInitialize` on the same object gives the new host back to them. Render thread and vertices in VRAM are on by default again. Verified: catch loop with both options on, Eevee and Pikachu, normal world after each encounter (before/after pairs); fast battery of both games with no crash, no validation error, flicker in the usual range. New tools: `tools/capture_loop.py` (walks in the grass, runs from the encounters: B, down three times, A), `LARECOMP_INPUT_LOOP`, `[scene]` markers in the log | `runtime/nvn/objects.c`, `nvn.h`, `runtime/hle/input.c`, `runtime/hle/fps60.c`, `tools/capture_loop.py` |
| 61 | Review of upstream LAS Recomp (18/10/2026): what changed since the copy in `reference/LASRecomp` (81bf718, 14/10) and applies to Let's Go, Windows part | Clone of upstream in `D:\projects\upstream\LASRecomp`; development is on the `beta_5` branch (255 commits; `main` only has the randomizer and README). Triage commit by commit; discarded the Zelda ones (randomizer, mini-bosses, `delegate_null` hooks, font patch), the ones we already have (condvar on timeout, store in gmem in draws, vertices in VRAM, CR/LF in the ini) and the Mac/Linux ones. Tests: game folder with a Cyrillic and Portuguese name (junction to `extracted/`); committed memory measured from outside (Get-Process) in Pewter City; scripted saving through the X menu → Save | (1) UTF-8 paths read through the system code page: `????-ação/exefs/main` did not open; (2) winpthreads commits the requested stack size: 256 MB per game thread; (3) `IFile`/`IDirectory` never closed the host file; (4) after a crash, the "Visual C++ Runtime" box from `abort()`; (5) controllers SDL does not recognize did not become gamepads | (1) manifest with UTF-8 `activeCodePage` (bcf999a): the new build opens the Cyrillic folder; (2) `_beginthreadex` with `STACK_SIZE_PARAM_IS_A_RESERVATION` (517a19f): private memory 7,2-7,4 GB → 4,7 GB, same working set; (3) `on_close` closes the descriptor (part of 1c805c2); the save still writes the same as the previous build; (4) `_set_abort_behavior` (bc26a48); (5) `gamecontrollerdb.txt` in the data folder (8f28a0e). Also: the crash report names `f_<endereço>` before the DLL name (dc30ccc). Fast battery of both games with no crash or error, flicker in the usual range | `runtime/platform/windows.manifest`, `windows.rc`, `windows_threads.h`, `windows.c`, `runtime/kernel.c`, `runtime/services/fs.c`, `runtime/hle/input.c`, `runtime/dispatch.c` |
| 62 | Same review, Vulkan and shader translator part | Reading the diffs against our `exec.c`/`translate.c`; battery before/after with the old executable compiled separately (new `EXE=` in `tools/bateria.sh`) and frames compared: rendering is the same, only the timing changes (the Eevee menu varies like that between two runs of the same build) | (1) `renderer_fence_seq` counted a reused slot that still had the flags of the previous submission: a fence on an empty queue waited for a value that would never be signaled; (2) empty color slots at the end of the pass went as `VK_NULL_HANDLE` (the Intel UHD 630 driver hangs on that); (3) the pipeline key copied the 8 color formats, including those of earlier passes: duplicated pipelines; (4) IPA multiply assumed the multiplier = w; (5) I2I .SAT, SHF and HSETP2 .H_AND outside the hardware semantics | (1) 37b7ff6; (2) f4cb039 (an empty slot in the middle goes to the log); (3) 3921201; (4) d1c2d8e; (5) d4c2ff6, but SHF .CC/.X/.HI are still ignored instead of bringing the program down | `runtime/nvn/vulkan/exec.c`, `runtime/nvn/maxwell/translate.c` |
| 63 | Same review, performance part: does what upstream measured in Zelda apply here? | GPU per frame (`LARECOMP_VK_PROF`) on the world route at 2x scale, two runs of each, interleaved; `tools/perf_ab.py` (5 + 5 interleaved runs, in both games) with an `EXE=` variant pointing at the previous exe and DLL; battery and frames compared | (1) the bit-by-bit half → float conversion (~12 operations and two branches per half) ran in all stages; (2) the SDK libc (memcpy, memcmp, strlen...) recompiled byte by byte and the `fesetround` of the slow floating-point path (which also touches the x87) cost ~3% of CPU in Zelda | (1) **ported** (50a09ed): fragment and compute use `unpackHalf2x16`, vertex stays exact (`LARECOMP_VK_EXACT_H2F=1` restores the old one). RTX 3080: GPU 4,01-4,06 → 3,70-3,81 ms per frame; the frames only differ in wind-blown foliage, like two runs of the same build. (2) **tested and not ported** (67bad06, a2b4aa3): DLLs rebuilt for both games, clean battery, but the A/B gave a ratio of 1,008 [0,989; 1,017] on Eevee and 1,001 [0,987; 1,012] on Pikachu (turbo ~180 fps): no measurable gain on this machine, and changing `GAMELIB_ID` forces every player to recompile the game code when updating. What stays is the correct dependency in the Makefile (game objects recompile when `simd.h`/`simd_fp.h` change). In the final battery, once the Pikachu menu route opened the Bag instead of the Pokédex (script timing with 6 routes in parallel); repeated 3 times, same as the old build | `runtime/nvn/maxwell/translate.c`, `Makefile`, `tools/perf_ab.py` |
| 64 | In 0.3.1, the launcher saved `larecomp.ini` with lines stuck together (`fullscreen = 0fps = 60render_height = 1440smaa = 1mod = ...`): on the next read fps, resolution, SMAA, translation, video audio, fps counter, d-pad and wild battles went back to default | Found while reading `settings.c` for the vibration option; reproduced with the launcher in autoplay saving to a test ini (`LARECOMP_SETTINGS`): the old build wrote the lines stuck together, the new one does not | Commit 8a2e713 (script-generated) lost the `\n` of 9 `fprintf` calls in `settings_save`: the command channel converts `\n` into a real line break (the same pitfall as in CLAUDE.md) | `\n` back; reading separates a key stuck to the previous value (lowercase letters and `_` right before a second ` = `), so an ini saved by 0.3.1 recovers everything: tested with a broken ini, the launcher rewrote fps 60, 1440, SMAA, translation, d-pad and wild battles | `runtime/settings.c` |
| 65 | Risk of a `c0000005` crash at a valid guest address when two threads touch a not-yet-committed 64 KB block at the same time (not seen in Let's Go; it appeared in the Sword port, which uses the same runtime, within 11 s of a test) | Reading `lazy_commit`: it only accepted a page in `MEM_RESERVE`. The first thread commits the block; the second, which had already faulted before that, finds `MEM_COMMIT`, the handler returns false and the exception becomes a crash. (The host threads' stacks, only reserved since 290e848, are outside the guest window: they do not go through here) | Race between simultaneous faults on the same block | Page already `MEM_COMMIT` and `PAGE_READWRITE`: retry the access; pages protected on purpose (`plat_protect`, `LARECOMP_WATCH`) still fault, and execution faults (DEP) do not go through `lazy_commit` (they do not loop). Battery of both games with no crash or error, flicker in the usual range | `runtime/platform/windows.c` |
| 66 | In 0.3.2 (30 fps), after leaving a catch: black trees and plants with yellow stripes, X-shaped bushes with a sign's texture or in a flat light-blue color, the ground without the dirt trail, until changing area. The player saw it almost always; the automated scripts, almost never | Detectors in the player's game, with the bug on screen, all at zero: pool slot empty or with another texture, pool memory touched from outside, buffer→texture copy on the GPU only, image different from memory, per-object registration copied or finalized (item 60 had handled the pool; it was not that). The F9 dump with each texture's slot showed the bush ground with a 5-texture fragment shader in place of the 4-texture one, texture 0 inherited from the previous draw: wrong shader, not wrong texture | The cache of translated programs (and the compute one) used only the code address as the key. After a catch the game loads shaders again at addresses that used to hold others; the renderer returned the old translation (another texture layout, other lighting) | The key includes a hash of the shader's code and constants, computed in `SetShaders` (`content_hash`); probing of the program table is bounded (64 positions). The log counts the addresses that now hold another shader (`[shader] ... now holds another shader`): in a catch loop (`LARECOMP_INPUT_LOOP_CATCH=1`), 7 catches on Eevee and 9 on Pikachu, no crash, the counter shows up in both right after the catches. New tools: the catch loop that really catches, `--leg` and `EXE=` in `tools/capture_loop.py` | `runtime/nvn/objects.c`, `nvn.h`, `vulkan/exec.c` |

---

## Lessons learned
- **Measure before guessing.** The biggest advances came from instrumentation: thread dumps, the reason for skipped
  draws, audio recording, logs of who calls each function.
- **LASRecomp is "right for Zelda".** Several bugs were assumptions that held for Zelda and not for Let's Go:
  text format, Pro Controller, per-stage programs, the game's video buffer, end-of-stream code.
- **The game's own library is the reference.** Reading the disassembly of the SDK and of `subsdk1` (for example
  `TimedWait`, `AndroidErrorToNnError`) gave the exact answer instead of trial and error.
- **Thread races show up as random symptoms.** The video froze on a different frame on each run;
  the cause was a race in the kernel, not the video.
- **A test that passes must exercise the failing case.** The shader on the CPU "proved" that the translation was right
  with vertices that did not go through the faulty instruction. Running with the exact data of a broken draw (exported
  from RenderDoc) and comparing with the GPU output solved it in minutes.
- **Register translation: watch out for a destination that is also a source.** Instructions that write several
  registers (`LDC.64`, `.128`) may have the address among the destinations; read the sources before writing.

## Real controller gyroscope (16/10/2026, not validated on hardware)
Idea taken from ScarletNX's `hid_motion.c` (reimplemented, no code copied): a controller with a sensor (DualSense, Pro
Controller) turns the aim like the mouse. `runtime/hle/input.c` turns on `SDL_SENSOR_GYRO` when opening the controller and integrates each
`SDL_EVENT_GAMEPAD_SENSOR_UPDATE` (rad/s × time between samples) into the virtual gyroscope's yaw/pitch, only while the game
is aiming; dead zone of 0,015 rad/s against the resting bias, samples with an interval > 100 ms discarded. Options:
`pad_gyro_sens` (launcher) and `LARECOMP_GYRO_PAD_FLIP`. It compiles (`input.o`, `settings.o`); axis directions still to be checked.

## "Wild battles" mode (17/10/2026, experimental)
Idea: when touching a wild Pokémon, instead of the Pokémon GO-style catch, a classic battle and then the catch,
as the game already does with Snorlax, the legendary birds and Mewtwo. Details and addresses in [BATTLE-MODE.md](BATTLE-MODE.md);
code in `runtime/hle/wildbattle.c`; "Wild battles (experimental)" option in the launcher (`wild_battle = 1` in
`larecomp.ini`) or `LARECOMP_WILDBATTLE=1`.
- **How it works.** "Static" battles are the class A process (factory `17f0a0`), which reads the record from
  `bin/script_event_data/event_encount.bin` by the hash in `[obj+0x98]`. The hook on the touch trigger (`305ad0`, replaced
  entirely: the original function then sets up the catch scene's handle, which a battle process does not accept) creates
  that process; the hook in the constructor writes a hash of ours; the hook in the lookup (`2bad00`) answers with a copy of the
  Snorlax record with the species, form, level, gender and IVs of the touched Pokémon (read from the `P` object of the
  settings constructor: level = byte `P+0xf0c`, IVs = `P+0xf18..0xf2c`, gender = `P+0x40c`) and the level-up moves from
  `waza_oboe.gfpak`. After the battle, catching the same Pokémon is the native flow.
- **Symptom: black screen when returning to the world.** Diagnosis: the world was drawn (large passes) and only the final passes
  came out black; `LARECOMP_DEBUG_FRAME`/`g_debug_frame_request` and the log of the fade calls (`11ece0`, `11ee50`) showed a
  type 1 fade out at the end of the catch and **no fade in** until the next catch started on its own. Cause: the event script
  (`FadeOut_` -> `CallWildBattle` -> `GetWildBattleResult_` -> `FadeIn_`) does the fade in; we skip the script. Fix: the
  hook on `2f78d0` (the world, every frame) repeats, 30 frames after the battle, a fade in of the same type as the last fade out.
- **Symptom: battle started with swapped flags.** `CallWildBattle` writes `a0 = a1 = 0, a2 = 1` and the constructor leaves
  `a0 = a1 = 1, a2 = 0`; we copy the script's values.
- **Symptom: crash when copying the "real settings".** Two attempts to drop the Pokémon's settings (0x2b8 bytes)
  over the battle's crashed the game: the `x0` object in the trigger has 0xb8 bytes (it is not the settings) and the one the
  constructor returns still had the species zeroed. Turned off (`LARECOMP_WILDBATTLE_USECFG`, default 0); the path that
  works is to read the fields of `P` and build the record.
- **Lessons.** (1) First look for what the community (pkNX) and the game itself already say: the names of the script natives
  were in strings in `main`. (2) Scripting the test (`LARECOMP_INPUT`) and saving images every N frames showed in
  minutes what three manual runs did not. (3) `LARECOMP_HOOK_DUMP` needs the full guest address.
- **Missing:** split XP (half on defeat, half on catch), nature and shininess of the touched Pokémon, regional
  forms (the moves table uses the species index), battles with large/aura Pokémon and testing in more areas.

## 2x text with vertical streaks (19/10/2026, PR #13 by Vancete)
- **Symptom.** With the glyph cache at 2x (`runtime/hle/font_hires.c`, on by itself from 1080 lines up), the
  "Continue your adventure" menu showed thin light streaks between the letters; with `LARECOMP_FONT_SCALE=1` there was nothing.
- **Diagnosis.** Saturating the fill subtraction (outlined fonts) changed nothing. The streaks were on the
  edges of the cells: the cell at Kx was written from `cx*K + 1` to `(cx+cw+2)*K - 1`, leaving the first and last
  column as they were.
- **Cause.** When the game reuses a cell, those columns kept pieces of the glyph that was there before. At 1x
  the game rewrites the whole cell (the empty columns too).
- **Fix.** The cell at Kx covers exactly K times the 1x one (`cl = cx*K`, `cr = (cx+cw+2)*K`). Checked in
  1080p fullscreen (automatic 2x) on Eevee: no streaks.

## 2x interface with garbage icons on Pikachu (19/10/2026)
- **Symptom.** Parity test of PR #13: on Pikachu, with `ui2x = 1`, the X menu icons came out as noise and the
  partner icon disappeared; with `ui2x = 0`, normal. On Eevee the same comparison came out right.
- **Diagnosis.** Pikachu with the package generated on Eevee was normal: the defect was in the package. The interfaces of the
  two games have the same 3081 textures (same keys), but the data of all 3081 differed. Regenerating on Eevee gave
  an identical package (it is deterministic), and so did a test folder linked to the Eevee files. Regenerating on
  Pikachu gave the right package, identical to Eevee's. In other words, it was an isolated failure: that time waifu2x finished in
  37 s instead of ~72 s, exited with code 0 and delivered garbage in all the images (1 in 7 generations, RTX 3080).
- **Cause.** The packaging (`runtime/ui2x.c`, `pack`) accepted any waifu2x output with the right size.
- **Fix.** Each 2x image, reduced to 1x by a 2x2 average, is compared with the waifu2x input (color and alpha).
  Mean difference above 16 (0..255): the image is left out. More than 10% left out: no package is produced, and the start screen
  says the upscaler failed and to try again. Measured: good images up to 7,2; the failure's garbage between 66 and 188.
  Three generations in a row with the check: 0 images left out.

## Wild battle broke at the start of the game (19/10/2026, issue #6)
- **Symptom.** With "Wild battles" on, the game closed with `exception c0000005 accessing 0x78` right after
  "A wild ... appeared!", before "Go on, ...!". Reported on Pikachu (level 3 Rattata, Route 1); the player said a
  Gamebanana mod did the same.
- **Diagnosis.** The catch loop (`tools/capture_loop.py`, `wild_battle=1`) with a Route 1 save (0:11 of play, only
  the partner Eevee and a Pidgey) reproduced it in both games; with more advanced saves, on the same Route 1, it did not. The crash is in
  `f_7000a9ddb0` (called by the battle sequence command `f_7000aeebb0`): it looks for a component of the Pokémon that
  walks along, does not find it and passes a null pointer on. Handling that null (and that of the setter `f_7000155870(0, 0)` that the
  code itself calls on this path, and which has compiler traps such as a write to `0x130`) only moved the crash to
  another command (`f_7000adc4c0` -> `f_7000a9a8e0`): the static battle opening was built assuming the companion.
  The battle object's flags (`a3..a6`) do not change that. Changing the party order in the menu (Pidgey in slot 0) made the
  battle run normally. The script native names (`PokePartyIsPartner`, `GetPartnerIndex_` = `f_700019a040`) gave
  the function that tells the partner's slot, `f_70002462e0(0, &slot)`. In the save, the party header is at `+0x5A00`
  (6 indices and, at `+0x0C`, the partner's) and the Pokémon at `+0x5C00 + índice x 0x104`; the game's live copy is a different one.
- **Cause.** When the partner is in the first slot no Pokémon walks along (at the start of the game this cannot even be
  changed), and the static battle opening does not account for that case.
- **Fix (safeguard).** On touch (`trigger_hook`), if the partner is in slot 0, the wild mode does not build the battle
  and the game goes on with its own catch. Tested in both games: Route 1 save, 3 encounters in each, all in the catch and
  no crash; with Pidgey in slot 0, the battle goes on. **Missing:** battle also without a companion (the full
  fix).

## Wild battle: the starter walking alone (19/10/2026, issue #6, 0.4.2)
- **Two reports after 0.4.1.** The user, with Eevee in front and Bulbasaur walking along, went to the catch without
  needing to: the "with companion" case had only been tested with Pidgey moved to slot 0, and the runs with his
  save found no encounter (I did not check). Vaskiemaia, with Oddish in front, got a battle, but the game closed when
  switching to Pikachu (video; log with the same `f_7000a9ddc0`, Pikachu).
- **The game's vocabulary.** In the script natives, "partner" is the Pokémon walking with the player and "buddy" is the starter
  (`f_70001760d0`: Pikachu form 8 or Eevee form 1). `f_70002462e0(0, &slot)` (the `GetPartnerIndex_`) gives the slot of
  the one walking along, not of the starter. In the save, the party header (`+0x5A00`) has the one walking along in slot 0; the
  save blocks are checked by CRC16-ARC (`FEEB` table at the end: size, id, checksum).
- **The cause.** In a single battle, `f_7000a88780` (opening) and `f_7000a8ced0` (switch) send out the starter with
  `ee422.bseq` (the virtual method `f_7000a7e090` asks whether it is the starter); the others go with `ee420.bseq` (Poké Ball). The
  `ee422` takes the field model of the one walking along into the battle; with the starter walking alone, the static battle
  does not have that model and `f_7000a9ddb0` reads a null. That is why it also breaks on a switch.
- **What did not solve it.** The game's static battle scripts (`bin/archive/script_amx.gfpak`, 64-bit AMX in
  compact encoding; native names by the hash `h = h * 0x83 ^ byte`, found in the registrar `f_70006705e0`)
  put away the one walking along and the ridden starter before `CallWildBattle` (`StartFollowPartnerIntoTheBall_` =
  `f_7000243bb0`, `StartRidePartnerIntoTheBall_` = `f_7000243d90`). Calling that before the battle, with or without a wait,
  deleting both (`f_7000244440`, `f_7000244620`), making it reappear ridden (`f_7000244270`, refuses), another template
  (Electrode) and forcing `ee420` (it has the shoulder-starter branch and breaks in `f_7000adc1f0` -> `f_7000a9bf20`)
  did not solve it.
- **Fix (safeguard, 0.4.2).** On touch (`starter_walks_alone`), if the one walking along is the starter itself (or no one),
  the encounter goes to the game's catch; with another Pokémon walking along, a battle, and the starter can join. The 0.4.1
  rule ("starter in slot 0") goes away.
- **Tools.** `LARECOMP_SCAN_LEN` / `LARECOMP_SCAN_NOPTR` in the memory scan (docs/DEBUGGING.md).
  **Lesson:** a case only counts as tested when the log shows its event; test with a copy of the user's save.

## SSAO and GI flickering with the camera moving (19/10/2026, PR #16 by Vancete)

- **Symptom.** With ambient occlusion (SSAO) or indirect light (GI) on, parts of the scene (rocks, bushes,
  characters) gained and lost the effect while the camera moved, as if it turned on and off. Seen by the
  user in battle and walking in the Pewter gym; it already existed before the PR.
- **Diagnosis.** In automated runs SSAO ran on every frame, with temporal history kept, and consecutive frames
  did not clash. A dump of the occlusion in battle (`LARECOMP_SSAO_DUMP`) only had the trees in the background. Counting
  the opaque draws before and after SSAO in each frame: in battle, 30 before and 29-30 after; walking in the Pewter
  gym, from 34 to 68 before (out of about 130), changing from one frame to the next.
- **Cause.** The effect was applied on the first translucent draw after 16 opaque ones. Let's Go draws translucents
  in the middle of the opaques, so what came after was left without occlusion and without indirect light, and the boundary moved with the
  camera (culling and draw order).
- **Fix.** SSAO and GI run when the scene pass ends, with all opaques drawn
  (`ssao_draw_hook`, exec.c). Pewter: 118-128 opaques before, none after, on every frame.
- **Second cause (battles and catches).** After the first fix, it still flickered in battle, most visibly in the
  defeated trainer scene. A build with diagnostics (the user reproduced it, the log counted) showed the scene target
  stable, no draw left out and the history kept, but the frame's camera changing: every few frames, the constant
  slot of the scene draws (vertex cbuf 5, +64) had a camera at the origin (view without translation, mirrored,
  near 16 and no far: the sky's), in all draws of that frame, and the scene's was not in any slot as its own
  projection. SSAO and GI read depth with that camera on those frames. The shader's constant mask
  did not help (it does not mark that slot even on the good frames).
- **Fix.** A camera at the origin is not the scene's (`cam_at_origin`); with no camera in the frame, the previous frame's applies
  if it is at most 8 frames old, held still (`ssao_settle_camera`, at the end of the pass). Three battles: 197 to 554 frames with the camera
  at the origin, all but 1 to 9 (start of scene) covered.
- **Side effect fixed.** Applied at the end of the pass, the effect also caught the fade that the game draws over
  the scene on the same target: in the opening (fade from white), the grass appeared gray through the white. Now it
  goes in before the first draw without depth test after the opaques (a fade, the full-screen passes), or
  at the end of the pass; the 3D translucents in the middle of the scene test depth and do not trigger it. Pewter still has no
  opaque after the effect.
- **Not part of this problem.** The horizontal bands in the dark frames of the map change come from the 2x interface package
  (they go away with `ui2x = 0`, with or without SSAO/GI; they have existed since 0.4.0).
- **Tool.** `LARECOMP_SET_AT` applies options in the middle of play, like F12 (docs/DEBUGGING.md).
- **Still open.** Switching anti-aliasing to TAA with the game running leaves 1-2 black frames: the scene shaders
  get jittered variants and the draws are skipped while they compile (this already happened in 0.4.x).

## Launcher opening fullscreen (21/10/2026, tests of PRs #17 and #19)

- **Symptom.** With `fullscreen = 1` in `larecomp.ini` (the Pikachu test folder), the launcher opened filling the whole
  screen, and not in the narrow window, taller than wide, that Eevee showed. Launcher capture: 1920x1080 instead
  of 762x887.
- **Cause.** The launcher calls `menu_apply_window_requests` every frame (for text input), and that function, on its
  first call, applied `SDL_SetWindowFullscreen(g_settings.fullscreen)`: right after `window_as_launcher`
  left fullscreen and set the launcher size, it went back to fullscreen. Inherited from the fork, from before
  `ui_window_init` and `window_as_game` applied the option. It already existed in 0.5.0, it did not come from the PRs.
- **Fix.** `menu_apply_window_requests` only applies fullscreen requested by F11 / Alt+Enter. Pikachu with
  `fullscreen = 1`: launcher at 762x887, and the game at 1920x1080 after Play (`LARECOMP_LAUNCHER_AUTOPLAY` +
  `LARECOMP_WINDOW_SHOT`).

## Slow Viridian City: buffer copies waiting for the GPU (22/10/2026, issue #20)

- **Symptom.** FPS drop in Viridian City (issue #20, RX 7600 + i5-10400F). Measured here (0.5.0, i5-14600KF + RTX 3080,
  1080p, no limit, `LARECOMP_VK_PROF`): Viridian ~110-155 fps, the render thread busy 89-93% of the time
  (~8 ms per frame) and the GPU only ~1,5-3 ms. The heaviest scene for the backend CPU, more than Pewter and the Forest.
- **Diagnosis.** The profile gained CPU time per command type (`ops:` on the `LARECOMP_VK_PROF` line). Almost a
  third of the thread was command 43, `CopyBufferToBuffer`: 2 per frame, ~1,2 ms each. `LARECOMP_LOG_B2B`: copies of 112 and
  32 bytes (constants), none in memory the GPU writes.
- **Cause.** The copy called `renderer_flush()` before the `memmove`: it submitted the work and waited for the GPU to finish everything. That is only
  needed when the GPU writes the source or the destination, or reads the destination straight from memory; the draws recorded earlier
  took copies of the constants and the vertices.
- **Fix.** Wait only in those cases: ranges the GPU writes (`gpuw`), storage buffers of shaders with global
  memory (new `gpur`, read in place) and vertices read in place (VRAM ring off). After the `memmove`,
  `guest_copy_flush` (as in `UpdateUniformBuffer`). `LARECOMP_VK_COPY_WAIT=1` restores the old behavior. Viridian, interleaved A/B
  on the same exe (4 + 4 runs per scene): Eevee standing 143 -> 180 fps, walking 146 -> 151, Pikachu walking
  122 -> 143 (ratio 1,175, 95% CI 1,08-1,23); the new one won in all 12 pairs. Render thread ~8,3 -> ~5,6 ms per
  frame. Battery of both games, new against old: 0 crashes, flicker in the usual range.
- **Still open.** The same exe varies a lot between runs on this machine (120-175 fps; efficiency cores of the 14600KF,
  not confirmed). The vertex copy (~12-19 MB per frame in Viridian) is the thread's next cost.

## Viridian City: the vertex copies leave the render thread (22/10/2026, issue #20)

- **Diagnosis.** After the fix above, the largest share of the render thread in Viridian was the copies of
  vertices and indices to the VRAM ring: ~12 MB and ~1,1 ms per frame (~10,6 GB/s); the index scan, ~0,05 ms.
  `LARECOMP_VCOPY_SAME`: 85% of the copied bytes equal to the previous copy, but checking with a hash cost the same as
  copying; reusing across frames would need Windows write tracking on the game's reservation (left for later).
- **Fix.** The render thread reserves the space in the ring and records the draw; two helper threads do the
  copies of 32 KB or more (in 256 KB chunks; the render thread helps when it waits). Everything finishes before the
  submission that reads the copies and before the thread itself writes to game memory in the middle of the list (`UpdateUniformBuffer`,
  buffer copy, texture-to-buffer copy, counters). The game only rewrites vertices after the frame fence,
  which comes after the submission. `LARECOMP_VK_ASYNC_COPY=0` restores the old behavior.
- **Measured.** Viridian, render thread CPU ~5,6 -> ~3,6 ms per frame (the helpers finish before the submission:
  0 ms of waiting). Interleaved A/B, same exe, 4 + 4 runs: Eevee standing 188 -> 196 fps (ratio 1,044, 95% CI
  1,027-1,057), walking 172 -> 181 (1,054; 1,037-1,063), Pikachu walking 171 -> 181 (1,061; 1,020-1,070); the new one won
  in all 12 pairs, lower p99. The render thread is no longer the bottleneck in Viridian: the rest belongs to the game threads.
  Battery of both games, 2 repetitions, with and without: 0 crashes, same flicker.
- **Test note.** In Git Bash, putting `/c/msys64/usr/bin` at the front of `PATH` makes scripts run in MSYS2's bash, and the
  exported variables do not get through (`TAG`, `REPS` of the battery, `LARECOMP_*`): do not change `PATH` before calling scripts.

## Viridian City: spinning before sleeping in condition waits - tested and not adopted (22/10/2026, issue #20)

- **Context.** After the two backend fixes, Viridian is limited by the three game threads: each one spends
  ~45-55% of its time waiting for the others in `WaitProcessWideKeyAtomic`; the rest is almost all recompiled game code,
  with no dominant function (sampling of the threads' instruction pointer, ~500 Hz; the runtime is ~5%).
- **Measured** (`LARECOMP_PROF_CONDVAR`, new): ~8.000 wake-ups every 2 s, mean latency of ~8-9 µs (~70 ms in total),
  almost half of the waits under 50 µs.
- **Experiment.** Spin for up to 40 µs on the wait state itself before sleeping (like the mutexes, HISTORY 57). The
  total latency dropped to ~45 ms, but the interleaved A/B (4 + 4 runs per scene) showed no gain: Eevee standing
  202 x 201 fps, walking ~187 x ~187, Pikachu walking ~185 x ~183. Removed; the meter stays.
- **Seen in the A/B.** Slow runs (~123-135 fps against ~185-205) appear in both variants, at random: the same exe is
  bimodal on this machine (i5-14600KF, performance and efficiency cores). Hypothesis to test: the game threads
  landing on efficiency cores.

## Slow runs in the measurements: the window composited by Windows, not the efficiency cores (22/10/2026)

- **Symptom.** In the Viridian measurements (no fps limit), the same exe came out bimodal: ~185-205 fps or ~115-140, at random.
- **Hypothesis ruled out.** The i5-14600KF efficiency cores: with the process pinned to the performance cores (logical
  0-11), and even to one logical core per performance core, the slow runs continued; on the efficiency cores only, 94-103 fps.
- **Cause.** The game window: `nvidia-smi` and the foreground window read every 0,5 s. Same GPU clock
  (1875 MHz, P0) in all; fast runs with the window focused and covering the screen (GPU load ~50%), slow ones with it
  behind others or smaller than the screen (65-77%). Windows composites those windows (DWM) instead of showing them directly:
  1920x1080 focused 182-188 fps, 1280x720 focused 151-158 (300 Hz monitor: it is not the screen's ceiling).
- **For players.** With a limit, no measured effect: Viridian at 60 fps with the composited window gave 60,0 fps, p99 ~17,4 ms.
- **Fix (measurement).** `tools/perf_ab.py` brings the window to the front (always on top, focused) on each run.

## Viridian City: vertices stay in VRAM from one frame to the next (22/10/2026, issue #20)

- **Diagnosis.** 85% of the vertex bytes copied to VRAM each frame in Viridian were the same as in the previous
  frame (`LARECOMP_VCOPY_SAME`): static meshes, copied again every frame (~12 MB). Checking with a hash cost the
  same as copying.
- **Fix.** The game heap is reserved separately with `MEM_WRITE_WATCH` (Windows marks the pages written by
  anyone: the game's code, ours, the system's; `plat_written`). A copy of vertices is kept in VRAM
  (two 128 MB areas, staging + VRAM, like the ring) and reused as long as none of its pages has been written after
  it (one epoch per page, noted on each query). What the game rewrites every frame (found written in two
  consecutive queries) goes to the ring as before, tested again every 64. When the active area fills up, the other one is
  retired (its copies are no longer used) and emptied when the GPU has finished reading it, without waiting. The file
  service rewrites one byte of each page read into game memory, so the tracking sees it for sure. A heap that
  changes size or a reboot: everything discarded. `LARECOMP_VK_KEEP_VERTICES=0` / `LARECOMP_WRITE_WATCH=0` turn it off.
- **Mistakes along the way** (caught in the measurements, fixed before the commit): waiting for the GPU on an area used by the frame
  not yet submitted hung for 10 s (Forest); with both areas read every frame, neither became free and the cache stopped
  accepting copies; the volatile-range counter let them be re-copied on every query.
- **Measured.** Vertex upload per frame in Viridian 12,5 -> 2,6 MB (~4 GB every 2 s read from the kept copies).
  Interleaved A/B, same exe, window in the foreground, 4 + 4 runs: Viridian Eevee standing 201 -> 224 fps (ratio
  1,110, 95% CI 1,102-1,124), walking 185 -> 215 (1,162; 1,148-1,169), Pikachu walking 187 -> 215 (1,151;
  1,145-1,162); Forest +7-10% on the comparable runs (the camera of that save varies). The tracking alone, without the cache:
  no measured cost (190,1 x 189,5 fps). Battery of both games with and without the cache, 3 repetitions: `bateria_scan cmp` OK
  on the 6 routes, 0 crashes; `capture_loop` in the Forest: the world comes back the same after the encounters.
- **Limits.** Windows only (Linux does not have the tracking: it stays off). Importing the heap into Vulkan with the tracked
  region tested only on NVIDIA: in 0.6.0 the tracking (and with it the cache) only turns on by itself when the main video
  adapter is NVIDIA (`EnumDisplayDevices`, before Vulkan exists); on other cards `LARECOMP_WRITE_WATCH=1` turns it on,
  and the log says whether it stayed on. On a card that refuses the import, the log shows "guest heap import failed".

## The Finish of every frame: the render thread made the game slower (23/10/2026)

- **Symptom.** With no fps limit and the GPU with headroom (effects off, or 4K with only SMAA), the game ran
  slower with the render thread than without it: Pallet Town (Pikachu) 122 fps on against 179 off,
  3 + 3 interleaved runs. The opposite of what item 59 measured in Pewter.
- **Diagnosis.** Thread sampling (instruction pointer and return addresses on the stack): no thread
  saturated (render 31-39 %, the three game ones 21-31 %), all waiting for each other. Temporary counters on
  `nvnSyncWait` / `nvnQueueFinish`: one `nvnQueueFinish` per frame, ~4,4 ms each (drain the render thread's
  queue and then wait for the GPU). Always called from the same point in the game's frame loop (Pikachu
  `main+0x29eec`): `fdiv d0, d0, 1e6` / `fmov d1, #4.0` / `fcmp` / `b.pl` - the game only calls Finish when the GPU time
  it measured in the previous frame (the two `ReportCounter`, in ns) was below 4 ms. The counters measure our
  translation time, always below 4 ms: Finish on every frame, and game, render thread and GPU in series.
- **Fix.** With the render thread, that Finish follows the path the game itself uses when the GPU takes
  4 ms or more (no Finish; its fences still apply). The point is recognized by the instructions (return
  address and the four before the call; a unique pattern in `main`), not by address. The other Finish calls (the same loop
  calls another when the video mode changes) do not change. `LARECOMP_FRAME_FINISH=1` restores the old behavior.
- **In passing.** `CMD_FENCE_SYNC` (fence recorded in a list) read `x[1]` (the condition, always 0) instead of `x[0]` (the
  sync): those fences were never marked. Fixed; with the render thread the sync receives, when the list is
  submitted, the ticket of the work that executes it (`nvn_list_*`, `objects.c`), and `nvnSyncWait` waits only for that work.
  Let's Go does not use fences in lists (the fence-less waits come from the sync of `nvnWindowAcquireTexture`): no effect here.
  `rt_dispatch` (the game's indirect calls): the slow lookup left the function, which saved 5 registers and set up the
  stack on every cache hit; `rt_fnhook` looks at the hook table before the per-thread variable. +1,5 % (4 + 4).
- **Measured** (RTX 5070 Ti, 4K fullscreen, no effects: `aa = off`, SSAO / GI / blur / soft shadows off;
  no fps limit, interleaved runs, same copied save): Pallet Town standing 145 -> 307-313 fps (3 + 3 and 4 + 4), menu
  open 140 -> 300, route to Route 1 147 -> 260, walking in Pallet Town mean frame time 8,5 -> 5,0 ms (P95 8,4 ->
  4,7); without the render thread 204 -> 211. With a limit: 30, 60 and 120 fps and 2x frame generation with the same pacing and
  P95/P99 as the previous build. Images within the variation between two runs of the same build (PSNR 29-36 dB against
  27-33); a complete Pidgey catch on Route 1 with no defect.
- **Now.** With the render thread ~50-70 % busy, the limit is the three game threads (recompiled code
  ~87 % of their active time). The heaviest function, `f_7000608b30` (Pikachu), ~13 %: a linear hash lookup that
  recomputes the FNV-1a of each candidate's name, byte by byte - the game's own algorithm. `rt_fnhook` (every function
  entry, because there are always hooks) ~4 % and `rt_dispatch` ~3 %: lowering them requires changing `FN_ENTER` and recompiling the
  game code. Compiling the game code with -O2 changes nothing: the player's `zig cc` produces identical objects with
  -O1, -O2 and -O3.

## Wild battles: catching mid-battle, with a native 5th button (23/10/2026)
Request: catch at any moment in the battle, easier with the wild Pokémon weakened or with a status, one ball at a time (it escaped
→ back to the battle), fainted = lost the chance, and a new native option in the menu. Research and addresses in
[BATTLE-MODE.md](BATTLE-MODE.md), step 6; code in `runtime/hle/wildbattle.c` and `runtime/hle/catch_button.c`.
- **The battle engine no longer has the classic throw** (only sequences for sending the Pokémon into battle). The catch is the GO
  scene: the battle ends as a run-away and the runtime changes the result (3) to a win (1), which the post-battle process handles
  by opening the native catch.
- **Symptom: the battle HP did not reach the catch.** The object that goes to the catch came out with full HP even after the
  wild Pokémon fainted. Cause: HP lives in the battle Pokémon (BTL_POKEPARAM, `+0x7e`) and is only returned at the end
  (`9e4280`); the catch uses another object. Fix: save HP and status at that end and give them back when building the rematch
  (`9e2490`).
- **Symptom: the read watch did not see the catch rate.** The raw copy of `personal_total.bin` in the heap is not read by
  anyone; access goes through `71dfd0` (GetPersonalParam, id 8). Hook there.
- **Symptom: with the 5th button, the cursor did not move and FIGHT crashed the game.** Cause: the `uikit` navigation grid
  stores the number of columns (4) in a field that looked like an array. Fix: 5 columns.
- **Symptom: the 4 buttons came out ~207 to the right of what the layout said.** Cause: the code positions them from the right
  edge of the `B_bounding_00` area (which I had widened). Fix: do not widen the area. (Later, in the user's test, the
  row ended up one step to the left of the original: with 5 buttons the game already starts one step earlier, and I was still shifting the
  area. Fix: the area stays where it is; CATCH ends where RUN used to end.)
- **Symptom: CATCH animated when chosen, but nothing happened.** Cause: the game only wires the 4 buttons of a
  fixed table to the decisions (a callback stored in each button). Fix: the runtime stores the same callback in CATCH, with a
  frame-5 view (id 0, ignored by the game), and handles that case.
- **Trainer and 2-player battles** keep the 4 buttons: `fs.c` serves the built files only during
  the mode's battles and the originals (padded with zeros) in the others. Tested with `LARECOMP_WILDBATTLE_CATCH_MENU=0`.
- **Lessons.** (1) Before changing data, see what the code does with it (positions and ids came from the code). (2) Battle
  tests need inputs counted from the start of the battle (`b<N>`): the encounter shifts by a frame on each run.
  (3) Changing the result of a game step (run-away → win) reused the whole native flow.
- **Symptom (user): the game crashed with the "Traducao PT-BR" mod.** The mod ships the button labels in BC3/BC1 (the game uses
  BC5/BC4); the texture swap refused the format, the language file was left without the button, but the menu settings
  already asked for 5 buttons. Fix: the swap converts to any of BC1/BC3/BC4/BC5 (BC1 with a 1-bit transparent background), the
  build is all-or-nothing (11 files or the game's menu) and the label becomes CAPTURAR in a Portuguese mod. **Open:** with
  that mod the button shows as a black square. The textures reach the GPU correctly (checked with
  `LARECOMP_DUMP_TEX_FMT`) and the mod's layout is the game's; swapping the CATCH textures for the mod's own FIGHT ones, the
  button draws correctly, meaning the problem is in using the BACK textures in the mod's file (the cause is not yet
  known). Without the mod, correct.
- **Symptom (user): a 5-minute "MATCH TIME" clock.** The static battle setup (`28c770`, the same one used for
  Snorlax and the legendaries) writes 300 s to `+0x562` of the battle event. Fix: a hook after it, only in our
  battles, zeroes the time (with no time the game does not show the clock). Also clearing bit `0x80` of `+0x574` crashed the
  game (see below).
- **Symptom (user): defeating the wild Pokémon gave no XP.** The legendaries' rule: with bit `0x80` of `+0x574` (and byte
  `+0x584`), a knockout ends the battle on the spot, with no fainting and no XP; the XP check is `9d8ae0` (competitor ≤ 1,
  `+0x4a4` zero, no `0x80`/`0x800` in the setup flags at `+0xe0`). First attempt: clear the bit. The wild Pokémon now
  faints as in a trainer battle, but that sequence looks for a model that does not exist (`a9c700` reads a null pointer:
  `c0000005` at `651b40`) and, with that worked around, still waits for seconds, stalled. Adopted fix: the bit stays (no fainting,
  like the legendaries) and the hook on `9d8ae0` answers "XP on" in our battles: "Bulbasaur got 10 Exp. Points!",
  the bar fills and "Your party Pokémon in waiting also got Exp. Points!", then the battle ends.
- **Request: the catch's "Run away" goes back to the battle, with a name that says so.** These are lines from `poke_get.dat` (line 24
  the button, 4 the question, 14 the confirmation), swapped by the line hook only during our catch: "Back to battle",
  "Do you want to go back to the battle?", "Back to the battle!" (9 languages).
- **Request: going back to the battle without the whole encounter opening.** The opening is the `ee330.bseq` sequence (the camera on the
  wild Pokémon, 160 frames), the line "A wild X appeared!" and the send-out (`ee420`). `fs.c` builds at boot a 1-frame `ee330`
  and serves it only on the return (`wildbattle_quick_intro`); the line becomes "The battle continues!". Measured: from the first line to
  "Go on" 27 frames on the return (~890 in the first battle, in the accelerated tests).
- **Test trap:** `capture_loop` runs at ~250 fps, so waits on real time (sound, loading) become hundreds of
  frames; "690 frames stalled" was ~3 s. Measure in seconds (log `[vk] t=`) before calling something slow.
- **Symptom (user): hitting the Pokémon did not change the ring color** (Pikachu yellow at full HP and at half HP).
  Cause: in the Let's Go formula (`7de080`) the species rate is raised to 0,54, and I multiplied the rate before that,
  capped at 255: on a Pikachu (190) the maximum gain was 17%. Fix: the HP/status factor (up to ×3, ×2,5 sleep) goes
  on the formula's result, from which the ring and the shakes come. Measured: Pidgey at 5/18 HP, 127,9 → 255.
- **CATCH refused with "You can't escape!"** (a Bellsprout trapped with Wrap): CATCH goes out as a run-away, and the client checks whether it
  can flee (`a5c350`). Fix: with CATCH requested, the check answers yes.
- **Request (user): the return to the battle still showed the Poké Ball throw.** `ee420.bseq` (the send-out) also goes with
  1 frame on the return, until the first menu choice (a later switch has the normal animation). Compressing or moving its
  commands earlier made the Pokémon not appear; cut down, it does not close the opening line's window, which stayed on top
  of the menu - so that line is not opened on the return (`aaf230`, hook just for it). Result: dark screen → flash → menu.
- **Question (user): does whoever reaches the level evolve after the battle?** It did not. The post-battle process (`319bc0`)
  marks in state 0 who leveled up (`+0xd8 + slot`) and, in a trainer battle (type 1), goes to state 1, the loop
  that calls `728300` (the check) and `83cef0` (the evolution event, found through the string `evolution_event.cpp`); the static one
  (type 2) goes straight to the catch or to the end. Fix: after a knockout of ours, state 0 → 4 becomes 0 → 1. Seen: Chansey
  level 100 with 1 HP → Bulbasaur at 19 → "Congratulations! Your Bulbasaur evolved into Ivysaur!" and Nidoran ♂ →
  Nidorino with the Pokédex, then the world; on Pikachu too.
- **My mistake along the way:** I counted three "grew to Lv." as three Bulbasaur levels; they were three party Pokémon, one level
  each (XP goes to the whole party). Check whose each line is before concluding.
- **The black square with the PT-BR mod: it was the mipmaps.** In the mod's file the label textures have 8 mipmap levels
  (the game's, 1); the swap rewrote only level 0 and zeroed the rest, and zeroed BC1 is opaque black. On a 1440p screen the
  game reads level 0 (the button showed correctly for the user); in the tests at 480x270, a smaller level (black). Fix:
  all levels, each one the 2x2 average of the previous, with each level's Tegra block height.
- **Request: "Back to battle" without confirmation.** At first the runtime pressed A on the question (`input_press_a_at`,
  runtime/hle/input.c); the user saw the question flash ("an automatic confirm"). The real fix: leaving the
  catch is a dialog (`7d2380` creates it at `[cena+0x760]`, state machine at `+0x10`): state 0 (`7de440`) asks
  and "Yes" leads to state 1 (`7de7d0`, the exit). In our catch the hook on `7de440` only writes the next state (1),
  without opening the question. The automatic A was kept only for the following line, "Back to the battle!". (Pressing A on the catch's
  opening line was discarded: the menu cursor starts wherever it happens to be, and A opened the Bag or threw.)
- **Request: the return to the battle without the throw and with the Pokémon on the field.** `ee420` (the send-out) cut, compressed,
  sped up or with only the final part left the player's Pokémon off the field until the first move (and the camera on the
  trainer). What stayed is the original `ee420` on the return: screen → "Go, X!" with the throw → the Pokémon → menu; cut only the
  presentation camera (`ee330`) and the line "A wild X appeared!".
- **Open (user requests):** the opening of the catch scene (the camera and "Now's your chance!": the scene is a
  generic state machine, `7cb780`, with the opening inside state 1); the battle music restarts on the return; the
  flashing "ghost" next to CAPTURAR did not show up in the tests (maybe the runtime's touch cursor, drawn 3 s
  after the mouse or the touch stick moves).
- **Request: the return to the battle straight to the menu, without the throw (with Eevee on the shoulder it jumped, with bugs).** No
  edit of the send-out sequence worked (the Pokémon stayed off the field). Solution: the send-out runs in full, hidden. On the
  return, the battle fade-in is held (the screen stays dark from the catch), frame pacing is turned off
  (`g_present_fast`, runtime/nvn/objects.c) and the sound muted (`g_audio_muted`, runtime/services/audio.c); when the command
  menu is built (`ab2730`), everything comes back and the fade-in happens: the screen opens with the Pokémon already on the field and the menu (this works
  for the ball and for the shoulder partner). Limit: 900 frames. Measured: ~540-630 frames hidden, ~2,5 s of dark
  screen on an RTX 3080. Also skipping the presentation of the hidden frames broke the frame count (discarded).
- **Request: after a ball that breaks open, the return starts with the wild Pokémon's turn** (the throw was the player's
  turn). A "skip turn" action (types 7, 5 and 0 in place of FIGHT) left the turn waiting for the move choice.
  Solution: with the screen still dark, the runtime presses A on the menu (`input_press_a_at`) and `decide_hook` swaps the choice for
  a run-away (4, a complete action, like CATCH); the server's run-away roll (`9f7300`, called by the run-away `9f3ed0`)
  answers "did not escape", and the "You couldn't get away!" window (line 83) does not open. The screen brightens, the wild Pokémon attacks
  ("Bellsprout used Vine Whip!") and the menu comes back. Test: `LARECOMP_WILDBATTLE_THEIRTURN=1` does this on every return.
- **Symptom (user): after the ball that broke open, the wild Pokémon only acted when he chose Fight.** The automatic A was
  pressed once, 2 frames after the menu was built; in his game (60 fps) it landed before the menu accepted input and the screen
  brightened through the safety limit. Fix: A every 8 frames while the hidden menu waits
  (`battle_event_hook`), until the choice goes in. Tested at 30 and at 60 fps, Eevee and Pikachu.

## Wild battles: the starter walking alone battles too (24/10/2026, issues #6 and #22)
- **Before.** Since 0.4.2, with the starter walking alone (or no one), the touch went to the game's catch: the static
  battle broke (`f_7000a9ddb0` reading a null). In 0.6.0 the launcher hint started saying so (issue #22).
- **Diagnosis.** With `f_7000a7e090` ("is it the starter?") answering no, the starter comes out of a Poké Ball (`ee420`) and the
  opening passes ("Go! Eevee!"), but Eevee does not appear and the game crashes on the first move (`ac2d70` via `aeb690`). The
  model key (`[modelo+0x1e0]`) stayed 0: in `f_7000a99b40` (x2 -> species u32, form u8), `f_7000176140`
  recognizes the starter (Pikachu form 8, Eevee form 1) and its branch uses the field model of the one walking along, which the
  static battle does not have; `f_7000a9bf20` (move switch) looks up the resource by that key and reads address 0.
  There is no `pm0133_01` (the starter forms are field models), only `pm0133_00` / `pm0025_00`.
- **Fix.** In our battle, with the starter walking alone: `f_7000a7e090` answers no (it comes out of the ball) and
  `f_7000a99b40` receives a copy of the descriptor with form 0, so the model is the species', loaded through the
  normal path (`b8b70`). The 0.4.2 safeguard goes away; the launcher hint is unconditional again.
- **Tested** by script with a save with Eevee and Pidgey (Eevee walking alone), on Eevee and Pikachu 1.0.0 (the
  same save): Eevee appears, moves, knockout with XP, CATCH, hidden return with the wild Pokémon's turn, and catch.
  Not tested: a starter Pikachu (form 8) on the shoulder, for lack of a save. Addresses on Pikachu: `a7e090` → `a7e0a0`,
  `a99b40` → `a99b50`.

## Wild battles: the wild Pokémon faints, and the world music playing together with the battle music (24/10/2026)
- **Symptom (user): defeating the Pokémon gave XP, but it did not faint.** It was the legendaries' rule (bit `0x80` of
  `+0x574` of the event): the knockout ends the battle without fainting. Clearing the bit at setup (`28c770`) crashes the game
  (`ee620`, `c0000005`). The earlier attempt (above) found the missing model and a wait of seconds; the wait came from the
  accelerated test mode (~250 frames/s): at real speed (`--turbo 0`), from the knockout to the XP there are ~2 s stalled after the
  fainting. Fix: the bit stays for the opening and is cleared when the command menu appears (`battle_event_hook`, state 4,
  after `ab2730`); the fainting is the trainer battles' one (`ee620`: "Rattata fainted!", it falls and is withdrawn). The
  sequence asks a model with key 0 for a joint (`f_7000a9c700`) (it is not the Pokémon on either side; probably the
  opposing trainer's) and read the null result (`651b40`, `+0x360`); the hook answers "no joint", the game's
  own answer for a joint that does not exist. XP still goes through `9d8ae0` (byte `+0x584` stays). Tested by script: Eevee
  (starter alone) and Pikachu (another Pokémon walking along), knockout with fainting and XP; the player's Pokémon defeated
  by a level 60 wild Pokémon also faints and the game asks for the next one (the "everyone fainted" case was not run to the end).
- **Symptom (user): going to the catch and back, the world music played together with the battle music.** Music changes
  are named Wwise events, sent by `f_700016d060` (`{hash, nome, tamanho}`). The static battle sends
  `Push_BGM_field_to_battle` when it starts (`28cbec`, if `f_700016cee0` says it has not been done yet) and
  `Pop_BGM_battle_to_field` when the process ends (`28d84c`), which for us is the end of the catch. Event log: every
  catch that went back to the battle sent the Pop, and the next battle sent the Push only every other time. Fix:
  in a catch that failed (the battle will come back) the Pop is not sent, and neither is the next Push; the battle music
  continues through the catch and the return. In the log, 8 returns in a row with no Pop or Push; the normal end (catch, knockout) sends the
  Pop. The tests run without sound: the user played (catches, a ball that broke open and the return, "Back to battle", knockouts) and
  confirmed the music and the fainting.
- Addresses on Pikachu: `a9c700` → `a9c710`, `16d060` → `16d030`.

## Issue #21: the game closed after minutes, in the re-execution for the intermediate images (24/10/2026)
- **Symptom.** Closes after 5 to 15 minutes (0.5.0 and 0.6.0, AMD RX 6600 XT; other reports with no log), with 60 fps
  on: `c0000005` reading `0x5000000001` on a runtime thread.
- **Diagnosis.** The log has only runtime addresses; I recompiled 0.5.0 in a worktree (same compiler) and symbolized
  with `nm`: `exec_list` inside `vk_interp_render` (the re-execution of a frame for the intermediate images),
  on the render thread.
- **Cause.** The re-execution read the command lists straight from the game's command memory, which it reuses to
  record the next frame while the render thread re-executes this one. The lists' hash was checked at the
  start (`vk_interp_begin`), but the read happened later: a half-rewritten record gave a called list (CALL) the
  address `0x5000000001`.
- **Fix.** In `vk_interp_begin` the lists are copied to a runtime buffer (`ip_list_copy`, 8 MB, doubles if
  short), with the hash computed over the copy and checked against the one from when they ran; the re-execution runs the copy (JUMPs
  followed, CALLs pointing at the copies, with a JUMP over them). The reads stay inside the guest window.
  First version wrong: the called lists sat right after the CALL and the calling list continued inside them
  ("0 blended"). Tested: 2x frame generation, blending at the same levels as runs without the change (normal
  variation between runs larger than the difference). The crash was not reproduced here; the fix addresses the cause.

## Issue #24: shadow sampling on a color texture, in battle (24/10/2026)
- **Symptom.** "Device lost" (`address ip invalid`) on an RTX 5070 Laptop in battle, when Bellsprout uses Wrap; no
  intermediate images at that point (battle at 30).
- **What was found.** On the RTX 3080 Wrap does not hang. With the validation layer: `VUID-vkCmdDrawIndexed-None-06479`, a
  battle shader does a depth (shadow) comparison on an RGBA8 texture: the game binds a 16x16 color texture there
  (format `0x25`) when it has no shadow map. On the Switch the comparison reads its red; on Vulkan it is
  undefined. (A GPU hang in one run with validation, on the summary screen, did not repeat without it.)
- **Fix.** A comparison slot without a depth texture gets a 1x1 depth image at 1,0
  (`vk_dummy_depth_view`): everything lit, as against the white texture. Clean validation in battle with Wrap (3 uses);
  identical images. Whether it was the cause of the 5070 hang only the issue's author can confirm.

## 0.7.0: updating in place asked for the game folder again (24/10/2026, fixed in 0.7.1)
- **Symptom.** With 0.7.0 unzipped over a 0.6.0 folder, the launcher showed the initial setup
  ("Select the extracted game folder"), not the recompilation.
- **Cause.** PR #26 put the CPU level in the library id (`<id>-v3` / `-v2`); the 0.6.0 one (`<id>` with no suffix) stopped
  being valid, `setup_ready()` became false and nothing called `setup_start(NULL)` (only the game code case, already handled in
  `gamesetup.c`). In earlier versions the id had not changed, so the case never came up.
- **How it slipped through.** The release test in the `D:\teste-0.7.0` folders never actually ran 0.7.0: their `lasrecomp.log` files and
  DLLs were still the 0.6.0 copies; I did not check that before publishing. My automated attempt with
  `LARECOMP_LAUNCHER_AUTOPLAY` stayed stuck in the launcher (it only plays when it is already ready).
- **Fix (0.7.1).** `setup_needs_build()` (game files installed, library from another version) and the launcher
  calls `setup_start(NULL)` by itself, once. Tested on a copy of the 0.6.0 folder with 0.7.1 on top: it recompiled
  by itself (id `...-v3`) and the game ran up to gameplay.
- **Lesson.** Before publishing, check the new version's log and DLL (date and id) in the test folders, not just the
  test's answer; and test the updater's path (previous version's folder with the new one on top).

## Tree flicker in battle with TAA (24/10/2026, not a regression)
- **Report (user, testing 0.7.1).** The vegetation flickers in battles.
- **Measurement.** Consecutive frames of a fixed battle, pixels in the upper part that go up and down (±24) over three frames:
  TAA 1,1 %, SMAA 0,05 %; SSAO Ultra changes little (0,9 % without). 0.6.0 and 0.7.1 with the battle stopped: the same (0,44–0,78 %
  against 0,33–1,09 %). A first comparison seemed to show a regression, but the 0.6.0 runtime ignored the script's
  `b<N>` inputs (the battle stayed stopped) - only compare with scripts that both versions understand.
- **Pending.** TAA on the cut-out tree leaves in battle (with action); SMAA does not have it.

## Issue #30: "Make the 2x images" failed on an integrated AMD (24/10/2026)
- **Log.** waifu2x generated the 4708 images (exit 0), but back at 1x 2981 of the 3081 textures were far from the
  original (mean > 16): the package was refused. At the same time the launcher warned "GPU did not finish submission
  within 10 s": the shared integrated GPU could not handle waifu2x and our drawing together.
- **What does not work.** The CPU mode of waifu2x-ncnn-vulkan 20250915 (`-g -1`) gives a segmentation fault (tested here, with
  and without `-t`).
- **Fix.** If the output fails or is refused, a second, lighter pass: 32 tiles and one thread per stage
  (`-t 32 -j 1:1:1`). On the RTX 3080 (`LARECOMP_UI2X_GENTLE=1`): 4708 images, none refused (largest error 7,35), ~5 min.
  Whether it solves it on the integrated AMD only the issue's author can say.

## Issue #24 on 0.7.0: full validation on Wrap, no error (24/10/2026)
- **Report.** Same crash on the RTX 5070 Laptop with 0.7.0 (without 60 fps); the GPU resets; with the integrated Intel it does not
  happen; something similar in another game's recomp.
- **Done here (RTX 3080).** Battle with Bellsprout using only Wrap: core validation, then GPU-AV (instrumented
  shaders), and GPU-AV with `LARECOMP_NO_ROBUST=1` (out-of-bounds accesses that robustness hides): no errors.
  Points to the driver or the card; requests to the author: driver version and tests turning off what only NVIDIA uses
  (`LARECOMP_WRITE_WATCH=0`, `LARECOMP_VK_NO_GMEM=1`, `LARECOMP_VERTEX_READ=copy`, `LARECOMP_ONE_QUEUE=1`).

## Issue #31: the game closed right at the start with 2002-6063 (24/10/2026)
- **Log.** Pikachu, RTX 3060 Ti: after `fsp-srv` only one RomFS read (`romfs reads: 1 (0 KB)`) and
  `svcBreak` with result 2002-6063. The `[fs] romfs index: N files` line is missing, which in a normal run comes before:
  the index of `extracted/romfs.bin` could not be read, so its image is invalid (truncated, or a `romfs.bin` from
  another tool that the setup copied as it was); the game read a meaningless header and stopped.
- **Fix.** `romfs_image_ok` (runtime/romfs_build.c) checks the image: 0x50-byte header, tables and data
  inside the file, and recognizes an IVFC image. Setup refuses such an image (new failure
  `SETUP_FAIL_IMAGE`, with a message on the start screen and the reason in the log); with the image already installed, the start screen shows
  that failure and asks for setup again; at boot, `fs.c` stops with a clear message instead of 2002-6063. Tested:
  image truncated at 1 MB ("tables point past its end") and one with an IVFC header stop with the message; the good image
  goes on (27818 files). I do not yet know what the issue author's image was: asked for its size and where it came from.
