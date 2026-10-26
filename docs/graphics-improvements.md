# Graphics audit

_2026-10-17. Review of the code and of the captures/measurements already documented in `docs/HISTORY.md`; no new RenderDoc capture in this round._

> **2026-10-17 review (later validation):** this audit's test ini (`build/audit-dynamic.ini`) was written with CRLF, and the `larecomp.ini` reader only stripped the CR from `mod` and `game_lang`: `resolution = dynamic` became "Automatic" and the in-game runs described below **did not have dynamic resolution on** (the old code itself also does not log `[dynres]` that way, even forced to 20 fps). Redone with an LF ini, same Eevee 1.0.0 route at 30 fps: old code 1.00 → 0.60; new code between 0.95 and 1.00 (it only reacts to loading stalls); 60 mode in the overworld at 60.49 fps average. The reader now strips CR/LF from every line (HISTORY 56). Revised priority: **P3**, because dynamic resolution does not appear in the launcher (only `resolution = dynamic` edited by hand).

## Current pipeline

NVN commands are translated in `runtime/nvn/cmdbuf.c` and executed in `runtime/nvn/vulkan/exec.c`. `runtime/nvn/vulkan/textures.c` sizes scalable render targets and keeps CPU-filled textures at their original size. `runtime/nvn/output_scale.c` computes an internal scale separate from the presentation rectangle. The backend applies viewport and render targets at the computed scale; presentation fills the window rectangle. So the path implements **upscaled internal rendering**, not just upscaling of the final image. The earlier evidence of passes and dimensions is in `docs/HISTORY.md`, part 8; visual correctness at 4K still needs current captures.

`runtime/nvn/vulkan/exec.c:post_view` applies SMAA 1x to the game image in three passes (edges, weights, blend). FSR 1 EASU/RCAS is used only when the image needs to be upscaled to the window. Downscaled presentation uses multiple samples; mipmapped textures with linear filtering can get 16× anisotropy (`textures.c`). The Vulkan pipeline cache is saved periodically (`device.c`, `exec.c`).

## Fixed problem

**P3 — Dynamic resolution lowered the scale at the nominal 30 fps rate.** The 17.4 ms comparison in `output_scale.c` reflected 60 Hz, but the game normally presents every ~33.3 ms (`runtime/nvn/objects.c`). Over 180 synthetic samples, the scale went from 1.00 to 0.70. Now the budget follows 30/60 Hz, and a scene change discards the previous sample window. The isolated test measured scale 1.00 at 33.33 ms and 0.70 at 45 ms. A scripted Eevee run showed the overworld at 1280×720 after 1450 presents at 30 fps, with no `[dynres]` line. Pikachu, transitions and a paired visual comparison with the previous version are still missing.

## Limitations and risks

- Vulkan dynamic resolution passes `gpu_ms = -1` in `device.c`; it decides by the time between presents. Drops caused by CPU, shader waits or loading can cause an unwarranted reduction. The heuristic already ignores pauses above 250 ms, but that does not separate every GPU hitch. **P2**, measure with GPU timestamps before changing.
- The experimental 60 mode is not uniform: overworld at 60, menus with an intermediate frame from 30 logic, battle/capture at 30. Dynamic resolution should be inspected at each transition. **P2**.
- Higher shadow resolution, TAA, LOD and render target precision were not adopted: visual comparison and GPU cost are missing. The 2048² shadow map and the risk of scaling targets that are not 16:9 are recorded in `docs/HISTORY.md`.
- SMAA affects the already composited image, including the interface. Evaluate UI edges and fine textures with capture pairs before switching to FXAA/TAA. TAA depends on motion vectors and handling of cuts/ghosting that this pipeline does not offer today.

## Recommended validation

Capture `LARECOMP_SHOT_AT` and `LARECOMP_WINDOW_SHOT` at 1080p, 1440p and 4K with the same camera. Check pixels and dimensions of the HDR RT, depth, bloom and UI in `LARECOMP_DEBUG_FRAME`/RenderDoc; compare with `LARECOMP_NO_SCALE=1`. Inspect viewport/scissor, shadow alignment and post-processing. Repeat with SMAA on/off and anisotropy 1×/16× and measure the cost with GPU timestamps. Run on Eevee and Pikachu before release.
