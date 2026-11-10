---
name: look
description: Render a scene or project of this repository, or screenshot it in the app, look at the image, report the lux on every work, and compare with the golden image if one exists. Use it whenever a change could alter what is drawn (shaders, materials, lights, geometry, frames, tone mapping, overlays), when the owner asks to "see", "show me", "render", "what does it look like", or "check the lighting", and before attaching before-and-after images to a pull request. For benchmark numbers use /perf-check instead.
---

# /look [scene] [--tier standard|path|both] [--app]

Visual changes are tested with numbers (golden images, lux), but a person still has to see them:
a render can pass every tolerance and still have an upside-down painting. This skill produces the
images, looks at them, and reports what the numbers say.

`scene` is a project folder or `.curation` file; it defaults to `tests/scenes/skeleton`. Also
accepted: a benchmark scene (`bench/scenes/empty-room`, or `build/bench/scenes/gallery-50` after
`cs-bench generate gallery-50`) or a golden test name
(`tests/golden/<name>`).

## 1. Build

```sh
cmake --preset release && cmake --build --preset release --target cs-render
```

## 2. Render

For each requested tier (default: both), into `build/look/`:

```sh
./build/release/apps/cs-render/cs-render <scene> --tier standard --size 960x540 --out build/look/<name>-standard
./build/release/apps/cs-render/cs-render <scene> --tier path --size 960x540 --spp 128 --seed 1 --out build/look/<name>-path
```

The Standard tier uses the real GPU unless `CS_GPU_DEVICE` or `VK_DRIVER_FILES` says otherwise;
say which device rendered it. With `--app`, launch the application instead with `/run`, open the
scene, and take a screenshot of the window.

## 3. Look

Open each PNG and describe what you see in a few lines: orientation of the works, frames,
light pools and shadows, anything black, over-exposed, misplaced or missing. Compare the two
tiers with each other: where they differ, which one is wrong is a question for the engine (the
path tier is the reference).

Show the images to the owner: attach them with the file-sending tool when one is available,
otherwise give their paths.

## 4. Report the lux

Read `<prefix>.lux.json` from the path tier and print one row per work: title, mean direct and
total lux, minimum and maximum, uniformity, and, once conservation classes exist, the target and
whether it is exceeded. Flag a work whose standard error is above 2%.

## 5. Compare with the golden image

If `tests/golden/<name>-<tier>/` exists for this scene:

```sh
./build/release/apps/cs-render/cs-render golden tests/golden/<name>-<tier>
```

Report the mean FLIP and mean absolute error against the tolerance. On a failure, open the
difference image and the FLIP heat map from `build/release/golden-out/<name>-<tier>/` and say
where the difference is. Never pass `--update` from here: updating a reference is a decision for
a pull request that explains why.

## Before and after for a pull request

Render the same scene, tier, size and seed on `origin/main` (in a temporary worktree:
`git worktree add ../look-main origin/main`) and on the branch, and attach both. Remove the
temporary worktree afterwards.
