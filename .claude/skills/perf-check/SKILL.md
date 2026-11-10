---
name: perf-check
description: Run this repository's benchmark scenes at a quality tier on the current machine and compare frame time, memory, load time, lux, bake and still times with the budgets in bench/budgets.json and the machine's last baseline. Use it before shipping any change to rendering, shaders, textures, scene loading, the engine or memory use; whenever the owner asks "is it fast enough", "check performance", "run the benchmarks", "did this regress"; and on a new machine to record its first baseline. For looking at images use /look instead.
---

# /perf-check [tier] [--update-baseline]

The blueprint's budgets (section 6) are promises: 60 fps on a Radeon 780M, 30 on an Iris Xe, lux
on every work within a second. They hold only if they are measured on real hardware after every
change that could break them. CI measures memory on software Vulkan, but frame times mean
something only on a real GPU, so this skill runs locally.

`tier` defaults to `standard`. `--update-baseline` records the results as the machine's new
baseline; use it only when the owner asks, or in a pull request that says why the numbers
changed.

## 1. Prepare

- Build release: `cmake --preset release && cmake --build --preset release`.
- Generate the benchmark scenes that are generated rather than committed:
  `./build/release/apps/cs-bench/cs-bench generate gallery-50 --out build/bench/scenes/gallery-50`.
- Identify the machine: run `cs-bench --device-report`; the baseline file is
  `bench/baselines/<machine>/<scene>-<tier>.json` (machine slugs such as `radeon-780m`, `rtx-2060`,
  `iris-xe`). With no
  baseline yet, this run becomes the first one if the owner agrees.
- Timings are only comparable under the same conditions: laptop on mains power, other GPU-heavy
  programs closed, no software Vulkan (`VK_DRIVER_FILES` unset). Say if a condition is not met.

## 2. Run

For each benchmark scene (`bench/scenes/*` and the generated `build/bench/scenes/*`), at the chosen
tier:

```sh
./build/release/apps/cs-bench/cs-bench bench/scenes/<scene> --tier <tier> --size 1920x1080 --frames 600 --out build/bench/<scene>-<tier>.json
```

Add the other measurements as they exist: `--lux` (lux on every work after an edit), `--bake`
(indirect light after an edit), `--still` (1080p and 4K stills). Run each frame-time benchmark
twice and keep the better run if they differ by more than 5%: the first run sometimes pays for
shader and texture caches.

## 3. Compare

```sh
python3 scripts/perf-compare.py build/bench/<scene>-<tier>.json --budgets bench/budgets.json --baseline bench/baselines/<machine>/<scene>-<tier>.json --milestone <current milestone>
```

The script exits 1 if a budget already enforced at the current milestone is exceeded (each budget's
`enforced_from` says when it starts to count), or a metric is more than 10% worse than the
baseline. Budgets not enforced yet are still reported, so the gap is visible.

## 4. Report

One table per scene: metric, budget, baseline, now, change, verdict. Under it:

- every budget exceeded or regression, with the largest costs if `cs-bench` reports a breakdown;
- whether the regression is plausibly caused by the branch (`git diff --stat origin/main...HEAD`)
  or by the machine (a driver update, thermal throttling);
- for a pull request, the table as it should be pasted into the "How it was verified" section.

With `--update-baseline`, write the new numbers to the baseline file and say which changed.
