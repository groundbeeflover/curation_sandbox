# Golden-image tests

A golden-image test renders a scene and compares the picture with a reference committed here, so
that a change to what is drawn fails a test instead of waiting for someone to notice it
(blueprint section 9). The comparison is NVIDIA FLIP, which estimates how different two images
look to a person flipping between them, and the mean absolute error of the linear values in nits
(`libs/core/include/cs/core/image_compare.hpp`).

## Layout

Each directory here with a `test.json` is one test. Its name is kebab-case, usually the scene
and the tier: `skeleton-path`.

```
tests/golden/<name>/
    test.json           what to render, and how close it must be
    ref.<driver>.exr    the reference, one per driver: a 16-bit half-float EXR, tens of kilobytes
```

`test.json`:

```json
{
  "scene": "../../scenes/skeleton",
  "tier": "path",
  "width": 128,
  "height": 72,
  "samplesPerPixel": 64,
  "seed": 1,
  "tolerances": { "meanFlip": 0.02, "meanAbsoluteError_nits": 0.04 }
}
```

| Key | Meaning |
| --- | --- |
| `scene` | A project folder, relative to the test's directory. The scene's first viewpoint is rendered |
| `tier` | `path`: the engine's path tracer, on the CPU. `standard` arrives with M1-29 |
| `width`, `height` | Pixels: 128 to 256 a side (ADR 0010) |
| `samplesPerPixel` | Samples, or accumulated frames for the Standard tier: at most 64 at 256 x 256 |
| `seed` | Every random number comes from it, so a render repeats exactly on one machine |
| `tolerances.meanFlip` | The largest mean FLIP that passes, 0 to 1 |
| `tolerances.meanAbsoluteError_nits` | The largest mean absolute error that passes, in nits |

Any other key is an error, so a misspelt tolerance cannot be ignored silently.

**Drivers.** The engine's references are `ref.cpu.exr`. One CPU reference serves every compiler and
machine, within the tolerances (below). The GPU tiers render differently on each Vulkan driver,
so each driver they run on has its own reference. M1-29 names them, and ADR 0010 asks for one
lavapipe set, named for lavapipe and its Mesa version. A test with no reference for the driver
that runs it is **skipped**, not passed.

## Running

```sh
ctest --preset release -L golden                             # every golden test and the runner's checks
./build/release/apps/cs-render/cs-render golden tests/golden/<name>
```

CMake finds each `test.json` by glob (`cmake/CsGolden.cmake`) and registers `golden.<name>`. Its
label is `golden`, plus `gpu` when the tier is not `path`. `cs-render golden` exits with:

- **0** when the mean FLIP and the mean absolute error are both within the tolerances;
- **1** when either is above its tolerance, or something went wrong;
- **2** for a mistake on its command line;
- **77**, which CTest reports as skipped, when there is no reference for the driver.

When a test fails, `build/<preset>/golden-out/<name>/` holds:

| File | What it shows |
| --- | --- |
| `render.exr` | The render, linear nits, 32-bit float |
| `render.png`, `reference.png` | Both at the same exposure (the viewpoint's EV100), to flip between |
| `difference.exr` | \|render − reference\| in each channel, in nits |
| `difference.png` | The same, exposed for its own mean, so even a small difference is visible |
| `flip.png` | FLIP's error at every pixel, as FLIP's magma heat map: black is no visible difference, yellow the largest |

The next passing run removes them. `--out <folder>` writes them elsewhere. Without `--update`,
nothing in the test's directory is ever written.

## Adding a test

1. Make `tests/golden/<name>/test.json`, with tolerances of 0 for now.
2. Configure again (`cmake --preset release`). `ctest --preset release -L golden` now lists
   `golden.<name>`, skipped.
3. Write the reference: `./build/release/apps/cs-render/cs-render golden tests/golden/<name> --update`.
   It prints the file's size. Keep it under about 60 KB; more samples per pixel make it smaller,
   since noise does not compress.
4. Choose the tolerances by measurement, as for skeleton-path below:
   1. render with every compiler and preset you can (`--out` keeps their images apart);
   2. take the largest difference from the reference and add a stated margin;
   3. check that the smallest change the test must catch is still above it.

   If the two are close, raise `samplesPerPixel` rather than loosen a tolerance.
5. Commit `test.json` and the reference, with the measured numbers in the pull request.

## Updating a reference

Only on purpose: when a change is meant to change the picture. Run the test first, and look at
the failure images in `golden-out/` to check that the difference is the intended one and nothing
else. Then run `cs-render golden tests/golden/<name> --update`, which prints how far the new
reference is from the old one. Commit it in a pull request that says **why** the picture changed,
with those numbers. The same holds for a tolerance: never raise one to make a test pass without
saying so in the pull request.

## skeleton-path

The skeleton scene (`tests/scenes/skeleton/`) from its viewpoint: the path tier, 128 x 72,
64 samples per pixel, seed 1. The reference was rendered by GCC 16.2 in release with Embree's
AVX-512 kernels, on an AMD Ryzen 7 PRO 8840U. It is 48,772 bytes.

Renders are identical whatever the number of threads, but not across compilers and CPUs. In a path
tracer, a last-bit difference in `std::sin` or in Embree's kernels can flip a random decision
(which way a path bounces, or where Russian roulette ends it). Then a whole path differs. The
tolerances therefore come from measurement: every build below against the reference.

| Render | Mean FLIP | Mean absolute error (nits) |
| --- | --- | --- |
| GCC 16.2, release (the reference's own render) | 0.00087 | 0.0012 |
| GCC 16.2, debug with sanitizers | 0.00087 | 0.0012 |
| Clang 23.1, release | 0.00087 | 0.0012 |
| Clang 23.1, debug with sanitizers | 0.00087 | 0.0012 |
| GCC release, Embree forced to AVX2 | 0.0015 | 0.0015 |
| GCC release, Embree forced to SSE2 | 0.0017 | 0.0016 |
| CI: GCC release, Clang debug, MSVC release | (measured in the pull request) | |

The 0.00087 and 0.0012 that every identical render shows is the reference's rounding to half
floats. Forcing Embree's other kernels made a couple of paths in the whole image diverge: two
specks in the FLIP heat map.

What the tolerances must catch, from the same reference, with the spot's lumens raised:

| Spot | Mean FLIP | Mean absolute error (nits) |
| --- | --- | --- |
| +1% | 0.030 | 0.071 |
| +2% | 0.049 | 0.141 |
| +5% | 0.094 | 0.353 |
| +10% | 0.153 | 0.705 |

For comparison, the same render with seed 2, in which every path differs, is 0.271 and 1.59. If a
platform's paths diverged wholesale, the test would fail there, and the answer would be more
samples, not a looser tolerance.

The tolerances, 0.02 mean FLIP and 0.04 nits, are (to be completed from CI's numbers).

## Checks of the runner

Three more tests with the `golden` label check `cs-render golden` itself (`runner_checks.cmake`).
They work on copies in the build directory:

- **The spot 10% brighter:** skeleton-path, with the spot's lumens raised by 10%, fails. It writes
  its six images, and leaves its test directory untouched.
- **A missing reference, and `--update`:** on a small copy of skeleton-path, a missing reference
  skips; `--update` writes one; the next run passes without writing the test directory; a
  different seed fails; the next passing run removes the failure images.
- **Mistakes:** a mistake on the command line exits with 2. A `test.json` that is not valid exits
  with 1: an unknown key, a missing scene, a bad size or tier, or text that is not JSON. Neither
  writes anything.
