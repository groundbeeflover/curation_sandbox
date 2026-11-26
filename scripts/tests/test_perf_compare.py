"""Tests for scripts/perf-compare.py. Run: python3 -m unittest discover -s scripts/tests"""

import contextlib
import copy
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "perf-compare.py"
spec = importlib.util.spec_from_file_location("perf_compare", SCRIPT)
perf = importlib.util.module_from_spec(spec)
sys.modules["perf_compare"] = perf
spec.loader.exec_module(perf)

BUDGETS = {
    "budgets": [
        {"metric": "frame_time_p95_s", "limit": 0.008, "unit": "s", "scene": "empty-room",
         "tier": "standard", "hardware": ["radeon-780m"], "size": [1920, 1080],
         "enforced_from": "M1"},
        {"metric": "gpu_memory_bytes", "limit": 1.5e9, "unit": "bytes", "scene": "empty-room",
         "tier": "standard", "hardware": ["any"], "enforced_from": "M2"},
        {"metric": "lux_refresh_time_s", "limit": 1.0, "unit": "s", "scene": "empty-room",
         "tier": "standard", "hardware": ["ryzen-7-8840u"], "enforced_from": "M1"},
        {"metric": "frame_time_p95_s", "limit": 0.001, "unit": "s", "scene": "gallery-50",
         "tier": "standard", "hardware": ["any"], "enforced_from": "M1"},
    ]
}

RESULT = {
    "schema": 1,
    "scene": "empty-room",
    "tier": "standard",
    "size": [1920, 1080],
    "frames": 600,
    "machine": "radeon-780m",
    "cpu": "ryzen-7-8840u",
    "device": {"name": "AMD Radeon 780M (RADV PHOENIX)", "type": "integrated",
               "driver": "Mesa 25.2.4", "api_version": "1.3.296"},
    "git_revision": "6656a51",
    "metrics": {
        "frame_time_p50_s": 0.004,
        "frame_time_p95_s": 0.005,
        "gpu_time_p95_s": 0.003,
        "gpu_memory_bytes": 400e6,
        "process_memory_bytes": 300e6,
        "load_time_s": 0.5,
    },
}


def with_metrics(base, **metrics):
    data = copy.deepcopy(base)
    data["metrics"].update(metrics)
    return data


class Run:
    """Writes the inputs to a temporary folder and runs the script's main() on them."""

    def __init__(self, test):
        folder = tempfile.TemporaryDirectory()
        test.addCleanup(folder.cleanup)
        self.dir = Path(folder.name)

    def write(self, name, data):
        path = self.dir / name
        path.write_text(data if isinstance(data, str) else json.dumps(data), encoding="utf-8")
        return path

    def __call__(self, result=RESULT, budgets=BUDGETS, baseline=None, milestone="M1",
                 baseline_path=None):
        args = [str(self.write("result.json", result)),
                "--budgets", str(self.write("budgets.json", budgets)), "--milestone", milestone]
        if baseline is not None:
            args += ["--baseline", str(self.write("baseline.json", baseline))]
        elif baseline_path is not None:
            args += ["--baseline", str(baseline_path)]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = perf.main(args)
        return code, out.getvalue() + err.getvalue()


def row(output, metric):
    """The table rows of a metric, as lists of cells."""
    rows = [line for line in output.splitlines() if line.startswith(f"| {metric} ")]
    return [[cell.strip() for cell in line.strip("|").split("|")] for line in rows]


class Verdicts(unittest.TestCase):
    def setUp(self):
        self.run = Run(self)

    def test_pass_prints_the_table_and_exits_0(self):
        code, out = self.run(baseline=RESULT)
        self.assertEqual(code, 0, out)
        header = row(out, "metric")[0]
        self.assertEqual(header, ["metric", "budget", "baseline", "now", "change", "verdict"])
        frame = row(out, "frame_time_p95_s")[0]
        self.assertEqual(frame, ["frame_time_p95_s", "8.00 ms", "5.00 ms", "5.00 ms", "+0.0%",
                                 "pass"])
        # Every measured metric gets a row, with or without a budget.
        for metric in RESULT["metrics"]:
            self.assertEqual(len(row(out, metric)), 1, metric)
        self.assertEqual(row(out, "gpu_memory_bytes")[0][1:4],
                         ["1.50 GB", "400.0 MB", "400.0 MB"])

    def test_an_enforced_budget_exceeded_fails(self):
        code, out = self.run(with_metrics(RESULT, frame_time_p95_s=0.0081), baseline=RESULT,
                             milestone="M1")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: over budget", row(out, "frame_time_p95_s")[0][5])

    def test_a_budget_enforced_before_the_milestone_still_counts(self):
        code, out = self.run(with_metrics(RESULT, gpu_memory_bytes=1.6e9), milestone="M3")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: over budget", row(out, "gpu_memory_bytes")[0][5])

    def test_the_limit_itself_is_within_budget(self):
        code, out = self.run(with_metrics(RESULT, frame_time_p95_s=0.008))
        self.assertEqual(code, 0, out)

    def test_a_budget_not_yet_enforced_is_reported_but_does_not_fail(self):
        over = with_metrics(RESULT, gpu_memory_bytes=1.6e9)
        code, out = self.run(over, milestone="M1")
        self.assertEqual(code, 0, out)
        verdict = row(out, "gpu_memory_bytes")[0][5]
        self.assertIn("over budget", verdict)
        self.assertIn("enforced from M2", verdict)
        self.assertNotIn("FAIL", verdict)

        code, out = self.run(milestone="M1")
        self.assertEqual(code, 0, out)
        self.assertIn("within budget, enforced from M2", row(out, "gpu_memory_bytes")[0][5])

        code, out = self.run(over, milestone="M2")
        self.assertEqual(code, 1, out)

    def test_more_than_10_percent_worse_than_the_baseline_fails(self):
        # A metric with no budget: only the baseline judges it.
        code, out = self.run(with_metrics(RESULT, gpu_time_p95_s=0.00333), baseline=RESULT)
        self.assertEqual(code, 1, out)
        cells = row(out, "gpu_time_p95_s")[0]
        self.assertEqual(cells[1:5], ["-", "3.00 ms", "3.33 ms", "+11.0%"])
        self.assertIn("FAIL", cells[5])
        self.assertIn("baseline", cells[5])

    def test_a_regression_fails_even_within_budget(self):
        code, out = self.run(with_metrics(RESULT, frame_time_p95_s=0.0058), baseline=RESULT)
        self.assertEqual(code, 1, out)
        verdict = row(out, "frame_time_p95_s")[0][5]
        self.assertIn("baseline", verdict)
        self.assertNotIn("over budget", verdict)

    def test_up_to_10_percent_worse_and_any_improvement_pass(self):
        code, out = self.run(with_metrics(RESULT, gpu_time_p95_s=0.0033, load_time_s=0.1),
                             baseline=RESULT)
        self.assertEqual(code, 0, out)
        self.assertEqual(row(out, "load_time_s")[0][4], "-80.0%")

    def test_a_missing_baseline_file_compares_only_with_the_budgets(self):
        missing = self.run.dir / "radeon-780m" / "empty-room-standard.json"
        code, out = self.run(baseline_path=missing)
        self.assertEqual(code, 0, out)
        self.assertIn("no baseline", out)
        self.assertEqual(row(out, "frame_time_p95_s")[0][2:5], ["-", "5.00 ms", "-"])
        # The budgets are still enforced.
        code, out = self.run(with_metrics(RESULT, frame_time_p95_s=0.009), baseline_path=missing)
        self.assertEqual(code, 1, out)

    def test_a_missing_baseline_anywhere_else_is_a_mistake(self):
        for wrong in (self.run.dir / "radeon-780m" / "empty-room-standrd.json",
                      self.run.dir / "iris-xe" / "empty-room-standard.json"):
            with self.subTest(path=wrong):
                code, out = self.run(baseline_path=wrong)
                self.assertEqual(code, 2, out)
                self.assertIn("radeon-780m/empty-room-standard.json", out)

    def test_a_baseline_of_zero_fails_on_any_growth_and_says_so(self):
        code, out = self.run(baseline=with_metrics(RESULT, gpu_memory_bytes=0))
        self.assertEqual(code, 1, out)
        self.assertEqual(row(out, "gpu_memory_bytes")[0][2:5], ["0.0 MB", "400.0 MB", "-"])
        self.assertIn("gpu_memory_bytes 400.0 MB against 0.0 MB in the baseline", out)

    def test_no_baseline_argument_compares_only_with_the_budgets(self):
        code, out = self.run()
        self.assertEqual(code, 0, out)
        self.assertEqual(row(out, "frame_time_p50_s")[0][2:6], ["-", "4.00 ms", "-", "-"])

    def test_a_metric_missing_from_the_baseline_is_not_compared(self):
        baseline = copy.deepcopy(RESULT)
        del baseline["metrics"]["load_time_s"]
        code, out = self.run(with_metrics(RESULT, load_time_s=5.0), baseline=baseline)
        self.assertEqual(code, 0, out)
        self.assertEqual(row(out, "load_time_s")[0][2:5], ["-", "5.00 s", "-"])

    def test_a_metric_the_run_did_not_measure_is_reported_without_failing(self):
        # In the baseline but not in this run: a row says so.
        dropped = copy.deepcopy(RESULT)
        del dropped["metrics"]["gpu_time_p95_s"]
        code, out = self.run(dropped, baseline=RESULT)
        self.assertEqual(code, 0, out)
        self.assertEqual(row(out, "gpu_time_p95_s")[0][1:6],
                         ["-", "3.00 ms", "-", "-", "not measured"])
        # An enforced budget that applies to this run, measured by a run of its own (--lux).
        cells = row(out, "lux_refresh_time_s")[0]
        self.assertEqual(cells[1:6], ["1.00 s", "-", "-", "-", "not measured"])


class WhichBudgetsApply(unittest.TestCase):
    def setUp(self):
        self.run = Run(self)

    def test_a_budget_for_other_hardware_does_not_apply(self):
        other = with_metrics(RESULT, frame_time_p95_s=0.009)
        other["machine"] = "iris-xe"
        code, out = self.run(other)
        self.assertEqual(code, 0, out)
        self.assertEqual(row(out, "frame_time_p95_s")[0][1], "-")

    def test_any_hardware_applies_everywhere(self):
        lavapipe = with_metrics(RESULT, gpu_memory_bytes=1.6e9)
        lavapipe["machine"], lavapipe["cpu"] = "llvmpipe", "xeon-8370c"
        code, out = self.run(lavapipe, milestone="M2")
        self.assertEqual(code, 1, out)

    def test_a_budget_on_the_cpu_matches_the_cpu(self):
        result = with_metrics(RESULT, lux_refresh_time_s=1.2)
        code, out = self.run(result)
        self.assertEqual(code, 1, out)
        result["cpu"] = "core-i7-1165g7"
        code, out = self.run(result)
        self.assertEqual(code, 0, out)

    def test_a_budget_at_another_size_does_not_apply(self):
        small = with_metrics(RESULT, frame_time_p95_s=0.009)
        small["size"] = [320, 180]
        code, out = self.run(small)
        self.assertEqual(code, 0, out)
        self.assertEqual(row(out, "frame_time_p95_s")[0][1], "-")

    def test_a_budget_for_another_scene_or_tier_does_not_apply(self):
        # The gallery-50 budget of 1 ms would fail every run here.
        code, out = self.run()
        self.assertEqual(code, 0, out)
        raytraced = copy.deepcopy(RESULT)
        raytraced["tier"] = "raytraced"
        code, out = self.run(raytraced)
        self.assertEqual(code, 0, out)
        self.assertEqual(row(out, "frame_time_p95_s")[0][1], "-")


class BadInput(unittest.TestCase):
    def setUp(self):
        self.run = Run(self)

    def assert_rejected(self, expected, **kwargs):
        code, out = self.run(**kwargs)
        self.assertEqual(code, 2, out)
        self.assertIn(expected, out)

    def test_an_unknown_metric_is_rejected(self):
        self.assert_rejected("frame_time_95_s", result=with_metrics(RESULT, frame_time_95_s=0.1))

    def test_a_value_that_is_not_a_finite_number_is_rejected(self):
        for value in ("5", True, -1, None):
            with self.subTest(value=value):
                self.assert_rejected("frame_time_p95_s",
                                     result=with_metrics(RESULT, frame_time_p95_s=value))
        nan = json.dumps(RESULT).replace("0.005", "NaN")
        self.assert_rejected("NaN", result=nan)

    def test_another_schema_or_not_json_is_rejected(self):
        for schema in (2, True, 1.0, "1"):
            with self.subTest(schema=schema):
                self.assert_rejected("schema", result=dict(RESULT, schema=schema))
        self.assert_rejected("result.json", result="{ not json")
        self.assert_rejected("scene", result={k: v for k, v in RESULT.items() if k != "scene"})

    def test_json_nested_too_deep_is_rejected_not_a_crash(self):
        self.assert_rejected("result.json", result="[" * 200000)

    def test_an_unknown_tier_or_a_git_revision_that_is_not_a_slug_is_rejected(self):
        self.assert_rejected("tier", result=dict(RESULT, tier="std"))
        self.assert_rejected("git_revision", result=dict(RESULT, git_revision="x\n# FAIL\n|a|"))
        self.assert_rejected("git_revision", baseline=dict(RESULT, git_revision="a" * 65))

    def test_a_baseline_for_other_conditions_is_rejected(self):
        for key, value in (("scene", "gallery-50"), ("tier", "raytraced"),
                           ("machine", "iris-xe"), ("size", [1280, 720])):
            with self.subTest(key=key):
                self.assert_rejected(key, baseline=dict(RESULT, **{key: value}))

    def test_a_malformed_budget_is_rejected(self):
        for change, expected in (({"unit": "ms"}, "unit"), ({"metric": "fps"}, "fps"),
                                 ({"enforced_from": "M1.5"}, "enforced_from"),
                                 ({"tier": "ultra"}, "tier"), ({"hardware": []}, "hardware"),
                                 ({"limit": -1}, "limit")):
            with self.subTest(change=change):
                budgets = copy.deepcopy(BUDGETS)
                budgets["budgets"][0].update(change)
                self.assert_rejected(expected, budgets=budgets)

    def test_a_file_too_large_is_rejected(self):
        big = dict(RESULT, padding="x" * (perf.MAX_FILE_BYTES + 1))
        self.assert_rejected("result.json", result=big)

    def test_a_malformed_milestone_is_rejected(self):
        with self.assertRaises(SystemExit) as caught:
            self.run(milestone="1")
        self.assertEqual(caught.exception.code, 2)


class RepositoryBudgets(unittest.TestCase):
    """bench/budgets.json against docs/product/blueprint.md, section 6, written out by hand."""

    # metric, scene, tier, hardware, limit, enforced_from
    EXPECTED = {
        ("frame_time_p95_s", "gallery-50", "standard", ("radeon-780m",), 0.0167, "M4"),
        ("frame_time_p95_s", "gallery-50", "standard", ("iris-xe",), 0.033, "M4"),
        ("frame_time_p95_s", "gallery-50", "raytraced", ("rtx-2060",), 0.0167, "M5"),
        ("frame_time_p95_s", "empty-room", "standard", ("radeon-780m",), 0.008, "M1"),
        ("frame_time_p95_s", "empty-room", "raytraced", ("rtx-2060",), 0.008, "M5"),
        ("gpu_memory_bytes", "gallery-50", "standard", ("any",), 1.5e9, "M2"),
        ("gpu_memory_bytes", "gallery-50", "raytraced", ("any",), 3e9, "M5"),
        ("process_memory_bytes", "gallery-50", "standard", ("any",), 2e9, "M2"),
        ("process_memory_bytes", "gallery-50", "raytraced", ("any",), 2.5e9, "M5"),
        ("load_time_s", "gallery-50", "standard", ("radeon-780m", "iris-xe"), 3, "M4"),
        ("load_time_s", "gallery-50", "raytraced", ("rtx-2060",), 3, "M5"),
        ("startup_time_s", "empty-room", "standard", ("radeon-780m", "iris-xe"), 2, "M4"),
        ("startup_time_s", "empty-room", "raytraced", ("rtx-2060",), 2, "M5"),
        ("bake_preview_time_s", "gallery-50", "standard", ("ryzen-7-8840u",), 2, "M3"),
        ("bake_full_time_s", "gallery-50", "standard", ("ryzen-7-8840u",), 30, "M3"),
        ("bake_frame_time_p95_s", "gallery-50", "standard", ("ryzen-7-8840u",), 0.0167, "M3"),
        ("lux_refresh_time_s", "gallery-50", "standard", ("ryzen-7-8840u",), 1, "M2"),
        ("lux_refresh_time_s", "gallery-50", "raytraced", ("rtx-2060",), 1, "M5"),
        ("still_1080p_time_s", "gallery-50", "standard", ("ryzen-7-8840u",), 60, "M3"),
        ("still_1080p_time_s", "gallery-50", "raytraced", ("rtx-2060",), 10, "M5"),
        ("still_4k_time_s", "gallery-50", "standard", ("ryzen-7-8840u",), 900, "M3"),
        ("still_4k_time_s", "gallery-50", "raytraced", ("rtx-2060",), 120, "M5"),
    }

    def setUp(self):
        path = ROOT / "bench" / "budgets.json"
        self.budgets = perf.check_budgets(perf.load_json(path), path)

    def test_every_budget_of_the_blueprint_is_there_once(self):
        found = [(b["metric"], b["scene"], b["tier"], tuple(b["hardware"]), b["limit"],
                  b["enforced_from"]) for b in self.budgets]
        self.assertEqual(len(found), len(set(found)))
        self.assertEqual(set(found), self.EXPECTED)

    def test_every_budget_has_the_fields_the_task_names(self):
        for budget in self.budgets:
            for field in ("metric", "limit", "unit", "scene", "tier", "hardware",
                          "enforced_from"):
                self.assertIn(field, budget)

    def test_frame_time_budgets_are_at_1080p(self):
        for budget in self.budgets:
            if "frame_time" in budget["metric"]:
                self.assertEqual(budget.get("size"), [1920, 1080], budget)


class CommandLine(unittest.TestCase):
    def test_help_documents_the_result_schema(self):
        done = subprocess.run([sys.executable, str(SCRIPT), "--help"], capture_output=True,
                              text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        for word in ("--budgets", "--baseline", "--milestone", '"schema": 1', '"metrics"',
                     *perf.METRICS):
            self.assertIn(word, done.stdout)


if __name__ == "__main__":
    unittest.main()
