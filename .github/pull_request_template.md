Closes #

## In plain words

<!-- For the owner, before the details. Two short paragraphs, three to six sentences in all, with
no file, function or library names, and any term the reader may not know explained in the same
sentence.
What it does: what the project can do after this pull request that it could not before, or what
problem it removes. If nothing is visible yet, say so and say what it is groundwork for.
Why it matters: where it fits in the product (the milestone's goal, a curator's journey, an exit
criterion), and what it unblocks next, by task ID and a few words. -->

**What it does.**

**Why it matters.**

## What changed

<!-- Two to five bullets, in the order a reviewer should read the diff. Name the main files. -->

-

## How it was verified

<!-- The commands that were actually run, and what they printed. Paste the summary lines, not
the whole log. Every acceptance criterion in the issue should map to one line here. -->

| Check | Command | Result |
| --- | --- | --- |
| Format | `python3 scripts/format.py --check` | |
| Debug build with sanitizers, tests | `ctest --preset debug` | |
| Release build, tests | `ctest --preset release` | |
| Benchmarks (rendering, scene loading or memory only) | `/perf-check` | |
| `/simplify` (M and L tasks) | | |
| `/code-review` (level) | | |

## Before and after

<!-- Visual changes only: renders or screenshots, before and after, at the same viewpoint. Say
which tier and which scene. Delete this section otherwise. -->

## Concepts

<!-- Three to six bullets that explain the ideas this code relies on: the theory, not the diff.
Write for a programmer who has not met the topic, and link one good source for each. -->

-

## Follow-ups

<!-- Anything found along the way that is out of this task's scope: a link to a new issue, or
"none". -->

- none
