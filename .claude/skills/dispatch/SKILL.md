---
name: dispatch
description: Start up to n ready task issues of this repository in parallel, one Claude Code session per task, each in its own git worktree running /task. Use it whenever the owner says "/dispatch", "/dispatch 2", "start the next tasks", "kick off three sessions", "what can run in parallel now", or asks to keep the sessions busy. It picks tasks that are unblocked, owned by agents, in different areas and with non-overlapping Touches, starts them, and reports what started and what is next in line. Not for doing a task in this session (/task).
---

# /dispatch [n]

The owner's review time is the bottleneck, so the goal is a steady flow of finished pull
requests that never conflict with each other. Parallel sessions conflict when they edit the same
files, so the choice of tasks matters more than the starting.

`n` defaults to 3 minus the number of tasks already in progress (at most three sessions run at
once). An explicit `n` from the owner wins.

## 1. Find the ready tasks and a safe set

```sh
python3 scripts/ready-tasks.py --n <n> --json
```

The script reads GitHub (it never writes) and returns:

- `in_progress`: tasks labelled `in-progress` (their areas and files are taken);
- `ready`: open, `owner:agent`, not `blocked` or `epic`, no open pull request, every issue on
  the `Blocked by` line closed; sorted with `priority:high` first, then by milestone, then by
  how many tasks each unblocks;
- `suggested`: a greedy set of up to n ready tasks, one per area, whose Touches overlap neither
  each other nor anything in progress;
- `not_ready`: everything else, with the reasons.

Look over the suggestion before starting. Prefer a critical-path task (docs/roadmap.md marks
them) when two are otherwise equal, and swap a task out if its issue says it needs the owner
first (for example a spike that asks the owner to run something on Windows).

If fewer than n tasks are safe to start, start those and say why the rest are not.

## 2. Start one session per task

Each session runs `/task <issue>` in its own worktree, on a fresh branch from `origin/main`.
Use the first of these that is available:

1. **Claude desktop app:** its start-session tool (named like `start_session`), once per task,
   with a new worktree, the prompt `/task <issue>`, and a title `<ID> <issue title>`. These are
   real sessions the owner can open, steer and stop, and they report back when they finish.
2. **Claude Code CLI:** one background session per task:

   ```sh
   claude --worktree task-<issue> --bg --name "<ID> <short title>" "/task <issue>"
   ```

   `claude agents` lists them; `claude attach <name>` opens one. The project's
   `.claude/settings.json` allows the routine commands, so sessions rarely stop for permission.
3. **Neither available** (for example inside a cloud session): use the Agent tool with
   `isolation: "worktree"` and the prompt `/task <issue>`, one call per task in one message.
   Results come back to this session, so keep the report short.

Do not claim the issues here: `/task` adds `in-progress` itself, after checking its blockers,
so a session that fails to start leaves nothing stale.

## 3. Report

- **Started:** issue, task ID, area, and the session's name or link.
- **Next in line:** the next three ready tasks the script did not choose, with why (area busy,
  files overlap).
- **Blocked:** the count, and any task blocked only by issues with open pull requests (merging
  those unblocks it).
- **Warnings:** fewer than three ready tasks in different areas (run `/groom`), or an
  `in-progress` issue with no activity for three days (it may be abandoned).
