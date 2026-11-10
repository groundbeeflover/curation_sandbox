"""Tests for scripts/github-bootstrap.py. Run: python3 -m unittest discover -s scripts/tests"""

import copy
import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("bootstrap", ROOT / "scripts" / "github-bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
sys.modules["bootstrap"] = bootstrap  # dataclasses look their module up here
spec.loader.exec_module(bootstrap)

BASE_LABELS = [{"name": n, "color": "ededed", "description": n} for n in (
    ["area:core", "area:render", "area:editor", "in-progress", "blocked", "epic"]
    + [f"type:{t}" for t in bootstrap.TYPES] + [f"size:{s}" for s in bootstrap.SIZES]
    + [f"owner:{o}" for o in bootstrap.OWNERS])]


def task(tid, area="core", blocked_by=(), touches=None, **kw):
    t = {
        "id": tid, "epic": "E1.1", "title": f"Task {tid}", "area": area, "type": "feature",
        "size": "S", "owner": "agent", "blocked_by": list(blocked_by),
        "touches": touches or [f"libs/{tid.lower()}/"], "goal": "A goal.",
        "context": ["ctx"], "acceptance": ["`ctest -R x` passes"], "out_of_scope": ["nothing"],
        "verification": ["ctest --preset debug"],
    }
    t.update(kw)
    return t


def dataset(tasks):
    return {
        "repository": "o/r", "project_board": "Board", "labels": BASE_LABELS,
        "milestones": [{"id": "M1", "title": "M1 Skeleton", "description": "d",
                        "due_on": "2026-12-01"}],
        "epics": [{"id": "E1.1", "milestone": "M1", "title": "Epic", "area": "core",
                   "goal": "g", "demo": "a render"}],
        "tasks": tasks,
    }


THREE_READY = [task("M1-01", "core"), task("M1-02", "render"), task("M1-03", "editor")]


class ValidateTests(unittest.TestCase):
    def test_valid_dataset_passes(self):
        r = bootstrap.validate(dataset(THREE_READY))
        self.assertEqual(r.errors, [])

    def test_cycle_is_an_error(self):
        tasks = copy.deepcopy(THREE_READY) + [task("M1-04", blocked_by=["M1-05"]),
                                              task("M1-05", blocked_by=["M1-04"])]
        r = bootstrap.validate(dataset(tasks))
        self.assertTrue(any("cycle" in e for e in r.errors), r.errors)

    def test_missing_field_is_an_error(self):
        tasks = copy.deepcopy(THREE_READY)
        del tasks[0]["verification"]
        r = bootstrap.validate(dataset(tasks))
        self.assertTrue(any("missing field verification" in e for e in r.errors), r.errors)

    def test_size_l_needs_design_note(self):
        tasks = copy.deepcopy(THREE_READY)
        tasks[0]["size"] = "L"
        r = bootstrap.validate(dataset(tasks))
        self.assertTrue(any("design_note" in e for e in r.errors), r.errors)

    def test_spike_must_name_its_adr(self):
        tasks = copy.deepcopy(THREE_READY)
        tasks[0]["type"] = "spike"
        r = bootstrap.validate(dataset(tasks))
        self.assertTrue(any("docs/adr/" in e for e in r.errors), r.errors)

    def test_fewer_than_three_ready_areas_is_an_error(self):
        tasks = [task("M1-01", "core"), task("M1-02", "core"),
                 task("M1-03", "render", blocked_by=["M1-01"])]
        r = bootstrap.validate(dataset(tasks))
        self.assertTrue(any("three ready" in e for e in r.errors), r.errors)

    def test_parallel_tasks_touching_the_same_path_warn(self):
        tasks = copy.deepcopy(THREE_READY)
        tasks[0]["touches"] = ["libs/core/"]
        tasks[1]["touches"] = ["libs/core/src/rng.cpp"]
        r = bootstrap.validate(dataset(tasks))
        self.assertTrue(any("M1-01 and M1-02" in w for w in r.warnings), r.warnings)

    def test_dependent_tasks_may_touch_the_same_path(self):
        tasks = copy.deepcopy(THREE_READY) + [
            task("M1-04", "core", blocked_by=["M1-01"], touches=["libs/m1-01/"])]
        r = bootstrap.validate(dataset(tasks))
        self.assertFalse(any("M1-01 and M1-04" in w for w in r.warnings), r.warnings)

    def test_vague_criterion_warns(self):
        tasks = copy.deepcopy(THREE_READY)
        tasks[0]["acceptance"] = ["the frame looks good"]
        r = bootstrap.validate(dataset(tasks))
        self.assertTrue(any("vague" in w for w in r.warnings), r.warnings)


class RenderTests(unittest.TestCase):
    def test_task_body_has_every_template_section(self):
        t = task("M1-04", blocked_by=["M1-01", "M1-02"])
        body = bootstrap.render_task(t, {"M1-01": 12, "M1-02": 15}, 3)
        self.assertIn("Blocked by: #12, #15", body)
        self.assertIn("Epic: #3", body)
        for heading in ("## Goal", "## Context", "## Acceptance criteria", "## Out of scope",
                        "## Verification"):
            self.assertIn(heading, body)
        self.assertIn("- [ ] `ctest -R x` passes", body)

    def test_unblocked_task_says_none(self):
        body = bootstrap.render_task(task("M1-01"), {}, None)
        self.assertIn("Blocked by: none", body)

    def test_title_id_pattern(self):
        self.assertEqual(bootstrap.TITLE_ID.match("[M2-07] Frame mesh").group("id"), "M2-07")
        self.assertEqual(bootstrap.TITLE_ID.match("[E1.3] Lights").group("id"), "E1.3")
        self.assertIsNone(bootstrap.TITLE_ID.match("M2-07 no brackets"))


class RelationParsingTests(unittest.TestCase):
    def test_numbers_of_accepts_several_shapes(self):
        self.assertEqual(bootstrap.numbers_of(None), set())
        self.assertEqual(bootstrap.numbers_of({"number": 4}), {4})
        self.assertEqual(bootstrap.numbers_of([{"number": 4}, {"number": 9}]), {4, 9})
        self.assertEqual(bootstrap.numbers_of({"nodes": [{"number": 2}]}), {2})


class DryRunTests(unittest.TestCase):
    def test_offline_dry_run_writes_nothing(self):
        gh = bootstrap.GitHub("o/r", apply=False, offline=True)
        bootstrap.run(dataset(THREE_READY), gh, None, False, board=True)  # raises on any gh call


class RepositoryTaskFileTests(unittest.TestCase):
    def test_repository_task_file_is_valid(self):
        path = ROOT / "docs" / "roadmap" / "tasks.toml"
        if not path.exists():
            self.skipTest("docs/roadmap/tasks.toml not written yet")
        r = bootstrap.validate(bootstrap.load(path))
        self.assertEqual(r.errors, [])
        self.assertEqual(r.warnings, [])

    def test_critical_path_is_the_longest_chain(self):
        tasks = [task("M1-01"), task("M1-02", blocked_by=["M1-01"]),
                 task("M1-03", blocked_by=["M1-02"]), task("M1-04", blocked_by=["M1-01"])]
        self.assertEqual(bootstrap.critical_path(tasks), ["M1-01", "M1-02", "M1-03"])


if __name__ == "__main__":
    unittest.main()
