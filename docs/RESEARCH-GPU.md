# Research: where the GPU spends the overworld frame

_2026-10-17/18. Eevee 1.0.0, RTX 3080, Route 1 route (`build/audit-save-20261017`, overworld standing still, ~600 draws per frame),
internal resolution 1920x1080. **Solved**: the GPU was reading the vertices over PCIe. Fix and numbers below; what is still
open is at the end._

## Summary

- The game drew the overworld with ~9-10 ms of GPU per frame, most of it in **4 draws** (instanced, vegetation) that
  produce almost no image.
- The cause was neither the shader nor the pixels: the vertices and indices were read **directly from the game memory imported into the GPU**
  (system memory), over PCIe, on every vertex shader invocation. Each instance has 8 streams of 16 bytes, reread for
  each of the draw's ~71 thousand vertices: ~9 MB per draw, ~0.8 ms at ~11 GB/s.
- On discrete cards the vertices and indices now go to VRAM (staging in system memory + a copy at the start of each
  submission). Ranges that the game itself writes via compute are still read in place. GPU: **10.2 -> 1.2 ms per frame
  (-88%)**; uncapped fps in the overworld **~75 -> ~90 (+19% at the median)**, because the CPU pays ~1.8 ms for the copies.
- On integrated cards (system memory is the GPU's memory) the old behavior was kept.

## A warning about the method

**The GPU time of the same frame varies from ~10 to ~18 ms from one run to another with the same code.** Comparing two
separate runs is only useful for large effects (2x or more). Several early conclusions of this research ("stacks in a
vector: -11%", "flattening the state machine: -12%") were noise and were disproved by interleaved tests. For smaller
effects, alternate the versions **within the same run** (`LARECOMP_EXP_ALT=1`: changed version only on odd frames,
a profile of 70 consecutive frames, compare the medians of even and odd) or interleave several A/B runs and use only overworld
frames (`[gpuprof]` header with 6 ms or more). The image is also not identical between two runs of the same mode
(~34 dB PSNR: the Pokémon walk around), so "equal" can only be measured against that noise. Tools: [DEBUGGING.md](DEBUGGING.md).

## How the cause was found

1. **The GPU frame is a single pass.** `LARECOMP_GPUPROF_AT`: main HDR 1920x1080 pass with 316 draws = 84% of the time;
   2048x2048 shadow map = 1.6 ms; all post-processing together, less than 1 ms.
2. **A group of 4 draws costs ~4.9 ms** (a third of the GPU): the same shader pair, 1,404 indices x 180 instances each. In the
   same run, even frames with them and odd frames without: median 14.58 versus 9.70 ms.
3. **The cost is per vertex, not per pixel.** 1x1 scissor: -0.27 ms (nothing). `LARECOMP_GPUPROF_STATS`: 252,720 input
   vertices, ~71,176 vertex shader invocations, 0 fragments and 0 primitives after clipping in 5 of 6 samples.
4. **It is not the shader's math.** A vertex shader that only reads the 11 attributes and sums everything costs the same as the full one
   (0.80 versus 0.81 ms per draw); without reading the attributes (return at the start of `main`) it drops to 0.09 ms. The driver
   (`LARECOMP_PIPE_STATS`, `VK_KHR_pipeline_executable_properties`) reports 48 registers, 10 KB of code and no
   spill: the compiled shader is small and efficient.
5. **The code had the reason**: `guest_direct` binds the game's imported heap as the vertex and index buffer, without a copy
   ("read in place", as on the Switch). The imported heap is system memory. The attribute layout of this program: 1 stream
   per vertex (24 bytes) and 8 streams per instance (`stride 16`, one `vec4` each).
6. **Decisive test**: `LARECOMP_VK_COPY_VERTICES=1` (copies to the ring): the same draw drops from 0.8 to 0.01 ms and the GPU
   frame from 9.2 to ~1 ms, with the same image.

## What was ruled out

| Hypothesis | Test | Result |
|---|---|---|
| 16x anisotropic filtering | `LARECOMP_ANISO=1` | same cost per index (4.8 versus 4.5 ns), between runs |
| Pixel fill | 1x1 scissor, same run | no effect |
| Reading the memory ring over PCIe | ring log | the constant ring is in GPU local memory; **it was the vertices that did not go through it** |
| `ssy/pbk/pcnt` stacks in local memory | `uvec4` instead of `uint[16]`, interleaved A/B | median 12.39 versus 12.88 ms; change reverted |
| `for + switch (pc)` state machine | shader converted by hand into blocks without a loop | 0.73 versus 0.83 ms per draw, within the noise |
| `robustBufferAccess` | `LARECOMP_NO_ROBUST=1` | inconclusive (drift between runs) |
| Shader ALU | GLSL and driver statistics | 427 instructions, no loop, no spill |

## The fix

`runtime/nvn/vulkan/exec.c`:

- **Two rings.** Ring 0 (as before) holds the constants and the texture upload sources. Ring 1 holds the draws' vertices and
  indices: a staging chunk in system memory where the CPU writes, and a chunk in VRAM from which the GPU reads; at the
  start of each submission (`ring_flush_gpu`) what was written since the previous one is copied through the upload buffer, which already runs
  before the main one with a barrier.
- **Ranges the GPU writes** (`gpuw`): this game uses compute to generate vertex data (a compute writes 16-byte
  buffers that two vertex programs read as streams). The storage buffers that a compute (or a vertex or
  fragment shader with `STG`) has bound go into a set of sorted intervals; vertices and indices inside them keep being
  read in place, when the GPU runs, and are never copied at draw time. If a shader writes to global memory **without** any
  storage buffer bound, the runtime reads everything in place from then on (warning in the log).
- **Policy**: `LARECOMP_VERTEX_READ=direct|copy|auto` (default `auto`: copy on a discrete card, in place on an integrated one).
- **Fewer bytes**: copy by start address (a draw that wants a shorter prefix reuses another's copy) and, for per-vertex
  streams, the whole bound buffer (up to 8 MB) the first time: 37.3 MB per frame became 16.2 MB. The scan for the largest
  index uses AVX2.

## Result (Eevee, RTX 3080, Route 1 overworld, interleaved runs)

| | direct | hybrid (default) |
|---|---:|---:|
| GPU per frame | 10.2 ms | 1.2 ms |
| uncapped fps (median of 11-13 windows) | 75.1 | 89.6 |
| extra CPU per frame (copies) | 0.04 ms | 1.8 ms |
| paced 60 fps: average / P99 | 60.0 fps / 17.4 ms | 60.0 fps / 17.3-18.6 ms |

With 2x internal resolution (the GPU starts to weigh in) the hybrid sustained ~79.7 fps versus 70.6-76.0 for direct. Uncapped fps
rises only a little on a 3080 because the CPU is the limit; the big gain is for those with a weaker GPU or slower PCIe, where those
~9 ms of reading over the bus become the bottleneck (the 9 ms figure applies to this machine; I did not measure another).

## What was tried to lower the CPU cost and did not stay

- Staging in cached memory and a vectorized index scan: no measurable effect (the cost is the volume copied).
- Windows **write watch** (`MEM_WRITE_WATCH`) to keep the meshes in VRAM between frames and only copy what the game
  wrote: the game writes only ~500-1,900 pages per frame (2-7 MB), but `GetWriteWatch` over the whole heap costs ~2.3 ms
  per call, and per range ~2.3 us (~300 ranges per frame: ~0.7 ms per submission). Small gain for the complexity and the
  risk; reverted.

## The chain between the game's threads

- Only **3 threads do work** (ids 80, 81, 82, priority 44), each at ~40% CPU (total ~1.15 cores); the other 7 stay
  100% idle on a condvar. Measured with `LARECOMP_THREAD_DUMP=16,0.25` and CPU per thread.
- When they "run", they are in `nvnQueueSubmitCommands` or `nvnQueuePresentTexture` (`LARECOMP_HLE_WHO` gives the names): the Vulkan
  backend executes the command list **inside the call, on the game's thread**.
- With the GPU at ~1 ms the uncapped fps is still ~90 (a ~11 ms cycle): **the limit is now the CPU chain between the three
  threads**, which take turns instead of overlapping. That is the next target.

## Next steps

1. **CPU chain between the threads**: move `backend_execute` off the game's thread (a render thread with a queue) and find
   out why the three take turns. Estimated ceiling: the cycle drops to that of the longest thread.
2. **Parallel copies** (workers) or a cross-frame cache with another way to detect writes: bring down the ~1.8 ms.
3. Measure on a weaker GPU and on an integrated card (the `auto` policy assumes an integrated card does not gain from copies).
