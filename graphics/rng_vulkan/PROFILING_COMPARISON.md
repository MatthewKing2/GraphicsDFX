# N-body and RNG profiling comparison

Reviewed after fetching `origin` on 1 November 2026. Matthew's latest `matthew`
branch was `0e3917d6b92e52d8e1f0936d8887ea881cbd70c3` (GPU N-body). This RNG
integration is based on that commit and leaves the N-body renderer unchanged.

## Outcome

The core Vulkan environment and GPU timing mechanism are consistent. Both use
GLFW with no OpenGL context, a graphics/compute queue, storage buffers, explicit
compute-to-graphics dependencies, Vulkan timestamp queries, timestamp-period and
valid-bit conversion, and fence-protected query readback. Both index presentation
completion semaphores by swapchain image.

The differences below reflect the workloads and measurement goals. They do not
require replacing the validated RNG kernels or adopting N-body's frame scheduler.
The timing scopes must remain explicit when comparing results.

| Area | Matthew N-body | RNG experiments |
| --- | --- | --- |
| Frame scheduling | Two frames in flight; reads GPU queries after reusing a completed frame slot | Serialized submit/fence wait, useful for attributing FPGA input and upload latency |
| GPU scopes | Gravity compute and sphere rendering | RNG or upload, simulation, visualization preparation, offscreen drawing; isolated RNG/consumer submissions |
| CPU measurement | Events, camera, matrices, recording, waits, submission, and presentation; pipelined loop averages | `cpu_ms` measures serialized frame work inside `Gpu::frame`, including external input/staging and submission/wait; excludes event polling, CSV output, and title updates |
| Reporting | Compile-time switch, 60 warmup frames, 240-frame windows, GPU averages and CPU average/p95 | Runtime sizes/warmup/frame count, per-frame CSV, median/p95 summaries and repeated process trials |
| Default seed | 12345 when profiling | 42, configurable through `--seed` |
| Compute state | Ping-pong snapshots for all-pairs gravity | Independent Monte Carlo samples and independent walkers; in-place walker updates are safe |
| Rendering | Textured sphere meshes, model matrices and depth testing | Bounded histogram/point preparation and one fullscreen triangle |
| Build/platform | Existing Windows/MSYS Makefile | Portable CMake, GLFW 3.3+, glslc/glslangValidator, Linux/Jetson and macOS portability support |

## Comparison rules

- Use Release builds with validation disabled for performance; run correctness
  and synchronization validation separately.
- Match hardware, driver, power/clocks, workload size, seed policy, warmup, and
  sample counts. RNG accepts `--seed 12345 --warmup 60 --frames 240` when matching
  Matthew's measurement schedule is useful. The algorithms still perform
  different work and produce different render output.
- Compare GPU scope timings and useful-work throughput. Do not equate N-body's
  pipelined CPU loop FPS with RNG's serialized `cpu_ms`, and do not add GPU time
  to overlapping CPU measurements.
- N-body's first timestamp precedes its simulation dependency. RNG's first
  timestamp follows its initial global barrier. Neither represents pure shader
  instruction cost. RNG-generation timings also include random-buffer writes;
  consumer timings include random-buffer reads.
- Use RNG `--headless` profiles for quantitative phase comparisons. In interactive
  mode, acquire-semaphore waits can affect measured GPU phases: N-body waits at
  color attachment output; RNG waits at transfer, and its measured work includes
  transfer operations. The presentation copy/readbacks are outside RNG scopes,
  but this alone does not guarantee that all WSI waiting is excluded.
- Keep timestamp methods separate. The Apple RNG path adds small blit sentinels
  for MoltenVK encoder-boundary sampling; phase intervals remain approximate.
  Isolated submission envelopes provide the stronger RNG/consumer cross-check.
- Keep the fused RNG implementation as the GPU acceleration baseline. A large
  split RNG share does not prove the fused workload is RNG-bound or predict FPGA
  speedup. Include PCIe input, staging, upload, and full frame latency.

The older `828940a` commit's "99% CPU gravity" result belongs to a CPU-gravity
checkpoint. At `0e3917d`, gravity and model-matrix generation run on the GPU; that
older result is not a measurement of the current N-body compute shader.

## Build scope

This branch adds the self-contained RNG CMake project alongside Matthew's
existing N-body sources. The existing N-body Makefile is Windows-specific and
references a missing `tests/nbody_test.cpp`; its test target cannot be treated as
a passing clean-checkout test. This comparison does not claim that N-body has
been ported to macOS or that its GPU statistics were reproduced here.

The RNG project is rebuilt from a fresh build directory and its CPU, shader,
GPU-reference, and external-stream checks are verified before publication. The
[retained development measurements](measurements/apple-m5-2026-10-28/README.md)
remain Apple M5 results; no Jetson or physical FPGA measurements are claimed.
