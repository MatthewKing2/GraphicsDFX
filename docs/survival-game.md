# VOID / DIVE

The accelerating tunnel iteration replaces the walking corridor MVP. The v1 code
remains in Git (`113835c`); `survival-game-results.csv` is historical v1 data.
The N-body app is unchanged.

## Play

From the repository root:

```powershell
.\graphics\vulkan\play.bat
```

Or from `graphics/vulkan`: `make game`, then `.\build\survival_game.exe`.

- Space / Enter: start or retry. R: immediate new run.
- A/D or horizontal mouse movement: steer sideways.
- No walking, braking, jumping, or free-look. Forward motion is automatic.
- Escape: pause and release mouse; Escape resumes. Close the window to quit.
- F1: CPU/GPU prediction; F2: signs/prediction off/on; F3: console/window statistics.

Forks arrive every **2.4 seconds**. Speed starts at 18 units/second, then rises by
1.2 units/second every second without a forward-speed cap. The first fork has two
openings; later forks randomly have 2-4. Enter a tunnel, avoid its hazards, merge
back into the shaft, then choose again. Collision kills immediately.

No gameplay HUD, crosshair, or fixed probability panels. Percentages float in world
space above the upcoming openings. Green outlines mark the preferred estimate;
ellipses indicate no fresh result. There are only title, pause and death screens.
The camera looks down an octagonal shaft, with banking, dark faceted walls, voxel
text, and sparse speed streaks.

Red spikes are triangular prisms. Rectangles slide, spin CW/CCW, or reverse
direction. Crusher edges narrow a passage. Collision is checked throughout each
obstacle's visible forward depth, not just at one central plane. That depth spans
a 0.1-second contact window as speed increases.

At least one route has a viable center line. Others can be genuinely impossible:
their aperture is smaller than the player. Such probabilities are computed from
collision, not manually forced to zero. There are no checkpoints or finish line;
survival time is the score.

Normal launches choose a fresh course seed; retries repeat it for practice.
The 32-section layout loops after 76.8 seconds while forward speed keeps rising.
This is intentionally not unlimited unique procedural content.

Useful options from `graphics/vulkan`:

```powershell
.\build\survival_game.exe --seed 12345 --backend gpu
.\build\survival_game.exe --backend cpu --rollouts 512
.\build\survival_game.exe --autoplay
.\build\survival_game.exe --fps 0
.\build\survival_game.exe --help
```

Default cap is 60 FPS with sleeping, not spinning. On Windows a high-resolution
waitable timer avoids coarse sleep rounding without changing the global timer
resolution. Physics runs at 120 Hz. The demo/test driver is never active in normal
play. The build retains the repository's Windows SDK configuration; a Jetson-native
build is not implemented yet.

## What the probability means

"Given the incoming lateral position/velocity, how often does our delayed/noisy
steering controller survive through this tunnel's exit?"

All choices use the **same section exit**. This versioned model replaces the old
eight-second jump-failure model. When signs are shown from the previous tunnel,
prediction is conditional on reaching the next fork: the merge offset/velocity is
projected forward. It does not promise survival through the currently selected
tunnel as well.

Each rollout stores lateral position/velocity, reaction countdown, obstacle index,
PRNG state, and alive/dead state. At 60 Hz it:

1. Advances delayed steering with acceleration and a lateral speed limit.
2. Checks nine candidate lateral targets against the next obstacle's arrival and
   contact-window endpoints, choosing a nearby clear target.
3. Samples bounded reaction delay, target noise, and arrival-timing uncertainty.
4. Tests entrance, wall and moving obstacle boundaries at each contact tick.
5. Stops on collision or counts survival at the common exit.

There is no independent random "die" coin. Real gameplay receives no random
steering errors or invisible failures. Controller assumptions are uncalibrated,
not measured human skill. Prediction is 60 Hz and quantized; actual play is 120 Hz.
Nine sampled targets are an approximation, particularly at narrow moving edges.

The same Q10 motion/collision functions drive graphical obstacle poses, real
collisions, C++ rollouts and GLSL rollouts. RNG streams are reproducible, separate
from course generation, and prefix-stable when rollout counts change.

Default: 512 rollouts per opening, up to four openings, at most 10 updates/second.
Three-point recommendation hysteresis limits flicker. Results expire and are
checked against run, section, course version and simulation age. Only one request
is pending; the render loop never waits for prediction readback.

## What the FPGA would actually compute

It would execute the **whole rollout-and-aggregation engine**, not render meshes,
run floating-point rigid-body physics, or stream random numbers back to the host.

| Hazard | Shared math | Small hardware building blocks |
| --- | --- | --- |
| Triangular spike | Triangular boundary with player-radius margin | Subtract, absolute value, constant scale, compare |
| Sliding block | Phase-counter position and moving interval overlap | Counter/mask, add/shift, absolute value, compare |
| Rotating blade | Q10 sine/cosine and projected rectangle tests | Tiny ROM, narrow multiplies/adds, compares |
| Contracting passage | Time-varying aperture must fit position + radius | Phase counter, triangular wave, scale, compare |

Periods are powers of two. Rotation uses 32 fixed-point directions, shared with
rendering; reversals reflect phase. No floating-point trigonometry is needed in
the predictor. Useful prospective widths include 14 signed bits for shaft position,
9 for velocity, 7-9 for motion phase, and 32 for PRNG. Begin with a few reusable
engines and local tables, not thousands of fully unrolled simulations. Actual
resource usage and clock rate require synthesis and timing closure.

Different rollouts update controllers at different times, choose different
targets, encounter different hazards, and die at different steps. This can cause
CPU branch misprediction and GPU divergence/inactive lanes. An FPGA could use
narrow private state machines, interleave contexts through a small pipeline, and
stop updating dead contexts rather than running a wide SIMD group.

**This is a plausible energy hypothesis, not an inherent CPU/GPU disadvantage or
an energy result.** CPUs/GPUs can handle these operations well. Shared poses can
be cached or precomputed by any backend; equivalent optimizations remain allowed.
Do not add pathological math or inflate rollouts merely to manufacture a win.

LiteFury is an Artix-7 100T board with PCIe Gen2 x4 according to
[RHS Research](https://github.com/RHSResearchLLC/NiteFury-and-LiteFury).
No RTL, driver, FPGA resource/timing result, or measured FPGA power is included.

## Memory, PCIe and the success criterion

- Course: **10,240 bytes**, uploaded once and resident (32 sections x four slots).
- Request: **48 bytes**, including IDs, section, incoming state, clock and seed.
- Result payload: **64 bytes**, four summaries; unused choice slots stay zero.

At 10 Hz this is **1,120 payload bytes/second**, independent of rollout count.
A future 16-byte response ID/version/run envelope would make it 1,280 bytes/second.
These are payload calculations, not measured link utilization or power. Include
framing, DMA descriptors, interrupts/polling, link transitions, host work and board
idle power in the eventual experiment. No per-tick or per-rollout PCIe traffic.

The overall project still requires both enjoyable gameplay **and measured lower
whole-system energy** than the better reasonable Jetson CPU/GPU baseline. Compare
the same course/replay, model, quality, update rate, deadlines, resolution and FPS.
Measure total system joules/watts, including board/carrier/link and idle periods,
without double-counting rails. An attached-idle baseline is informative but does
not replace comparison to a Jetson without the extra board cost. A cheap CPU/GPU
job may save less energy than an always-on FPGA/link consumes. Laptop timing
cannot establish hardware success.

## Code and tests

- `game.cpp`: motion, tunnel geometry, collisions, world signs and menus.
- `prediction.hpp/.cpp`: versioned layouts and seeded compact course.
- `shaders/tunnel_math.inc`: shared motion, collision and safe-target selection.
- `shaders/prediction_kernel.inc`: shared scalar C++/GLSL rollout.
- `shaders/predict.comp`: four-choice dispatch and workgroup aggregation.
- `game_main.cpp`: CPU worker, fence-polled GPU results, freshness and frame pacing.

From the repository root:

```powershell
.\graphics\vulkan\tests\game-checks.ps1 -Benchmark -NBody
```

Checks cover 20 consecutive viable forks, acceleration/no braking, 2/3/4 choices,
all obstacle kinds, impossible routes, deterministic streams, stale results, and
48 exact CPU/GPU comparisons including partial workgroups and course wrapping.
Rendered tests verify live guidance. Screenshots: `build/game-fork.bmp`,
`build/game-fourway.bmp`, and `build/game-title.bmp`.

The optimized benchmark fixes seed 12345 and tests three sections at 256/512/1024
rollouts per choice, two warmups and six measured requests per case. Output:
`graphics/vulkan/build/tunnel-benchmark.csv`. It excludes concurrent rendering;
interactive latency also includes observing completion on a later frame.
Latency and throughput are **not energy measurements**. Debug screenshots block
one frame and must be excluded from performance comparisons.

Intentional rough edges: no audio, repeated 32-section course, flat geometry and
a coarse, uncalibrated steering model. Next: playtest steering, decision timing,
and obstacle readability rather than build a bigger engine.

## Laptop snapshot (2026-11-09)

Intel i7-12700H / Vulkan-selected RTX 3050 Ti Laptop GPU. Optimized prediction-only
requests, seed 12345; six measured samples after two warmups. Raw data:
[tunnel-game-results.csv](tunnel-game-results.csv). Not Jetson or energy results.

| Section / openings | Rollouts per opening | CPU request latency | GPU request latency | GPU kernel |
| --- | --- | --- | --- | --- |
| 0 / 2 | 512 | 1.822 ms | 0.667 ms | 0.528 ms |
| 1 / 4 | 512 | 3.429 ms | 0.752 ms | 0.531 ms |
| 4 / 4 | 512 | 3.659 ms | 0.691 ms | 0.543 ms |

All aggregates matched exactly. Rendering tests averaged about 17.1 ms/frame with
p95 about 17.4 ms (~58 FPS under the 60 FPS cap, including the timer wait), with
Vulkan synchronization validation enabled. Both backends passed 20 consecutive
forks and applied 100 live predictions in the accelerated test. The hazard set,
course wrap, impossible paths, screenshot views and original N-body regressions
also passed. Actual human play and hardware power remain to be tested.
