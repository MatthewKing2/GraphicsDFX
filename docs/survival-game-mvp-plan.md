# Survival runner MVP roadmap

Status: approved for implementation. The playable MVP is the first software
milestone; whole-project success additionally requires measured hardware energy savings.

## Goal and working style

Build a small, fast-paced first-person survival game on the existing Vulkan
infrastructure. Race through branching corridors while a pursuing threat closes
in. A survival predictor recommends routes, but movement, jumping, and reacting to
hazards remain the player's job. The game should be fun without the predictor.

Whole-project acceptance has two requirements: an enjoyable game and lower
whole-system power/energy using Jetson + LiteFury than using the Jetson CPU/GPU
alone at equivalent guidance quality, update rate, and rendering settings. Include
FPGA board power, PCIe traffic and link/host overhead, and idle intervals. Laptop
timings cannot establish this result; accelerator idle power may outweigh savings
for a small or infrequent workload. Do not manufacture an advantage by handicapping
the CPU/GPU or increasing rollouts without a demonstrated guidance-quality benefit.

Prioritize a playable prototype and fast experiments over architectural polish.
The first useful 75% is the target; cosmetic jank is acceptable. Crashes, broken
controls, misleading results, and unwinnable required routes are not.

- Build and play every 30–60 minutes. Spend a few minutes actually playing.
- Keep a working executable after each milestone; make small, reversible changes.
- Hard-coded course arrays and simple special cases are fine for the MVP.
- Timebox cosmetic problems. Add abstractions only when they unblock the next step.
- Do not build an ECS, general-purpose engine, plugin system, or elaborate UI.
- Treat the defaults below as starting points to critique and tune, not commitments.

Implementation order:

**Render a corridor → movement and restart → playable branching course → CPU
guidance → matching GPU guidance → measurements and targeted tuning.**

## Existing infrastructure to reuse

| Area | Reuse | Missing or coupled behavior |
| --- | --- | --- |
| Rendering | Vulkan window, swapchain, depth testing, colored vertices, instanced drawing | Renderer requires particles and advances N-body physics during drawing |
| Compute | Storage buffers, descriptors, dispatch, barriers, GPU timestamps | Prediction needs its own shader, resources, and result collection |
| Input | GLFW mouse capture, mouse-look, WASD, scroll | Current movement is a flying camera; no grounded controller or jump |
| Timing | Wall-clock timing, CPU timers, frame statistics | N-body advances once per rendered frame; gameplay needs a fixed timestep |
| Validation | Numerical CPU/GPU tests and benchmark scripts | Add prediction-specific cases and benchmark inputs |
| UI | Colored geometry | No HUD or text renderer |

Relevant starting points: [renderer interface](../graphics/vulkan/src/renderer.hpp),
[renderer implementation](../graphics/vulkan/src/renderer.cpp),
[camera/input](../graphics/vulkan/src/camera.cpp),
[profiling](../graphics/vulkan/src/profiling.hpp), and
[build targets](../graphics/vulkan/Makefile).

## 1. Render a game scene without running N-body

- Preserve the N-body executable and add a separate game target.
- Add a narrow rendering-only path accepting application-owned model matrices.
  Do not rewrite the Vulkan infrastructure or disguise game objects as particles.
- Start with one instanced cube mesh. Scale cubes into floors, walls, obstacles,
  and blocky signs. Upload reusable geometry once.
- Keep vertex colors. Simple directional face shading is sufficient; no models,
  textures, shadow maps, or material system.
- Separate application gameplay from drawing, and prediction requests from both.
  Keep the code in a few concrete files for course data, game state, and prediction.

**Checkpoint:** a visible 3D corridor, with no N-body simulation running.

## 2. Make the smallest playable loop

- Mouse looks; WASD moves horizontally; Space jumps; Escape exits.
- Extract input from the flying-camera behavior. Looking upward must not make
  forward movement fly upward. Normalize diagonal movement.
- Use a simple player bounding box, gravity, and box collisions. Avoid a physics
  engine. Prevent ordinary movement from passing through walls.
- Start at 8 units/second movement, 6 units/second jump speed, and 18 units/second²
  gravity. Use a 120 Hz gameplay timestep driven by elapsed wall time, with a
  bounded catch-up loop. Render independently.
- Add short jump buffering and a little forgiveness after leaving a ledge.
- Add one obstacle, one gap, one finish, clear death feedback, and nearly immediate
  restart. Restart must reset gameplay state, timers, and pending guidance.

**Checkpoint:** run, jump, fail, restart, and finish a tiny course. Playtest before
adding prediction or more infrastructure.

## 3. Turn the corridor into a short survival game

- Handcraft a roughly 45–60-second course with three two-way junctions and two
  checkpoints. No procedural generation initially.
- Offer meaningful tradeoffs: shorter but more demanding routes versus longer,
  safer routes. Keep required routes physically traversable.
- Add static obstacles, gaps, and one periodic gate with obvious warning colors.
- Represent pursuit with an approaching wall/fog tied to a checkpoint deadline,
  avoiding enemy pathfinding. Make the remaining time/danger readable.
- New checkpoints restore a little breathing room. Revisiting a checkpoint must
  not repeatedly reset its deadline.
- Show why the player died: fall, hazard, or pursuit. Make retrying quick.
- Score progress/checkpoints and completion time using a simple documented rule.

Use one shared course description for rendering, collision construction, and
prediction. Corridor records hold lengths, connections, obstacles, hazard periods
and phases, checkpoints, and pursuit rules. Do not maintain three independent
versions of the level.

**Checkpoint:** an enjoyable, replayable course with the predictor disabled.

## 4. Add CPU survival prediction and visible guidance

### Prediction contract

Answer: “If the player takes this route now, how likely are they to survive the
next fixed interval under our simplified player-and-hazard model?”

Use explicit, fixed-width data layouts and integer/fixed-point arithmetic where
practical. Proposed representations are millimetres, integer ticks, and bounded
32-bit state. Document units, rounding, probability thresholds, and value ranges.

- Request: request ID, course version, junction ID, current decision state, seed,
  prediction horizon, and rollouts per route.
- Result: matching IDs, per-route survival counts and progress totals, completed
  rollout/step counts, and timing metadata.
- Backend operations: submit a request and try to collect a completed result.
- Upload the compact course when it changes; rendered meshes are not predictor
  input. The request also captures relevant current hazard/pursuit state.

### First rollout implementation

Start with an 8-second horizon, 1/30-second prediction ticks, at most 240 steps per
rollout, 512 rollouts per route, and prediction requests at 5 Hz independently of
rendering. These are configurable starting values, not workload targets to inflate.

Each rollout:

1. Commits to the candidate immediate route.
2. Advances compact player state along corridor segments.
3. Samples modeled reaction delays, jump mistakes, and uncertain future hazard
   arrival timing; checks hazards and the pursuit budget.
4. At subsequent junctions, follows the shortest nominal-time path to the next
   checkpoint, with deterministic tie-breaking. All backends share this policy.
   Do not model the player as randomly wandering.
5. Records survival at the common horizon and progress. Course completion is an
   absorbing success state; death is an absorbing failure state.

Actual play retains continuous movement and collisions. Prediction is a cheaper
corridor-level approximation, but uses the same course and hazard rules. Define
reaction/jump uncertainty explicitly; do not inject invisible random deaths into
the real game or imply calibrated guarantees about a human player.

Keep gameplay randomness separate from prediction. Assign reproducible prediction
streams by request, route, and rollout so changing rollout count never changes
actual game events.

### Connect it to the game

- Run CPU prediction on a worker thread so gameplay does not wait for it.
- Start with colored world-space route arrows. Add percentages and a minimal
  status/death overlay using a tiny bitmap/digit approach, not a UI framework.
- Label probabilities as model estimates. Retain the current recommendation
  unless another route leads by roughly five percentage points.
- Allow one outstanding request and coalesce newer state while busy. Reject
  results for old course versions, passed junctions, or previous runs. Clear or
  mark overly old guidance unavailable; do not silently show stale certainty.
- If no result is available, play continues normally.

**Checkpoint:** useful CPU guidance that changes with state without playing the
game for the player. The backend boundary is small enough to explain directly.

## 5. Add the matching Vulkan compute backend

- Port the bounded integer rollout loop, not the detailed 3D player physics.
- Start with one invocation per rollout and small per-route aggregate outputs.
  Aggregate on GPU rather than reading back every rollout or random number.
- Match CPU units, rules, random-number stream assignment, and continuation policy.
  Verify fixed-seed agreement before interpreting performance numbers.
- Reuse buffer, descriptor, pipeline, and dispatch patterns from the existing
  compute code, without entangling prediction with the N-body shaders.
- Use a small set of request/result buffers protected by completion fences. Poll
  results only after completion; do not call blocking particle readback,
  `finishFrame()`, or device-idle waits for interactive predictions.
- Start on the existing graphics/compute queue. Nonblocking CPU readback does not
  guarantee GPU compute cannot delay rendering: measure it, bound work per request,
  and lower rollout count or prediction frequency if necessary.
- Do not build a dedicated asynchronous-compute scheduling framework for the MVP.
- Switch backends without changing course events or prediction semantics.

**Checkpoint:** CPU and GPU guidance can be selected during equivalent gameplay,
with matching results and no render-loop wait for prediction completion.

## 6. Validate, measure, and tune only what matters

Add lightweight checks alongside each feature, not a large final testing phase:

- Clear route, unavoidable failure, timed-hazard boundary, checkpoint, and finish.
- Matching CPU/GPU survival counts and progress totals for identical requests and
  seeds. Log disagreements instead of hiding them behind similar percentages.
- Bounded termination, valid graph indices, and documented arithmetic ranges.
- Stale-result rejection, restart reset, and unchanged game events when prediction
  settings change.
- Repeated playthroughs for collision failures and unwinnable required geometry.

Add a prediction benchmark using fixed course/state snapshots, independent of
interactive play. Report:

- End-to-end request latency, including scheduling/transfers/readback; GPU compute
  time separately; predictions per second.
- Rollouts and actual simulated steps per second, including early terminations.
- Average and p95 render frame time, backend, horizon, and rollout count.
- CPU/GPU agreement and recommendation stability across repeated seeds.

Compare 256, 1,024, and 4,096 rollouts per route. Keep the smallest count producing
useful, stable guidance. Include ambiguous decisions, not just easy cases. Keep
rendering settings and prediction frequency fixed for whole-system comparisons.
Never label latency or throughput as measured energy.

**Checkpoint:** one repeatable demo, a runnable benchmark, and a concise report
that distinguishes gameplay quality, predictor fidelity, and backend performance.

## Scope cuts and stopping rule

Biggest risks are renderer coupling, movement feel, unreadable guidance, predictor
model mismatch, and GPU scheduling/readback bugs. Tackle them in the sequence
above; avoid polishing around a broken core loop.

Cut elaborate art, procedural generation, third route choices, extra hazard types,
and UI decoration before cutting playability or CPU/GPU agreement. If GPU work
remains unstable, preserve the playable CPU version and report the GPU work as
unfinished rather than blocking the game or claiming a completed comparison.

The MVP is complete when the course is playable, controls/restarts feel responsive,
route guidance is understandable, both backends pass focused checks, and benchmark
results are reproducible. Remaining cosmetic roughness is acceptable.

## Future FPGA boundary — document, do not implement

The eventual accelerator receives the compact course and decision state, performs
PRNG, bounded integer state updates, lookups, survival checks, and aggregation, then
returns small summaries. Course records map to local memory and rollout state to
registers; no meshes or detailed rendering/physics cross this boundary.

FPGA energy efficiency is a hypothesis to test, not a result to manufacture. Keep
CPU/GPU implementations reasonable and comparable. RTL, Jetson/LiteFury integration,
PCIe drivers, DFX, networking, multiplayer, large worlds, and elaborate physics are
outside this MVP.
