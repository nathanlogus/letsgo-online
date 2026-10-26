# Performance analysis

_2026-10-17. The measurements below distinguish earlier logs, a new deterministic test and work not yet measured. No number is attributed to a GPU in this audit._

> **2026-10-17 review (later validation):** this audit's test ini (`build/audit-dynamic.ini`) was written with CRLF, and the `larecomp.ini` reader only stripped the CR from `mod` and `game_lang`: `resolution = dynamic` became "Automatic" and the in-game runs described below **did not have dynamic resolution on** (the old code itself also does not log `[dynres]` that way, even forced to 20 fps). Redone with an LF ini, same Eevee 1.0.0 route at 30 fps: old code 1.00 → 0.60; new code between 0.95 and 1.00 (it only reacts to loading stalls); 60 mode in the overworld at 60.49 fps average. The reader now strips CR/LF from every line (HISTORY 56). Revised priority: **P3**, because dynamic resolution does not appear in the launcher (only `resolution = dynamic` edited by hand).

## Available baseline

The renderer logs windows of roughly 2 s in `runtime/nvn/vulkan/device.c`: window fps, longest interval, draws, passes, uploads and compile time. `tools/perf_summary.py` summarizes them. In this audit, processing **pre-existing** logs (without repeating the session, so not comparable as a benchmark):

| Log | Duration | Average FPS of the windows after boot | Windows with a frame >50 ms | Skipped draws | Compiled programs |
|---|---:|---:|---:|---:|---:|
| `logs/repro-menu.log` | 52 s | 31.4 | 4/25 | 716 | 473 |
| `logs/repro-link2.log` | 96 s | 30.7 | 6/47 | 514 | 444 |

A new run, `build/audit-dynamic-world.log` (Eevee 1.0.0, copied save, 30 fps, dynamic resolution, 1280×720 window, audio off), reached the overworld at present 1450: 48 s, 23 windows of ~2 s at 30.0 fps, one window with a maximum frame of 64 ms, 527 skipped draws in 7 compile windows, 479 compiled programs. There is no `[dynres]` line, so no scale reduction was logged. The `build/window.png` screenshot showed the 3D scene. **Audio was not tested** in this run.

These sessions involve different scenes, compilation and options; the numbers do **not** measure the gain from the fix. The parser gives an average of windows, not a frame-weighted average FPS. The longest interval of each window does not allow reconstructing percentiles either.

Optional per-present collection was added in `runtime/nvn/vulkan/device.c` (`LARECOMP_FRAMETIME_CSV=<caminho>`), summarized by `tools/frametime_summary.py` with *nearest rank* percentiles. In a new Eevee run, `build/audit-30-frametimes.csv`, filtering overworld presents 1320–1449: **130 samples, average 33.332 ms (30.00 fps), P95 34.326 ms, P99 43.453 ms, maximum 58.980 ms, one frame >50 ms**. These are intervals between **game presents**, not GPU work times. CPU/GPU, RAM and VRAM remain unmeasured.

In the 1360–1449 range, after entering the overworld in both modes, with the same copied save and scripted input:

| Mode | Samples | Average | P95 | P99 | Maximum | >50 ms |
|---|---:|---:|---:|---:|---:|---:|
| 30 fps | 90 | 33.334 ms (30.00 fps) | 33.723 ms | 34.966 ms | 34.966 ms | 0 |
| 60 experimental | 90 | 16.540 ms (60.46 fps) | 18.152 ms | 18.585 ms | 18.585 ms | 0 |

The 60 mode log marked `world: 60 fps` at present 1342. The scenes may be at different game times despite having the same present number; the table compares **cadence**, not gameplay quality or GPU gain. There was no `[dynres]` line in either run. Audio was off in the tests.

## Bottlenecks and costs identified in the code

- **Initial shader/pipeline compilation:** `runtime/nvn/vulkan/exec.c` uses workers and a per-draw wait budget; `renderer_stats()` exposes translation, SPIR-V, pipelines and waited draws. The earlier research in `docs/HISTORY.md`, part 8, measured pop-in and the effect of the cache. Repeat with cold/warm cache and on both target GPUs before changing the budget.
- **Submission and synchronization:** `LARECOMP_VK_PROF=1` separates execution time, descriptor sets, vertex copies and submit; the `[hitch]` log identifies frames above 50 ms. There are no new samples of these counters in this audit.
- **Dynamic resolution:** there was an incorrect decision at 30 Hz, which lowered the resolution without real performance pressure. The adjustment is a **quality and control fix**, not an FPS optimization.
- **Dispatch and recompilation:** `runtime/dispatch.c` uses a direct per-thread cache for indirect calls; `runtime/cpu.h` adds a profiler/hook branch at each entry. There is a potential cost, but it was not profiled, so replacing it with LTO/PGO or speculative parallelism is not warranted.

## Reproducible comparison of the fix

`tools/test_output_scale.c` calls the real `output_scale` implementation over 180 synthetic presents. Compiling against `git show HEAD:runtime/nvn/output_scale.c` and against the changed file isolates the decision logic:

| Scenario | Before | After | Criterion |
|---|---:|---:|---|
| Mode 30, 33.33 ms per present | scale 0.70, test fails | scale 1.00, test passes | Do not reduce the resolution at the nominal rate. |
| Mode 30, 45 ms per present | — | scale 0.70 | Still react to persistent load. |
| Mode 60, 16.67 ms per present | — | scale 1.00 | Preserve the nominal 60 Hz behavior. |
| Mode 60, 25 ms per present | — | scale 0.70 | Reduce the scale when 60 Hz cannot be sustained. |
| Transition 30→60→30, 90 presents in each nominal phase | — | scale 1.00 | Do not mix samples from different frequencies. |

Test command: `clang -O2 -Iruntime tools/test_output_scale.c runtime/nvn/output_scale.c -o build/test_output_scale.exe`, followed by `build/test_output_scale.exe <30|60> <ms>` for the four values and `build/test_output_scale.exe transition`. The final scale of 0.70 depends on the 180 frames and the current cooldown; the test checks the direction of the decision.

## Next profiling round

1. Repeat the `LARECOMP_FRAMETIME_CSV` collection on the same overworld stretch, save, input, resolution and cache, separating boot/loading; quantify the CSV overhead in a paired session.
2. In 30 and 60 sessions, record `LARECOMP_VK_PROF`, `LARECOMP_FNPROF`, renderer statistics, CPU per thread, RAM/VRAM usage and GPU timestamps. Capture RenderDoc/Nsight only on the slow frames; continuous capture changes the pacing.
3. Compare cold/warm cache, SMAA on/off and 1080p/1440p/4K. Only then choose between optimizing shaders, descriptor sets, uploads, submits or synchronization.
4. Run the same scenario on Eevee and Pikachu; keep results from different machines separate.

There is no before/after FPS comparison of the fix because no paired game benchmark was run. The isolated test proves the **scale decision**; the new run verifies that the game started and presented the overworld without a logged reduction, but does not measure GPU time gain.
