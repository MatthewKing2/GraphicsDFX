#!/usr/bin/env python3
"""Run the Vulkan workload matrix and summarize its GPU timestamp CSVs.

Uses only Python's standard library. Measurements are collected by rng_vulkan,
not by timing this subprocess. See README.md for attribution limitations.
"""

import argparse
import csv
import itertools
import json
import math
from pathlib import Path
import random
import shlex
import statistics
import subprocess
import sys


TIME_FIELDS = (
    "rng_ms", "simulation_ms", "compute_ms", "visual_prepare_ms",
    "render_ms", "gpu_ms", "cpu_ms", "upload_ms", "rng_input_ms", "rng_staging_ms",
)
OPTIONAL_TIME_DEFAULTS = {"upload_ms": 0.0, "rng_input_ms": 0.0, "rng_staging_ms": 0.0}
GROUP_FIELDS = (
    "device", "driver_version", "workload", "mode", "count", "blocks",
    "visualization_enabled", "isolation", "timestamp_method", "rng_source",
)
OPTIONAL_GROUP_DEFAULTS = {"isolation": "none", "timestamp_method": "unspecified", "rng_source": "gpu"}
INTEGER_FIELDS = ("count", "blocks", "seed", "frame", "random_values", "operations")
COMPARISON_FIELDS = ("seed", "warmup", "step_size", "visual_samples")


def positive(value):
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def nonnegative(value):
    result = int(value)
    if result < 0:
        raise argparse.ArgumentTypeError("must be zero or greater")
    return result


def integer_list(value):
    try:
        result = [positive(part) for part in value.split(",")]
    except (ValueError, argparse.ArgumentTypeError) as error:
        raise argparse.ArgumentTypeError("expected comma-separated positive integers") from error
    if len(result) != len(set(result)):
        raise argparse.ArgumentTypeError("duplicate sizes would overwrite measurements")
    return result


def percentile(values, fraction):
    """Linearly interpolated quantile, also defined for a single sample."""
    ordered = sorted(values)
    location = (len(ordered) - 1) * fraction
    lower = int(location)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (location - lower)


def distribution(values):
    return {
        "median": statistics.median(values),
        "p95": percentile(values, 0.95),
        "min": min(values),
        "max": max(values),
    }


def ratio(numerator, denominator):
    return numerator / denominator if denominator > 0 else None


def read_measurements(paths):
    rows = []
    required = set(GROUP_FIELDS + INTEGER_FIELDS + TIME_FIELDS).difference(OPTIONAL_GROUP_DEFAULTS).difference(OPTIONAL_TIME_DEFAULTS)
    for path in paths:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            missing = required.difference(reader.fieldnames or [])
            if missing:
                raise ValueError(f"{path}: missing CSV columns {', '.join(sorted(missing))}")
            count = 0
            for line, row in enumerate(reader, start=2):
                try:
                    for field, default in OPTIONAL_GROUP_DEFAULTS.items():
                        row.setdefault(field, default)
                    for field, default in OPTIONAL_TIME_DEFAULTS.items():
                        row.setdefault(field, default)
                    for field in INTEGER_FIELDS:
                        row[field] = int(row[field])
                        if row[field] < 0:
                            raise ValueError(f"negative {field}")
                    for field in TIME_FIELDS:
                        row[field] = float(row[field])
                        if not math.isfinite(row[field]) or row[field] < 0:
                            raise ValueError(f"invalid {field}")
                    row["visualization_enabled"] = int(row["visualization_enabled"])
                    if row["visualization_enabled"] not in (0, 1):
                        raise ValueError("visualization_enabled must be 0 or 1")
                    if row["mode"] not in ("split", "fused"):
                        raise ValueError("unknown mode")
                    if row["workload"] not in ("pi", "walk"):
                        raise ValueError("unknown workload")
                    if row["isolation"] not in ("none", "rng", "consumer"):
                        raise ValueError("unknown isolation")
                    if row["isolation"] != "none" and (row["mode"] != "split" or row["visualization_enabled"]):
                        raise ValueError("isolated measurements require split mode and visualization off")
                    if row["rng_source"] not in ("gpu", "external"):
                        raise ValueError("unknown rng_source")
                    if row["rng_source"] == "external" and (row["mode"] != "split" or row["isolation"] != "none"):
                        raise ValueError("external streams require split mode without isolation")
                    if row["count"] < 1 or row["blocks"] < 1:
                        raise ValueError("empty workload")
                    if row["random_values"] != 4 * row["count"] * row["blocks"]:
                        raise ValueError("random_values does not match count and blocks")
                    expected_operations = 0 if row["isolation"] == "rng" else 2 * row["count"] * row["blocks"]
                    if row["operations"] != expected_operations:
                        raise ValueError("operations does not match count and blocks")
                    if "timestamp_period_ns" in row:
                        row["timestamp_period_ns"] = float(row["timestamp_period_ns"])
                        if not math.isfinite(row["timestamp_period_ns"]) or row["timestamp_period_ns"] <= 0:
                            raise ValueError("invalid timestamp_period_ns")
                    for field in ("warmup", "visual_samples"):
                        if field in row:
                            row[field] = int(row[field])
                            if row[field] < 0 or (field == "visual_samples" and row[field] == 0):
                                raise ValueError(f"invalid {field}")
                    if "step_size" in row:
                        row["step_size"] = float(row["step_size"])
                        if not math.isfinite(row["step_size"]) or not 0 < row["step_size"] <= 1:
                            raise ValueError("invalid step_size")
                except (ValueError, TypeError) as error:
                    raise ValueError(f"{path}:{line}: {error}") from error
                row["source_csv"] = str(path.resolve())
                rows.append(row)
                count += 1
            if count == 0:
                raise ValueError(f"{path}: no measured frames")
    if not rows:
        raise ValueError("no CSV measurements found")
    return rows


def summarize(rows):
    groups = {}
    for row in rows:
        key = tuple(row[field] for field in GROUP_FIELDS)
        groups.setdefault(key, []).append(row)
    results = []
    for key, samples in sorted(groups.items()):
        item = dict(zip(GROUP_FIELDS, key))
        item["samples"] = len(samples)
        item["source_csvs"] = sorted({row["source_csv"] for row in samples})
        item["trials"] = len(item["source_csvs"])
        item["seeds"] = sorted({row["seed"] for row in samples})
        item["run_parameters"] = {}
        for field in COMPARISON_FIELDS:
            values = {row.get(field) for row in samples}
            if len(values) != 1:
                raise ValueError(f"cannot pool different {field} values for {key}; use separate output directories")
            item["run_parameters"][field] = next(iter(values))
        item["timings_ms"] = {field: distribution([row[field] for row in samples]) for field in TIME_FIELDS}
        item["visual_total_ms"] = distribution([row["visual_prepare_ms"] + row["render_ms"] for row in samples])
        split_combined = item["mode"] == "split" and item["isolation"] == "none"
        gpu_split_combined = split_combined and item["rng_source"] == "gpu"
        item["compute_dependency_ms"] = distribution([
            max(0.0, row["compute_ms"] - row["rng_ms"] - row["upload_ms"] - row["simulation_ms"])
            if split_combined else 0.0 for row in samples
        ])
        item["scope_dependency_ms"] = distribution([
            max(0.0, row["gpu_ms"] - row["compute_ms"] - row["visual_prepare_ms"] - row["render_ms"])
            for row in samples
        ])
        # This fraction attributes the two separately dispatched passes only.
        # RNG includes writes to the materialized stream; simulation includes reads.
        split_shares = [ratio(row["rng_ms"], row["rng_ms"] + row["simulation_ms"])
                        for row in samples] if gpu_split_combined else []
        split_shares = [share for share in split_shares if share is not None]
        item["split_rng_dispatch_fraction"] = distribution(split_shares) if split_shares else None
        compute_shares = [ratio(row["rng_ms"], row["compute_ms"])
                          for row in samples] if gpu_split_combined else []
        compute_shares = [share for share in compute_shares if share is not None]
        item["split_rng_compute_fraction"] = distribution(compute_shares) if compute_shares else None
        visual_shares = [ratio(row["visual_prepare_ms"] + row["render_ms"], row["gpu_ms"])
                         for row in samples]
        visual_shares = [share for share in visual_shares if share is not None]
        item["visualization_fraction"] = distribution(visual_shares) if visual_shares else None
        rates = [ratio(row["random_values"], row["compute_ms"] * 1e6) for row in samples]
        rates = [rate for rate in rates if rate is not None]
        item["effective_random_gvalues_per_s"] = distribution(rates) if rates else None
        operation_rates = [ratio(row["operations"], row["compute_ms"] * 1e6) for row in samples]
        operation_rates = [rate for rate in operation_rates if rate is not None]
        item["goperations_per_s"] = distribution(operation_rates) if operation_rates else None
        envelope_rates = [ratio(row["random_values"], row["gpu_ms"] * 1e6) for row in samples]
        envelope_rates = [rate for rate in envelope_rates if rate is not None]
        item["envelope_random_gvalues_per_s"] = distribution(envelope_rates) if envelope_rates else None
        periods = sorted({row["timestamp_period_ns"] for row in samples if "timestamp_period_ns" in row})
        if periods:
            item["timestamp_periods_ns"] = periods
        results.append(item)
    return results


def paired_group(group, groups, **differences):
    return next((other for other in groups
                 if all(other[field] == value for field, value in differences.items())
                 and all(other[field] == group[field] for field in GROUP_FIELDS if field not in differences)
                 and other["run_parameters"] == group["run_parameters"]), None)


def markdown_report(groups):
    lines = [
        "# Vulkan RNG workload measurements", "",
        "GPU timestamp measurements; warmup frames are excluded. Times are milliseconds.",
        "The tables pool frame samples across trials; p95 describes frame-time variation, not a confidence interval.", "",
        "**Timestamp scope:** MoltenVK may defer phase timestamps until a Metal encoder ends. "
        "timestamp_method=encoder_boundaries inserts probe/sentinel transfers to establish encoder boundaries, adding instrumentation overhead. "
        "Individual phase windows and their fractions remain approximate. The independent RNG-only and consumer-only submission "
        "envelopes below provide a stronger cross-check using gpu_ms. Legacy data with timestamp_method=unspecified must not be treated as equivalent.", "",
        "## Combined workloads (approximate phase windows)", "",
        "| Device | Timestamp method | Workload | Mode | Count | Blocks | Visual | Frames | RNG median | Simulation median | Compute median / p95 | RNG / compute median | Compute gap median | Visual prepare + draw median | Scope gap median | GPU total median |",
        "| --- | --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    combined = [group for group in groups if group["isolation"] == "none" and group["rng_source"] == "gpu"]
    for group in combined:
        timing = group["timings_ms"]
        device = group["device"].replace("|", "\\|")
        rng = f"{timing['rng_ms']['median']:.4f}" if group["mode"] == "split" else "included in simulation"
        rng_fraction = group["split_rng_compute_fraction"]
        rng_compute = f"{100 * rng_fraction['median']:.1f}%" if rng_fraction else "unattributed"
        visual = group["visual_total_ms"]["median"]
        lines.append(
            f"| {device} | {group['timestamp_method']} | {group['workload']} | {group['mode']} | {group['count']} | {group['blocks']} | "
            f"{'on' if group['visualization_enabled'] else 'off'} | {group['samples']} | {rng} | "
            f"{timing['simulation_ms']['median']:.4f} | {timing['compute_ms']['median']:.4f} / "
            f"{timing['compute_ms']['p95']:.4f} | {rng_compute} | {group['compute_dependency_ms']['median']:.4f} | "
            f"{visual:.4f} | {group['scope_dependency_ms']['median']:.4f} | {timing['gpu_ms']['median']:.4f} |"
        )
    external = [group for group in groups if group["rng_source"] == "external"]
    lines += ["", "## External RNG streams", "",
              "External streams are read and metadata-checked into aligned CPU RAM, copied into a reused mapped Vulkan staging buffer, "
              "then copied to the GPU random buffer. rng_input_ms includes blocking stream/driver reads and metadata checks; "
              "rng_staging_ms measures the CPU memcpy into Vulkan staging; upload_ms is the "
              "GPU staging-to-device copy window. compute_ms includes upload plus the consumer. cpu_ms includes the input wait, "
              "upload/consumer submission and wait, and any presentation. No GPU RNG instruction fraction is attributed to these runs. "
              "A file or CPU producer is a protocol fixture, not evidence of FPGA or PCIe performance.", ""]
    if external:
        lines += ["| Device | Timestamp method | Workload | Count | Blocks | Visual | Frames | Input CPU median / p95 | Staging CPU median | Upload GPU median | Consumer median | Compute median | GPU total median | Frame CPU median / p95 |",
                  "| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for group in external:
            timing = group["timings_ms"]
            device = group["device"].replace("|", "\\|")
            lines.append(f"| {device} | {group['timestamp_method']} | {group['workload']} | {group['count']} | {group['blocks']} | "
                         f"{'on' if group['visualization_enabled'] else 'off'} | {group['samples']} | "
                         f"{timing['rng_input_ms']['median']:.4f} / {timing['rng_input_ms']['p95']:.4f} | {timing['rng_staging_ms']['median']:.4f} | {timing['upload_ms']['median']:.4f} | "
                         f"{timing['simulation_ms']['median']:.4f} | {timing['compute_ms']['median']:.4f} | {timing['gpu_ms']['median']:.4f} | "
                         f"{timing['cpu_ms']['median']:.4f} / {timing['cpu_ms']['p95']:.4f} |")
    else:
        lines.append("No external stream measurements supplied. The matrix runner collects GPU-generated baselines only.")
    lines += ["", "## Independent submission envelopes", "",
              "RNG-only submits generation without simulation. Consumer-only pre-generates that epoch's exact stream in a separate "
              "untimed submission, then measures only the consumer. Both require headless split mode with visualization off. "
              "The fraction is RNG-only median gpu_ms / (RNG-only median gpu_ms + consumer-only median gpu_ms). "
              "It combines independent submit scopes, including each scope's instrumentation/dependency overhead; it is not an "
              "end-to-end application fraction or a prediction of FPGA speedup. Consumer-only cpu_ms includes its untimed RNG preparation.", "",
              "| Device | Timestamp method | Workload | Count | Blocks | Frames RNG / consumer | RNG-only GPU median / p95 | Consumer-only GPU median / p95 | RNG fraction of independent envelopes |",
              "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    isolated = [group for group in groups if group["isolation"] != "none"]
    for group in isolated:
        if group["isolation"] == "consumer" and paired_group(group, groups, isolation="rng"):
            continue
        rng_group = group if group["isolation"] == "rng" else None
        consumer_group = paired_group(group, groups, isolation="consumer") if rng_group else group
        rng_time = rng_group["timings_ms"]["gpu_ms"] if rng_group else None
        consumer_time = consumer_group["timings_ms"]["gpu_ms"] if consumer_group else None
        fraction = ratio(rng_time["median"], rng_time["median"] + consumer_time["median"]) if rng_time and consumer_time else None
        rng_text = f"{rng_time['median']:.4f} / {rng_time['p95']:.4f}" if rng_time else "missing"
        consumer_text = f"{consumer_time['median']:.4f} / {consumer_time['p95']:.4f}" if consumer_time else "missing"
        fraction_text = f"{100*fraction:.1f}%" if fraction is not None else "unpaired"
        frames = f"{rng_group['samples'] if rng_group else 0} / {consumer_group['samples'] if consumer_group else 0}"
        device = group["device"].replace("|", "\\|")
        lines.append(f"| {device} | {group['timestamp_method']} | {group['workload']} | {group['count']} | {group['blocks']} | {frames} | {rng_text} | {consumer_text} | {fraction_text} |")
    if not isolated:
        lines.append("| No isolated submission measurements supplied | | | | | | | | |")
    lines += ["", "## Approximate phase attribution and combined comparisons", ""]
    for group in combined:
        name = f"{group['device']} / {group['timestamp_method']} / {group['workload']} / {group['count']} × {group['blocks']} / visual {'on' if group['visualization_enabled'] else 'off'}"
        fraction = group["split_rng_dispatch_fraction"]
        if fraction:
            lines.append(f"- {name}: RNG is {100 * fraction['median']:.1f}% of the split RNG + consumer dispatch windows.")
            lines.append(f"- {name}: RNG is {100 * group['split_rng_compute_fraction']['median']:.1f}% of the complete split compute span, including its dependency gaps.")
        counterpart = paired_group(group, combined, mode="fused")
        if group["mode"] == "split" and counterpart:
            speedup = ratio(group["timings_ms"]["compute_ms"]["median"], counterpart["timings_ms"]["compute_ms"]["median"])
            if speedup is not None:
                lines.append(f"- {name}: fused compute is {speedup:.2f}× the split compute throughput (same random streams and useful work).")
    lines += [
        "", "## Interpretation limits", "",
        "A large split RNG fraction is evidence to investigate, not proof that an optimized application is RNG-bound. "
        "The split RNG pass includes stream stores and the consumer includes stream reads. Fused mode removes this traffic; "
        "its RNG and simulation instructions cannot be assigned separate timestamp durations. On MoltenVK, prefer the whole-submit "
        "isolated envelope cross-check over a conclusion based on individual phase fractions.", "",
        "RNG / compute is the median of each frame's rng_ms / compute_ms, rather than a ratio of separately pooled medians. "
        "The dispatch-only RNG fraction excludes dependencies between passes. Compute gap is max(0, compute_ms - rng_ms - upload_ms - simulation_ms) "
        "for combined split mode (zero for fused/isolated); scope gap is max(0, gpu_ms - compute_ms - visual_prepare_ms - render_ms). "
        "These approximate residual scopes include dependency and measurement effects; they are not precise barrier-cost counters. "
        "Negative residuals from timestamp-scope approximation are clamped to zero. Separate medians need not add to the total median.", "",
        "Use the visualization-off rows and size sweeps to check whether rendering, dispatch overhead, or memory traffic "
        "dominates. The offscreen draw is measured; interactive presentation/blit, CPU submission, and final readback are "
        "outside the GPU phase timing. cpu_ms is a separate wall-time observation, including frame submission/wait and presentation.", "",
        "GPU-generated baselines and external stream measurements remain separate. FPGA speedup requires an actual FPGA producer "
        "plus measured stream transfer, synchronization, consumption, and end-to-end frame costs. CPU-generated fixtures establish "
        "protocol correctness and do not establish FPGA acceleration.", "",
        "summary.json retains median, p95, min, max, trial counts, device/driver identifiers, throughput, and source CSV paths. "
        "Raw CSVs and subprocess logs are retained for reanalysis.", "",
    ]
    return "\n".join(lines)


def collect(args):
    executable = args.exe.expanduser().resolve()
    if not executable.is_file():
        raise ValueError(f"executable not found: {executable}; build the application first")
    frames = args.frames if args.frames is not None else (20 if args.quick else 120)
    warmup = args.warmup if args.warmup is not None else (5 if args.quick else 20)
    trials = args.trials if args.trials is not None else (1 if args.quick else 3)
    jobs = [(count, blocks, workload, mode, visual, "none", trial)
            for count, blocks, workload, mode, visual, trial in
            itertools.product(args.counts, args.blocks, ("pi", "walk"), ("split", "fused"), (False, True), range(trials))]
    jobs += [(count, blocks, workload, "split", False, isolation, trial)
             for count, blocks, workload, isolation, trial in
             itertools.product(args.counts, args.blocks, ("pi", "walk"), ("rng", "consumer"), range(trials))]
    # Randomized order reduces systematic thermal/order bias. The order is reproducible.
    random.Random(args.order_seed).shuffle(jobs)
    destinations = []
    for count, blocks, workload, mode, visual, isolation, trial in jobs:
        suffix = f"_isolate{isolation}" if isolation != "none" else ""
        name = f"{workload}_{mode}_n{count}_b{blocks}_viz{int(visual)}{suffix}_trial{trial}"
        destinations.append((name, args.output / f"{name}.csv"))
    for name, path in destinations:
        if path.exists() or (args.output / f"{name}.log").exists():
            raise ValueError(f"refusing to overwrite {name}; use a new --output directory or --summarize-only")
    manifest = {
        "executable": str(executable), "frames_per_trial": frames, "warmup_frames": warmup,
        "trials": trials, "counts": args.counts, "blocks": args.blocks, "seed": args.seed,
        "order_seed": args.order_seed, "isolations": ["none", "rng", "consumer"], "runs": [],
    }
    for index, (job, destination) in enumerate(zip(jobs, destinations), start=1):
        count, blocks, workload, mode, visual, isolation, trial = job
        name, csv_path = destination
        command = [str(executable), "--workload", workload, "--mode", mode,
                   "--count", str(count), "--blocks", str(blocks), "--frames", str(frames),
                   "--warmup", str(warmup), "--seed", str(args.seed), "--csv", str(csv_path)]
        if isolation != "none":
            command.extend(["--isolate", isolation])
        command.append("--headless")
        if not visual:
            command.append("--no-viz")
        print(f"[{index}/{len(jobs)}] {name}", flush=True)
        log_path = args.output / f"{name}.log"
        with log_path.open("w", encoding="utf-8") as log:
            log.write(shlex.join(command) + "\n\n")
            log.flush()
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
        manifest["runs"].append({"name": name, "command": command, "exit_code": result.returncode,
                                 "csv": str(csv_path.resolve()), "log": str(log_path.resolve())})
        (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"{name} exited with {result.returncode}; see {log_path}")
        if not csv_path.is_file():
            raise RuntimeError(f"{name} did not create {csv_path}; see {log_path}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="directory for raw CSVs, logs, and reports")
    parser.add_argument("--exe", type=Path, default=Path(__file__).resolve().parents[1] / "run.sh", help="application or launcher path")
    parser.add_argument("--counts", type=integer_list, default=[65536], help="comma-separated invocation counts")
    parser.add_argument("--blocks", type=integer_list, default=[16], help="comma-separated Philox blocks per invocation")
    parser.add_argument("--frames", type=positive, help="measured frames per run (default 120; quick 20)")
    parser.add_argument("--warmup", type=nonnegative, help="unmeasured warmup frames (default 20; quick 5)")
    parser.add_argument("--trials", type=positive, help="independent process runs per configuration (default 3; quick 1)")
    parser.add_argument("--seed", type=nonnegative, default=42, help="same Philox seed for all configurations")
    parser.add_argument("--order-seed", type=nonnegative, default=1028, help="reproducible run-order shuffle")
    parser.add_argument("--quick", action="store_true", help="short smoke-profile matrix; use normal defaults for measurements")
    parser.add_argument("--summarize-only", action="store_true", help="read existing output CSVs without launching workloads")
    args = parser.parse_args(argv)
    args.output = args.output.expanduser().resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        if not args.summarize_only:
            collect(args)
        groups = summarize(read_measurements(sorted(args.output.glob("*.csv"))))
        (args.output / "summary.json").write_text(json.dumps(groups, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        (args.output / "report.md").write_text(markdown_report(groups), encoding="utf-8")
    except (OSError, ValueError, RuntimeError) as error:
        print(f"profile: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {args.output / 'report.md'} and {args.output / 'summary.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
