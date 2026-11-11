#!/usr/bin/env python3
"""List the task issues that are ready to start, and choose a set that can run in parallel.

A task is ready when it is open, labelled owner:agent, not labelled in-progress, blocked or
epic, has no open pull request, and every issue on its "Blocked by" line is closed. A parallel
set has one task per area and no overlapping "Touches", also against tasks already in progress.

    python3 scripts/ready-tasks.py            ready tasks and a suggested set of 3
    python3 scripts/ready-tasks.py --n 2      suggest 2
    python3 scripts/ready-tasks.py --json     machine-readable, for /dispatch and /groom

Reads GitHub through `gh`; never writes. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field

TITLE_ID = re.compile(r"^\[(M(?P<ms>\d+)-(?P<num>\d{2}))\]\s")
ISSUE_REF = re.compile(r"#(\d+)")
CLOSES = re.compile(r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\s+#(\d+)", re.I)


@dataclass
class Task:
    number: int
    task_id: str
    title: str
    labels: set[str]
    blocked_by: list[int]
    touches: list[str]
    unblocks: int = 0
    reasons: list[str] = field(default_factory=list)

    @property
    def area(self) -> str:
        return next((lab.split(":", 1)[1] for lab in self.labels if lab.startswith("area:")), "?")

    @property
    def size(self) -> str:
        return next((lab.split(":", 1)[1] for lab in self.labels if lab.startswith("size:")), "?")

    @property
    def milestone(self) -> int:
        return int(TITLE_ID.match(f"[{self.task_id}] ").group("ms"))


def section(body: str, name: str) -> str | None:
    """A field from either layout: 'Name: value' (bootstrap script) or '### Name' (issue form)."""
    m = re.search(rf"^{re.escape(name)}:\s*(.*)$", body, re.M)
    if m:
        return m.group(1).strip()
    m = re.search(rf"^###\s+{re.escape(name)}\s*\n+(.*?)(?=\n###\s|\Z)", body, re.S | re.M)
    return m.group(1).strip() if m else None


def parse_blocked_by(body: str) -> list[int]:
    value = section(body, "Blocked by") or ""
    return [int(n) for n in ISSUE_REF.findall(value)]


def parse_touches(body: str) -> list[str]:
    value = section(body, "Touches") or ""
    quoted = re.findall(r"`([^`]+)`", value)
    items = quoted or [v.strip() for v in re.split(r"[,\n]", value)]
    return [i.strip().lstrip("-* ").strip() for i in items if i.strip().lstrip("-* ").strip()]


def paths_overlap(a: str, b: str) -> bool:
    a, b = a.rstrip("/"), b.rstrip("/")
    return a == b or a.startswith(b + "/") or b.startswith(a + "/")


def overlaps(a: Task, b: Task) -> list[str]:
    return sorted({pa for pa in a.touches for pb in b.touches if paths_overlap(pa, pb)})


def gh_json(*args: str) -> object:
    proc = subprocess.run(["gh", *args], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"gh {' '.join(args[:2])} failed: {proc.stderr.strip()}")
    return json.loads(proc.stdout or "null")


def load() -> tuple[list[Task], set[int], set[int]]:
    """Open task issues, closed issue numbers, and issues with an open pull request."""
    rows = gh_json("issue", "list", "--state", "open", "--limit", "1000",
                   "--json", "number,title,labels,body")
    tasks = []
    for r in rows:
        m = TITLE_ID.match(r["title"])
        if not m:
            continue
        tasks.append(Task(r["number"], m.group(1), r["title"], {lab["name"] for lab in r["labels"]},
                          parse_blocked_by(r["body"] or ""), parse_touches(r["body"] or "")))
    closed = {r["number"] for r in gh_json("issue", "list", "--state", "closed", "--limit", "3000",
                                           "--json", "number")}
    with_pr: set[int] = set()
    for pr in gh_json("pr", "list", "--state", "open", "--limit", "200",
                      "--json", "number,body,headRefName"):
        with_pr |= {int(n) for n in CLOSES.findall(pr["body"] or "")}
        m = re.match(r"task/(\d+)-", pr["headRefName"])
        if m:
            with_pr.add(int(m.group(1)))
    return tasks, closed, with_pr


def classify(tasks: list[Task], closed: set[int], with_pr: set[int]) -> tuple[list[Task], list[Task]]:
    """Returns (ready, in_progress). Fills each task's reasons and unblock counts."""
    for t in tasks:
        t.unblocks = sum(1 for o in tasks if t.number in o.blocked_by)
    ready, running = [], []
    for t in tasks:
        if "in-progress" in t.labels:
            running.append(t)
            continue
        if "owner:agent" not in t.labels:
            t.reasons.append("owner is not agent")
        for lab in ("blocked", "epic"):
            if lab in t.labels:
                t.reasons.append(f"labelled {lab}")
        if t.number in with_pr:
            t.reasons.append("has an open pull request")
        open_blockers = [b for b in t.blocked_by if b not in closed]
        if open_blockers:
            t.reasons.append("blocked by " + ", ".join(f"#{b}" for b in open_blockers))
        if not t.reasons:
            ready.append(t)
    ready.sort(key=lambda t: (t.milestone, -t.unblocks, t.task_id))
    return ready, running


def choose(ready: list[Task], running: list[Task], n: int) -> list[Task]:
    """Greedy: highest priority first, one per area, no Touches overlap with anything chosen or
    already running."""
    chosen: list[Task] = []
    busy_areas = {t.area for t in running}
    for t in ready:
        if len(chosen) >= n:
            break
        if t.area in busy_areas or any(t.area == c.area for c in chosen):
            continue
        if any(overlaps(t, other) for other in chosen + running):
            continue
        chosen.append(t)
    return chosen


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--n", type=int, default=3, help="how many tasks to suggest (default 3)")
    p.add_argument("--json", action="store_true")
    args = p.parse_args()
    tasks, closed, with_pr = load()
    ready, running = classify(tasks, closed, with_pr)
    chosen = choose(ready, running, max(0, args.n))
    if args.json:
        def row(t: Task) -> dict:
            return {"number": t.number, "id": t.task_id, "title": t.title, "area": t.area,
                    "size": t.size, "unblocks": t.unblocks, "touches": t.touches}
        print(json.dumps({"in_progress": [row(t) for t in running], "ready": [row(t) for t in ready],
                          "suggested": [row(t) for t in chosen],
                          "not_ready": [{**row(t), "reasons": t.reasons} for t in tasks
                                        if t.reasons and t not in running]}, indent=2))
        return 0
    print(f"In progress ({len(running)}): " +
          (", ".join(f"#{t.number} {t.task_id} [{t.area}]" for t in running) or "none"))
    print(f"\nReady ({len(ready)}):")
    for t in ready:
        mark = "*" if t in chosen else " "
        print(f" {mark} #{t.number:<4} {t.task_id}  {t.area:<9} {t.size}  unblocks {t.unblocks}  "
              f"{t.title}")
    print("\n* = suggested parallel set: one per area, no overlapping Touches, none clashing with "
          "work in progress")
    return 0


if __name__ == "__main__":
    sys.exit(main())
