"""Regression checks for measurement parsing, grouping, and report attribution."""

import contextlib
import csv
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import profile as profiler


def sample(**changes):
    row = {
        "device": "Test GPU", "driver_version": "123", "workload": "pi",
        "mode": "split", "count": 8, "blocks": 2, "seed": 42, "frame": 0,
        "visualization_enabled": 1, "timestamp_period_ns": 1.0,
        "rng_ms": 3.0, "simulation_ms": 1.0, "compute_ms": 4.5,
        "visual_prepare_ms": 0.2, "render_ms": 0.3, "gpu_ms": 5.0,
        "cpu_ms": 6.0, "random_values": 64, "operations": 32,
    }
    row.update(changes)
    return row


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


class ProfileTests(unittest.TestCase):
    def test_distribution_pools_trials_and_keeps_fused_rng_unattributed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_csv(root / "split0.csv", [sample(frame=0), sample(frame=1, compute_ms=5.5)])
            write_csv(root / "split1.csv", [sample(frame=0, compute_ms=6.5)])
            write_csv(root / "fused.csv", [sample(mode="fused", rng_ms=0.0, simulation_ms=2.0, compute_ms=2.0, gpu_ms=2.5)])
            groups = profiler.summarize(profiler.read_measurements(sorted(root.glob("*.csv"))))
            split = next(group for group in groups if group["mode"] == "split")
            fused = next(group for group in groups if group["mode"] == "fused")
            self.assertEqual(split["samples"], 3)
            self.assertEqual(split["trials"], 2)
            self.assertEqual(split["timings_ms"]["compute_ms"]["median"], 5.5)
            self.assertAlmostEqual(split["timings_ms"]["compute_ms"]["p95"], 6.4)
            self.assertEqual(split["split_rng_dispatch_fraction"]["median"], 0.75)
            self.assertAlmostEqual(split["split_rng_compute_fraction"]["median"], 3 / 5.5)
            self.assertIsNone(fused["split_rng_dispatch_fraction"])
            self.assertIsNone(fused["split_rng_compute_fraction"])
            self.assertEqual(fused["compute_dependency_ms"]["median"], 0)
            self.assertAlmostEqual(fused["effective_random_gvalues_per_s"]["median"], 64 / 2e6)
            report = profiler.markdown_report(groups)
            self.assertIn("2.75×", report)
            self.assertIn("not proof", report)

    def test_dependency_gaps_and_rng_compute_fraction_use_each_frame(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "samples.csv"
            write_csv(path, [sample(rng_ms=8, simulation_ms=2, compute_ms=12,
                                   visual_prepare_ms=1, render_ms=0.5, gpu_ms=15)])
            group = profiler.summarize(profiler.read_measurements([path]))[0]
            self.assertEqual(group["compute_dependency_ms"]["median"], 2)
            self.assertEqual(group["scope_dependency_ms"]["median"], 1.5)
            self.assertEqual(group["split_rng_dispatch_fraction"]["median"], 0.8)
            self.assertAlmostEqual(group["split_rng_compute_fraction"]["median"], 2 / 3)

    def test_approximate_negative_dependency_gaps_are_clamped(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "samples.csv"
            write_csv(path, [sample(rng_ms=3, simulation_ms=2, compute_ms=4, gpu_ms=4)])
            group = profiler.summarize(profiler.read_measurements([path]))[0]
            self.assertEqual(group["compute_dependency_ms"]["median"], 0)
            self.assertEqual(group["scope_dependency_ms"]["median"], 0)

    def test_visualization_off_is_a_separate_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "samples.csv"
            write_csv(path, [sample(), sample(visualization_enabled=0, visual_prepare_ms=0, render_ms=0)])
            groups = profiler.summarize(profiler.read_measurements([path]))
            self.assertEqual(len(groups), 2)
            off = next(group for group in groups if not group["visualization_enabled"])
            self.assertEqual(off["visualization_fraction"]["median"], 0)

    def test_invalid_measurements_are_rejected(self):
        for change in ({"rng_ms": "nan"}, {"compute_ms": -1}, {"random_values": 63}, {"visualization_enabled": 2}):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "invalid.csv"
                write_csv(path, [sample(**change)])
                with self.assertRaisesRegex(ValueError, r"invalid.csv:2:"):
                    profiler.read_measurements([path])

    def test_isolated_envelopes_use_whole_gpu_times_and_zero_rng_operations(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "isolated.csv"
            write_csv(path, [sample(isolation="rng", mode="split", visualization_enabled=0,
                                   operations=0, rng_ms=1, simulation_ms=0, compute_ms=1,
                                   gpu_ms=8, timestamp_method="encoder_boundaries"),
                             sample(isolation="consumer", mode="split", visualization_enabled=0,
                                    rng_ms=0, simulation_ms=1, compute_ms=1,
                                    gpu_ms=2, timestamp_method="encoder_boundaries")])
            groups = profiler.summarize(profiler.read_measurements([path]))
            self.assertEqual(len(groups), 2)
            for group in groups:
                self.assertIsNone(group["split_rng_compute_fraction"])
                self.assertIsNone(group["split_rng_dispatch_fraction"])
            report = profiler.markdown_report(groups)
            self.assertIn("80.0%", report)
            self.assertIn("8.0000 / 8.0000", report)
            self.assertIn("2.0000 / 2.0000", report)
            self.assertNotIn("same random streams", report)

    def test_timestamp_methods_and_legacy_isolation_remain_distinct(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "legacy.csv"
            write_csv(path, [sample()])
            legacy = profiler.read_measurements([path])
            self.assertEqual(legacy[0]["isolation"], "none")
            self.assertEqual(legacy[0]["timestamp_method"], "unspecified")
            self.assertEqual(legacy[0]["rng_source"], "gpu")
            self.assertEqual(legacy[0]["upload_ms"], 0)
            self.assertEqual(legacy[0]["rng_input_ms"], 0)
            self.assertEqual(legacy[0]["rng_staging_ms"], 0)
            modern_path = Path(temporary) / "modern.csv"
            write_csv(modern_path, [sample(timestamp_method="encoder_boundaries")])
            groups = profiler.summarize(legacy + profiler.read_measurements([modern_path]))
            self.assertEqual(len(groups), 2)

    def test_external_stream_timings_are_separate_and_not_gpu_rng_attribution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            gpu_path, external_path = root / "gpu.csv", root / "external.csv"
            write_csv(gpu_path, [sample()])
            write_csv(external_path, [sample(rng_source="external", rng_ms=0,
                                            rng_input_ms=12, rng_staging_ms=0.4, upload_ms=0.7,
                                            simulation_ms=2, compute_ms=3,
                                            gpu_ms=4, cpu_ms=20)])
            groups = profiler.summarize(profiler.read_measurements([gpu_path, external_path]))
            self.assertEqual(len(groups), 2)
            external = next(group for group in groups if group["rng_source"] == "external")
            self.assertIsNone(external["split_rng_compute_fraction"])
            self.assertIsNone(external["split_rng_dispatch_fraction"])
            self.assertAlmostEqual(external["compute_dependency_ms"]["median"], 0.3)
            self.assertEqual(external["timings_ms"]["rng_input_ms"]["median"], 12)
            report = profiler.markdown_report(groups)
            self.assertIn("External RNG streams", report)
            self.assertIn("12.0000 / 12.0000", report)
            self.assertIn("0.7000", report)
            self.assertIn("0.4000", report)
            self.assertIn("not evidence of FPGA", report)

    def test_external_stream_cannot_be_fused_or_isolated(self):
        for changes in ({"mode": "fused"}, {"isolation": "rng", "operations": 0}):
            with self.subTest(changes=changes), tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "invalid.csv"
                write_csv(path, [sample(rng_source="external", visualization_enabled=0, **changes)])
                with self.assertRaisesRegex(ValueError, "external streams require"):
                    profiler.read_measurements([path])

    def test_incompatible_run_parameters_cannot_be_pooled_or_compared(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "mixed.csv"
            write_csv(path, [sample(step_size=0.01), sample(step_size=0.1)])
            with self.assertRaisesRegex(ValueError, "different step_size"):
                profiler.summarize(profiler.read_measurements([path]))
            write_csv(path, [sample(seed=42), sample(mode="fused", seed=43)])
            report = profiler.markdown_report(profiler.summarize(profiler.read_measurements([path])))
            self.assertNotIn("same random streams", report)

    def test_summarize_cli_writes_valid_json_and_markdown(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_csv(root / "one.csv", [sample(mode="fused", rng_ms=0, simulation_ms=0, compute_ms=0, gpu_ms=0)])
            with contextlib.redirect_stdout(io.StringIO()):
                result = profiler.main(["--output", str(root), "--summarize-only"])
            self.assertEqual(result, 0)
            groups = json.loads((root / "summary.json").read_text(encoding="utf-8"))
            self.assertIsNone(groups[0]["effective_random_gvalues_per_s"])
            self.assertIsNone(groups[0]["visualization_fraction"])
            self.assertTrue((root / "report.md").is_file())

    def test_collection_matrix_preserves_raw_outputs_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fake_gpu"
            executable.touch()

            def run_fake(command, **kwargs):
                options = dict(zip(command[1::2], command[2::2]))
                visual = "--no-viz" not in command
                isolation = options.get("--isolate", "none")
                rows = [sample(workload=options["--workload"], mode=options["--mode"],
                               count=int(options["--count"]), blocks=int(options["--blocks"]),
                               seed=int(options["--seed"]), frame=frame,
                               isolation=isolation, operations=0 if isolation == "rng" else 32,
                               visualization_enabled=int(visual)) for frame in range(int(options["--frames"]))]
                write_csv(Path(options["--csv"]), rows)
                return type("Result", (), {"returncode": 0})()

            output = root / "measurements"
            arguments = ["--output", str(output), "--exe", str(executable), "--counts", "8", "--blocks", "2", "--quick", "--frames", "2"]
            with mock.patch.object(profiler.subprocess, "run", side_effect=run_fake) as run, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(profiler.main(arguments), 0)
            self.assertEqual(run.call_count, 12)
            self.assertEqual(len(list(output.glob("*.csv"))), 12)
            self.assertEqual(len(list(output.glob("*.log"))), 12)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(len(manifest["runs"]), 12)
            self.assertEqual(sum("--isolate" in run["command"] for run in manifest["runs"]), 4)
            self.assertEqual(manifest["frames_per_trial"], 2)
            self.assertEqual(manifest["warmup_frames"], 5)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(profiler.main(arguments), 1)


if __name__ == "__main__":
    unittest.main()
