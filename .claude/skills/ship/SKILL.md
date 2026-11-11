---
name: ship
description: Finish a task branch in this repository: run the full definition of done (format, debug and release builds, tests, GPU and UI tests where relevant, benchmarks, /simplify, /code-review, /security-review), push, and open the pull request with the template filled from what was actually run, linked to its issue, then see CI through. Use it at the end of every /task, and whenever the user says "ship it", "open the PR", "we're done, make the pull request" on a task/<issue>-* branch. Not for planning documents or branches without an issue.
---

# /ship

A pull request reaches the owner only when it is finished: the owner's review time is the
bottleneck of the whole project, so everything a machine can check is checked first, and the
pull request says exactly what was run. Report honestly: a step skipped or failing is written
down as such, never presented as passing.

## 0. Know the task

The branch is `task/<n>-<slug>`; `<n>` is the issue. Read it again
(`gh issue view <n> --json title,body,labels`) for its size, area, acceptance criteria and
verification commands. List what changed: `git diff --stat origin/main...HEAD`.

Note which of these the diff touches, because they decide the steps below:
`libs/gpu` (GPU tests, gpu-reviewer), `libs/ui` or `apps/curation` (app build, UI tests,
screenshots), rendering, scene loading or memory (benchmarks), a parser or importer (security
review), physics (physics-reviewer, high-level code review).

## 1. Definition of done

Run each step and keep its command and result for the pull request. Fix failures and rerun; if
a fix would leave the issue's **Touches**, stop and comment on the issue instead.

1. **Format:** `python3 scripts/format.py`, then `python3 scripts/format.py --check`.
2. **Debug build with sanitizers and its tests:**
   `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`.
   When `libs/ui` or `apps/curation` changed, also build the `app` preset and run its UI tests:
   `cmake --preset app && cmake --build --preset app && QT_QPA_PLATFORM=offscreen ctest --preset app -L ui`.
3. **Release build and tests:** the same with `--preset release`, including `-L golden`.
4. **GPU tests** when `libs/gpu` changed, with `CS_REQUIRE_GPU=1` so that a driver that does not
   load fails the tests instead of skipping them: on the real GPU with
   `CS_REQUIRE_GPU=1 ctest --preset debug -L gpu`, and on lavapipe with
   `CS_REQUIRE_GPU=1 VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --preset debug -L gpu`.
   Some distributions name the file lvp_icd.x86_64.json; `ls /usr/share/vulkan/icd.d/` shows which.
5. **Benchmarks** when rendering, scene loading or memory changed: `/perf-check`. A budget
   exceeded is a failure unless the issue says otherwise.
6. **Acceptance criteria:** go through them one by one; each needs a test or command whose
   output shows it. Tick them in the issue only when shown.
7. **Visual work:** renders (`/look`) or screenshots (`/run`), before and after, from the same
   viewpoint.
8. **M and L tasks:** run `/simplify` and keep its changes if tests still pass.
9. **Code review:** `/code-review high` for rendering, lighting, materials, sampling and
   photometry; `/code-review medium` otherwise. Fix each finding or write why not.
10. **Security review** for a new or changed parser or importer (project files, IES, images,
    CSV, manifests): `/security-review`, findings fixed or answered.
11. **Generated or reference files** (golden images, baselines): changed only on purpose, with
    the reason ready for the pull request.

Re-run steps 1 to 3 after any fix made in steps 5 to 10.

## 2. Commit and push

Commits follow the repository style: an imperative summary line, a blank line, a body of
bullets, the Co-Authored-By trailer. Then `git push -u origin HEAD`.

## 3. Open the pull request

Fill `.github/pull_request_template.md` from what was actually run, and write it to a temporary
file:

- **Closes #<n>** on the first line, so merging closes the issue and the board moves it.
- **What changed:** two to five bullets in reading order, naming the main files.
- **How it was verified:** the table, one row per step above, with the command and the summary
  line of its output (pass counts, the benchmark table, FLIP values). A skipped step says
  "skipped" and why.
- **Before and after:** attach the renders or screenshots for visual work; otherwise delete the
  section.
- **Concepts:** three to six bullets explaining the ideas the code relies on (the theory, not
  the diff), each with one good source. The owner learns from these; write them for a programmer
  new to the topic.
- **Follow-ups:** issues found along the way, as links to new issues (create them with the task
  form) or "none".

Title: an imperative summary under 72 characters with the task ID, for example
`Write EXR and PNG files and compare images with FLIP [M1-07]`.

```sh
gh pr create --base main --title "<title>" --body-file <file> --label area:<area>
```

## 4. See CI through

Watch the checks: `gh pr checks <pr> --watch --fail-fast` (run it in the background if it may
take long). If a job fails, give the log (`gh run view <run> --log-failed`) to the
`build-doctor` subagent, fix, push, and watch again. In the Claude desktop app, Auto-fix may
handle CI for you; do not duplicate it.

## 5. Report

The pull request URL, the definition-of-done table, anything skipped or answered rather than
fixed, and the follow-ups. Leave the `in-progress` label: the open linked pull request shows the
task is in review, and merging closes the issue.
