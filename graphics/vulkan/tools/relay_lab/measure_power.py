"""Device-scope NVML energy measurements, NOT CPU or whole-laptop watts.

No administrator driver, external packages, rendering, or game changes.
Run from any directory: python measure_power.py --seconds 18 --repeats 2
"""
import argparse
import csv
import ctypes as C
import json
from pathlib import Path
import queue
import statistics
import subprocess
import threading
import time

HERE = Path(__file__).resolve().parent
EXE = HERE / "../../build/relay_lab/compute.exe"
ROOT = HERE.parents[3]


class Nvml:
    def __init__(self):
        self.lib = C.WinDLL("nvml.dll")
        self.check(self.lib.nvmlInit_v2())
        self.handle = C.c_void_p()
        self.check(self.lib.nvmlDeviceGetHandleByIndex_v2(0, C.byref(self.handle)))
        name = C.create_string_buffer(128)
        self.check(self.lib.nvmlDeviceGetName(self.handle, name, len(name)))
        self.name = name.value.decode()
        self.energy_supported = self.energy() is not None

    @staticmethod
    def check(code):
        if code:
            raise RuntimeError(f"NVML error {code}")

    def energy(self):
        energy = C.c_ulonglong()
        code = self.lib.nvmlDeviceGetTotalEnergyConsumption(self.handle, C.byref(energy))
        return energy.value / 1000 if code == 0 else None

    def sample(self):
        power = C.c_uint()
        self.check(self.lib.nvmlDeviceGetPowerUsage(self.handle, C.byref(power)))
        class Utilization(C.Structure):
            _fields_ = [("gpu", C.c_uint), ("memory", C.c_uint)]
        util = Utilization()
        code = self.lib.nvmlDeviceGetUtilizationRates(self.handle, C.byref(util))
        return {"clock": time.perf_counter(), "gpu_w": power.value / 1000,
                "gpu_util_percent": util.gpu if code == 0 else None,
                "gpu_total_energy_j": self.energy()}

    def close(self):
        self.lib.nvmlShutdown()


def run_case(nvml, mode, seconds, period, samples, steps, repeat):
    backend = "gpu" if mode.startswith("gpu") else "cpu" if mode.startswith("cpu") else mode
    period = 0 if mode.endswith("saturated") else period
    args = [str(EXE), "--backend", backend, "--seconds", str(seconds), "--warmup", "3",
            "--period-ms", str(period), "--samples", str(samples), "--steps", str(steps)]
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, creationflags=subprocess.CREATE_NO_WINDOW)
    events = queue.Queue()
    def reader():
        for line in process.stdout:
            try:
                events.put(json.loads(line))
            except json.JSONDecodeError:
                events.put({"event": "error", "text": line})
        events.put({"event": "eof"})
    threading.Thread(target=reader, daemon=True).start()
    points, result, start, finish = [], None, None, None
    deadline = time.perf_counter() + seconds + 90
    try:
        while time.perf_counter() < deadline:
            try:
                event = events.get(timeout=.1)
            except queue.Empty:
                if start is not None and finish is None:
                    points.append(nvml.sample())
                continue
            if event["event"] == "measure_start":
                start = nvml.sample()
                points.append(start)
            elif event["event"] == "measure_end":
                finish = nvml.sample()
                points.append(finish)
                result = event
            elif event["event"] == "checksum" and result:
                result["checksum"] = event["value"]
            elif event["event"] == "error":
                raise RuntimeError(event["text"])
            elif event["event"] == "eof":
                break
        else:
            raise TimeoutError("power case timed out")
        process.wait(timeout=15)
        errors = process.stderr.read()
        if process.returncode or result is None or start is None or finish is None:
            raise RuntimeError(f"benchmark failed: {errors}")
        if "Validation Error" in errors or "SYNC-HAZARD" in errors:
            raise RuntimeError(errors)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    elapsed = finish["clock"] - start["clock"]
    integral = sum((b["clock"] - a["clock"]) * (a["gpu_w"] + b["gpu_w"]) / 2
                   for a, b in zip(points, points[1:]))
    counter = finish["gpu_total_energy_j"]
    counter_energy = (counter - start["gpu_total_energy_j"]
                      if counter is not None and start["gpu_total_energy_j"] is not None else None)
    # Some laptop driver/firmware combinations expose an energy counter that
    # contradicts both NVML power.draw and nvidia-smi. Never silently trust it.
    energy, source = integral, "trapezoidal integration of NVML-reported GPU power"
    result.update(mode=mode, repeat=repeat, measurement_seconds=elapsed,
                  gpu_energy_j=energy, gpu_mean_w=energy / elapsed,
                  gpu_integrated_power_mean_w=integral / elapsed,
                  gpu_mean_util_percent=statistics.mean(p["gpu_util_percent"] for p in points
                                                       if p["gpu_util_percent"] is not None),
                  gpu_energy_source=source, sample_count=len(points),
                  gpu_counter_energy_j=counter_energy,
                  gpu_counter_mean_w=counter_energy / elapsed if counter_energy is not None else None,
                  gpu_counter_rejected=(abs(counter_energy - integral) > .2 * integral
                                        if counter_energy is not None else None),
                  jobs_per_s=result["jobs"] / result["seconds"],
                  cpu_package_w=None, whole_system_w=None)
    origin = start["clock"]
    for point in points:
        point.update(mode=mode, repeat=repeat, seconds=point.pop("clock") - origin)
    return result, points


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=18)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--samples", type=int, default=4096)
    parser.add_argument("--steps", type=int, default=100)
    parser.add_argument("--period-ms", type=float, default=1000)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/relay-power-results.json")
    parser.add_argument("--raw", type=Path, default=ROOT / "docs/relay-power-samples.csv")
    args = parser.parse_args()
    if not 5 <= args.seconds <= 120 or not 1 <= args.repeats <= 5:
        parser.error("use 5..120 seconds and 1..5 repeats")
    subprocess.run(["make", "-C", str(HERE), "all"], check=True)
    subprocess.run([str(EXE), "--validate"], check=True)
    nvml = Nvml()
    rows, raw = [], []
    try:
        # Bracket each repeat with idle. Reverse CPU/GPU order on the second run.
        for repeat in range(args.repeats):
            order = ["idle", "live", "cpu-paced", "gpu-paced", "cpu-saturated", "gpu-saturated", "idle"]
            if repeat % 2:
                order = ["idle", "gpu-paced", "cpu-paced", "gpu-saturated", "cpu-saturated", "live", "idle"]
            first = len(rows)
            for mode in order:
                print(f"Measuring {mode}, repeat {repeat + 1}...", flush=True)
                row, points = run_case(nvml, mode, args.seconds, args.period_ms,
                                       args.samples, args.steps, repeat + 1)
                rows.append(row)
                raw.extend(points)
                print(f"  GPU {row['gpu_mean_w']:.2f} W; {row['jobs_per_s']:.2f} jobs/s; "
                      f"{row['mean_job_ms']:.2f} ms/job", flush=True)
            baseline = (rows[first]["gpu_mean_w"] + rows[-1]["gpu_mean_w"]) / 2
            drift = abs(rows[first]["gpu_mean_w"] - rows[-1]["gpu_mean_w"])
            for row in rows[first:]:
                row["bracket_idle_gpu_w"] = baseline
                row["idle_gpu_drift_w"] = drift
                row["gpu_increment_above_idle_w"] = row["gpu_mean_w"] - baseline
                row["gpu_increment_j_per_job"] = ((row["gpu_mean_w"] - baseline) / row["jobs_per_s"]
                                                   if row["jobs_per_s"] else None)
    finally:
        nvml.close()
    document = {"device": nvml.name, "scope": "GPU plus associated circuitry only; not laptop or CPU power",
                "game_rendered": False, "sample_interval_seconds": .1,
                "work": "One job = independent futures * reactor updates * 128 relays; no collision or player controller",
                "cpu_package_w": None, "whole_system_w": None,
                "limitations": ["GPU power sensor and counter include other applications and desktop work",
                                "Primary energy integrates reported GPU power; contradictory cumulative counter is retained but rejected",
                                "Every mode keeps the same Vulkan device context and GPU sampler; idle is not a powered-off GPU",
                                "Idle subtraction is a bracketed estimate, not a separate rail measurement",
                                "No CPU-package sensor or whole-system meter was used",
                                "Saturated modes have different throughput; compare joules per equal work, not watts alone"],
                "runs": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    with args.raw.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=list(raw[0]))
        writer.writeheader()
        writer.writerows(raw)
    print(f"Saved {args.output} and {args.raw}")


if __name__ == "__main__":
    main()
