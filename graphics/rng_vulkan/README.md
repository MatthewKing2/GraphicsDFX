# RNG workload experiments in Vulkan

This application implements **Monte Carlo π estimation** and **2D random-walk
diffusion** in Vulkan compute shaders. RNG can run on the GPU or arrive through
a framed external stream for the Jetson Orin Nano + PCIe FPGA integration.
Each workload has a small visual display
and GPU timestamps for RNG, simulation, visualization preparation, and drawing.
The purpose is to measure whether RNG is a useful FPGA acceleration target on
your hardware. The application does not assume that RNG will dominate.

This is a sibling of the existing `graphics/vulkan` particle demo. It follows
GraphicsDFX's GLFW, Vulkan, shader-build, and macOS MoltenVK setup without changing
the particle renderer. See [Jetson and FPGA integration](JETSON_FPGA.md) for the
Linux build, input protocol, buffer ownership, transfer timing, and bandwidth
budget. The external path is a portable staged bridge; the board's PCIe/DMA
driver must supply the stream.

## Build and run

Requirements: CMake 3.20+, C++17, Vulkan loader/headers, `glslc` or
`glslangValidator`, GLFW 3.3+, and a
Vulkan implementation supporting graphics/compute timestamps. On the existing
Homebrew setup:

```sh
brew install cmake glfw vulkan-loader molten-vk shaderc vulkan-validationlayers
```

From the GraphicsDFX repository root:

```sh
cmake -S graphics/rng_vulkan -B graphics/rng_vulkan/build -DCMAKE_BUILD_TYPE=Release
cmake --build graphics/rng_vulkan/build --parallel

# Monte Carlo samples inside/outside a quarter circle:
graphics/rng_vulkan/run.sh --workload pi --mode fused

# Diffusing walkers, initially concentrated at the center:
graphics/rng_vulkan/run.sh --workload walk --mode fused
```

Escape or closing the window exits. Use `--frames N` for a finite run and
`--headless` to use real Vulkan compute and offscreen rendering without a window.
The launcher discovers Homebrew's MoltenVK ICD on macOS and respects explicit
`VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, and `VK_LAYER_PATH` settings. On other
platforms the normal Vulkan loader configuration applies.

```sh
# A reproducible finite GPU run with its timestamp CSV and final image:
graphics/rng_vulkan/run.sh --headless --workload walk --mode split \
  --count 65536 --blocks 16 --seed 42 --warmup 20 --frames 120 \
  --csv /tmp/walk.csv --image /tmp/walk.ppm

# Check whether drawing changes compute timing:
graphics/rng_vulkan/run.sh --headless --no-viz --workload pi --mode fused \
  --count 65536 --blocks 16 --warmup 20 --frames 120 --csv /tmp/pi-compute.csv
```

`--frames` counts measured frames **after** warmup. Warmup advances the RNG epoch
and walker state normally but is omitted from the CSV. `--image` writes a PPM
snapshot after the run and requires visualization. Readback and file encoding
are outside the GPU measurements. See `--help` for the full option list.

## Workloads and random streams

The default GPU path uses the complete ten rounds of **Philox4x32-10**, a counter-based
generator designed for parallel statistical workloads. One Philox block produces
four 32-bit words. Its counter is `(invocation, block, epoch, workload)` and its
key is `(seed, 0)`. Different epochs and workloads use distinct counters; no RNG
state is copied from the CPU every frame in that path. Uniform floats use the upper 24 bits
scaled by `2^-24`, giving values in `[0,1)`.
External streams supply the same four-word blocks and metadata; their generator
is chosen by the producer. Philox fixtures allow exact transport/consumer
verification against the CPU reference.

For Monte Carlo π, each block gives two `(x,y)` samples in the unit square. The
shader counts `x*x + y*y <= 1`; the CPU reduces the final batch's hit counts and
prints `4*hits/samples`. It does not read back individual points every frame.
The display shows a bounded subset of each invocation's last sample, colored by
whether it falls inside the quarter circle. It is a view of the sampled
distribution, not every trial used by the estimator.

For diffusion, each block gives two steps with independent uniform increments
`dx,dy ∈ [-step_size,step_size)`. Walkers begin at the center and persist across
frames; positions wrap periodically in `[-1,1)` on both axes. The default step
size is `0.01`. This is a continuous uniform-step random walk, rather than a
four-direction lattice walk: all four floating-point outputs are used, so the
experiment does not inflate RNG demand by throwing away 30 of 32 bits per
direction. Before wrapping matters, the expected mean squared displacement is
`steps * 2 * step_size² / 3` (up to float quantization). The visible cloud spreads
and eventually fills the periodic domain.

The visualization uses at most 32,768 endpoints/samples to fill a 512×512 integer
display buffer. A fullscreen triangle draws that buffer into a 768×768 offscreen
image. This bounds display work as simulation size grows. Interactive mode blits
the image to the swapchain. Headless visualization runs the same compute display
preparation and actual graphics draw.

Each combined measured frame consumes `4 * count * blocks` random words and performs
`2 * count * blocks` useful operations: samples for π, steps for diffusion. These
units make workload-size and RNG-throughput comparisons explicit. RNG-only
isolation generates the same number of words but performs zero samples/steps.

## Split and fused GPU baselines

| Mode | GPU work | What it measures |
| --- | --- | --- |
| `split` | Generate a random buffer, then run the consumer shader | Separately timed generation and consumption, including buffer traffic |
| `fused` | Generate and consume Philox blocks within one invocation | GPU baseline avoiding materialized random-buffer writes/reads |

The two modes use identical counters and useful calculations. The random buffer
is block-major so adjacent invocations access adjacent blocks. Use the fused
baseline when judging a future FPGA implementation: the split design may spend
substantial time materializing a stream that a normal GPU implementation can
keep in registers.

The split RNG pass includes generation **and stores**. Its simulation pass
includes random-buffer **loads**. Their timing fractions are not pure RNG
instruction fractions. The fused shader's combined timestamp cannot assign a
separate duration to its RNG instructions. A large split RNG fraction alone is
not sufficient to declare the optimized workload RNG-bound.

Two diagnostic modes cross-check attribution using independent submission
envelopes. They require `--mode split --headless --no-viz`:

```sh
graphics/rng_vulkan/run.sh --workload pi --mode split --headless --no-viz \
  --isolate rng --warmup 20 --frames 120 --csv /tmp/pi-rng-only.csv
graphics/rng_vulkan/run.sh --workload pi --mode split --headless --no-viz \
  --isolate consumer --warmup 20 --frames 120 --csv /tmp/pi-consumer-only.csv
```

`--isolate rng` measures generation without running simulation. For
`--isolate consumer`, each epoch's exact random stream is generated in a
separate, untimed GPU submission before the measured consumer submission.
Thus `gpu_ms` spans only that consumer submission; `cpu_ms` includes stream
preparation. The isolated runs retain the same counters and useful work as
combined split mode, but their independent submission overheads differ from a
combined frame. They are diagnostic baselines, not accelerated simulations.

`--rng-stream PATH` selects external blocks in normal split mode. A batch is
read and checked into aligned CPU memory, copied into persistent mapped Vulkan
staging, then uploaded to the GPU random buffer. Simulation and drawing stay on
the GPU. External input cannot be combined with fused mode or isolation. A
regular file, FIFO, or readable driver device can supply the framing; hardware
control and DMA completion remain the driver's responsibility. The
[integration guide](JETSON_FPGA.md) defines the protocol and a small verification
fixture. CPU-produced fixtures validate software behavior and do not measure FPGA
performance.

## Collect and compare profiles

The Python runner uses only the standard library. It launches both workloads,
both modes, and visualization on/off, plus RNG-only/consumer-only isolation for
each workload: twelve configurations per size/trial. It shuffles the run order
reproducibly.
Defaults are 65,536 invocations, 16 blocks each, 20 warmup frames, 120 measured
frames, and three separate process trials per configuration.

```sh
# Quick end-to-end smoke profile, twelve short runs:
python3 graphics/rng_vulkan/tools/profile.py --quick --output /tmp/rng-quick

# Three-trial baseline plus count/block sweeps:
python3 graphics/rng_vulkan/tools/profile.py \
  --counts 4096,65536,262144 --blocks 1,16,64 --output /tmp/rng-profile

# Rebuild reports from retained CSVs:
python3 graphics/rng_vulkan/tools/profile.py --summarize-only --output /tmp/rng-profile
```

Use a fresh output directory for a new run; existing measurements are not
overwritten. `--exe /absolute/path/to/run.sh` selects another build or launcher.
Explicit `--frames`, `--warmup`, and `--trials` override quick/default settings.
Large count×block combinations can exceed device storage-buffer limits; the
application checks these limits rather than silently reducing the workload.

The output directory contains raw per-frame CSVs, process logs, the executed
command manifest, `summary.json`, and `report.md`. Reports include median/p95 GPU
times, separate whole-submit isolation comparisons, approximate split dispatch
fractions, visualization fractions, and effective random-word/useful-operation
throughput. Frame samples are pooled across trials;
their p95 describes variation, not a statistical confidence interval.
The summarizer rejects mixed seeds, warmup schedules, step sizes, or display
sample limits within a configuration and compares split/fused rows only when
those parameters agree.
Isolation and `timestamp_method` also distinguish configurations, so different
measurement methods are not pooled. `rng_source=gpu|external` distinguishes the
producer path. The runner automates GPU-generated baselines only: it never
reuses a finite external file across cases with different epochs, shapes, seeds,
or workloads. Collect external runs manually with a fresh/configured producer,
then use `--summarize-only` on their CSV directory.

CSV timing fields:

| Field | GPU/CPU scope |
| --- | --- |
| `rng_ms` | Separate GPU RNG dispatch in split mode; zero for fused/external input |
| `rng_input_ms` | External blocking read and header checks on CPU; zero for GPU-generated RNG |
| `rng_staging_ms` | CPU memcpy from aligned input RAM into mapped Vulkan staging |
| `upload_ms` | GPU staging-to-random-buffer copy window for external input |
| `simulation_ms` | Consumer dispatch; includes RNG in fused mode |
| `compute_ms` | Whole RNG/upload plus simulation region and its dependencies |
| `visual_prepare_ms` | Clear display buffer, synchronization, bounded visualization dispatch |
| `render_ms` | Offscreen fullscreen draw |
| `gpu_ms` | Entire measured GPU submission envelope; only the selected work for isolation |
| `cpu_ms` | Frame wall time for submission/wait and presentation; includes untimed RNG preparation in consumer-only mode |

Device name, driver version, timestamp period, mode, sizes, seed, epoch,
visualization setting, isolation, timestamp method, random-word count, and operation count accompany each
sample. The application checks queue timestamp support, converts ticks using
`timestampPeriod`, and handles the queue's valid timestamp-bit width. Queries use
the same queue. Pass times remain approximate measurements of GPU execution,
not instruction-level hardware-counter attribution. Interactive blit/presentation
and final result/image readbacks are outside the GPU timing scope.

**MoltenVK timestamp scopes require care.** Its Apple GPU stage-counter path can
defer timestamp samples until a Metal encoder ends. The
`timestamp_method=encoder_boundaries` path uses small probe/sentinel transfers
to establish encoder boundaries; those transfers add instrumentation overhead.
Individual RNG, consumer, and display phase windows remain approximate and can
have misleading outliers. Compare isolated **whole-submission `gpu_ms`** envelopes
for the stronger attribution cross-check. Keep different timestamp methods
separate, and repeat on the target native Vulkan GPU before drawing a final
bottleneck conclusion. The [MoltenVK implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm)
documents the deferred stage-counter behavior in its encoding path.

The report also preserves the time outside named dispatch windows.
`compute_dependency_ms` is `max(0, compute_ms - rng_ms - upload_ms - simulation_ms)` for
combined split mode, and zero for fused/isolated modes. `scope_dependency_ms` is
`max(0, gpu_ms - compute_ms - visual_prepare_ms - render_ms)`. These are residual
timestamp scopes, including dependencies and measurement effects, rather than
precise barrier-cost counters. The nonnegative clamp handles approximate scope
boundaries; medians of separate fields need not add to the median total.
`split_rng_compute_fraction` uses the complete compute span as its denominator.
The older `split_rng_dispatch_fraction` excludes dependency gaps and is labeled
as a dispatch-only fraction. Both fractions are medians of per-frame ratios.
They are approximate phase diagnostics, not substitutes for the independent
whole-submit measurements. No GPU RNG fractions are assigned to external input.
For that path, `compute_ms` includes upload plus simulation; `cpu_ms` includes
the input wait and both CPU/GPU copies. Compare end-to-end frame time as well as
individual scopes when evaluating the FPGA path.

To assess the acceleration hypothesis, compare RNG-only and consumer-only
`gpu_ms` distributions, combined split/fused spans, then visualization on/off and
increasing sizes. The isolated table reports
`median(RNG-only gpu_ms) / (median(RNG-only gpu_ms) + median(consumer-only gpu_ms))`.
This combines two independent submission envelopes, each with its own
instrumentation and dependency overhead. It is not a fraction of the original
application's total time and cannot predict FPGA speedup.
Very small dispatches can mostly
measure launch/timestamp overhead. If visualization remains cheap and RNG has a
large split share over a useful size range, that supports further investigation.
A fused baseline that is much faster may instead show the cost of stream memory
traffic. No result is forced to fit an RNG-bound conclusion.

For an FPGA comparison, include random stream production, transfer to the GPU,
buffering, synchronization, and consumer time. An idealized fraction-based
speedup bound assumes free delivery and can overstate achievable acceleration.
This project supplies GPU baselines; it does not measure an FPGA implementation.

The [retained Apple M5 measurement package](measurements/apple-m5-2026-10-28/README.md)
contains the validated run protocol, raw samples, and interpretation. Earlier
phase-only measurements were superseded after detecting deferred timestamp
scope attribution; their phase fractions are not used as bottleneck evidence.
These are local Apple M5 software baselines, not Jetson Orin Nano or FPGA/PCIe
results. Run the same baseline and external-path protocol on the Jetson before
assessing the hardware acceleration hypothesis.

## Verification

```sh
ctest --test-dir graphics/rng_vulkan/build --output-on-failure
python3 -m unittest discover -s graphics/rng_vulkan/tools -p 'test_*.py'
for workload in pi walk; do
  for mode in split fused; do
    graphics/rng_vulkan/run.sh --headless --verify --validation \
      --workload "$workload" --mode "$mode"
  done
done
```

The CPU tests check Philox against published known-answer vectors and exercise
workload/reference invariants. GPU verification compares small split and fused
runs against the CPU reference over multiple epochs, including bounds handling.
Diffusion comparisons allow floating-point tolerance. These checks establish
implementation consistency; a histogram or mean test is not a substitute for
the upstream generator's statistical validation.

`--validation` explicitly enables Khronos synchronization validation for
correctness checks. Performance runs use validation disabled, the default.

## References

- [Random123 upstream library](https://github.com/DEShawResearch/random123) and
  [Philox paper, Salmon et al. (SC11)](https://www.thesalmons.org/john/random123/papers/random123sc11.pdf):
  the counter-based RNG design and parallel baseline.
- [University of Louisville random-walk teaching model](https://prancer.physics.louisville.edu/modules/random_walks/index.html):
  diffusion visualization and the relationship between steps and spreading;
  our implementation uses continuous uniform increments.
- [Khronos timestamp-query sample](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html):
  GPU timestamp interpretation and support requirements.
- [ThundeRiNG paper](https://arxiv.org/abs/2105.09578): published FPGA RNG and
  application results motivating investigation. Its hardware-specific results
  are not a prediction of speedup for these workloads or your eventual system.
