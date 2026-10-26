# Technical audit of the native port

_2026-10-17. Scope: static review of the critical paths of the runtime and the generator, research already reproduced in this repository, a runtime build and an isolated test of dynamic resolution. It is not a proof of correctness of every ARM64 instruction, service or scene._

> **2026-10-17 review (later validation):** this audit's test ini (`build/audit-dynamic.ini`) was written with CRLF, and the `larecomp.ini` reader only stripped the CR from `mod` and `game_lang`: `resolution = dynamic` became "Automatic" and the in-game runs described below **did not have dynamic resolution on** (the old code itself also does not log `[dynres]` that way, even forced to 20 fps). Redone with an LF ini, same Eevee 1.0.0 route at 30 fps: old code 1.00 → 0.60; new code between 0.95 and 1.00 (it only reacts to loading stalls); 60 mode in the overworld at 60.49 fps average. The reader now strips CR/LF from every line (HISTORY 56). Revised priority: **P3**, because dynamic resolution does not appear in the launcher (only `resolution = dynamic` edited by hand).

## Observed architecture

`tools/gen.py`, `analyze.py`, `lifter.py` and `patches.py` convert the four NSOs into C; the `Makefile` compiles the result into `lasgame.dll` with `-O1`, `-fno-strict-aliasing` and `-march=x86-64-v3` (or `x86-64-v2` on a CPU without AVX2: `gamelib_cpu_level`). `runtime/loader.c` loads the segments, symbols and relocations into the guest address space. `runtime/cpu.h` holds the general and vector registers, flags, FPCR/FPSR and memory/atomic helpers. `runtime/dispatch.c` resolves indirect calls through sorted tables with a per-thread cache. `runtime/fnhook.c` makes it possible to intercept game functions without editing generated C, translating known Eevee 1.0.0 addresses to Pikachu 1.0.0.

The runtime implements kernel and IPC (`runtime/kernel.c`, `ipc.c`), services (`runtime/services/`), input and video (`runtime/hle/`) and NVN (`runtime/nvn/`). The Vulkan backend translates NVN commands, Maxwell shaders and resources; shaders and pipelines are compiled in workers (`runtime/nvn/vulkan/exec.c`). The game keeps the logic; the runtime controls the presentation pacing in `runtime/nvn/objects.c`.

## Prioritized findings

| Priority | Evidence | Consequence and action |
|---|---|---|
| **P3 fixed** | `runtime/nvn/output_scale.c`: `dyn_update` used thresholds of 17.4 ms and 14.5 ms for every scene; `runtime/nvn/objects.c` uses ~33.3 ms in the default mode. | Dynamic resolution could drop from 1.00 to 0.50 at a steady 30 fps. The thresholds now use 33.33 ms in 30 Hz scenes and 16.67 ms in the overworld/menu of 60 mode, discarding samples of one frequency when the scene changes. Deterministic test: 180 frames at 33.33 ms resulted in 0.70 before and 1.00 after. |
| **P1 under investigation** | `runtime/dispatch.c:hle_unimplemented` returns `x0=0` for symbols without HLE; `runtime/ipc.c` also has offline services. | Some callers may interpret this as success and try to use a missing object. Inventory the symbols/services used by Eevee and Pikachu with `LARECOMP_TRACE_SVC` and `[hle] UNIMPLEMENTED` logs; implement the correct result according to the SDK contract. The presence of the stub alone does not prove a gameplay failure. |
| **P2 under investigation** | `runtime/hle/fps60.c:accessor_hook` temporarily writes `G[1]` in shared memory, calls the guest and restores it. | A reader on another thread may observe 16.67 ms during the hook window. Measure readers and threads before replacing it with a local value in the timer object. No race was reproduced in this audit. |
| **P2 under investigation** | `runtime/nvn/vulkan/exec.c` keeps old `Prog`/`Pipe` objects to avoid key collisions by pointer, according to the comment near the cache. | Memory use may grow with program re-creations. Measure object count, bytes and the length of long sessions; switch the identity to generation/ID only after validating every key. |
| **P2 under investigation** | `runtime/cpu.h` performs ordinary stores directly and implements exclusives with `__atomic_*`; `tools/lifter.py` emits a fence for barriers. | A broad differential proof of the semantics of atomics, NEON and FPCR/FPSR under concurrency is missing. Prioritize an instruction corpus and comparison against genuine ARM64 execution; do not convert thread-shared code without that proof. |
| **P2 under investigation** | `runtime/nvn/vulkan/exec.c:vk_full_barrier` uses `ALL_COMMANDS`/`MEMORY_WRITE` at several points between passes, copies and dispatches. | A broad barrier can serialize GPU work, but whether it is safe to narrow it depends on the producer/consumer of each resource. Capture GPU timestamps and Vulkan validation before specializing; no gain is measured here. |

The program cache risk and the concurrent `G[1]` read risk are **hypotheses supported by the code**, still without a reproduced defect. They were not treated as confirmed bugs.

## Static review coverage

| Area | Evidence checked | Limit of the conclusion |
|---|---|---|
| Mapping and indirect branches | `tools/analyze.py` extracts starts from `.eh_frame_hdr` and code pointers in data; `tools/lifter.py` distinguishes `blr`/`br`; `runtime/dispatch.c` resolves addresses with search and cache. | No differential test of all jump tables and targets against ARM64 was done. |
| CPU state, SIMD and FP | `runtime/cpu.h` pins offsets with `_Static_assert`; `tools/simd_c.py`, `runtime/simd.h`/`simd_fp.h` handle FPCR, NaN, FZ and FMA with slow paths. | The presence of handling does not prove bit-for-bit equivalence; reference vectors for rounding, flags and edge cases are missing. |
| Kernel, services, filesystem, audio and input | `runtime/kernel.c` uses mutex/condvar and implements documented timeout semantics; `runtime/services/`, `runtime/hle/input.c` and `movie.c` cover the game paths observed in the history. | This round had no concurrent load, save/load test, audio enabled or full sweep of return codes. |
| Vulkan | `exec.c` keeps the pipeline cache, workers, descriptor pools, transient ring and barriers; `textures.c` contains staging and conversion paths. | There was no RenderDoc capture/validation nor a profile of stalls, overdraw, VRAM or barrier cost. |

## Regression strategy

For scripted tests, `tools/repro.sh` copies the save to `build/test-save` and uses `LARECOMP_INPUT`; it **deletes** the temporary folder and `build/frames`, so check those locations before running it. Use `TURBO=0` to measure the real pacing. Test each scene on both 1.0.0 builds, with 30 mode and 60 mode off/on as applicable. Compare corresponding frames by game time, not only by present number.

| Scenario | Required check |
|---|---|
| Boot and menus | Boot without a new `[hle] UNIMPLEMENTED`; selection, Pokédex and animations at the original speed. |
| Creation, save/load | Correct name; save on a copy; reload and check integrity. |
| Overworld, NPCs and transitions | Position, camera, dialogue, fade, objects and no skipped draws after loading. |
| Battle and capture | Turns, animations, aiming, ring and timers; experimental 60 must keep the speed of 30 mode. |
| Audio and video | No underrun; sync with the frame and with wall-clock time. |
| 1080p, 1440p, 4K | Capture of the internal image and of the window, viewport, depth, UI, shadows and post-processing. |
| Dynamic resolution | At a steady 30 Hz, the scale must stay at 1.00; under persistent load it must drop; repeat when entering/leaving 60 mode. |

This run included a runtime build, the isolated test and scripted **Eevee 1.0.0** runs in 30 and experimental 60, with `resolution = dynamic`, the save copied to `build/audit-save-20261017` and audio off. Both reached the overworld and produced a 3D scene capture at 1280×720, with no `[dynres]` line. The optional instrumentation measured, over the 90 presents 1360–1449, averages of 33.334 ms (30) and 16.540 ms (60); details in `performance-analysis.md`. **Pikachu, audio, scene switching, gameplay speed and manual tests were not validated**; a release requires these steps. `tools/test_output_scale.c` covers the scale decision, not the whole Vulkan presentation.
