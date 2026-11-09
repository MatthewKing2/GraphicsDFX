# REACTOR / RUN

## The game

Escape down a failing reactor's low-poly maintenance tunnels. Forward motion
accelerates automatically; choose a branch and fly around broken struts, cargo
rails, laser grids and failing interlock doors. Warm rust/metal passages, amber
conduits, caution stripes, relay lamps and green control rings replace the abstract
neon void. Mouths remain genuinely octagonal. No screen-space gameplay HUD.

- WASD / mouse: horizontal and vertical steering. No brakes or free-look.
- Space / Enter: start or retry. **Space during play: emergency power pulse.**
- A pulse lasts 1.2 seconds and holds shutters open / lasers off. It does not
  remove solid struts, cargo blocks or walls. Two glowing cockpit cells show charges.
- Fly through an optional green **REROUTE** ring: gain one charge (maximum two),
  get 0.6 seconds of emergency power, and toggle that branch's power-bank boost.
  Boosting one bank reduces its spontaneous faults but overloads the next bank.
- Green interlocks are healthy; amber warns of developing faults; red closes /
  energizes. Faults accumulate through three warning ages before powered hazards
  become dangerous, then recover or worsen. Connected failures cause chain reactions.
- Percentages above forks are live forecasts from current position, velocity,
  relay state, charge count and rerouting. They are not fixed tunnel difficulty.
- R restarts; Escape pauses; F1 selects CPU/GPU forecasting; F2 disables it;
  F3 displays development stats in the window title/console.

From the repository root: `graphics/vulkan/play.bat`. Or `make game` in
`graphics/vulkan`, then `build/survival_game.exe`. The Windows SDK build is retained;
a Jetson/Linux build and FPGA driver are **not** implemented in this iteration.

The fast survival ramp is retained: start at 27 units/s, add 9 units/s each second.
Initial six-second approaches tighten later; there is no forced one-minute timer.
The 32 obstacle templates repeat. Normal runs have a private random reactor seed;
retries repeat that seed for practice. Explicit replay/test seeds are never sent
as the predictor's future reactor random stream. This is a simulation-model
contract, not a cryptographic promise against a predictor inspecting game internals.

## The actual computation

There are **128 coupled relays**, arranged into four power banks. Each relay has:

- one fault bit;
- a three-bit warning/closing age (0 through 7);
- a packed 32-bit rule selecting three other relays, a bank and a fault rate.

At 10 Hz, double-buffered transitions combine spontaneous faults, neighbor failure
cascades, recovery, elapsed-run stress and rerouting. No relay sees a neighbor's
partially updated state. The whole evolving reactor is **64 bytes**. Rule storage
is **512 bytes**. There are no floating-point trigonometric operations in this model.
Fault pressure increases over the first minute. Each opening's powered hazards
belong to its corresponding bank, so rerouting has a direct gameplay consequence.

The actual world currently advances this circuit on the CPU. **Both CPU and GPU
predictors execute the same shared circuit and collision code.** For each route,
they sample possible future fault/recovery streams and player reaction errors,
evolve the full reactor from now through that route's exit, simulate steering,
apply ring interactions, and count surviving attempts. Every route has a powered
interlock; the old hard-coded always-safe marker and sealed-route special cases
are gone. An isolated gate probability cannot capture shared failure history.

The virtual pilot uses emergency pulses near a threatened shutter while charges
last. Thus a displayed estimate assumes that documented pulse policy, not perfect
play and not a measurement of human skill. The current controller samples a coarse
3x3 set of steering targets. It can miss valid maneuvers. Future forecasting is
conditional on surviving the current passage when it previews the next fork.

Default: 1,024 trials per opening, up to four openings, at most five requests/s.
At a 50% probability, 1,024 independent samples have about +/-3.1 percentage points
of 95% normal-approximation sampling uncertainty; model error is separate.
The sample count is configurable, not padding to manufacture accelerator work.

GPU requests are divided into batches of 128 trials per opening. Each completed
batch schedules the next without a host wait. Graphics can run between batches;
integer aggregates must match the CPU reference exactly, including
partial workgroups and final batches. Results are rejected after a new run/fork,
an intervening pulse/reroute/charge change, or excessive simulation age. Only one
request is outstanding; old results cannot form an unbounded queue.
The freshness deadline is 0.5 seconds of simulation time. The game never waits
for a prediction to finish; unfresh signs show an ellipsis.

The CPU implementation runs independent routes on up to four workers with local
accumulators; small validation requests run serially. It is optimized C++, not a
deliberately slow baseline. Bit-slicing, SIMD and more aggressive scheduling remain
worth testing before making hardware energy claims.

This feature motivates joint stochastic prediction, **not uniquely Monte Carlo**.
State factoring, decision diagrams, event-driven methods, bit-sliced CPU/GPU code,
shared-prefix trajectory reuse across choices, and calibrated cheaper predictors
are legitimate competitors. Being difficult
to solve exactly does not imply approximate prediction must consume much power.

## Proposed Jetson <-> LiteFury contract

The bandwidth target is interpreted as **0.25 GB/s = 250,000,000 bytes/s**, combined
payload in both directions. This is a rate, not 0.25 GB per frame or total run.

| Item | Bytes | Frequency / ownership |
| --- | ---: | --- |
| Course + relay rules | 43,520 | Upload once; resident on accelerator |
| Current reactor | 64 | Resident on authoritative engine; snapshot in host-owned forecast request |
| Forecast request | 128 | IDs/version, clock, two-axis player state, pulse/routing state, snapshot |
| Forecast result | 64 | Four integer aggregates; no sampled trajectories |
| Authoritative world telemetry | 64 | Current/next fork ages: ten 3-bit hazards per choice, plus IDs/actions/status |

At the current 5 Hz request ceiling, forecast payload is **960 bytes/s**. Even a
future 60 Hz control packet and 60 Hz telemetry stream, each 64 bytes, plus 10 Hz
forecasts total only **9,600 bytes/s**: about 0.00384% of the target. The startup
model is also small if the intended limit was 0.25 GB per run instead.

`Game::telemetry()` implements the 64-byte compressed status representation and
tests round-trip its hazard ages. A future FPGA can own circuit evolution and send
these ages; the host reconstructs geometry from its existing course copy. Do NOT
send meshes, transforms, framebuffers, particle histories or per-relay/per-rollout
state every tick. The GPU implementation currently copies a 64-byte partial
aggregate per batch; that is an internal scheduling detail, not a requirement on
the FPGA protocol. Actual link transactions also include framing, DMA, interrupts
and PCIe idle/link-transition costs; payload arithmetic is not a measured trace.
The 64-byte result contains aggregates only: the completion must be associated
with the single outstanding request/ID. An out-of-order protocol needs an explicit
ID header. If the FPGA becomes authoritative, it already owns the live snapshot;
the host need not send that snapshot back with every forecast command.

## LiteFury resource feasibility: budget, NOT synthesis results

The vendor lists LiteFury as XC7A100T with PCIe Gen2 x4 and 512 MB DDR3:
[RHS Research](https://github.com/RHSResearchLLC/NiteFury-and-LiteFury).
AMD DS180 specifies 15,850 slices (four LUTs and eight FFs each), 135 RAMB36 blocks
and 240 DSP slices: **63,400 LUTs, 126,800 FFs, about 607.5 KiB block RAM**.
[AMD device table](https://docs.amd.com/v/u/en-US/ds180_7Series_Overview)

Concrete storage counts and an illustrative architecture:

| Proposed resource | Count / arithmetic |
| --- | --- |
| One reactor snapshot | 512 bits / 64 bytes |
| Relay rule table | 4,096 bits / 512 bytes |
| Conservative complete rollout context | 128 bytes, including reactor, motion, RNGs and controller/actions |
| 1,024 contexts, reused across choices | 128 KiB; ideal storage floor 29 RAMB36 blocks |
| Resident model | 43,520 bytes; ideal storage floor 10 more RAMB36 blocks |
| Eight relay-step lanes, each caching old/new state and one RNG | 8 x (2 x 512 + 32) = 8,448 register bits, before controller/pipeline/DMA logic |

The RAM counts are **ideal byte-capacity floors**, not inferred RAM utilization.
Port widths, duplication, banking and FIFOs can increase them. Geometry needs a
few reusable integer multipliers; the relay transition itself needs no DSPs.
For illustration, eight one-node-per-cycle lanes at an unproven 100 MHz would
provide 6.25 million 128-node transitions/s before stalls. That is not a measured
clock, deadline guarantee, or fitted design.

No RTL, PCIe driver or fitted bitstream is supplied here. Vivado/Yosys are not
available in this environment, so **actual LUT/FF/DSP counts, routing/timing and
FPGA power are unknown**. Reserve substantial room for PCIe/DMA and the complete
forecast engine; fitting relay state alone does not prove the whole system fits.

## Profiling / reproducibility

Normal play has profiling off. Set `PROFILE_REACTOR` in `game_main.cpp`, or use:

```powershell
graphics/vulkan/tests/game-checks.ps1 -Benchmark -Profile -NBody
```

From `graphics/vulkan`, useful direct commands:

```powershell
build/survival_benchmark.exe --benchmark --output build/reactor-benchmark.csv
build/survival_benchmark.exe --benchmark --freeze-reactor --output build/reactor-frozen.csv
build/survival_benchmark.exe --autoplay --seed 12345 --frames 600 --profile --profile-output build/reactor-profile-gpu.csv
build/survival_benchmark.exe --autoplay --seed 12345 --frames 600 --profile --backend cpu --profile-output build/reactor-profile-cpu.csv
build/survival_benchmark.exe --autoplay --seed 12345 --frames 600 --profile --no-prediction --profile-output build/reactor-profile-off.csv
```

The optimized binary excludes validation. Prediction benchmarks use fixed requests,
two warmups and six measured samples. GPU kernel timestamps sum all batches;
latency includes dispatch, batching and host completion observation.
Later-section benchmark requests use a warmed, evolving reactor snapshot rather
than resetting every relay to healthy. Setup is outside the timed region.
Interactive
profiles separately report world CPU, scene construction, CPU draw/wait/upload,
GPU render time, completed forecast CPU/GPU work and frame pacing. CPU/GPU work
can overlap; their times must not be summed as a serialized frame breakdown.
In-flight work at the end is excluded from completed-forecast totals.
CPU forecast time is parallel-request wall time, **not summed CPU core time**.

`--freeze-reactor` is an **invalid-predictor diagnostic ablation**, not a proposed
cheap algorithm: it freezes future circuit state. It helps identify circuit cost,
but changes survival/early termination, so its timing difference is not an exact
per-stage attribution. Compare work counts too. `--no-prediction` keeps the actual
world running and isolates the rendering/gameplay floor. Autoplay reads current
circuit state, not prediction results, so it adds no hidden MC workload.

## Measured results: laptop, not Jetson

Measured 2026-11-09 on an i7-12700H / RTX 3050 Ti Laptop GPU, Windows, clang++
`-O3`. Interactive profiles use 1280x720, the 60 FPS cap, seed 12345, default
1,024 trials/opening and 600 frames (about 10.3 seconds of the introductory section).
Optimized profiling builds exclude Vulkan validation. Timing varies with laptop
clocks, thermals and scheduling; these are one reproducible run, not energy data.

| Interactive mode | FPS | p95 frame | Mean forecast latency | Completed / accepted |
| --- | ---: | ---: | ---: | ---: |
| Predictions off | 58.24 | 17.60 ms | -- | -- |
| GPU predictions | 58.27 | 17.50 ms | 226.83 ms | 41 / 40 |
| CPU predictions | 58.49 | 17.35 ms | 349.20 ms | 28 / 21 |

- GPU forecasting accumulated **6,479 ms** during **10,296 ms** wall time:
  approximately **63% GPU forecast duty**. Rendering averaged **0.063 ms/frame**,
  roughly 38 ms across the run. Forecasting is over 99% of these two measured
  GPU-work totals. This is useful computation, not a frame-time stall.
- CPU forecasting accumulated **9,512 ms of parallel-request wall time** in
  10,258 ms. It kept its background worker occupied for about 93% of the run;
  this is not 93% of all CPU cores or a power percentage.
- Actual world updates averaged **0.0067 ms/frame**, scene construction **0.255 ms**,
  and CPU draw submission **0.640 ms** in the GPU profile. About **15.96 ms/frame**
  was pacing sleep. CPU uploads/fence waits are sub-measurements of draw, not extra
  times to add. Rendering is not the dominant compute here.
- Some forecasts expire or are invalidated by actions/fork transitions. Fixed
  full-horizon CPU jobs below sometimes exceed the 500 ms freshness deadline;
  these are discarded, never allowed to block play. GPU batching also adds latency
  between rendered frames compared with the standalone benchmark.

Standalone fixed requests, 1,024 trials per opening, mean of six timed samples:

| Section / openings | CPU latency | GPU latency | GPU kernel |
| --- | ---: | ---: | ---: |
| 0 / 2 | 659.48 ms | 179.85 ms | 176.82 ms |
| 1 / 4 | 600.18 ms | 161.04 ms | 157.88 ms |
| 4 / 4 | 434.90 ms | 95.37 ms | 92.33 ms |

Freezing future relay transitions reduced section-4 GPU kernel time from 92.33 ms
to 4.50 ms, but changed opening C's estimated survival from **83.59% to 99.41%**.
Thus the expensive reactor evolution affects the answer; it is not decoration.
This diagnostic does **not** rule out an accurate cheaper algorithm, prove a
precise cost fraction, or validate the virtual pilot against human players.

Raw data: [fixed-request CPU/GPU results](reactor-results.csv),
[frozen diagnostic](reactor-frozen-results.csv),
[interactive on/off profiles](reactor-profile-results.csv).

Verification passed: compiled game and optimized benchmark; SPIR-V validation;
48 bit-exact CPU/GPU aggregate comparisons (partial batches, wrapped course,
mixed damaged states, bank boosts, pulses and charge counts); CPU/GPU live-guidance
smoke runs; title/inside/exit/four-way captures; ring/recharge/telemetry tests;
and existing N-body regressions. Debug runs reported no Vulkan synchronization
validation errors. Visual captures were inspected; human playtesting remains.

## Success criterion

Fun gameplay AND lower measured **whole-system energy** than the best reasonable
Jetson CPU/GPU implementation, at equal model quality, prediction deadline, FPS
and resolution. Include FPGA, carrier/PCIe, DMA/driver and idle power. The GPU
still renders; its entire power is not saved. A dominant compute *time* share is
not proof of dominant *energy*, and laptop profiling cannot establish a Jetson /
LiteFury energy win. Do not inflate samples, deliberately weaken the baseline,
or introduce hidden computation that the game does not use.
