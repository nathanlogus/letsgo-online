# Diagnosis: Pokémon Let's Go Eevee (v1.0.0, cartridge)

Date: 2026-10-14 · Extracted with hactool 1.4.0 + the user's console `prod.keys` · Generator: `tools/gen` (from LASRecomp, without the Zelda patches)

## ExeFS modules

| Module | What it is | Image | Build id | Exports / imports |
|---|---|---|---|---|
| `main` | the game (Game Freak engine) | 30.9 MB (text 13.2 MB) | `0a3f0d21e67dad80` | 34 / 842 |
| `sdk` | nnSdk (Nintendo libraries, NVN included) | 12.2 MB | `eee08631aff2601e` | 22,986 / 13 |
| `subsdk0` | **glslc**: GLSL → Maxwell shader compiler | 14.2 MB | `7a7ca048269ec2e6` | 17 / 84 |
| `subsdk1` | **multimedia**: video player (`movie::`, based on Android Stagefright + FDK AAC) | 7.0 MB | `15c1c7f4f364010c` | 10,605 / 642 |
| `rtld` | dynamic loader | 16 KB | n/a | n/a |

- `main` fits comfortably before LASRecomp's `SDK_BASE` (64 MB).
- The LASRecomp runtime only loads `main` and `sdk`. **We need to extend the loader** to `subsdk0`/`subsdk1`, or implement what `main` uses from them directly on the host (HLE).

## ARM64 → C translator coverage

| Module | Functions | Unsupported instructions | Jump tables resolved | Time |
|---|---|---|---|---|
| `main` | 60,380 | **0** | 1,457 | 34 s |
| `sdk` | 31,906 | **0** | 467 | 7 s |
| `subsdk0` | 17,984 | **0** | 1,603 | 21 s |
| `subsdk1` | 13,342 | **0** | 430 | 4 s |

The LASRecomp lifter translated **100% of the code** of the four modules. The `rt_udf` present in the generated C are
traps for `udf`/padding instructions and function ends, not translation failures.

Generated C: ~550 MB in `generated/` (outside git).

## What `main` uses from each module

- **sdk**: 802 imports. Most of it runs as recompiled code of the sdk itself. What matters to us is
  what the sdk asks of the system (IPC and SVCs), which only shows up at run time.
- **glslc (subsdk0)**: 8 functions (`glslcInitialize`, `glslcCompile`, `glslcCompileSpecialized`,
  `glslcCompilePreSpecialized`, `glslcGetDefaultOptions`, `glslcSetAllocator`, `glslcFinalize`, `glslcGetVersion`).
  ⚠️ The game may **compile GLSL shaders at run time**. Zelda does not do this. There are two options:
  1. recompile `subsdk0` too (it already translates 100%) to produce Maxwell, which the runtime's shader translator already understands;
  2. HLE of glslc: compile the GLSL directly to SPIR-V with shaderc and skip Maxwell. Faster and more reliable, but requires changing the NVN layer.
- **multimedia (subsdk1)**: 28 `movie::` functions, **26 already implemented** in LASRecomp's `hle/movie.c`.
  Only `movie::MediaData::FindInt64` and `movie::SetCoreMask` are missing. Video should work almost for free.

## System libraries used by `main` (imports by namespace)

| Namespace | Imports | Status in LASRecomp |
|---|---|---|
| nn::os | 86 | ✅ kernel/SVCs |
| nn::hid | 80 | 🟡 partial (Npad). Let's Go uses Joy-Con with gyroscope and Poké Ball Plus |
| nn::socket | 40 | ❌ stub (no network) |
| nn::bluetooth | 30 | ❌ stub (Pokémon GO / Poké Ball Plus) |
| nn::ldn | 26 | ❌ stub (local communication) |
| nn::fontll / nn::pl | 24 + 4 | 🟡 system shared fonts |
| nn::audio | 22 | 🟡 audren exists, no effects |
| nn::bcat | 20 | ❌ stub (events/news) |
| nn::fs | 19 | ✅ |
| nn::account | 17 | ✅ acc:u0 |
| nn::oe | 16 | 🟡 am/applet |
| nn::time | 15 | ✅ |
| nn::err, swkbd, nifm, prepo, ssl, friends, ngc, nsd | ~45 | ❌ almost all stubs |

Most of the ❌ are network or online features, which can answer "unavailable" without breaking the single-player game.

## Conclusion

The picture is better than expected:
- **CPU translation: solved** (0 instructions missing).
- **Video: almost solved** (26/28 functions already exist).
- **Main risks**:
  1. glslc / run-time shaders;
  2. the NVN → Vulkan layer and the Maxwell shader translator with the Game Freak engine's shaders;
  3. many network services to stub;
  4. gyroscope controls, for the Poké Ball throwing moments.

## Next steps

1. Set up the build environment on Windows (MSYS2 UCRT64: clang, SDL3, FFmpeg, shaderc, Vulkan SDK).
2. Lean fork of the LASRecomp runtime in `src/`: remove Zelda's rando/QoL/UI and the BUILD_ID check;
   loader for 4 modules (`main`, `sdk`, `subsdk0`, `subsdk1`).
3. Compile everything and run. Read the `[hle] UNIMPLEMENTED` logs, unknown IPC services and crashes.
4. Goal: title screen.
