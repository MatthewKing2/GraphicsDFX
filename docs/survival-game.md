# Current game: REACTOR / RUN

The reactor iteration is now implemented. See [reactor-escape.md](reactor-escape.md)
for the current mechanics, controls, coupled-relay computation, profiling commands,
compact link contract, and explicitly unsynthesized LiteFury resource budget.
The material below is the historical tunnel-only version, not the current
predictor, pulse controls, memory sizes or benchmark results.

---

# Historical VOID / DIVE

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
- W/S or vertical mouse movement: steer up/down (mouse up means up).
- No walking, braking, jumping, or free-look. Forward motion is automatic.
- Escape: pause and release mouse; Escape resumes. Close the window to quit.
- F1: CPU/GPU prediction; F2: signs/prediction off/on; F3: console/window statistics.

Tunnel mouths are now **21 units across**, three times the previous width, with
obstacles and steering speed scaled to match. Movement is in the full horizontal/
vertical cross-section, with octagonal wall collision. Rotating blades can be
passed above/below; crushers have four visible edges, not invisible vertical walls.

Forward speed now starts at **27 units/second** (1.5x the previous start) and gains
**9 units/second every second** (previously 1.2), reaching **567 at 60 seconds**.
The first section still gives six seconds to compare openings and a roughly
4.3-second passage. Reaction windows then tighten: total sections shrink from
11 to 2.2 seconds, and the entrance window drops to 0.3 seconds around one minute.
Simply increasing speed previously stretched the geometry without shortening
reaction time; this authored pacing ramp makes the late game genuinely difficult.
There is no forced 60-second death timer; survival still depends on input/course.
Early passages have four hazards; this grows to ten. The first fork has two
openings; later forks have 2-4.
Collision kills immediately. Both steering axes preserve position, mouse targets
and velocity at exits: no automatic drift, snap, or recentering.

Passage walls are warm plum, the open shaft is cool blue, and an amber rim marks
each exit. This keeps the dark low-poly style while making the transition visible.
Eight solid triangular-prism fillers per mouth close the space between its square
bulkhead cutout and the actual octagonal tunnel. No see-through square corners.

The first sections have small blocks, slow blades, and wide crusher gaps. Over
the first twelve sections, obstacles grow, gaps narrow and motion speeds up.
Impossible openings start only after the opening three sections. This is an
authored difficulty/timing ramp alongside acceleration, not a claim of constant
physical tunnel length. Physical lengths follow the accelerated distance curve.

No gameplay HUD, crosshair, or fixed probability panels. Percentages float in world
space above the upcoming openings. Green outlines mark the preferred estimate;
ellipses indicate no fresh result. There are only title, pause and death screens.
The camera looks down an octagonal shaft, with banking, dark faceted walls, voxel
text, and sparse speed streaks.

Red spikes are triangular prisms. Rectangles slide, spin CW/CCW, or reverse
direction. Crusher edges narrow a passage. Collision is checked throughout each
obstacle's visible forward depth, not just at one central plane. That depth spans
a 0.1-second contact window initially, shrinking to about 0.033 seconds as hazards
pack closer together. Rendering and both predictors share the same contact ticks.

At least one route has a clear center line, but reaching it in time is not
guaranteed late in a run. Others can be genuinely impossible:
their aperture is smaller than the player. Such probabilities are computed from
collision, not manually forced to zero. There are no checkpoints or finish line;
survival time is the score.

Normal launches choose a fresh course seed; retries repeat it for practice.
The 32-section obstacle templates wrap; the section-length ramp does not reset.
Forward speed keeps rising.
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

"Given the incoming horizontal/vertical position and velocity, how often does our
delayed/noisy steering controller survive through this tunnel's exit?"

All choices use the **same section exit**. This versioned model replaces the old
eight-second jump-failure model. When signs are shown from the previous tunnel,
prediction is conditional on reaching the next fork at the current world-space
horizontal/vertical position and velocity. It does not predict your intervening
inputs or promise survival through the currently selected tunnel as well.

Each rollout stores two-axis position/velocity, reaction countdown, obstacle index,
PRNG state, and alive/dead state. At 60 Hz it:

1. Advances delayed steering with acceleration and a per-axis speed limit.
2. Checks a 3x3 grid of nine horizontal/vertical targets against the next obstacle's
   arrival and contact-window endpoints, choosing a nearby clear target.
3. Samples bounded reaction delay, target noise, and arrival-timing uncertainty.
4. Tests entrance, octagonal walls and 2D obstacle boundaries at each contact tick.
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

Percentages are live, not intrinsic tunnel ratings. Position, velocity, time left
to enter, and arrival phase change the answer as you approach. The per-section RNG
seed is stable; this is not simply replacing samples with fresh random seeds.
At 512 independent trials, worst-case standard error is about 2.2 percentage
points (roughly +/-4.3 points for a 95% normal-approximation interval). That is
sampling error *under this controller model*, not a human-survival guarantee.
Better sampling cannot fix an inaccurate player model.

This live behavior is intentional: moving in front of an opening changes its
estimate. We do NOT freeze a centered-player rating or compute only once per fork.
Refresh remains at most 10 Hz. Vertical state is now included in the same shared
CPU/GLSL Monte Carlo model; no gusts, reactive gates or other proposed algorithm
features have been added.

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
the predictor. Useful prospective widths include 17 signed bits per shaft-position axis,
11 per velocity axis, 7-9 for motion phase, and 32 for PRNG. Begin with a few reusable
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

- Course: **43,008 bytes**, uploaded once and resident (32 sections x four slots,
  ten 32-byte obstacle slots plus a 16-byte header per route; course version 5).
- Request: **64 bytes**, including IDs, section, two-axis state, clock and seed.
- Result payload: **64 bytes**, four summaries; unused choice slots stay zero.

At 10 Hz this is **1,280 payload bytes/second**, independent of rollout count.
A future 16-byte response ID/version/run envelope would make it 1,440 bytes/second.
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

## Prediction design: what we can defend (proposals, NOT implemented)

The current predictor estimates a probability over trajectories, not whether an
optimal path exists. A planner answering "there is a safe route" is not answering
"how often does this imperfect controller survive?" That distinction motivates
probabilistic analysis, **not Monte Carlo specifically**. Our current hazards are
deterministic, even with two-axis steering. Reachability grids,
probability propagation/dynamic programming, precomputed phase tables, and small
quadrature schemes are credible competitors. Even the guaranteed center-line and
sealed routes have cheap special cases. They must be allowed in a fair baseline.

Statistical model checking samples trajectories without constructing an entire
state-transition model; it is useful when that model is too large to construct.
Numerical probabilistic methods are alternatives, not invalid shortcuts. See the
[PRISM sampling explanation](https://www.prismmodelchecker.org/manual/RunningPRISM/StatisticalModelChecking)
and [numerical solution methods](https://www.prismmodelchecker.org/manual/ConfiguringPRISM/SolutionMethodsAndOptions).

Three potential tunnel configurations, in order of preference:

1. **Gusty shaft with correlated uncertainty.** Visible fans create gusts; player
   inertia and several gust/filter modes persist through many gates. Sample a
   few uncertain parameters once per rollout, then evolve correlated gust state
   and reaction errors. Passing gate A changes the state arriving at gate B.
   Multiplying independent per-gate survival rates is wrong, and a nominal path
   misses the distribution. A sufficiently fine DP grid must include velocity,
   gust/filter state, latent parameters and controller state, not just position.
   Test DP and sparse/quasi-Monte-Carlo integration anyway: a low-dimensional
   version may still be cheap. Fans and gates can use bounded integer recurrences,
   so a small FPGA pipeline could retain local state and share collision units.

2. **Reactive, history-dependent gates.** Passing left/right sensors changes
   later doors, reverses blades or changes fan modes; switches have delayed
   effects and visible warnings. Add a bounded steering/heat budget so choices
   affect future maneuverability. Under the same imperfect-player model, gate
   state depends on the whole realized trajectory. A fixed obstacle timetable or
   single shortest path cannot give the survival probability. Exact analysis can
   augment its state with latch bits, delay queues, heat and movement, but their
   joint state space grows. Local bits/counters/compares fit the FPGA hypothesis
   better than heavy floating-point physics. Still benchmark event-driven DP or
   compressed decision diagrams; a few independent switches are not sufficient
   evidence of a difficult problem.

3. **Small interacting hazard swarms.** A few telegraphed drones/shards bounce,
   redirect each other or trigger shutters, with uncertain initial states and
   correlated steering modes. Predict joint player-plus-hazard trajectories.
   Summing obstacle risks ignores correlations; a full joint-state probability
   grid becomes expensive as moving-body state grows. Use a bounded body count
   and simple fixed-point interactions so an FPGA engine remains feasible.
   More entities also increase FPGA storage/multiplier cost, and a GPU may handle
   these batches very well. This is the highest implementation and fairness risk;
   compare simplified analytic bounds and low-order probability models first.

My recommendation is **(1), then a small amount of (2)**. These provide meaningful
uncertainty, temporal correlation and history without requiring a physics engine
that may overwhelm LiteFury. Randomness must be reflected in the real game's
rules or honestly represent calibrated player uncertainty, not exist only to
make a benchmark expensive. Telegraph changes; avoid unavoidable random deaths.

### Energy argument and its limits

Small fixed-point state, shifts, comparisons, local RNG and early termination
are a plausible match for a custom rollout engine. CPU instruction/control
overhead and GPU divergent execution may cost energy; NVIDIA documents the
[divergence issue](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/#branching-and-divergence).
That is an architecture hypothesis, not a measurement: compiler predication,
vectorization, grouping, event skipping and shared-pose caching can improve the
CPU/GPU too. The current CPU is a scalar worker, not a tuned Jetson baseline.

LiteFury's vendor specifies an Artix-7 XC7A100T and PCIe Gen2 x4; the Orin Nano
developer kit exposes a Key-M 2280 PCIe x4 slot. That is a starting point, not
proof of driver, power, cooling or bitstream compatibility. Sources:
[LiteFury vendor](https://github.com/RHSResearchLLC/NiteFury-and-LiteFury),
[Jetson carrier layout](https://docs.nvidia.com/jetson/orin-nano-devkit/user-guide/latest/hardware_layout.html).
The vendor's 10 W supply rating is NOT a measured LiteFury operating-power claim.
Keep course/state resident and return only aggregates; tiny payload bandwidth
does not make PCIe PHY/link/board idle power free.

Longer tunnels allow earlier, less frequent, batched predictions. That benefits
**every** backend: a CPU can also finish and sleep. The code still refreshes at
10 Hz; a slower/stable preview policy is a separate future change. Compare equal
quality and deadlines, not an always-running host against a sleeping FPGA.

There is no honest proof that a tunnel forces Monte Carlo or guarantees a large
energy win. A defensible claim is: **for a stated model, error target, confidence
and deadline, this implementation uses less whole-system energy than the best
reasonable alternatives tested**. Fix the controller, use held-out courses and
high-accuracy reference results, and compare rollout sampling against reachable
sets (feasibility only), probability-grid DP, cheaper integration and lookup/
surrogate models. Measure probability calibration/error AND route-choice regret,
not merely CPU/GPU bit agreement. If a cheaper method meets the quality target,
it wins. For example, choose a provisional +/-3 percentage-point error budget
and a 0.5-second deadline before tuning sample counts; do not manufacture either
requirement after seeing benchmark results. Measure total joules per decision
and total game power at fixed FPS, including PCIe, driver and board idle costs.
If rendering dominates power, even a large predictor-only improvement may yield
only a small whole-game reduction.

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

Checks cover six opening tunnels, a seeded test pilot challenged near one minute,
solid octagonal mouth coverage, per-tick two-axis continuity including moving exits,
W/S and mouse vertical movement, blade escape, octagonal wall corners,
live-position probability changes, acceleration/no braking, 2/3/4 choices,
all obstacle kinds, impossible routes, deterministic streams, stale results, and
48 exact CPU/GPU comparisons including partial workgroups and course wrapping.
Rendered tests verify live guidance. Screenshots: `build/game-fork.bmp`,
`build/game-fourway.bmp`, `build/game-inside.bmp`, `build/game-exit.bmp`, and
`build/game-title.bmp`.

The optimized benchmark fixes seed 12345 and tests three sections at 256/512/1024
rollouts per choice, two warmups and six measured requests per case. Output:
`graphics/vulkan/build/tunnel-benchmark.csv`. It excludes concurrent rendering;
interactive latency also includes observing completion on a later frame.
Latency and throughput are **not energy measurements**. Debug screenshots block
one frame and must be excluded from performance comparisons.

Intentional rough edges: no audio, repeated 32-section course, flat geometry and
a coarse, uncalibrated steering model. Next: playtest steering, decision timing,
and obstacle readability rather than build a bigger engine.

## One-minute pacing check (course version 5)

CPU tests passed six opening sections with moving exit continuity; the seeded,
perfect-course-knowledge test pilot then crashed at 60.56 seconds. This is a
regression check for the ramp, not a measured human survival-time distribution.
An 81-point aperture test checks that the mouth fillers cover corners without
blocking the octagon. The shared timing checks prevent overlapping hazard contact
windows as the course accelerates. Previous version timing benchmarks below are
historical, not measurements of this new pacing.
Both rendered backends passed the six-section opening playthrough with live
guidance; all 48 CPU/GPU comparisons matched. Vulkan synchronization validation
and N-body regressions passed, and two-/four-mouth screenshots were inspected to
confirm the square-corner leaks are sealed.

## Historical wide two-axis gameplay check (course version 4)

Both CPU and GPU backends completed 20 growing passages with live predictions.
All 48 CPU/GPU aggregate comparisons matched with nonzero vertical state and
wide-shaft positions; Vulkan synchronization validation and the original N-body
regressions passed. CPU-only tests additionally cover W/S, vertical mouse input,
blade clearance above/below, octagonal corners, variable timing continuity,
unchanged live-position dependence, and moving exits without recentering.
Inside/exit/four-way screenshots were inspected for the new palette and geometry.
This is automated coverage, not human playtesting or an FPGA energy result.

## Historical long-tunnel check (2026-11-09, course version 3)

These timings predate the wider two-axis and one-minute-paced versions above.
Both rendered backends passed 20 twelve-second sections with live predictions,
about 17.1 ms/frame under the 60 FPS cap. The lateral-continuity regression,
48 exact CPU/GPU request comparisons, Vulkan synchronization validation and the
original N-body regression tests passed. Screenshots were inspected for the
wider mouths and four-way signs. This is automated testing, not human playtesting.

On the same i7-12700H / RTX 3050 Ti laptop, optimized prediction-only requests at
512 rollouts per opening measured 13.118 ms CPU / 4.433 ms GPU for two openings,
and 22.802-24.223 ms CPU / 4.469-4.735 ms GPU for four openings. GPU kernel time
was about 4.1 ms. Six measured samples per case followed two warmups; all integer
aggregates matched. Raw data: [tunnel-long-results.csv](tunnel-long-results.csv).
These are laptop latency figures, **not Jetson or FPGA energy measurements**.
The scalar CPU and current GPU implementation are not final optimized baselines.

## Historical short-tunnel snapshot (2026-11-09, course version 2)

Intel i7-12700H / Vulkan-selected RTX 3050 Ti Laptop GPU. Optimized prediction-only
requests, seed 12345; six measured samples after two warmups. These figures are
for the OLD 2.4-second/two-obstacle tunnels, not the longer version above. Raw data:
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
