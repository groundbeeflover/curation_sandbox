#!/usr/bin/env python3
"""Create the project's GitHub milestones, labels, epics, task issues and board from
docs/roadmap/tasks.toml, idempotently.

Issues are matched by the ID at the start of their title ("[M1-07] ..."), so the script can be
re-run whenever a milestone is decomposed: it creates what is missing and leaves the rest
alone. Once an issue exists, the issue (not tasks.toml) is the source of truth for its status.

Usage:
    scripts/github-bootstrap.py --check              validate tasks.toml, no network
    scripts/github-bootstrap.py --table M1           print a milestone's tasks as a Markdown table
    scripts/github-bootstrap.py --graph M1           print a milestone's task graph as Mermaid
    scripts/github-bootstrap.py --show M1-07         print one issue body as it will be created
    scripts/github-bootstrap.py                      dry run: read GitHub, print the plan
    scripts/github-bootstrap.py --offline            dry run without reading GitHub
    scripts/github-bootstrap.py --apply              do it (asks nothing, writes to GitHub)
    scripts/github-bootstrap.py --apply --milestone M3   only one milestone's epics and tasks

Needs Python 3.11+ (for tomllib) and an authenticated `gh` (with the `project` scope for the
board). Uses the standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import tomllib
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TASKS = ROOT / "docs" / "roadmap" / "tasks.toml"

TASK_ID = re.compile(r"^M\d+-\d{2}$")
EPIC_ID = re.compile(r"^E\d+\.\d+$")
TITLE_ID = re.compile(r"^\[(?P<id>(?:M\d+-\d{2})|(?:E\d+\.\d+))\]\s")

TYPES = {"feature", "spike", "test", "bug", "chore", "docs"}
SIZES = {"S", "M", "L"}
OWNERS = {"agent", "human"}
TASK_FIELDS = ("id", "epic", "title", "area", "type", "size", "owner", "blocked_by", "touches",
               "goal", "context", "acceptance", "out_of_scope", "verification")
VAGUE = re.compile(r"\b(works well|properly|correctly|looks good|nice|as expected)\b", re.I)
BOARD_COLUMNS = ("Backlog", "Ready", "In progress", "In review", "Done")
# Pull requests are squash-merged with the pull request's title; branches are deleted after merge.
REPO_SETTINGS = {"allow_squash_merge": True, "allow_merge_commit": False,
                 "allow_rebase_merge": False, "delete_branch_on_merge": True,
                 "squash_merge_commit_title": "PR_TITLE",
                 "squash_merge_commit_message": "COMMIT_MESSAGES"}
WRITE_PAUSE_S = 1.0  # GitHub's secondary rate limit allows about 80 content writes a minute


# ---------------------------------------------------------------------------------------------
# Loading and validation (offline)
# ---------------------------------------------------------------------------------------------

@dataclass
class Report:
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warnings.append(msg)


def load(path: Path) -> dict:
    if path.suffix == ".toml":
        with path.open("rb") as f:
            return tomllib.load(f)
    with path.open(encoding="utf-8") as f:
        return json.load(f)


def critical_path(tasks: list[dict]) -> list[str]:
    """The longest chain of blockers, counting each task as one step."""
    order, _ = topo_order(tasks)
    by_id = {t["id"]: t for t in tasks}
    best: dict[str, tuple[int, str | None]] = {}
    for i in order:
        prev = [(best[b][0], b) for b in by_id[i]["blocked_by"] if b in best]
        length, parent = max(prev) if prev else (0, None)
        best[i] = (length + 1, parent)
    if not best:
        return []
    end = max(best, key=lambda i: (best[i][0], i))
    path = []
    while end:
        path.append(end)
        end = best[end][1]
    return list(reversed(path))


def milestone_table(data: dict, milestone: str) -> str:
    tasks = [t for t in data["tasks"] if t["id"].startswith(milestone + "-")]
    crit = set(critical_path(tasks))
    rows = ["| ID | Title | Area | Type | Size | Owner | Blocked by | Touches |",
            "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for t in tasks:
        tid = f"**{t['id']}**" if t["id"] in crit else t["id"]
        touches = ", ".join(f"`{p}`" for p in t["touches"])
        rows.append(f"| {tid} | {t['title']} | {t['area']} | {t['type']} | {t['size']} | "
                    f"{t['owner']} | {', '.join(t['blocked_by']) or '-'} | {touches} |")
    return "\n".join(rows)


def milestone_graph(data: dict, milestone: str) -> str:
    tasks = [t for t in data["tasks"] if t["id"].startswith(milestone + "-")]
    ids = {t["id"] for t in tasks}
    crit = critical_path(tasks)
    crit_edges = set(zip(crit, crit[1:]))
    lines = ["graph LR"]
    by_epic: dict[str, list[dict]] = defaultdict(list)
    for t in tasks:
        by_epic[t["epic"]].append(t)
    epics = {e["id"]: e for e in data["epics"]}
    for eid, group in sorted(by_epic.items()):
        lines.append(f'  subgraph {eid.replace(".", "_")}["{eid} {epics[eid]["title"]}"]')
        for t in group:
            short = t["title"] if len(t["title"]) <= 34 else t["title"][:32] + "..."
            lines.append(f'    {t["id"].replace("-", "_")}["{t["id"]} {short}"]')
        lines.append("  end")
    link = 0
    crit_links = []
    for t in tasks:
        for b in t["blocked_by"]:
            if b in ids:
                lines.append(f'  {b.replace("-", "_")} --> {t["id"].replace("-", "_")}')
                if (b, t["id"]) in crit_edges:
                    crit_links.append(str(link))
                link += 1
    lines.append("  classDef crit stroke:#b60205,stroke-width:3px")
    lines.append("  class " + ",".join(i.replace("-", "_") for i in crit) + " crit")
    if crit_links:
        lines.append(f"  linkStyle {','.join(crit_links)} stroke:#b60205,stroke-width:3px")
    return "\n".join(lines)


def topo_order(tasks: list[dict]) -> tuple[list[str], list[str]]:
    """Kahn's algorithm. Returns (order, ids left in cycles)."""
    ids = {t["id"] for t in tasks}
    indegree = {t["id"]: 0 for t in tasks}
    children: dict[str, list[str]] = defaultdict(list)
    for t in tasks:
        for b in t.get("blocked_by", []):
            if b in ids:
                indegree[t["id"]] += 1
                children[b].append(t["id"])
    queue = deque(sorted(i for i, d in indegree.items() if d == 0))
    order: list[str] = []
    while queue:
        i = queue.popleft()
        order.append(i)
        for c in sorted(children[i]):
            indegree[c] -= 1
            if indegree[c] == 0:
                queue.append(c)
    stuck = sorted(i for i, d in indegree.items() if d > 0)
    return order, stuck


def ancestors(tasks: list[dict]) -> dict[str, set[str]]:
    """Every task's transitive blockers."""
    by_id = {t["id"]: t for t in tasks}
    memo: dict[str, set[str]] = {}

    def visit(i: str, stack: frozenset[str]) -> set[str]:
        if i in memo:
            return memo[i]
        result: set[str] = set()
        for b in by_id[i].get("blocked_by", []):
            if b in by_id and b not in stack:
                result.add(b)
                result |= visit(b, stack | {b})
        memo[i] = result
        return result

    for t in tasks:
        visit(t["id"], frozenset({t["id"]}))
    return memo


def paths_overlap(a: str, b: str) -> bool:
    a, b = a.rstrip("/"), b.rstrip("/")
    return a == b or a.startswith(b + "/") or b.startswith(a + "/")


def validate(data: dict) -> Report:
    r = Report()
    labels = {lab["name"] for lab in data.get("labels", [])}
    milestones = {m["id"]: m for m in data.get("milestones", [])}
    epics = {e["id"]: e for e in data.get("epics", [])}
    tasks = data.get("tasks", [])

    areas = {name.split(":", 1)[1] for name in labels if name.startswith("area:")}
    for name in [f"type:{t}" for t in TYPES] + [f"size:{s}" for s in SIZES] + \
            [f"owner:{o}" for o in OWNERS] + ["in-progress", "blocked", "epic"]:
        if name not in labels:
            r.error(f"label {name!r} is missing from labels")

    for m in data.get("milestones", []):
        for key in ("id", "title", "description"):
            if not m.get(key):
                r.error(f"milestone {m.get('id', '?')}: missing {key}")
        due = m.get("due_on")
        if due and not re.match(r"^\d{4}-\d{2}-\d{2}$", due):
            r.error(f"milestone {m['id']}: due_on must be YYYY-MM-DD, got {due!r}")

    for e in data.get("epics", []):
        if not EPIC_ID.match(e.get("id", "")):
            r.error(f"epic id {e.get('id')!r} does not look like E1.2")
        if e.get("milestone") not in milestones:
            r.error(f"epic {e.get('id')}: unknown milestone {e.get('milestone')!r}")
        if e.get("area") not in areas:
            r.error(f"epic {e.get('id')}: unknown area {e.get('area')!r}")
        for key in ("title", "goal", "demo"):
            if not e.get(key):
                r.error(f"epic {e.get('id')}: missing {key}")

    seen: set[str] = set()
    for t in tasks:
        tid = t.get("id", "?")
        if tid in seen:
            r.error(f"duplicate task id {tid}")
        seen.add(tid)
        if not TASK_ID.match(tid):
            r.error(f"task id {tid!r} does not look like M1-07")
        for key in TASK_FIELDS:
            if key not in t:
                r.error(f"{tid}: missing field {key}")
        for key in ("goal", "title"):
            if not t.get(key):
                r.error(f"{tid}: empty {key}")
        for key in ("touches", "context", "acceptance", "out_of_scope", "verification"):
            if not t.get(key):
                r.error(f"{tid}: {key} must be a non-empty list")
        epic = epics.get(t.get("epic"))
        if epic is None:
            r.error(f"{tid}: unknown epic {t.get('epic')!r}")
        elif not tid.startswith(epic["milestone"] + "-"):
            r.error(f"{tid}: id does not match its epic's milestone {epic['milestone']}")
        if t.get("area") not in areas:
            r.error(f"{tid}: unknown area {t.get('area')!r}")
        if t.get("type") not in TYPES:
            r.error(f"{tid}: unknown type {t.get('type')!r}")
        if t.get("size") not in SIZES:
            r.error(f"{tid}: unknown size {t.get('size')!r}")
        if t.get("owner") not in OWNERS:
            r.error(f"{tid}: unknown owner {t.get('owner')!r}")
        if t.get("size") == "L" and not t.get("design_note"):
            r.error(f"{tid}: size L needs a design_note saying why it cannot be split")
        if len(t.get("title", "")) > 72:
            r.warn(f"{tid}: title longer than 72 characters")
        if t.get("type") == "spike" and not any("docs/adr/" in a for a in t.get("acceptance", [])):
            r.error(f"{tid}: a spike's acceptance criteria must name its record in docs/adr/")
        for a in t.get("acceptance", []):
            if VAGUE.search(a):
                r.warn(f"{tid}: vague acceptance criterion: {a[:70]!r}")
        for b in t.get("blocked_by", []):
            if b not in {x.get("id") for x in tasks}:
                r.error(f"{tid}: blocked by unknown task {b}")
            if b == tid:
                r.error(f"{tid}: blocks itself")

    _, stuck = topo_order(tasks)
    if stuck:
        r.error("dependency cycle among: " + ", ".join(stuck))
        return r

    # Tasks that may run at the same time (neither blocks the other, directly or transitively)
    # should not touch the same files. The next milestone's tasks start while the current one is
    # finishing, so pairs across milestones count too. Report each overlapping pair once.
    anc = ancestors(tasks)
    by_ms: dict[str, list[dict]] = defaultdict(list)
    for t in tasks:
        by_ms[t["id"].split("-")[0]].append(t)
    for i, a in enumerate(tasks):
        for b in tasks[i + 1:]:
            if a["id"] in anc[b["id"]] or b["id"] in anc[a["id"]]:
                continue
            shared = sorted({pa for pa in a["touches"] for pb in b["touches"]
                             if paths_overlap(pa, pb)})
            if shared:
                r.warn(f"{a['id']} and {b['id']} may run in parallel but both touch "
                       + ", ".join(shared))

    # The first milestone needs at least three ready tasks, in different areas, that do not
    # touch the same files.
    if tasks:
        first = sorted(by_ms)[0]
        ready = [t for t in by_ms[first] if not t["blocked_by"] and t["owner"] == "agent"]
        areas_ready = {t["area"] for t in ready}
        r.notes.append(f"{first}: {len(ready)} agent tasks ready at the start, in areas "
                       + ", ".join(sorted(areas_ready)))
        if len(areas_ready) < 3:
            r.error(f"{first}: fewer than three ready agent tasks in different areas")
    return r


# ---------------------------------------------------------------------------------------------
# Rendering issue bodies
# ---------------------------------------------------------------------------------------------

def bullet(items: list[str], check: bool = False) -> str:
    prefix = "- [ ] " if check else "- "
    return "\n".join(prefix + i for i in items)


def render_task(t: dict, numbers: dict[str, int], epic_number: int | None) -> str:
    def ref(i: str) -> str:
        return f"#{numbers[i]}" if i in numbers else i

    blocked = ", ".join(ref(b) for b in t["blocked_by"]) or "none"
    touches = ", ".join(f"`{p}`" for p in t["touches"])
    epic = f"#{epic_number}" if epic_number else t["epic"]
    verification = "\n".join(t["verification"])
    parts = [
        f"ID: {t['id']}        Area: {t['area']}      Type: {t['type']}      "
        f"Size: {t['size']}      Owner: {t['owner']}",
        f"Epic: {epic}",
        f"Blocked by: {blocked}",
        f"Touches: {touches}",
        "",
        "## Goal",
        t["goal"],
        "",
        "## Context",
        bullet(t["context"]),
        "",
        "## Acceptance criteria",
        bullet(t["acceptance"], check=True),
        "",
        "## Out of scope",
        bullet(t["out_of_scope"]),
        "",
        "## Verification",
        "```sh",
        verification,
        "```",
    ]
    if t.get("verification_visual"):
        parts += ["", t["verification_visual"]]
    if t.get("design_note"):
        parts += ["", "## Design note", t["design_note"]]
    parts += ["", f"<!-- generated from docs/roadmap/tasks.toml ({t['id']}) -->"]
    return "\n".join(parts) + "\n"


def render_epic(e: dict, tasks: list[dict], numbers: dict[str, int]) -> str:
    rows = [f"- {('#' + str(numbers[t['id']])) if t['id'] in numbers else t['id']} "
            f"[{t['id']}] {t['title']}" for t in tasks]
    parts = [
        f"Epic: {e['id']}        Milestone: {e['milestone']}        Area: {e['area']}",
        "",
        "## Goal",
        e["goal"],
        "",
        "## Done when",
        e["demo"],
        "",
        "## Tasks",
        "\n".join(rows) if rows else "Not decomposed yet: see docs/roadmap.md. `/groom` "
        "decomposes this epic when the milestone before it is about 70% done.",
        "",
        f"<!-- generated from docs/roadmap/tasks.toml ({e['id']}) -->",
    ]
    return "\n".join(parts) + "\n"


def task_labels(t: dict) -> list[str]:
    return [f"area:{t['area']}", f"type:{t['type']}", f"size:{t['size']}", f"owner:{t['owner']}"]


# ---------------------------------------------------------------------------------------------
# GitHub access through gh
# ---------------------------------------------------------------------------------------------

class GitHub:
    def __init__(self, repo: str, apply: bool, offline: bool):
        self.repo = repo
        self.owner = repo.split("/")[0]
        self.apply = apply
        self.offline = offline

    def gh(self, *args: str, stdin: str | None = None, write: bool = False) -> str:
        if write and not self.apply:
            raise AssertionError("write attempted in dry-run mode")
        if self.offline:
            raise AssertionError("network access attempted in offline mode")
        proc = subprocess.run(["gh", *args], input=stdin, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"gh {' '.join(args[:3])} ... failed: {proc.stderr.strip()}")
        if write:
            time.sleep(WRITE_PAUSE_S)
        return proc.stdout

    def gh_json(self, *args: str) -> object:
        out = self.gh(*args)
        return json.loads(out) if out.strip() else None

    # Reads ------------------------------------------------------------------------------------
    def labels(self) -> dict[str, dict]:
        if self.offline:
            return {}
        rows = self.gh_json("label", "list", "-R", self.repo, "--limit", "500",
                            "--json", "name,color,description")
        return {row["name"]: row for row in rows}

    def milestones(self) -> dict[str, dict]:
        if self.offline:
            return {}
        rows = self.gh_json("api", f"repos/{self.repo}/milestones?state=all&per_page=100")
        return {row["title"]: row for row in rows}

    def issues(self) -> dict[str, dict]:
        """Existing issues by task or epic ID."""
        if self.offline:
            return {}
        rows = self.gh_json("issue", "list", "-R", self.repo, "--state", "all", "--limit", "3000",
                            "--json", "number,title,state,labels,milestone,body,url")
        found: dict[str, dict] = {}
        for row in rows:
            m = TITLE_ID.match(row["title"])
            if m:
                found[m.group("id")] = row
        return found

    def relations(self, number: int) -> dict:
        return self.gh_json("issue", "view", str(number), "-R", self.repo,
                            "--json", "parent,blockedBy")


def numbers_of(items: object) -> set[int]:
    """Issue numbers from gh's JSON for a relation, whatever its exact shape."""
    if items is None:
        return set()
    if isinstance(items, dict):
        if "number" in items:
            return {int(items["number"])}
        for key in ("nodes", "issues", "items"):
            if key in items:
                return numbers_of(items[key])
        return set()
    if isinstance(items, list):
        out: set[int] = set()
        for i in items:
            out |= numbers_of(i)
        return out
    return set()


# ---------------------------------------------------------------------------------------------
# The plan
# ---------------------------------------------------------------------------------------------

def run(data: dict, gh: GitHub, only_milestone: str | None, update_bodies: bool,
        board: bool) -> None:
    say = print
    mode = "APPLY" if gh.apply else ("DRY RUN, offline" if gh.offline else "DRY RUN")
    say(f"== {mode}: {gh.repo}")

    # Repository merge settings and branch protection --------------------------------------------
    say("\n-- repository settings")
    if gh.offline:
        say("   would set: " + ", ".join(f"{k}={v}" for k, v in REPO_SETTINGS.items()))
    else:
        have = gh.gh_json("api", f"repos/{gh.repo}")
        diff = {k: v for k, v in REPO_SETTINGS.items() if have.get(k) != v}
        if not diff:
            say("   ok      squash merge only, branches deleted after merge")
        for k, v in diff.items():
            say(f"   set     {k}: {have.get(k)} -> {v}")
        if diff and gh.apply:
            args = ["api", "-X", "PATCH", f"repos/{gh.repo}"]
            for k, v in diff.items():
                args += ["-F" if isinstance(v, bool) else "-f", f"{k}={str(v).lower() if isinstance(v, bool) else v}"]
            gh.gh(*args, write=True)
        try:
            gh.gh("api", f"repos/{gh.repo}/branches/main/protection")
            say("   ok      main is protected (rules are managed by hand)")
        except RuntimeError as err:
            if "Upgrade to GitHub Pro" in str(err):
                say("   note    branch protection is unavailable for a private repository on GitHub "
                    "Free; set it when the repository is public or on Pro")
            elif "not protected" in str(err).lower() or "404" in str(err):
                say("   note    main is not protected: set the rules by hand (docs/roadmap.md)")
            else:
                raise

    # Labels ------------------------------------------------------------------------------------
    existing_labels = gh.labels()
    say("\n-- labels")
    for lab in data["labels"]:
        have = existing_labels.get(lab["name"])
        same = have and have["color"].lower() == lab["color"].lower() and \
            (have.get("description") or "") == lab["description"]
        if same:
            say(f"   ok      {lab['name']}")
            continue
        say(f"   {'update' if have else 'create'}  {lab['name']}")
        if gh.apply:
            gh.gh("label", "create", lab["name"], "-R", gh.repo, "--color", lab["color"],
                  "--description", lab["description"], "--force", write=True)

    # Milestones --------------------------------------------------------------------------------
    existing_ms = gh.milestones()
    say("\n-- milestones")
    for m in data["milestones"]:
        if only_milestone and m["id"] != only_milestone:
            continue
        have = existing_ms.get(m["title"])
        due = f"{m['due_on']}T23:59:59Z" if m.get("due_on") else None
        if have:
            same_due = (have.get("due_on") or None) == due
            if same_due and (have.get("description") or "") == m["description"]:
                say(f"   ok      {m['title']}")
                continue
            say(f"   update  {m['title']} (due {m.get('due_on') or 'none'})")
            if gh.apply:
                args = ["api", "-X", "PATCH", f"repos/{gh.repo}/milestones/{have['number']}",
                        "-f", f"description={m['description']}"]
                if due:
                    args += ["-f", f"due_on={due}"]
                gh.gh(*args, write=True)
            continue
        say(f"   create  {m['title']} (due {m.get('due_on') or 'none'})")
        if gh.apply:
            args = ["api", "-X", "POST", f"repos/{gh.repo}/milestones",
                    "-f", f"title={m['title']}", "-f", f"description={m['description']}"]
            if due:
                args += ["-f", f"due_on={due}"]
            gh.gh(*args, write=True)
    ms_title = {m["id"]: m["title"] for m in data["milestones"]}

    # Issues ------------------------------------------------------------------------------------
    existing = gh.issues()
    numbers: dict[str, int] = {i: row["number"] for i, row in existing.items()}
    urls: dict[str, str] = {i: row["url"] for i, row in existing.items()}
    epics = [e for e in data["epics"] if not only_milestone or e["milestone"] == only_milestone]
    tasks_all = data["tasks"]
    order, _ = topo_order(tasks_all)
    by_id = {t["id"]: t for t in tasks_all}
    tasks = [by_id[i] for i in order
             if not only_milestone or by_id[i]["id"].startswith(only_milestone + "-")]
    tasks_of_epic: dict[str, list[dict]] = defaultdict(list)
    for t in tasks_all:
        tasks_of_epic[t["epic"]].append(t)

    created = 0
    placeholder = 10_000  # dry-run numbers, so that rendered bodies stay readable

    def create_issue(item_id: str, title: str, body: str, labels: list[str], milestone: str,
                     parent: int | None, blocked_by: list[int]) -> None:
        nonlocal created, placeholder
        created += 1
        extra = []
        if parent:
            extra.append(f"parent #{parent}")
        if blocked_by:
            extra.append("blocked by " + ", ".join(f"#{n}" for n in blocked_by))
        say(f"   create  {title}" + (f"   ({'; '.join(extra)})" if extra else ""))
        if not gh.apply:
            placeholder += 1
            numbers[item_id] = placeholder
            return
        args = ["issue", "create", "-R", gh.repo, "--title", title, "--body-file", "-",
                "--milestone", milestone]
        for lab in labels:
            args += ["--label", lab]
        if parent:
            args += ["--parent", str(parent)]
        if blocked_by:
            args += ["--blocked-by", ",".join(str(n) for n in blocked_by)]
        url = gh.gh(*args, stdin=body, write=True).strip().splitlines()[-1]
        numbers[item_id] = int(url.rstrip("/").split("/")[-1])
        urls[item_id] = url

    say("\n-- epics")
    for e in epics:
        title = f"[{e['id']}] {e['title']}"
        if e["id"] in existing:
            say(f"   ok      #{numbers[e['id']]} {title}")
            continue
        create_issue(e["id"], title, render_epic(e, tasks_of_epic[e["id"]], numbers),
                     ["epic", f"area:{e['area']}"], ms_title[e["milestone"]], None, [])

    say("\n-- tasks")
    epic_of = {e["id"]: e for e in data["epics"]}
    for t in tasks:
        title = f"[{t['id']}] {t['title']}"
        epic_no = numbers.get(t["epic"])
        blockers = [numbers[b] for b in t["blocked_by"] if b in numbers]
        body = render_task(t, numbers, epic_no)
        if t["id"] not in existing:
            create_issue(t["id"], title, body, task_labels(t),
                         ms_title[epic_of[t["epic"]]["milestone"]], epic_no, blockers)
            continue
        row = existing[t["id"]]
        say(f"   ok      #{row['number']} {title}")
        have_labels = {lab["name"] for lab in row["labels"]}
        missing = [lab for lab in task_labels(t) if lab not in have_labels]
        if missing:
            say(f"           add labels {', '.join(missing)}")
            if gh.apply:
                gh.gh("issue", "edit", str(row["number"]), "-R", gh.repo,
                      "--add-label", ",".join(missing), write=True)
        if update_bodies and row["state"] == "OPEN" and row["body"].strip() != body.strip():
            say("           update body")
            if gh.apply:
                gh.gh("issue", "edit", str(row["number"]), "-R", gh.repo, "--body-file", "-",
                      stdin=body, write=True)
        if not gh.offline:
            rel = gh.relations(row["number"])
            if epic_no and epic_no not in numbers_of(rel.get("parent")):
                say(f"           set parent #{epic_no}")
                if gh.apply:
                    gh.gh("issue", "edit", str(row["number"]), "-R", gh.repo,
                          "--parent", str(epic_no), write=True)
            want = set(blockers) - numbers_of(rel.get("blockedBy"))
            if want:
                say("           add blocked-by " + ", ".join(f"#{n}" for n in sorted(want)))
                if gh.apply:
                    gh.gh("issue", "edit", str(row["number"]), "-R", gh.repo,
                          "--add-blocked-by", ",".join(str(n) for n in sorted(want)),
                          write=True)

    # Epic bodies list their tasks by number, which is only known once the tasks exist.
    say("\n-- epic task lists")
    for e in epics:
        if e["id"] in existing and not update_bodies:
            continue
        say(f"   {'write' if gh.apply else 'would write'} task list of {e['id']} "
            f"({len(tasks_of_epic[e['id']])} tasks)")
        if gh.apply:
            gh.gh("issue", "edit", str(numbers[e["id"]]), "-R", gh.repo, "--body-file", "-",
                  stdin=render_epic(e, tasks_of_epic[e["id"]], numbers), write=True)

    if board:
        setup_board(data, gh, [urls[i] for i in list(urls)] if gh.apply else [], say)

    say(f"\n== {created} issues to create" if not gh.apply else f"\n== created {created} issues")


def setup_board(data: dict, gh: GitHub, item_urls: list[str], say) -> None:
    title = data["project_board"]
    say(f"\n-- project board {title!r}")
    if gh.offline:
        say("   would create the board, link it to the repository and add every issue")
        return
    projects = gh.gh_json("project", "list", "--owner", gh.owner, "--format", "json")
    found = next((p for p in projects.get("projects", []) if p["title"] == title), None)
    if not found:
        say("   create board, link to repository")
        if not gh.apply:
            say(f"   would set Status options to: {', '.join(BOARD_COLUMNS)}")
            say("   would add every epic and task issue")
            return
        found = gh.gh_json("project", "create", "--owner", gh.owner, "--title", title,
                           "--format", "json")
        gh.gh("project", "link", str(found["number"]), "--owner", gh.owner,
              "--repo", gh.repo.split("/")[1], write=True)
    else:
        say(f"   ok      board #{found['number']}")
    number = str(found["number"])
    fields = gh.gh_json("project", "field-list", number, "--owner", gh.owner, "--format", "json")
    status = next((f for f in fields.get("fields", []) if f["name"] == "Status"), None)
    names = [o["name"] for o in (status or {}).get("options", [])]
    if status and names != list(BOARD_COLUMNS):
        say(f"   set Status options {names} -> {list(BOARD_COLUMNS)}")
        if gh.apply:
            options = ",".join(
                f'{{name: "{n}", color: GRAY, description: ""}}' for n in BOARD_COLUMNS)
            query = ("mutation { updateProjectV2Field(input: {fieldId: \"%s\", "
                     "singleSelectOptions: [%s]}) { clientMutationId } }") % (status["id"], options)
            try:
                gh.gh("api", "graphql", "-f", f"query={query}", write=True)
            except RuntimeError as err:
                say(f"   could not set the columns ({err}); set them by hand in the board")
    for url in item_urls:
        if gh.apply:
            gh.gh("project", "item-add", number, "--owner", gh.owner, "--url", url, write=True)
    if gh.apply:
        say(f"   added {len(item_urls)} items")
    say("   by hand, once: board > Workflows > enable 'Item added to project' (Backlog), "
        "'Pull request linked to issue' (In review), 'Item closed' and 'Pull request merged' (Done)")


# ---------------------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--tasks", type=Path, default=DEFAULT_TASKS)
    p.add_argument("--check", action="store_true", help="validate tasks.toml and exit")
    p.add_argument("--apply", action="store_true", help="write to GitHub (default: dry run)")
    p.add_argument("--offline", action="store_true", help="dry run without reading GitHub")
    p.add_argument("--milestone", help="only this milestone's epics and tasks, e.g. M3")
    p.add_argument("--update-bodies", action="store_true",
                   help="rewrite the bodies of open issues that differ from tasks.toml")
    p.add_argument("--no-board", action="store_true", help="skip the Projects board")
    p.add_argument("--show", metavar="ID", help="print the rendered body of one task or epic")
    p.add_argument("--table", metavar="MILESTONE", help="print a milestone's task table")
    p.add_argument("--graph", metavar="MILESTONE", help="print a milestone's task graph (Mermaid)")
    args = p.parse_args(argv)

    data = load(args.tasks)
    report = validate(data)
    for n in report.notes:
        print(f"note: {n}")
    for w in report.warnings:
        print(f"warning: {w}")
    for e in report.errors:
        print(f"error: {e}", file=sys.stderr)
    if report.errors:
        print(f"{len(report.errors)} errors in {args.tasks}", file=sys.stderr)
        return 1
    if args.check:
        print(f"ok: {len(data['epics'])} epics, {len(data['tasks'])} tasks, no errors, "
              f"{len(report.warnings)} warnings")
        return 0
    if args.table:
        print(milestone_table(data, args.table))
        return 0
    if args.graph:
        print(milestone_graph(data, args.graph))
        return 0
    if args.show:
        tasks = {t["id"]: t for t in data["tasks"]}
        epics = {e["id"]: e for e in data["epics"]}
        if args.show in tasks:
            print(f"[{args.show}] {tasks[args.show]['title']}\n")
            print(render_task(tasks[args.show], {}, None))
        elif args.show in epics:
            print(f"[{args.show}] {epics[args.show]['title']}\n")
            print(render_epic(epics[args.show],
                              [t for t in data["tasks"] if t["epic"] == args.show], {}))
        else:
            print(f"no task or epic {args.show}", file=sys.stderr)
            return 1
        return 0
    if args.apply and args.offline:
        p.error("--apply needs GitHub; drop --offline")
    gh = GitHub(data["repository"], apply=args.apply, offline=args.offline)
    run(data, gh, args.milestone, args.update_bodies, board=not args.no_board)
    return 0


if __name__ == "__main__":
    sys.exit(main())
