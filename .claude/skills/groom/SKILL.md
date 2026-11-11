---
name: groom
description: Weekly care of this repository's backlog and roadmap. Use it when the owner says "/groom", "weekly review", "tidy the backlog", "where are we", "update the roadmap", "plan the next milestone", or when /dispatch finds fewer than three ready tasks. It closes finished work, updates progress and dates in docs/roadmap.md, splits tasks that turned out too big, checks the dependency graph, makes sure at least three ready tasks exist in different lanes, and decomposes the next milestone into tasks once the current one is about 70% done. Changes to GitHub and to main are proposed first and applied with the owner's agreement.
---

# /groom

The backlog drifts every week: tasks finish without closing, some turn out too big, blockers
change, curator feedback arrives. Grooming keeps the issues truthful and the parallel sessions
supplied, so that `/dispatch` always has safe work to start. The issues are the source of truth
for status; `docs/roadmap.md` is the high-level picture; `docs/roadmap/tasks.toml` is the seed
for tasks that do not exist as issues yet.

Work in a fresh branch `groom/<yyyy-mm-dd>`. Read everything first, then present one summary of
proposed changes, and apply them after the owner agrees. Grooming that silently rewrites the
backlog is worse than none.

## 1. Take stock

```sh
gh issue list --state all --limit 1000 --json number,title,state,labels,milestone,closedAt,updatedAt
gh pr list --state all --limit 100 --json number,title,state,mergedAt,createdAt,headRefName
python3 scripts/ready-tasks.py --json
```

Per milestone: closed, open, in progress, in review (open pull request). Throughput: tasks closed
in each of the last two weeks. Pull requests waiting for the owner, oldest first.

## 2. Close finished work

- An issue whose pull request merged but which is still open (a missing `Closes`): close it with
  a link to the pull request.
- An epic whose sub-issues are all closed: check that its demo (the epic's "Done when") exists,
  in a pull request or as `docs/product/screens/`; close it with a link, or note what is missing.
- An `in-progress` issue with no commits, comments or pull request activity for three days: ask
  in the issue whether it is abandoned; propose removing the label.

## 3. Repair tasks that went wrong

Signs: a session stopped and commented that the task needs more; a pull request over about 400
lines of non-test code or touching files outside its issue's Touches; a task reopened twice.

For each, propose a split into tasks that each meet the rules of docs/roadmap.md ("How the work
is cut"): one area, an automated check, 50 to 400 lines, exact Touches. New tasks take the next
free IDs of their milestone; add them to `docs/roadmap/tasks.toml`, fix the `Blocked by` of tasks
that depended on the old one, and create them with the bootstrap script (step 7).

Issues created with the task form: add the labels their fields name, and rewrite the body into
the standard layout (`python3 scripts/github-bootstrap.py --show <ID>` prints it once the task is
in tasks.toml).

## 4. Check the graph

- Every open task's `Blocked by` line names issues that exist; none names itself; no cycles
  (`python3 scripts/github-bootstrap.py --check` checks tasks.toml; check the live issues the same
  way from their bodies).
- Native GitHub "blocked by" links match the body lines (the bootstrap script adds missing ones).
- Tasks that may run in parallel do not have overlapping Touches.

## 5. Keep three lanes open

`scripts/ready-tasks.py` must show at least three ready tasks in different areas with no
overlapping Touches. If not, find why: a chain waiting on one pull request (ask the owner to
review it first), a task that could be split so part of it starts now, or a later task that
could move forward. Propose the smallest change that restores three lanes.

Label `priority:high` a ready task that must land before others start, such as a fix to a
command every session runs; `/dispatch` takes it first. Give it an area whose lane is free, so
it does not push a critical-path task out of its lane, and remove the label once it no longer
needs to jump the queue.

## 6. Update the roadmap

In `docs/roadmap.md`:

- the **Progress** table: done, open, in progress per milestone, with one line of notes;
- milestone **target dates**: remaining tasks divided by the measured throughput, plus the
  milestone's known slack; propose new dates when they move by more than a week, and change the
  GitHub milestone due dates after the owner agrees;
- the task tables and graphs of milestones being decomposed, regenerated with
  `python3 scripts/github-bootstrap.py --table <M>` and `--graph <M>`.

Curator feedback issues: weigh each against the blueprint's "What their feedback can change"
table, and propose roadmap changes it justifies.

## 7. Decompose the next milestone at about 70%

When the current milestone is about 70% closed, decompose the next one so its tasks are ready
before the current one runs dry:

1. Read the next milestone's section and epics in `docs/roadmap.md`, the blueprint, the relevant
   ADRs, and the code as it now is. Run any spike the milestone lists first; its decision record
   shapes the tasks.
2. Draft its tasks in `docs/roadmap/tasks.toml`, every field filled, following "How the work is
   cut" and the examples of M1 and M2. Write exit criteria as checks, each delivered by a task.
3. `python3 scripts/github-bootstrap.py --check` must report no errors and no warnings.
4. Have a **fresh subagent** critique the draft against the granularity rules (too big, too
   small, missing checks, overlapping Touches, cycles, exit criteria no task delivers); fix what
   it finds.
5. Show the owner the table and the graph, and wait for approval.

## 8. Apply

After the owner agrees:

```sh
python3 scripts/github-bootstrap.py --milestone <M>            # dry run: read what it will do
python3 scripts/github-bootstrap.py --milestone <M> --apply
```

Commit the roadmap and tasks.toml changes on the groom branch and open a pull request
(`docs: groom <date>`), so main changes only through pull requests.

## Close a finished milestone

When every task and epic of a milestone is closed and its exit criteria in `docs/roadmap.md`
are met (each check names the tasks that deliver it), propose to the owner:

- closing the GitHub milestone;
- an annotated tag on `main`: `v0.0.<n>` for milestones 1 to 3 (`v0.0.1` for M1), `v0.1.0` for
  M4 (the curator alpha) and `v1.0.0` for M6, with the milestone's exit-criteria table as the
  message (`git tag -a <tag> -F <file> origin/main && git push origin <tag>`);
- the milestone's demo screenshots linked in the Progress table.

Tag only after the owner agrees: a pushed tag is public history.

## Once, after the first week of sessions

Suggest running `/fewer-permission-prompts` to add the commands sessions actually ran to the
project's allowlist.

## Report

Throughput and dates; what was closed; tasks split or added; graph problems fixed; the ready
lanes for next week; decisions waiting for the owner.
