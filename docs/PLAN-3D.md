# Plan: fix the skinned 3D models

_Created on 15/10/2026. **Solved on 15/10/2026** (Phases 3 and 4, at the end). Full context in [HISTORY.md](HISTORY.md) (part 6) and tools in
[DEBUGGING.md](DEBUGGING.md)._

## The problem
Characters and Pokémon (skinned models) appear stretched into "spikes" that **converge on a point on the
character itself** (near the feet/center of the model). Recognizable parts of the model (eyes, cap) appear in the
right place. The **static scenery renders correctly** (bedroom, Pallet Town, sky), so camera, lighting,
textures and ordinary geometry work.

## Evidence

### What is already proven
| # | Evidence | How it was obtained | What it rules out |
|---|---|---|---|
| E1 | Static scenery correct in several scenes | user screenshots (bedroom, Pallet Town) | general camera, projection, depth or ordinary vertex format error |
| E2 | No unknown attribute format | log in `vertex_format` | attribute read with the wrong format |
| E3 | Skinned vertex layout: position RGB32F, normal RGBA16F, UV RG32F, **a11 = indices RGBA8 UINT**, **a12 = weights RGBA8 UNORM**, stride 44/48/56 | `LARECOMP_DEBUG_FRAME` / `LARECOMP_LOG_SKIN` | — |
| E4 | Vertices with consistent values: bones `25, 24`, weights `238 + 17 = 255` (100%) | dump of the first vertices | corrupted vertex data |
| E5 | Bone palette (`cb4`) valid: 131 or 107 4x4 matrices, rotations + translations, animating every frame, no NaN | palette statistics | bone matrices computed wrong by the CPU |
| E6 | Palettes always with a valid size (8,384 / 6,848 bytes) | `vcb` log | buffer bound without a size |
| E7 | The skinning shader reads the index from `a11` (declared `uvec4`, correct), multiplies by 64 and reads a 4x4 from `cb4` with `LDC.64` | reading the translated GLSL | wrong type declaration of the index |
| E8 | The `LDC.64` instructions checked are translated correctly (offset, buffer, size) | manual decoding | — (only for the instructions seen) |
| E9 | The shader uses `cb3` rows 4–7 (world matrix) and **8–11 (camera, view-projection)** for `gl_Position` | reading the GLSL | — |
| E10 | Per-stage programs and vertex-shader-only programs are already accepted; 0 draws skipped for "noobj" | `[skip: ...]` | character draws being skipped |

### What is suspicious but not proven
| # | Observation | Why it is suspicious |
|---|---|---|
| S1 | In the **only** dump taken (the 1st skinned draw, still in the intro), `cb3` rows 8–11 (camera) and 0–3 were **zeroed** | with a zeroed camera `gl_Position` comes out zero, as the harness showed. But it was too early: it may have been a frame with no camera set up |
| S2 | 3D scenes have ~360 compute dispatches every 2 s | the game may compute data (matrices, vertices) on the GPU; the renderer makes CPU copies before the GPU runs |
| S3 | The convergence point is **on the character** | vertices with all bones zeroed fall at the model's origin, which the world matrix carries to the character's position. Points to bones read as zero on the GPU, even though the palette is correct in memory |

### Inconclusive experiments (redo with verification)
| Experiment | Result | Why it is inconclusive |
|---|---|---|
| `LARECOMP_VK_UBO_DIRECT=1` (uniforms straight from memory) | no change | no counter: it is unknown whether the alignment condition let it apply to `cb3`/`cb4` |
| `LARECOMP_VK_COPY_VERTICES=1` | no change | same, but E4 already shows the vertices are correct |
| CPU harness | `gl_Position = 0` | ran with the S1 dump (zeroed camera); needs a gameplay dump |

## Hypotheses (from most to least likely)
| H | Hypothesis | Explains | Decisive test |
|---|---|---|---|
| H1 | On the GPU, the bones arrive **zeroed or partial** (the `cb4` binding does not cover the palette, or it is read from another place/time) | S3 (convergence on the character), E5 (memory correct) | X2 + X4 |
| H2 | The **camera (`cb3` 8–11) arrives zeroed or stale** on the GPU (filled by compute, `UpdateUniformBuffer` or after the copy) | S1, S2 | X1 + X3 |
| H3 | **Translation error** in some instruction on the skinning path (weighted sum, weight conversion, etc.) | correct parts and wrong parts in the same model | X5 |
| H4 | The bones come from **another source** in some shaders (there are variants: `cb4`, `cb9`, one shader by `gl_VertexIndex`) | different models may fail in different ways | X6 |

## Experiments (in order of cost)

### Phase 0: deterministic reproduction (prerequisite)
- [ ] Test script that loads the **save** and gets into gameplay on its own (`LARECOMP_INPUT` with A on the title screen),
      captures fixed frames and records the log. Goal: the same scene, every time, in ~1 minute.
- [ ] Pin the capture by **frame**, not by draw order (the "n-th draw" dump changed scene between runs).

### Phase 1: cheap binary cuts (each one answers yes/no)
- [ ] **X1. Camera in gameplay:** dump the `cb3` of a character in the bedroom. Are rows 8–11 filled? Who writes them?
      (log of `UpdateUniformBuffer` at the address and a check of whether any compute writes there).
- [ ] **X2. Identity palette:** in the `cb4` copy, replace all matrices with identity. If the characters
      appear correct in the default pose (T-pose), the world/camera path is correct and the problem is the palette on the GPU (H1).
      If they stay exploded, the problem is in `cb3`/camera (H2) or in the translation (H3).
- [ ] **X3. Redo `UBO_DIRECT` with a counter:** count how many uniform binds became direct reads. If it applies to
      `cb3`/`cb4` and the result changes, the data is written by the GPU after the copy (H2/S2).
- [ ] **X4. A single bone:** inject into the GLSL a 100% weight on the first bone (`#define` over `a12`). If the model stays
      whole (rigid, but in place), the problem is in the secondary bones or the weighted sum.

### Phase 2: CPU harness with gameplay data
- [ ] **X5.** `LARECOMP_DUMP_SKIN` + `make_harness.py` on a character draw from gameplay. Compare the `gl_Position`
      of the translated shader with the reference (skinning + `cb3` 4–7 + 8–11):
      - **equal and consistent:** the translation is correct; the error is in the data path to the GPU (H1/H2).
      - **different:** translation error (H3). Bisection: print intermediate registers and find the first
        instruction that diverges from the reference.

### Phase 3: see what the GPU actually receives
- [ ] **RenderDoc** (free): capture a frame and, on the character draw, see the uniforms as the GPU sees them and the
      *mesh viewer* with the vertex shader output per vertex. Answers H1/H2 directly.
- [ ] **X6.** List all skinned shader variants (`cb4`, `cb9`, by vertex index) and repeat
      X5 on one of each.

### Phase 4: fix and lock in
- [ ] Apply the fix according to the cause:
      - **H1/H2 (data):** read these uniforms straight from memory at the right time, or synchronize the copy with the compute.
      - **H3 (translation):** fix the instruction in `runtime/nvn/maxwell/translate.c` and create a test
        (isolated instruction in the harness).
- [ ] Visual regression: three fixed scenes (bedroom, Pallet Town, battle) with reference captures, compared on every
      renderer change (idea from ssbu-recomp's deterministic mode).
- [ ] Record it in [HISTORY.md](HISTORY.md) and update [PROGRESS.md](PROGRESS.md).

## Plan B (if the NVN layer is not enough)
HWDER recompiles the game's **own NVN driver** (which is already in our translated `sdk`) and implements a
**Maxwell command processor**. In that model there are no assumptions about the NVN API, only about the GPU. It is a
large architectural change: it is only worth it if phases 1–3 show several API problems that cannot be fixed
one by one. Until then, it stays as a reference only (HWDER's license is not declared: study, do not copy).

## References
| Source | What for |
|---|---|
| `runtime/nvn/vulkan/exec.c` (`cbuf_info`, `draw_`, `bind_program_stages`) | where uniforms, vertices and programs reach Vulkan |
| `runtime/nvn/maxwell/translate.c` | Maxwell → GLSL translator |
| `tools/skintest/` | CPU shader harness |
| [HWDER](https://github.com/sonsegajp/HWDER): `runtime/src/shader/`, `runtime/src/gpu/gpu.cpp`, `texture_cache.cpp` | plan B; SASS → SPIR-V translator to compare instruction semantics |
| [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp): `runtime/mx_translate.c`, `nvn_vk*.c`, `tests/DEBUG_MODE.md` | the same layer as ours (NVN → Vulkan); deterministic mode with a per-frame hash |
| RenderDoc | inspecting what the GPU receives on each draw |
| Switchbrew / envytools | reference for Maxwell instructions and NVN |

## Definition of done
Characters and Pokémon rendered correctly in the bedroom, in Pallet Town and in a battle, with no regression in the scenery,
the video or the interface, and the problem documented in the history.

---

## Results (15/10/2026)

### Phase 0: deterministic reproduction ✅
`tools/repro.sh <nome> <frames> [VAR=valor ...]` loads a copy of the save (`build/test-save`), ignores the real controller
(`LARECOMP_NO_USER_INPUT` now also ignores controller *polling*) and reaches gameplay (Route 1) in ~1150 frames.
Each test takes ~55 s. Capture mosaic: `tools/contact_sheet.py`.

### Phase 1 and beyond
| Exp. | What changes on the GPU | Result | Conclusion |
|---|---|---|---|
| X1 | dump of `cb3` in gameplay | camera filled, same before/after the GPU | **H2 ruled out** (camera ok) |
| X2 | palette = identity | models whole | world → camera → screen path ok; sum of weights ≈ 1 |
| X3 | uniforms read straight from memory (27,246 times) | still exploded | not the copy timing (**H1 by timing ruled out**) |
| X4 | 100% weight on the 1st bone | models whole | 1st index (`a11.x`) and 1st matrix ok |
| X5 | translated shader on the **CPU** with real data | real weights ≈ one bone (max. diff. 0.002) | **translation correct (H3 ruled out)** |
| X6 | all slots with `a11.x`, real weights | exploded | (see X14: other shaders use other attributes) |
| X8 | sum of weights on the 1st bone | models whole | weights sum to 1 on the GPU |
| X9 | `w.x` on the 1st, `1−w.x` on the 2nd bone | exploded | the 2nd bone is wrong |
| X11 | shaderc with optimization | no change | it is not the shader compiler |
| X13 | 100% weight on the **2nd** bone (real index `a11.y`) | **very exploded** | the 2nd index is wrong on the GPU |
| X14 | 50/50 between the 1st and 2nd slot, **both with `a11.x`** | **models correct** | the 2nd slot's math is correct; the problem is the **value of `a11.y` on the GPU** |

### Current conclusion
The shader and the in-memory data are correct. **The `y` component (and probably `z`/`w`) of the bone indices
(`a11`, RGBA8 UINT format) arrives wrong on the GPU**, while `x` arrives correct. The cause is in the vertex attribute's
path to Vulkan (format, offset, stride or buffer). Next step: **RenderDoc** (Phase 3) to see the values
of `a11` per vertex as the GPU reads them and compare them with memory.

> **Correction (15/10/2026, later):** the conclusion above was wrong. See Phase 3 below.

### Phase 3: RenderDoc ✅ (cause found and fixed)
- Capture through the runtime: `RENDERDOC=1 tools/repro.sh rdc 1310 LARECOMP_RENDERDOC_AT=1290` (several frames in a row:
  the game does not draw the whole scene on every present). Analysis with `tools/rdc_skin.py` and `tools/rdc_draw.py` inside
  `qrenderdoc` (details in [DEBUGGING.md](DEBUGGING.md)).
- **The vertex bytes the GPU reads are identical to those in memory**, including `a11` (e.g. `[7, 6]`) and the weights. The
  bound format is `R8G8B8A8_UINT`, as it should be.
- The bad vertices do not "collapse" to a point: they come out with **`w` near 0 or negative**. In draw 4746 (bones `7, 6`) the
  `w` was proportional to the 1st bone's weight (weight 8 → `w` 9; 128 → 466; 255 → 950): the 2nd bone's contribution was zero.
- Running the translated shader **on the CPU with the exact GPU data** (vertices, `cb3` and `cb4` exported from the capture)
  reproduced the GPU's output number for number. So the defect was in the GLSL itself.
- Cause: `LDC.64 R4, c[4][R4+0x38]` translated as `r4 = cb4[r4+56]; r5 = cb4[r4+60]`. The second read used the
  already overwritten R4, and the `w` of the 2nd bone's translation (1.0) became zero or garbage.
- Why the Phase 1 cuts were misleading: the 1st slot loads the matrix into `r0`/`r1` with the address in `r38` (no
  conflict), so "only the 1st bone" worked. X5 used vertices where the wrong read happened to land on a
  harmless value. X14 was left without a complete explanation.

### Phase 4: fix ✅
`runtime/nvn/maxwell/translate.c`: when the address register of a multi-word `LDC` is also a destination, it is
copied to a temporary before the writes. Validated with `tools/repro.sh` with no experiment enabled:
the player character, Eevee and NPCs whole, with the real bone blend. The `jogar.bat` workaround
(`LARECOMP_VK_SKIN_ONEBONE=1`) was removed.
