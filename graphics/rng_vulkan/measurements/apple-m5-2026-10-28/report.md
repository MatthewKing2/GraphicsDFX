# Vulkan RNG workload measurements

GPU timestamp measurements; warmup frames are excluded. Times are milliseconds.
The tables pool frame samples across trials; p95 describes frame-time variation, not a confidence interval.

**Timestamp scope:** MoltenVK may defer phase timestamps until a Metal encoder ends. timestamp_method=encoder_boundaries inserts probe/sentinel transfers to establish encoder boundaries, adding instrumentation overhead. Individual phase windows and their fractions remain approximate. The independent RNG-only and consumer-only submission envelopes below provide a stronger cross-check using gpu_ms. Legacy data with timestamp_method=unspecified must not be treated as equivalent.

## Combined workloads (approximate phase windows)

| Device | Timestamp method | Workload | Mode | Count | Blocks | Visual | Frames | RNG median | Simulation median | Compute median / p95 | RNG / compute median | Compute gap median | Visual prepare + draw median | Scope gap median | GPU total median |
| --- | --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Apple M5 | encoder_boundaries | pi | fused | 4096 | 16 | off | 360 | included in simulation | 0.0394 | 0.0394 / 0.1316 | unattributed | 0.0000 | 0.0000 | 0.0057 | 0.0454 |
| Apple M5 | encoder_boundaries | pi | fused | 4096 | 16 | on | 360 | included in simulation | 0.0404 | 0.0404 / 0.0616 | unattributed | 0.0000 | 0.0900 | 0.0030 | 0.1408 |
| Apple M5 | encoder_boundaries | pi | fused | 4096 | 64 | off | 360 | included in simulation | 0.0992 | 0.0992 / 0.4154 | unattributed | 0.0000 | 0.0000 | 0.0060 | 0.1052 |
| Apple M5 | encoder_boundaries | pi | fused | 4096 | 64 | on | 360 | included in simulation | 0.1001 | 0.1001 / 0.1118 | unattributed | 0.0000 | 0.0203 | 0.0030 | 0.1236 |
| Apple M5 | encoder_boundaries | pi | fused | 65536 | 16 | off | 360 | included in simulation | 0.2429 | 0.2429 / 0.2442 | unattributed | 0.0000 | 0.0000 | 0.0059 | 0.2489 |
| Apple M5 | encoder_boundaries | pi | fused | 65536 | 16 | on | 360 | included in simulation | 0.2467 | 0.2467 / 0.2558 | unattributed | 0.0000 | 0.0239 | 0.0030 | 0.2737 |
| Apple M5 | encoder_boundaries | pi | fused | 65536 | 64 | off | 360 | included in simulation | 0.9129 | 0.9129 / 1.0097 | unattributed | 0.0000 | 0.0000 | 0.0061 | 0.9190 |
| Apple M5 | encoder_boundaries | pi | fused | 65536 | 64 | on | 360 | included in simulation | 0.9198 | 0.9198 / 3.7913 | unattributed | 0.0000 | 0.0240 | 0.0030 | 0.9468 |
| Apple M5 | encoder_boundaries | pi | split | 4096 | 16 | off | 360 | 0.0273 | 0.0046 | 0.0334 / 0.1347 | 81.6% | 0.0015 | 0.0000 | 0.0112 | 0.0575 |
| Apple M5 | encoder_boundaries | pi | split | 4096 | 16 | on | 360 | 0.0296 | 0.0067 | 0.0380 / 0.1405 | 79.3% | 0.0015 | 0.1041 | 0.0086 | 0.1787 |
| Apple M5 | encoder_boundaries | pi | split | 4096 | 64 | off | 360 | 0.0826 | 0.0345 | 0.1190 / 0.1558 | 69.4% | 0.0020 | 0.0000 | 0.0061 | 0.1251 |
| Apple M5 | encoder_boundaries | pi | split | 4096 | 64 | on | 360 | 0.0834 | 0.0322 | 0.1175 / 0.1598 | 70.9% | 0.0019 | 0.0196 | 0.0038 | 0.1409 |
| Apple M5 | encoder_boundaries | pi | split | 65536 | 16 | off | 360 | 0.2144 | 0.1072 | 0.3935 / 0.5310 | 54.5% | 0.0743 | 0.0000 | 0.0060 | 0.3995 |
| Apple M5 | encoder_boundaries | pi | split | 65536 | 16 | on | 360 | 0.2147 | 0.1077 | 0.4008 / 0.4360 | 54.0% | 0.0780 | 0.0241 | 0.0040 | 0.4290 |
| Apple M5 | encoder_boundaries | pi | split | 65536 | 64 | off | 360 | 0.9434 | 0.3495 | 1.5772 / 1.8534 | 60.5% | 0.2715 | 0.0000 | 0.0075 | 1.5859 |
| Apple M5 | encoder_boundaries | pi | split | 65536 | 64 | on | 360 | 0.8357 | 0.3451 | 1.4678 / 3.4834 | 58.3% | 0.2725 | 0.0246 | 0.0044 | 1.4987 |
| Apple M5 | encoder_boundaries | walk | fused | 4096 | 16 | off | 360 | included in simulation | 0.0438 | 0.0438 / 0.0756 | unattributed | 0.0000 | 0.0000 | 0.0060 | 0.0498 |
| Apple M5 | encoder_boundaries | walk | fused | 4096 | 16 | on | 360 | included in simulation | 0.0462 | 0.0462 / 0.1178 | unattributed | 0.0000 | 0.0800 | 0.0030 | 0.1326 |
| Apple M5 | encoder_boundaries | walk | fused | 4096 | 64 | off | 360 | included in simulation | 0.1144 | 0.1144 / 0.1202 | unattributed | 0.0000 | 0.0000 | 0.0060 | 0.1203 |
| Apple M5 | encoder_boundaries | walk | fused | 4096 | 64 | on | 360 | included in simulation | 0.1154 | 0.1154 / 0.1289 | unattributed | 0.0000 | 0.0198 | 0.0030 | 0.1382 |
| Apple M5 | encoder_boundaries | walk | fused | 65536 | 16 | off | 360 | included in simulation | 0.2885 | 0.2885 / 0.3060 | unattributed | 0.0000 | 0.0000 | 0.0060 | 0.2944 |
| Apple M5 | encoder_boundaries | walk | fused | 65536 | 16 | on | 360 | included in simulation | 0.2899 | 0.2899 / 0.3016 | unattributed | 0.0000 | 0.0227 | 0.0030 | 0.3156 |
| Apple M5 | encoder_boundaries | walk | fused | 65536 | 64 | off | 360 | included in simulation | 1.0890 | 1.0890 / 1.2022 | unattributed | 0.0000 | 0.0000 | 0.0061 | 1.0950 |
| Apple M5 | encoder_boundaries | walk | fused | 65536 | 64 | on | 360 | included in simulation | 1.0940 | 1.0940 / 1.2070 | unattributed | 0.0000 | 0.0232 | 0.0031 | 1.1201 |
| Apple M5 | encoder_boundaries | walk | split | 4096 | 16 | off | 360 | 0.0272 | 0.0093 | 0.0381 / 0.1163 | 71.5% | 0.0015 | 0.0000 | 0.0067 | 0.0450 |
| Apple M5 | encoder_boundaries | walk | split | 4096 | 16 | on | 360 | 0.0281 | 0.0093 | 0.0403 / 0.1590 | 71.6% | 0.0015 | 0.1250 | 0.0031 | 0.1754 |
| Apple M5 | encoder_boundaries | walk | split | 4096 | 64 | off | 360 | 0.0824 | 0.0365 | 0.1207 / 0.1504 | 68.3% | 0.0018 | 0.0000 | 0.0061 | 0.1268 |
| Apple M5 | encoder_boundaries | walk | split | 4096 | 64 | on | 360 | 0.0827 | 0.0348 | 0.1192 / 0.2900 | 69.5% | 0.0017 | 0.0192 | 0.0036 | 0.1420 |
| Apple M5 | encoder_boundaries | walk | split | 65536 | 16 | off | 360 | 0.2143 | 0.1844 | 0.4062 / 0.4347 | 52.8% | 0.0066 | 0.0000 | 0.0060 | 0.4122 |
| Apple M5 | encoder_boundaries | walk | split | 65536 | 16 | on | 360 | 0.2146 | 0.1848 | 0.4072 / 0.4316 | 52.9% | 0.0068 | 0.0234 | 0.0038 | 0.4344 |
| Apple M5 | encoder_boundaries | walk | split | 65536 | 64 | off | 360 | 1.0563 | 0.6470 | 1.7281 / 2.8828 | 60.6% | 0.0113 | 0.0000 | 0.0085 | 1.7366 |
| Apple M5 | encoder_boundaries | walk | split | 65536 | 64 | on | 360 | 0.8639 | 0.6104 | 1.5188 / 1.8651 | 58.4% | 0.0091 | 0.0245 | 0.0052 | 1.5561 |

## External RNG streams

External streams are read and metadata-checked into aligned CPU RAM, copied into a reused mapped Vulkan staging buffer, then copied to the GPU random buffer. rng_input_ms includes blocking stream/driver reads and metadata checks; rng_staging_ms measures the CPU memcpy into Vulkan staging; upload_ms is the GPU staging-to-device copy window. compute_ms includes upload plus the consumer. cpu_ms includes the input wait, upload/consumer submission and wait, and any presentation. No GPU RNG instruction fraction is attributed to these runs. A file or CPU producer is a protocol fixture, not evidence of FPGA or PCIe performance.

No external stream measurements supplied. The matrix runner collects GPU-generated baselines only.

## Independent submission envelopes

RNG-only submits generation without simulation. Consumer-only pre-generates that epoch's exact stream in a separate untimed submission, then measures only the consumer. Both require headless split mode with visualization off. The fraction is RNG-only median gpu_ms / (RNG-only median gpu_ms + consumer-only median gpu_ms). It combines independent submit scopes, including each scope's instrumentation/dependency overhead; it is not an end-to-end application fraction or a prediction of FPGA speedup. Consumer-only cpu_ms includes its untimed RNG preparation.

| Device | Timestamp method | Workload | Count | Blocks | Frames RNG / consumer | RNG-only GPU median / p95 | Consumer-only GPU median / p95 | RNG fraction of independent envelopes |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Apple M5 | encoder_boundaries | pi | 4096 | 16 | 360 / 360 | 0.0396 / 0.1480 | 0.0409 / 0.1269 | 49.2% |
| Apple M5 | encoder_boundaries | pi | 4096 | 64 | 360 / 360 | 0.0912 / 0.1457 | 0.0582 / 0.0898 | 61.0% |
| Apple M5 | encoder_boundaries | pi | 65536 | 16 | 360 / 360 | 0.2230 / 0.2900 | 0.1931 / 0.2101 | 53.6% |
| Apple M5 | encoder_boundaries | pi | 65536 | 64 | 360 / 360 | 0.8645 / 1.0705 | 0.6071 / 0.6485 | 58.7% |
| Apple M5 | encoder_boundaries | walk | 4096 | 16 | 360 / 360 | 0.0397 / 0.1476 | 0.0414 / 0.1249 | 49.0% |
| Apple M5 | encoder_boundaries | walk | 4096 | 64 | 360 / 360 | 0.0914 / 0.1353 | 0.0602 / 0.0887 | 60.3% |
| Apple M5 | encoder_boundaries | walk | 65536 | 16 | 360 / 360 | 0.2231 / 0.2721 | 0.2014 / 0.2160 | 52.6% |
| Apple M5 | encoder_boundaries | walk | 65536 | 64 | 360 / 360 | 0.8404 / 0.9934 | 0.6261 / 1.1413 | 57.3% |

## Approximate phase attribution and combined comparisons

- Apple M5 / encoder_boundaries / pi / 4096 × 16 / visual off: RNG is 85.5% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 4096 × 16 / visual off: RNG is 81.6% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 4096 × 16 / visual off: fused compute is 0.85× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / pi / 4096 × 16 / visual on: RNG is 82.9% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 4096 × 16 / visual on: RNG is 79.3% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 4096 × 16 / visual on: fused compute is 0.94× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / pi / 4096 × 64 / visual off: RNG is 70.6% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 4096 × 64 / visual off: RNG is 69.4% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 4096 × 64 / visual off: fused compute is 1.20× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / pi / 4096 × 64 / visual on: RNG is 72.1% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 4096 × 64 / visual on: RNG is 70.9% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 4096 × 64 / visual on: fused compute is 1.17× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / pi / 65536 × 16 / visual off: RNG is 66.9% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 65536 × 16 / visual off: RNG is 54.5% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 65536 × 16 / visual off: fused compute is 1.62× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / pi / 65536 × 16 / visual on: RNG is 66.7% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 65536 × 16 / visual on: RNG is 54.0% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 65536 × 16 / visual on: fused compute is 1.62× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / pi / 65536 × 64 / visual off: RNG is 72.7% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 65536 × 64 / visual off: RNG is 60.5% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 65536 × 64 / visual off: fused compute is 1.73× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / pi / 65536 × 64 / visual on: RNG is 71.8% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / pi / 65536 × 64 / visual on: RNG is 58.3% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / pi / 65536 × 64 / visual on: fused compute is 1.60× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 4096 × 16 / visual off: RNG is 74.5% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 4096 × 16 / visual off: RNG is 71.5% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 4096 × 16 / visual off: fused compute is 0.87× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 4096 × 16 / visual on: RNG is 74.6% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 4096 × 16 / visual on: RNG is 71.6% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 4096 × 16 / visual on: fused compute is 0.87× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 4096 × 64 / visual off: RNG is 69.4% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 4096 × 64 / visual off: RNG is 68.3% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 4096 × 64 / visual off: fused compute is 1.05× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 4096 × 64 / visual on: RNG is 70.5% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 4096 × 64 / visual on: RNG is 69.5% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 4096 × 64 / visual on: fused compute is 1.03× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 65536 × 16 / visual off: RNG is 53.7% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 65536 × 16 / visual off: RNG is 52.8% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 65536 × 16 / visual off: fused compute is 1.41× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 65536 × 16 / visual on: RNG is 53.8% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 65536 × 16 / visual on: RNG is 52.9% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 65536 × 16 / visual on: fused compute is 1.40× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 65536 × 64 / visual off: RNG is 61.3% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 65536 × 64 / visual off: RNG is 60.6% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 65536 × 64 / visual off: fused compute is 1.59× the split compute throughput (same random streams and useful work).
- Apple M5 / encoder_boundaries / walk / 65536 × 64 / visual on: RNG is 59.3% of the split RNG + consumer dispatch windows.
- Apple M5 / encoder_boundaries / walk / 65536 × 64 / visual on: RNG is 58.4% of the complete split compute span, including its dependency gaps.
- Apple M5 / encoder_boundaries / walk / 65536 × 64 / visual on: fused compute is 1.39× the split compute throughput (same random streams and useful work).

## Interpretation limits

A large split RNG fraction is evidence to investigate, not proof that an optimized application is RNG-bound. The split RNG pass includes stream stores and the consumer includes stream reads. Fused mode removes this traffic; its RNG and simulation instructions cannot be assigned separate timestamp durations. On MoltenVK, prefer the whole-submit isolated envelope cross-check over a conclusion based on individual phase fractions.

RNG / compute is the median of each frame's rng_ms / compute_ms, rather than a ratio of separately pooled medians. The dispatch-only RNG fraction excludes dependencies between passes. Compute gap is max(0, compute_ms - rng_ms - upload_ms - simulation_ms) for combined split mode (zero for fused/isolated); scope gap is max(0, gpu_ms - compute_ms - visual_prepare_ms - render_ms). These approximate residual scopes include dependency and measurement effects; they are not precise barrier-cost counters. Negative residuals from timestamp-scope approximation are clamped to zero. Separate medians need not add to the total median.

Use the visualization-off rows and size sweeps to check whether rendering, dispatch overhead, or memory traffic dominates. The offscreen draw is measured; interactive presentation/blit, CPU submission, and final readback are outside the GPU phase timing. cpu_ms is a separate wall-time observation, including frame submission/wait and presentation.

GPU-generated baselines and external stream measurements remain separate. FPGA speedup requires an actual FPGA producer plus measured stream transfer, synchronization, consumption, and end-to-end frame costs. CPU-generated fixtures establish protocol correctness and do not establish FPGA acceleration.

summary.json retains median, p95, min, max, trial counts, device/driver identifiers, throughput, and source CSV paths. Raw CSVs and subprocess logs are retained for reanalysis.
