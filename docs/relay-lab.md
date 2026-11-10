# Relay visualization and laptop power

The relays are **128 fictional state machines**, not electrical components or a
realistic reactor model. A live update is cheap. Repeating it for thousands of
possible futures is expensive. The laptop measurements do not establish a large
FPGA whole-system energy saving.

## Open the standalone visualization

From the repository root:

```powershell
.\graphics\vulkan\tools\relay_lab\open.bat
```

It needs Python with Tk, already available on this laptop. It does not build,
launch, change, or connect to the game. The new tools have their own build folder.

Start with these experiments:

1. Click a light. Its three incoming wires and last transition appear on the right.
2. Press Right Arrow to advance one 0.1-second simulation tick. All 128 lights read
   the old state before any new state becomes visible.
3. Use Ring A. Its bank gets fewer spontaneous faults; bank B pays an overload
   penalty. Purple outlines show differences from a no-action replay using the
   **same random draws**, so random variation alone does not explain them.
4. Press P. The example door opens, but the relay states do not change. This is
   what a game's emergency pulse actually does: bypass hazards, not repair faults.
5. Inject a fault, then step. Injection is a **lab-only** teaching control, not a
   game feature. Reset healthy or load a seeded 30-second snapshot at any time.
6. Try 64 possible futures. Each small tile is the selected relay two seconds
   later under a different random stream. This is not a game survival prediction;
   64 trials also have substantial sampling uncertainty.

Space pauses/plays; 1–4 use the four rings. Default playback is slowed down so
you can follow the changes. Each step still represents exactly 0.1 simulated seconds.

## What a relay stores

Think of a tiny synchronous hardware state machine: **one fault flip-flop and a
three-bit warning counter**. The 128 states occupy 512 bits, or 64 bytes. The
separate table of connections and fault rates occupies 512 bytes.

Each relay reads three other relays' **fault bits**, not their warning counters.
It may fault spontaneously, because two specified neighbors both fail, because
a third neighbor fails, or because its own earlier fault persists. Random draws
make these events probabilistic. Its warning counter rises by one on a fault and
falls by two on recovery, clamped to 0–7.

Green means warning 0; amber means 1–3; red means 4–7. The little dot is the fault
bit itself. A dot can disappear while the light remains red: recovery started,
but the warning counter has not yet fallen far enough. The example door reads
this counter; actual game obstacles read the relay assigned to them.

“Coupled” just means **one relay's fault can influence another's next state**.
This synthetic model was introduced as an FPGA candidate. It does not force
Monte Carlo, demonstrate realistic reactor physics, or rule out cheaper predictors.

## Laptop measurements

Measured on 2026-11-10: i7-12700H, RTX 3050 Ti Laptop GPU, optimized C++, four
persistent CPU workers. There is **no game or rendering** in the compute benchmark.
A forecast batch means **4,096 independent futures × 100 reactor updates × 128
relays = 52,428,800 relay transitions**. It starts from the same seeded snapshot
at simulated time 30 seconds. It excludes steering, collision, aggregation and
per-job bulk state readback; it is not the complete game predictor.
Every mode keeps the same Vulkan device context and GPU telemetry sampler active.
“Idle” means this controlled benchmark baseline, not a GPU powered completely off;
GPU wakeup costs and CPU-only deep-idle savings are not measured.

| Work | Observed time | GPU-reported power |
| --- | ---: | ---: |
| Idle benchmark process | No compute | 8.1–13.3 W across sessions |
| One live reactor update on CPU | 13–15 microseconds | No GPU increase distinguishable from background variation |
| Batch on four CPU workers | 0.87–1.36 seconds | **CPU watts unavailable** |
| Batch on GPU | 30–42 milliseconds | 16.1–19.9 W when run continuously |

Idle-subtracted GPU power under continuous batches ranged from **3.0 to 11.8 W**.
Dividing by completed batches gives approximately **0.13–0.36 J/batch** of added
GPU energy. These are noisy sensor-based estimates, not calibrated meter results.
At one GPU batch/second, added power ranged from indistinguishable from idle to
about 1.6 W. That low-duty result is especially sensitive to power states and
background activity. The CPU sometimes missed the requested one-second cadence;
the saved results include actual throughput and missed deadlines.

### What the power sensor measures

The sampler reads NVML GPU power every approximately 0.1 seconds and integrates
it over time. NVIDIA describes this API as GPU power plus associated circuitry,
not CPU or whole-laptop power. The reported value may be averaged rather than an
instantaneous reading. [NVIDIA NVML device queries](https://docs.nvidia.com/deploy/nvml-api/620/api/group__nvmlDeviceQueries.html)

The separate cumulative energy counter contradicted both NVML power readings and
`nvidia-smi`: it implied roughly 73–84 W during the longer runs while the power
sensor reported roughly 13–18 W. It was **rejected**, not used to report an energy
win. Both readings remain in the raw evidence; their disagreement is unresolved.

There is no accessible CPU-package power reading in this setup. The battery was
charging on AC, so battery charge rate is not laptop power. Intel's 45 W processor
base-power rating is not a measurement of this workload and was not substituted.
[Intel processor specifications](https://www.intel.com/content/www/us/en/products/sku/132228/intel-core-i712700h-processor-24m-cache-up-to-4-70-ghz/specifications.html)

For an explicitly **assumed** 10 W increase in CPU power, a measured 0.87–1.36 s
batch would cost 8.7–13.6 J of added CPU energy. This is a sensitivity example,
not a CPU power estimate supported by a sensor. A plug-in whole-system meter
or trustworthy CPU-package energy counter is needed to replace that assumption.

Raw evidence: [measurements](relay-power-results.json) and
[power samples](relay-power-samples.csv). Two repeats requested 12-second windows;
a separate five-second repeat checked the corrected sampler. Idle controls bracket
each repeat, and CPU/GPU order reverses on the second. Timing and power varied;
these results do not establish the best possible CPU/GPU implementation.

## Why an FPGA could save energy

The CPU executes many general-purpose instructions to select a neighbor bit,
combine conditions, and update a three-bit counter. A GPU repeats that work across
many futures. An FPGA could build that operation directly from narrow logic,
registers and local RAM, with a small XOR-based random generator. Less instruction
and data movement overhead is the architectural opportunity—not a free energy win.

For scale only, an unproven lane processing one relay per cycle at 100 MHz would
take 524 ms for this batch; eight lanes would take about 66 ms before stalls.
That is an illustrative schedule, **not measured FPGA speed, resource usage or power**.

Whole-system savings require:

```text
added FPGA + PCIe + host/DMA power < CPU/GPU power actually removed
```

The GPU still renders the game, so its idle/rendering power does not disappear.
At infrequent predictions, the FPGA board and PCIe overhead may cost more than
they save. One live reactor alone is far too cheap to justify this accelerator.
The current evidence supports testing an implementation, not predicting a dramatic
gain. CPU/GPU bit-slicing, SIMD, shared work and cheaper prediction algorithms
remain fair competitors.
These are laptop observations, not Jetson Orin Nano or LiteFury measurements.

## Reproduce the standalone tests

```powershell
make -C graphics/vulkan/tools/relay_lab test
python graphics/vulkan/tools/relay_lab/measure_power.py --seconds 18 --repeats 2
```

The tests compare all state bits in 48 CPU/GPU cases and 72 Python/C++ snapshots,
plus pulse/reroute/reset/injection and the background future preview. Vulkan
synchronization validation also passed. No FPGA driver or bitstream is included.
