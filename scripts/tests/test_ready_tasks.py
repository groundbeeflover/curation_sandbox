"""Tests for scripts/ready-tasks.py. Run: python3 -m unittest discover -s scripts/tests"""

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ready", ROOT / "scripts" / "ready-tasks.py")
ready = importlib.util.module_from_spec(spec)
sys.modules["ready"] = ready
spec.loader.exec_module(ready)

SCRIPT_BODY = """ID: M1-07        Area: core      Type: feature      Size: M      Owner: agent
Epic: #3
Blocked by: #12, #15
Touches: `libs/core/src/image_io.cpp`, `libs/core/tests/`

## Goal
Something.
"""

FORM_BODY = """### ID

M2-31

### Blocked by

#40

### Touches

`libs/geom/src/frame.cpp`, `libs/geom/tests/test_frame.cpp`

### Goal

Something.
"""


def task(number, tid, area, blocked_by=(), touches=(), labels=()):
    return ready.Task(number, tid, f"[{tid}] t", {f"area:{area}", "owner:agent", "size:S", *labels},
                      list(blocked_by), list(touches))


class ParseTests(unittest.TestCase):
    def test_script_layout(self):
        self.assertEqual(ready.parse_blocked_by(SCRIPT_BODY), [12, 15])
        self.assertEqual(ready.parse_touches(SCRIPT_BODY),
                         ["libs/core/src/image_io.cpp", "libs/core/tests/"])

    def test_form_layout(self):
        self.assertEqual(ready.parse_blocked_by(FORM_BODY), [40])
        self.assertEqual(ready.parse_touches(FORM_BODY),
                         ["libs/geom/src/frame.cpp", "libs/geom/tests/test_frame.cpp"])

    def test_none(self):
        self.assertEqual(ready.parse_blocked_by("Blocked by: none\n"), [])


class ClassifyTests(unittest.TestCase):
    def test_open_blocker_and_labels_make_a_task_not_ready(self):
        tasks = [task(1, "M1-01", "build"), task(2, "M1-02", "core", blocked_by=[1]),
                 task(3, "M1-03", "gpu", labels=["in-progress"]),
                 task(4, "M1-04", "ui", labels=["blocked"]), task(5, "M1-05", "doc")]
        r, running = ready.classify(tasks, closed=set(), with_pr={5})
        self.assertEqual([t.number for t in r], [1])
        self.assertEqual([t.number for t in running], [3])
        self.assertIn("blocked by #1", tasks[1].reasons)
        self.assertIn("has an open pull request", tasks[4].reasons)

    def test_closed_blocker_frees_the_task(self):
        tasks = [task(2, "M1-02", "core", blocked_by=[1])]
        r, _ = ready.classify(tasks, closed={1}, with_pr=set())
        self.assertEqual([t.number for t in r], [2])

    def test_priority_is_milestone_then_unblocks(self):
        tasks = [task(1, "M2-01", "geom"), task(2, "M1-09", "doc"), task(3, "M1-02", "core"),
                 task(4, "M1-10", "editor", blocked_by=[2])]
        r, _ = ready.classify(tasks, closed=set(), with_pr=set())
        self.assertEqual([t.task_id for t in r], ["M1-09", "M1-02", "M2-01"])

    def test_high_priority_goes_first(self):
        tasks = [task(1, "M1-02", "build"), task(2, "M1-36", "gpu", labels=["priority:high"]),
                 task(3, "M1-19", "gpu", blocked_by=[9]), task(4, "M1-08", "doc"),
                 task(5, "M1-10", "geom", blocked_by=[1])]
        r, _ = ready.classify(tasks, closed={9}, with_pr=set())
        self.assertEqual([t.task_id for t in r], ["M1-36", "M1-02", "M1-08", "M1-19"])


class ChooseTests(unittest.TestCase):
    def test_one_per_area_and_no_overlap(self):
        a = task(1, "M1-01", "core", touches=["libs/core/src/a.cpp"])
        b = task(2, "M1-02", "core", touches=["libs/core/src/b.cpp"])
        c = task(3, "M1-03", "gpu", touches=["libs/gpu/"])
        d = task(4, "M1-04", "doc", touches=["libs/gpu/src/x.cpp"])
        e = task(5, "M1-05", "geom", touches=["libs/geom/"])
        chosen = ready.choose([a, b, c, d, e], running=[], n=3)
        self.assertEqual([t.number for t in chosen], [1, 3, 5])

    def test_running_work_blocks_its_area_and_files(self):
        running = [task(9, "M1-09", "core", touches=["libs/doc/"])]
        a = task(1, "M1-01", "core", touches=["libs/core/"])
        b = task(2, "M1-02", "doc", touches=["libs/doc/src/x.cpp"])
        c = task(3, "M1-03", "gpu", touches=["libs/gpu/"])
        self.assertEqual([t.number for t in ready.choose([a, b, c], running, 3)], [3])


if __name__ == "__main__":
    unittest.main()
