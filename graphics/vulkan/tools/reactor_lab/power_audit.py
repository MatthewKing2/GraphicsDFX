"""Repeatable CPU reference timing + offload payload audit, NOT a power meter.

Run from Git Bash: python graphics/vulkan/tools/reactor_lab/power_audit.py
No Jetson/FPGA, PCIe transfer, DFX operation, or optimized GPU baseline is run.
Compare the eventual best CPU/GPU variant, NOT deliberately tiny CPU batches.
The audit does not change gameplay settings, trial counts or spray volume.

Whole-system break-even at identical FPS/quality/prediction deadlines:
    saved Jetson watts > added FPGA-board + link watts + DFX joules/interval.
Do not count FPGA idle/PCIe power as zero just because compute is duty-cycled.
An FPGA kernel energy win alone does not establish that inequality.
"""
import argparse
import contextlib
import io
import json
from pathlib import Path
import statistics
import time

import numpy as np
from config import DEFAULT_PATH, load_config
from model import Rules, Reactor
from voxel import VoxelWorld,SurfaceWorld


def reactor_audit(cfg, repeats, device_batch_size=8):
    samples = []
    expected = None
    for size in sorted({min(device_batch_size,cfg.forecast.trials),min(cfg.forecast.trial_batch_size,cfg.forecast.trials),cfg.forecast.trials}):
        rules = Rules(load_config(overrides={**cfg.source,"forecast":{**cfg.source["forecast"],"trial_batch_size":size}}))
        world = Reactor(rules)
        timings = []
        for _ in range(repeats):
            start = time.perf_counter()
            result = rules.forecast(world.heat,world.fault,sockets=world.sockets)
            timings.append((time.perf_counter()-start)*1000)
            if expected is None:
                expected = result
            else:
                np.testing.assert_array_equal(result.probability,expected.probability)
                np.testing.assert_array_equal(result.rod_hits,expected.rod_hits)
        samples.append({
            "batch_trials":size,"median_cpu_reference_ms":round(statistics.median(timings),3),
            "persistent_trial_state_bytes":result.persistent_batch_bytes,
            "state_exclusions":"temporary neutron histories, scratch, schedules, reduction counters, topology, shell, extra hardware buffers",
        })
    return {
        "cells":rules.count,"rods":rules.total_rods,"trials":result.trials,"steps_per_future":rules.steps,
        "total_cell_updates_per_forecast":rules.count*result.trials*rules.steps,
        "refresh_seconds":cfg.forecast.refresh_seconds,
        "state_per_cell":"uint8 whole heat + uint8 flags/Q6 heat fraction + uint32 RNG + bool ever-failed (7 host bytes)",
        "thermal_model":"bounded neutron transport + fission heat + diffusion/runaway" if cfg.neutron.enabled else "legacy random thermal events",
        "neutron_sources_per_future_step":rules.total_rods*cfg.neutron.walkers_per_rod if cfg.neutron.enabled else 0,
        "neutron_generations":cfg.neutron.generations if cfg.neutron.enabled else 0,
        "exact_results_equal_across_batch_sizes":True,
        "batch_comparison":samples,
        "rod_result_payload_bytes_excluding_transport_framing":len(result.result_packet()),
        "load_schedule_bytes_uint8":rules.steps*rules.total_rods,
        "note":"Shared load trajectory compiled once; bounded batches target device RAM, NOT CPU handicap. Timing includes Python overhead.",
    }


def audit(cfg, repeats, device_batch_size=8):
    from test_voxel import benchmark
    with contextlib.redirect_stdout(io.StringIO()) as captured:
        benchmark(Rules(cfg))
    particles = json.loads(captured.getvalue())
    # Isolate the idle path without changing game defaults or spawning debris.
    idle_rules = Rules(load_config(overrides={**cfg.source,"voxel":{**cfg.source["voxel"],"debris_every_steps":0}}))
    idle = (SurfaceWorld if cfg.lattice.enabled else VoxelWorld)(idle_rules)
    start = time.perf_counter()
    for _ in range(240):
        idle.step()
    particles["isolated_empty_core_mean_ms"] = round((time.perf_counter()-start)*1000/240,3)
    particles["idle_test_note"] = "Only this isolated empty-core timing disables debris; game defaults unchanged."
    particles["wire_note"] = ("Impact request/accept-bit codecs run in reference; no real transfers. Flight arrays stay host-side. "
                              +("Film snapshot: one byte/site, six moving bits." if cfg.lattice.enabled else "Legacy voxel snapshot: two bits/site."))
    return {
        "measurement":"Laptop CPU reference timings only; milliseconds are not watts.",
        "reactor":reactor_audit(cfg,repeats,device_batch_size),"particles":particles,
        "whole_system_power_watts":None,"fpga_board_power_watts":None,
        "optimized_jetson_baseline_power_watts":None,"dfx_energy_joules":None,
        "power_break_even":"Jetson watts saved > added FPGA board + PCIe watts + amortized DFX joules/second",
        "fair_test":"Same rendering/FPS, output accuracy and deadline; same idle and batching optimizations allowed on CPU/GPU. Include real duty cycles.",
        "hardware_risks":[
            "Small useful workloads can save less than the added board/link idle power.",
            "Cylindrical films, source/drain/forcing and this neutron model are not the measured upstream FPGA kernels.",
            "Random topology reads, RNG state, scratch/double buffers, routing and timing still need synthesis.",
            "Phase exclusivity is software-tested; no runtime DFX integration or energy measurement yet.",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config",type=Path,default=DEFAULT_PATH)
    parser.add_argument("--repeats",type=int,default=3)
    parser.add_argument("--device-batch-size",type=int,default=8)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 20:
        parser.error("--repeats must be between 1 and 20")
    if not 1 <= args.device_batch_size <= 8192:
        parser.error("--device-batch-size must be between 1 and 8192")
    print(json.dumps(audit(load_config(args.config),args.repeats,args.device_batch_size),indent=2))
