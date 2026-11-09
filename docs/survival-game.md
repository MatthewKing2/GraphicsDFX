# DEADLINE / RUNNER

A playable first-person survival prototype: choose routes, jump gaps/beams, wait
for green gates, and beat pursuit to the next checkpoint. The galaxy app remains
a separate executable. This is the software MVP, not proof of FPGA energy savings.

## Play

From the repository root on Windows:

```powershell
.\graphics\vulkan\play.bat
```

The launcher builds if needed and selects the correct working directory. Or:

```powershell
cd graphics/vulkan
make game
.\build\survival_game.exe
```

Uses the existing Vulkan SDK/GLFW/GLM/Clang setup. No new libraries or art downloads.
The Makefile retains the existing Windows SDK configuration; a Jetson-native build
is not implemented yet.

| Control | Action |
| --- | --- |
| WASD / mouse | Grounded movement / look |
| Space | Jump, with buffering and ledge forgiveness |
| R or Enter | Restart the entire course |
| Escape | Pause/resume; release/capture mouse |
| F1 | Switch CPU/GPU predictor |
| F2 | Disable/enable prediction and guidance |
| F3 | Show timing statistics |
| Close window | Quit |

Losing focus pauses; press Escape to resume. Death explains the cause and restarts
at the last checkpoint. Revisited checkpoints do not farm pursuit resets. The
finish screen waits for R/Enter. Orange is faster but has a larger gap and low beam
to jump. Blue is slower with a smaller gap and no beam. Gates are green/open,
amber/warning, and red/lethal. Stop before red gates; jumping cannot bypass them.

The test driver completes the three junctions and two checkpoints in roughly 49
seconds without deaths. Score is distance in decimetres + 1000 per checkpoint,
minus five points per elapsed second at completion. Death costs time.

Optional arguments, from `graphics/vulkan`:

```powershell
.\build\survival_game.exe --backend cpu --rollouts 512
.\build\survival_game.exe --fps 60
.\build\survival_game.exe --autoplay
.\build\survival_game.exe --help
```

Default rendering is capped at 60 FPS by sleeping. Physics runs at 120 Hz independently
of rendering. `--fps 0` removes the CPU cap, but driver presentation may still pace
frames. F3/logged frame time includes the cap. `--autoplay` is only a demo/test aid;
normal prediction never moves or jumps for the player.

## Code map

- [game_main.cpp](../graphics/vulkan/src/game_main.cpp): loop, persistent CPU worker,
  backend switching, request freshness, tests/benchmark entry points.
- [game.cpp](../graphics/vulkan/src/game.cpp): geometry, collisions, movement,
  hazards, checkpoints, score, and tiny cube-based bitmap HUD.
- [game_input.cpp](../graphics/vulkan/src/game_input.cpp): GLFW input and key edges.
- [renderer.hpp](../graphics/vulkan/src/renderer.hpp): rendering-only constructor
  and `drawScene(camera, instances)`; objects are not N-body particles.
- [prediction.hpp](../graphics/vulkan/src/prediction.hpp): explicit course/request/
  summary layouts; `makeCourse()` in `prediction.cpp` supplies shared course records.
- [prediction_kernel.inc](../graphics/vulkan/shaders/prediction_kernel.inc): one
  scalar integer algorithm compiled as C++ and GLSL.
- [predict.comp](../graphics/vulkan/shaders/predict.comp): one invocation per rollout,
  workgroup reduction, four aggregate atomics per group rather than per step.
- [gpu_prediction.cpp](../graphics/vulkan/src/gpu_prediction.cpp): resident course,
  device-local output, staged 32-byte result, timestamps, and fence polling.

The galaxy shaders are unchanged. A separate scene shader handles world instances
and HUD rectangles. The game's placeholder texture is generated white, with no
dependency on the Viking Room asset.

## Prediction semantics

Each immediate choice receives 512 rollouts over the same 8-second horizon, using
millimetres and 30 ticks/second. Early death/completion bounds work; completion is
absorbing success. Subsequent turns choose the shortest nominal-time route to the
next checkpoint, left on ties. Shared records define route geometry, speeds, gate
periods/phases, and checkpoint structure; meshes never enter the predictor.

Defaults model a novice: orange jump failure is 7/8/9% across sectors, blue is
4.5/5.5/6.5%, low-beam failure is half orange jump failure, and necessary braking
for a closed gate has a 15% failure chance once per encounter. Initial reaction
delay, slight speed variation, and +/-3 ticks of arrival/timing error are sampled.
These probabilities are explicit uncalibrated assumptions, not measured human data.

The real player never receives random failures; actual gate phases are deterministic
and telegraphed. Prediction is a corridor-progress approximation, not continuous
3D motion: exact strafing, jump arcs, box contacts, and human choices are omitted.
Candidate-route speed during approach is also approximate. Advice is a model
estimate, never a guarantee or an autopilot.

Unsigned xorshift32 uses hashed seed/route/rollout streams. Sector refreshes use a
stable seed to suppress resampling noise. Increasing counts preserves stream
prefixes; gameplay does not depend on prediction RNG.

Requests run at up to 5 Hz near junctions, one outstanding request at a time. The
loop polls without waiting, checks run/course/junction tags and simulation age,
expires guidance after 0.75 seconds, and switches an existing recommendation only
for a five-percentage-point advantage. Restart/backend changes invalidate guidance.
Normal interactive prediction does not use device-idle or blocking readback waits.

Graphics and compute share a GPU queue: nonblocking host polling does not guarantee
compute cannot delay rendering. F3 latency includes observation at the next render
poll, so can be a frame long despite a much shorter kernel.

## Validation and benchmarks

From the repository root:

```powershell
.\graphics\vulkan\tests\game-checks.ps1 -Benchmark -NBody
```

This builds, runs headless gameplay checks and exact CPU/GPU aggregate comparisons,
then performs accelerated rendered playthroughs of both backends with Vulkan
synchronization validation. Windows are hidden, not minimized. The GPU test captures
`graphics/vulkan/build/game-fork.bmp` for visual inspection.
Headless checks complete all eight route combinations without deaths. `-NBody`
also runs the original galaxy simulation's numerical regression suite.

Direct commands from `graphics/vulkan`:

```powershell
.\build\survival_game.exe --self-test
.\build\survival_game.exe --validate
make build/survival_benchmark.exe
.\build\survival_benchmark.exe --benchmark --output build/game-benchmark.csv
make test
```

The benchmark uses `-O3 -DNDEBUG`, three fixed decision states, 256/1024/4096 rollouts
per choice, three warmups and ten measured requests per case. CPU uses its persistent
worker; GPU uses the play backend. Polling yields until completion. There is no
concurrent interactive rendering in these prediction-only measurements.

CSV fields: submit-to-observation latency, CPU loop/GPU kernel time, rollouts and
actual executed steps/second, survival estimates, wire payload sizes, exact agreement.
A 32-seed near-tie stability experiment is printed separately. Render tests log
average/p95 frame time. Neither these timings nor throughput are energy measurements.
Debug `--capture build/image.bmp` intentionally blocks one frame and must not be
enabled for performance comparisons.

Validated limits: 16,384 rollouts per choice, 480 prediction ticks, three sectors,
six routes, speeds up to 500 mm/tick. uint32 progress/step aggregates fit these
bounds. Independent clear-route/deadline/full-course/phase tests supplement shared-
kernel CPU/GPU agreement. Test cases include partial workgroups and early exits.

## Laptop snapshot: 2026-11-09

Intel Core i7-12700H, Vulkan-selected NVIDIA GeForce RTX 3050 Ti Laptop GPU.
Optimized prediction-only benchmark; ten measured requests after three warmups.
Raw rows: [survival-game-results.csv](survival-game-results.csv). Laptop clocks,
driver scheduling, and background load affect timings; these are not Jetson results.

| Rollouts per choice | CPU end-to-end, state 0 | GPU end-to-end, state 0 | GPU kernel only |
| --- | --- | --- | --- |
| 256 | 0.299 ms | 0.247 ms | 0.068 ms |
| 1024 | 1.096 ms | 0.266 ms | 0.068 ms |
| 4096 | 4.326 ms | 0.284 ms | 0.080 ms |

State 0 is approach at -10 m with a 500-tick pursuit budget. State 1 is a deeper
hypothetical lane decision at +21 m, world tick 53; state 2 uses that progress at
world tick 106 with only 90 pursuit ticks left. State 2 terminates early: at 256
rollouts the CPU was slightly faster overall (0.113 vs 0.124 ms). Transfer/dispatch
overhead matters at small workloads, and a speedup is not an energy measurement.

CPU/GPU integer aggregates matched for all benchmark rows and 18 additional edge
requests. Both rendered backends completed the course without deaths, each applying
18 live predictions. Capped, validation-enabled accelerated playthroughs averaged
roughly 17.8–18.0 ms/frame with p95 around 18.4 ms (about 56 FPS under a 60 FPS cap,
including sleep/scheduler overhead). Earlier uncapped smoke runs were near 144 FPS.
These are automated rendering checks, not a human playtest or power measurements.
N-body regression tests and SPIR-V Vulkan 1.0 validation also passed.

For a synthetic 0.5-percentage-point near tie, the theoretically better left route
won 16/32, 22/32, and 24/32 sampled seeds at 256/1024/4096 rollouts. More work reduces
noise but does not make tiny distinctions compelling. Retain 512 per choice and
five-point recommendation hysteresis for the initial playtest rather than increasing
work merely to create an accelerator workload.

## Jetson + LiteFury: whole-project success

Both enjoyable gameplay and **measured lower whole-system power/energy** are project
acceptance requirements. The laptop MVP completes neither FPGA implementation nor
hardware energy validation, and claims no energy advantage.

The workload has narrow integer state, small lookups, branch-dependent paths, and
variable rollout lifetimes, without floating point, recursion, or external per-step
traffic. A small reusable FPGA rollout engine is plausible. This does not make the
CPU/GPU inherently inefficient: both may execute it well. No padded workloads or
deliberately weak baselines are acceptable.

LiteFury is an Artix-7 100T board with PCIe Gen2 x4 according to the
[vendor documentation](https://github.com/RHSResearchLLC/NiteFury-and-LiteFury).
The Orin Nano reference carrier provides an M.2 Key-M PCIe Gen3 x4 slot according to
[NVIDIA](https://docs.nvidia.com/jetson/orin-nano-devkit/user-guide/latest/hardware_layout.html).
Verify the actual hardware/link configuration; bandwidth and supply ratings are
not measurements of power consumption.

| Item | Current payload | Frequency |
| --- | --- | --- |
| Course | 288 bytes | Once per course/version |
| Request | 48 bytes | Up to 5/second near junctions |
| Result | 32 bytes total for both routes | Once per completed request |

At 5 Hz, steady-state payload is 400 bytes/second regardless of rollout count.
This excludes packet framing, DMA descriptors, interrupts/polling, and link power.
The GPU associates output with its retained request/fence; a future PCIe response
should echo ID/version/run tags in a small envelope, e.g. another 16 bytes. That
would be 480 payload bytes/second before other overhead. Never stream PRNG output
or individual rollout states over PCIe.

Keep the course resident and perform complete rollouts/aggregation on the FPGA.
Start with a few reusable engines, not an assumption of thousands of circuits.
RTL, synthesis/timing closure, DMA drivers, and actual FPGA transport remain future
work. This iteration does not implement or simulate FPGA power numbers.

Later measurement protocol:

1. Fix course/state/replay, render resolution/FPS, prediction cadence/horizon, and
   acceptable guidance quality. Include representative idle intervals and near ties.
2. Compare Jetson CPU, Jetson GPU, and Jetson + FPGA at the same complete-system
   power boundary. Include board/carrier/link power; do not count rails twice if
   Jetson input power already includes the M.2 card. Module telemetry may omit
   relevant carrier or board consumption.
3. Measure attached-but-idle FPGA/link cost and submission/readback overhead
   separately. This informative baseline does not replace Jetson-only comparison
   without the extra card cost. Count only results delivered before their deadline.
4. Report average watts, integrated joules for an identical replay interval, and
   joules/useful decision with repeated runs. Compare to the better reasonable
   Jetson backend, not an artificially weak implementation.
5. Accept improvement only if saved Jetson energy exceeds extra FPGA/link/host
   energy. If not, revisit genuinely useful model fidelity or idle/clock behavior,
   or report the hypothesis failed. Increase rollout count only when guidance
   quality measurably benefits.

At this small workload and intermittent update rate, FPGA/PCIe idle power may
outweigh saved CPU/GPU energy. This is a central experimental risk, not a result
that laptop timings can establish either way.

## Intentional rough edges

Handcrafted level, face-shaded cubes, no audio, basic box physics, no render
interpolation, tiny bitmap text, and an uncalibrated coarse predictor. The next
useful step is a human playtest and targeted tuning, not an engine rewrite.
