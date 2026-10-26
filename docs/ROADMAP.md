# Roadmap to a mature native port

_2026-10-17. Inventory of the code and the current documentation. This file is a verifiable backlog, not a claim that every item is a bug. The status of each item should be updated with links to reproductions, logs, captures or tests before a release._

## Scope and priority

The main target is **Eevee and Pikachu 1.0.0 on Windows**, with faithful gameplay at 30 fps. The launcher also accepts 1.0.1/1.0.2, but the [README](../README.md) says those versions have not been tested locally and that the address-based options only work on 1.0.0. Announcing support for the updates requires testing them; otherwise, present them as unverified compatibility. 60 mode and wild battles remain **experimental** until they meet their own criteria. Online features, Poké Ball Plus and co-op need an explicit scope decision; a mature product can state that it does not offer them, as long as they do not hang or confuse the user.

Priorities: **P0** save loss, serious reproducible failure or widespread corruption; **P1** blocks the quality of the stable version; **P2** improves robustness, performance or fidelity after the gates; **P3** optional expansion. There is no confirmed P0 in this inventory. If one appears, it interrupts the queue.

Each item has a type: **observed** (already reproduced), **validation** (coverage missing), **hypothesis** (the code suggests a risk, no failure measured yet) or **decision** (product scope).

## Phase 1 — Prove the complete game and protect the data

This phase is the critical path. Before raising quality or FPS, we need to know where the stable version fails.

| ID | Pri. / type | Work | Completion criterion |
|---|---|---|---|
| J01 | P1 / validation | Play a full campaign of **Eevee 1.0.0**, from new game to the credits, with saves at fixed points. Include gyms, caves, cities, events, battles, captures, evolution, box and returning to earlier areas. The README only records tests up to Viridian City. | Per-chapter script with build, hardware, test save, failures and result; no blocked progression or reproducible crash. |
| J02 | P1 / validation | Repeat a dedicated campaign of **Pikachu 1.0.0**, beyond the name screen; test playing with Pikachu and exclusives. Do not rely only on a save copied from Eevee to validate parity. | Same script and criteria as J01, including specific differences. |
| J03 | P1 / validation | Exercise save/load at checkpoints, normal exit, restart, multiple saves and write error/no space. Audit `runtime/services/fs.c` and the replacement/backup policy before changing any format. | Original save preserved; the test copy opens after each case; write failures reported without losing a valid save. |
| J04 | P1 / validation | Build a regression matrix for boot, menus, creation, overworld, NPCs, transitions, battle, capture, save, video and audio, in **both** games. Use `tools/repro.sh` with isolated test directories and complete it with a manual checklist. | Every relevant fix runs the affected scenarios; logs and images tied to the build; no test touches the player's save. |
| J05 | P1 / validation | Test the **1.0.1/1.0.2** updates of both games, or reduce the public promise of support until there is evidence. Check launcher, generator, HLE, saves and features without fixed addresses. | Table per title/build with tested and disabled features; README and launcher agree. |
| J06 | P1 / validation | Rehearse the release zip on a clean Windows, without MSYS2/Python/Zig on the PATH: extraction, choosing one's own files, generation, close/reopen, errors and version update. Repeat for Eevee/Pikachu. | Reproducible package, with no game files/keys, and installation script completed for both titles. |
| J07 | P1 / validation | Test Windows 10/11 and NVIDIA, AMD and Intel GPUs (dedicated and compatible integrated ones), including different drivers, focus changes, resize, fullscreen, suspend/resume and device lost. | Published matrix with driver version, resolution, mode, validation logs and limitations. Do not announce a combination without testing. |
| J08 | P1 / validation | Test a long session with audio on, opening video, area changes, battle, capture and repeated save/load. Measure RAM/VRAM, handles, Vulkan objects and underruns. | No unexplained continuous growth, no noticeable desync and no reproducible crash; measurements and duration recorded. |

## Phase 2 — Fix visible defects and harden the runtime

| ID | Pri. / type | Work | Completion criterion |
|---|---|---|---|
| R01 | P1 / observed | Solve the **aura of special Pokémon that disappears for one frame**, including in the release at 30 fps (`docs/PROGRESS.md`). Use `LARECOMP_COUNT_TEX`, `LARECOMP_COUNT_FS`, `LARECOMP_TRACE_COUNT` and a save with an aura Pokémon; avoid using F9 as proof, since it changes the first recorded frame. | Cause located in draw/state/effect, minimal fix and frame-by-frame comparison in Eevee/Pikachu. |
| R02 | P1 / observed | Reduce objects that appear late on the first visit because of a new shader/pipeline. Separate translation, SPIR-V, pipeline creation and draw wait; investigate per-area early preparation and cache persistence. | Same route with cold/warm cache, skipped draw count and P95/P99 before/after, with no loading or RAM regression. A pipeline cache is recommended practice according to the [Khronos documentation](https://docs.vulkan.org/guide/latest/pipeline_cache.html), but the current mechanism needs to be measured in the game. |
| R03 | P1 / hypothesis | Inventory `UNIMPLEMENTED` and success responses without an object in `runtime/dispatch.c`/`ipc.c`. Check in the SDK which `nn::Result` and payload each call really expects; keep the offline stubs that prevent loops/crashes. | Zero false successes on exercised paths; correct error/object demonstrated per call and by a scenario test. |
| R04 | P1 / validation | Run Vulkan validation, synchronization validation and RenderDoc captures in representative scenes; investigate the known case of an RGBA8 texture sampled as a shadow (`docs/HISTORY.md`). | No error attributable to the runtime on supported paths, or a specific documented exception with measured impact. [Khronos](https://docs.vulkan.org/guide/latest/development_tools.html) describes these layers as tools for access conflicts and shader errors. |
| R05 | P1 / validation | Check 3D outside Pallet Town: skinned models in battle, large Pokémon, several cities, caves, shadows, particles and effects. | Set of reference captures at 720p/1080p/1440p/4K and inspection of geometry, depth, viewport, scissor and UI in both games. |
| R06 | P2 / hypothesis | Measure concurrent readers of `G[1]` while `accessor_hook` temporarily changes it (`runtime/hle/fps60.c`). If there is a race, move the change to the timer object or another validated local point. | List of readers/threads and a concurrency test; no exposure of the temporary value to unintended scenes/threads. |
| R07 | P2 / hypothesis | Audit the growth of `Prog`/`Pipe`, descriptor pools, textures, command buffers and the view cache in long sessions. `exec.c` retains certain objects to preserve pointer identity. | Object and byte counters stabilize on a repeated route; any new release of resources goes through a lifetime test with validation. |
| R08 | P2 / validation | Check the semantics of the recompiled code: indirect branches/jump tables, registers, exclusives, barriers, FPCR/FPSR, rounding, NaN, NEON/AVX2, alignment and stores. Prioritize instructions in hot functions or ones associated with real bugs. | Differential corpus with genuine ARM64 reference results and per-function traces; no adjustment based only on an isolated microbenchmark. |
| R09 | P2 / validation | Confirm return codes, timeout and cancellation of the kernel, filesystem, input, offline network, audio and video services. The history shows several cases where the SDK semantics differed from LASRecomp. | Cases reproduced against SDK/disassembly, timeout and failure tests, no deadlocks on reopen/reboot. |
| R10 | P2 / decision | Handle offline actions with no useful way out (e.g. "Connecting..." in Mystery Gift): exit with B, a clear message or marking as unavailable in the launcher, without faking an online service. | No supported flow gets stuck; documentation and interface reflect the real behavior. |

## Phase 3 — Performance, image and 60 fps with measurements

| ID | Pri. / type | Work | Completion criterion |
|---|---|---|---|
| D01 | P1 / validation | Define reference hardware and fixed routes per scene. Record per-present frametimes (`LARECOMP_FRAMETIME_CSV`), CPU per thread, GPU timestamps, RAM/VRAM, shader compile, skipped draws and audio; separate cold/warm cache and boot/gameplay. | Reproducible 30 fps baselines in Eevee/Pikachu and at least one GPU from each supported vendor, with average/P95/P99 and the full configuration. |
| D02 | P2 / hypothesis | Use a CPU profile to choose recompiled functions and runtime costs (`LARECOMP_FNPROF`, CPU sampler); evaluate dispatch, uniform/vertex copies, mutexes, allocations and hot loops. Only then experiment with flags, LTO or PGO. | Before/after improvement on the same route and hardware, with no functional divergence; build cost and compatibility recorded. |
| D03 | P2 / hypothesis | Measure `vk_full_barrier`, submits, descriptor sets, staging, uploads, framebuffer copies and waits in `runtime/nvn/vulkan/exec.c`. Reduce barriers only with known resource dependencies. | GPU timestamps and sync validation before/after, without artifacts; the [Khronos synchronization guide](https://docs.vulkan.org/guide/latest/synchronization.html) warns about errors and idleness from incorrect synchronization. |
| D04 | P2 / validation | Give dynamic resolution real GPU time in Vulkan; today it receives `gpu_ms=-1` and reacts to the interval between presents, which also includes CPU/loading. Test 30↔60 transitions. | Stable scale at 30/60 when the GPU has headroom; drops only under sustained pressure; no noticeable oscillation in menus/battles. |
| D05 | P2 / validation | Compare SMAA on/off, anisotropy, post-processing, fine textures and true internal resolution at 1080p/1440p/4K. Do not add TAA without motion vectors, history and a ghosting test. | Capture pairs and GPU cost, without blurred UI, new shimmering, misaligned shadows or a relevant increase in latency. |
| F01 | P1 to promote 60 / validation | Measure position, movement, animation, camera, scripts, battle, capture, audio and clocks at the **same seconds of gameplay** at 30 and 60. A present counter alone is not enough. | Comparative suite per scene in both games; deviation quantified and explained. |
| F02 | P1 to promote 60 / validation | Complete the map of frame counters and readers of the `G` table/steps 0 and 1; test menus, Pokédex, cutscenes, transitions and interactions between modes. | List of covered functions and subsystems, with no speed-up or freeze when changing scenes. |
| F03 | P2 / research | Improve intermediate frames only with stable object identity and handling of cuts, bones, shadows, UI and effects. The generic prototype was removed due to ghosting and deformations. | Frame-by-frame and in-motion visual comparison; per-scene disabling when there is no reliable identity. |
| F04 | P2 / research | Evaluate battle/capture at native 60 Hz after fixing counters and physics/timers, or keep 30 explicitly. | Mechanics, duration, audio and animation the same as at 30 fps within the tolerance defined by the test; no regression in either game. |

## Phase 4 — Product, quality and maintenance

| ID | Pri. / type | Work | Completion criterion |
|---|---|---|---|
| M01 | P1 / validation | Create CI for building the **runtime without game files**, unit and static tests, script/format analysis and packaging of a test artifact. Keep tests that require one's own console dump on a local runner, outside the public CI. | A pull request fails on a build/test error; no game content, key, generated file or log goes into Git. |
| M02 | P1 / validation | Define a release checklist and a per-version report: clean binary, both games, mandatory scenarios, save, audio, GPUs, Vulkan validation, performance, documentation and rollback. | A release only ships with attached evidence and published limitations; experimental tests have their own section. |
| M03 | P1 / observed | Fix discrepancies between `PROGRESS.md`, `BATTLE-MODE.md`, README and the code: there are old passages saying the game went back to 30 fps or listing as the next step the battle prototype that is already implemented. | Each document describes the current build and points to the history for earlier versions; a single support matrix. |
| M04 | P2 / validation | Improve diagnostics for the user: setup/version/driver error, crash cause, copyable log and recovery instructions without needing the development environment. | Problem reports contain build, GPU/driver, title/version, configuration and last error without including saves/keys. |
| M05 | P2 / validation | Review pinned dependencies, hashes and licenses of the package (`make dist`), as well as reproducing the zip on a clean machine. | Inventory of components and licenses; reproducible package with no unexpected dependency on local tools. |
| M06 | P2 / decision | Establish a compatibility policy: Windows as the supported platform; Linux/Steam Deck only after a build **and** a real test. Distinguish missing feature, known defect and experimental in the launcher/README. | Public expectations match the J05/J07 matrix, with no promise based only on compilation. |

## Optional track — new features

These items should not delay the stabilization of the original 30 fps experience. If they are offered as official options, they go through the same regression and parity gates.

| ID | Pri. / type | Work | Completion criterion |
|---|---|---|---|
| O01 | P2 / observed | Complete the experimental wild battles: split XP, nature/shiny/form, fleeing, the Pokémon disappearing from the map, encounter types and more areas (`docs/BATTLE-MODE.md`). | Cases per species/form/outcome, with no corrupted save and with Eevee/Pikachu equivalent. |
| O02 | P2 / validation | Integrate and test the real gyroscope of compatible controllers, axis direction, calibration and drift; keep stick/mouse as a fallback. | Physical controller tested in capture and in long sessions; no relevant drift. |
| O03 | P3 / decision | Evaluate local co-op, Poké Ball Plus and online integration only if there is scope, hardware and a test protocol. | The project decides to implement with complete tests or to declare it unsupported with a safe way out. |
| O04 | P3 / decision | Evaluate Linux/Steam Deck as an additional port, considering Vulkan, audio, input, paths and distribution. | Real game, performance and installation tests on the platform; compiling the inherited path is not enough. |

## Suggested execution order

1. **J01–J04, J06 and M03:** build gameplay, save and release coverage; fix any P0/P1 found. Run R01 in parallel with the script because a visual reproduction already exists.
2. **J05, J07–J08, R02–R05 and M01–M02:** widen compatibility and remove presentation/first-use defects, with automatic and manual gates.
3. **D01 before D02–D05:** obtain baselines per hardware/scene. Optimization without a profile and without a comparison does not count as a completed improvement.
4. **F01–F04:** promote 60 fps per subsystem, without changing the 30 default until speed and fidelity are validated.
5. **O01–O04:** new features according to product decisions and testing capacity.

## Definition of "mature stable version"

- Main campaign completed in Eevee and Pikachu on the announced builds, with intact saves and no reproducible blocker.
- Critical J04 scenarios pass on a clean release; no known P0/P1 without a fix or a specific public limitation.
- Vulkan rendering and synchronization verified on the announced GPUs; aura and first-visit pop-in solved or classified with explicit impact and plan.
- Frametime, audio and memory baselines published for reference hardware; performance measured in gameplay, not inferred from an FPS counter.
- Reproducible release with no game content or keys, with installation and diagnostics that work outside the development machine.
- 60 fps and wild battles only drop the "experimental" label when their own gameplay, timing and parity matrix is complete.
