---
name: task
description: Do one GitHub task issue of this repository from start to pull request, cold, in the current worktree. Use it whenever the user or a dispatch prompt says "/task 42", "do issue 42", "work on M1-07", "pick up #42" or starts a session on a single issue, even if they only paste the issue link. It checks blockers, claims the issue, branches, plans, writes tests from the acceptance criteria, implements inside the issue's Touches, verifies, and finishes with /ship. Not for grooming the backlog (/groom) or starting several tasks (/dispatch).
---

# /task <issue>

One issue, one session, one branch, one pull request. The issue is the specification: its
**Goal**, **Acceptance criteria**, **Touches**, **Out of scope** and **Verification** say what
done means. `CLAUDE.md` holds the rules every task follows. This skill is the order of work.

The argument is an issue number (`42`, `#42`, a URL) or a task ID (`M1-07`). For a task ID, find
the issue: `gh issue list --state all --search "[M1-07] in:title" --json number,title`.

## 1. Read the issue and check it can start

```sh
gh issue view <n> --json number,title,body,state,labels,milestone,blockedBy,parent,closedByPullRequestsReferences
```

Stop and report, without changing anything, if any of these hold. Each one means another
session or the owner is already handling it, or the work cannot succeed yet:

- The issue is closed, or has an open pull request linked.
- It has the `in-progress` label (unless the user said to resume it; then go to step 2 and
  continue on its existing branch).
- It is labelled `owner:human`: the owner writes these themselves.
- It is an `epic`.
- **A blocker is still open.** Blockers are the union of the native `blockedBy` links and the
  body's `Blocked by:` line (`Blocked by: #12, #15` or `none`; issues made with the form have a
  `### Blocked by` section instead). Check each with `gh issue view <b> --json state`.

Then read every blocker's merged pull request briefly (`gh pr list --state merged --search
"<blocker number>"`): it shows the interfaces this task builds on, as they really are.

## 2. Claim it and branch

```sh
gh issue edit <n> --add-label in-progress
git fetch origin main
git switch -c task/<n>-<slug> origin/main      # slug: 3 to 5 words of the title, kebab-case
```

If the session was started in a fresh worktree on another branch name, rename it with
`git branch -m task/<n>-<slug>`. Claiming first matters: `/dispatch` treats `in-progress`
issues as taken.

## 3. Understand before planning

Read the **Context** links, but only the parts the task needs: the blueprint section, the ADR,
the code to build on. For an external specification or paper (IES LM-63, a Vulkan chapter, an
API reference), hand it to a general-purpose subagent and ask for the details that matter, so the
document does not flood this session. Use Explore to find code across the repository.

## 4. Plan in proportion to the size

- **S:** a short plan in your head is enough; go on.
- **M:** use plan mode: list the files you will change (all inside **Touches**), the public
  interfaces, and how each acceptance criterion will be checked. Then leave plan mode and work.
- **L:** use the Plan subagent, then plan mode; check the plan against the issue's **Design
  note** and post the plan as an issue comment before writing code.

If the plan needs a file outside **Touches**, a new dependency, or a design decision the issue
does not settle, stop: comment on the issue with what is missing and why, remove
`in-progress`, and report. Growing the task silently is what makes pull requests unreviewable
and parallel sessions collide.

## 5. Tests first

Turn every acceptance criterion into a test before writing the implementation, so the tests say
what the issue says rather than what the code happens to do.

- **M and L:** ask the `test-author` subagent, giving it the issue number. It reads only the
  issue and the public headers.
- **S:** write the tests yourself, from the acceptance criteria.

Build and confirm that the new tests fail (or do not compile yet) for the right reason.

## 6. Implement

Work inside **Touches**, respecting **Out of scope** and the architecture rules of `CLAUDE.md`
(module boundaries, units, the mutation API, hostile input). Commit as you reach working states,
in the repository's commit style: an imperative summary line, a blank line, bullets, the
Co-Authored-By trailer.

## 7. Verify

Run the issue's **Verification** commands exactly, and the definition of done's build and test
steps. Then the reviews that apply:

- Changes to materials, lights, photometry, sampling or lighting in any tier: the
  `physics-reviewer` subagent. Fix what it finds, or answer it in the pull request.
- Changes under `libs/gpu`: the `gpu-reviewer` subagent (once it exists).
- Changes under `libs/ui`: screenshots, then the `ux-reviewer` subagent (once it exists).
- A long compiler, sanitizer, validation or CI log: hand it to `build-doctor` rather than
  reading it here.
- Visual work: `/look` for renders, `/run` for the app.

## 8. Ship

Run `/ship`. It runs the rest of the definition of done, pushes, opens the pull request with the
template filled from what was actually run, and links the issue.

## If the session cannot finish

When context runs short or something outside the task blocks it: commit and push the branch,
comment on the issue with what is done, what is left and the exact next step, and leave
`in-progress` on. The next session resumes with `/task <n>` and the user's go-ahead to resume.
