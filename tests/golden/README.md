# Golden-image tests

A golden-image test renders a scene and compares the picture with a reference committed here, so
that a change to what is drawn fails a test instead of waiting for someone to notice it
(blueprint section 9). The comparison is NVIDIA FLIP, which estimates how different two images
look to a person flipping between them, and the mean absolute error of the linear values in nits
(`libs/core/include/cs/core/image_compare.hpp`).

## Layout

Each directory here with a `test.json` is one test. Its name is kebab-case, usually the scene
and the tier: `skeleton-path`, `skeleton-standard`.

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
  "tolerances": { "meanFlip": 0.02, "meanAbsoluteError_nits": 0.02 }
}
```

| Key | Meaning |
| --- | --- |
| `scene` | A project folder, relative to the test's directory. The scene's first viewpoint is rendered |
| `tier` | `path`: the engine's path tracer, on the CPU. `standard`: the Standard tier, on the GPU, its radiance (not tone mapped) |
| `width`, `height` | Pixels: 128 to 256 a side (ADR 0010) |
| `samplesPerPixel` | Samples: at most 64 at 256 x 256. The Standard tier draws one frame, so 1, until it accumulates jittered frames |
| `seed` | Every random number comes from it, so a render repeats exactly on one machine. The Standard tier uses none |
| `tolerances.meanFlip` | The largest mean FLIP that passes, 0 to 1 |
| `tolerances.meanAbsoluteError_nits` | The largest mean absolute error that passes, in nits |

Any other key is an error, so a misspelt tolerance cannot be ignored silently.

**Drivers.** The engine's references are `ref.cpu.exr`. One CPU reference serves every compiler and
machine, within the tolerances (below). The GPU tiers render differently on each Vulkan driver,
so each driver they run on has its own reference, named from the device report's driver
(`cs-render --device-report` prints it): `ref.lavapipe.exr` for Mesa's software renderer
(llvmpipe), which CI runs, and `ref.radv.exr` for Mesa's AMD driver, which the owner's Radeon
780M runs. Another driver gets its name in lower case, such as `ref.nvidia.exr`. A test with no
reference for the driver that runs it is **skipped**, not passed.

**Which Mesa.** ADR 0010 makes CI's build the authority for lavapipe: `ref.lavapipe.exr` is taken
from the GPU workflow's `golden-out` artefact (below), rendered by the Mesa and LLVM that
`.github/workflows/gpu.yml` pins, never regenerated locally. A Mesa upgrade is a pull request
that bumps the workflow's versions and takes the new references from its artefact. `ref.radv.exr`
is the owner's machine's; the section of each test says which Mesa wrote it.

**The GPU.** The Standard tier's tests run on the device `cs-render --device-report` names;
`CS_GPU_DEVICE=llvmpipe` (part of the device's name) chooses lavapipe on a machine that also has
a GPU. Any message from Vulkan's validation layer fails the test, as it fails every GPU test; the
layer is on in debug builds when installed, and required there under `CS_REQUIRE_GPU=1`. With no
Vulkan device the test skips, unless `CS_REQUIRE_GPU=1` is set, when it fails.

## Running

```sh
ctest --preset release -L golden                             # every golden test and the runner's checks
./build/release/apps/cs-render/cs-render golden tests/golden/<name>
```

CMake finds each `test.json` by glob (`cmake/CsGolden.cmake`) and registers `golden.<name>`. Its
label is `golden`, plus `gpu` when the tier is not `path`. `cs-render golden` exits with:

- **0** when the mean FLIP and the mean absolute error are both within the tolerances;
- **1** when either is above its tolerance, or something went wrong (a validation message
  included);
- **2** for a mistake on its command line;
- **77**, which CTest reports as skipped, when there is no reference for the driver, or no Vulkan
  device for the Standard tier without `CS_REQUIRE_GPU=1`.

When a test fails, `build/<preset>/golden-out/<name>/` holds:

| File | What it shows |
| --- | --- |
| `render.exr` | The render, linear nits, 32-bit float |
| `render.png`, `reference.png` | Both at the same exposure (the viewpoint's EV100), to flip between |
| `difference.exr` | \|render − reference\| in each channel, in nits |
| `difference.png` | The same, exposed for its own mean, so even a small difference is visible |
| `flip.png` | FLIP's error at every pixel, as FLIP's magma heat map: black is no visible difference, yellow the largest |
| `ref.<driver>.exr` | The candidate reference: the render as `--update` would write it |

A test skipped for want of a reference writes the candidate alone. CI uploads `golden-out/` as an
artefact, so a first run on a driver, or one after a Mesa upgrade, gives the file to commit. The
next passing run removes them all. `--out <folder>` writes them elsewhere. Without `--update`,
nothing in the test's directory is ever written.

## Adding a test

1. Make `tests/golden/<name>/test.json`, with tolerances of 0 for now.
2. Configure again (`cmake --preset release`). `ctest --preset release -L golden` now lists
   `golden.<name>`, skipped.
3. Write the reference: `./build/release/apps/cs-render/cs-render golden tests/golden/<name> --update`.
   It prints the file's size. Keep it under about 60 KB; more samples per pixel make it smaller,
   since noise does not compress. For lavapipe, take `ref.lavapipe.exr` from the GPU workflow's
   `golden-out-linux-release` artefact instead.
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

The runner rounds each render to half floats the way the reference was stored before comparing,
so a render identical to the reference's compares as exactly 0.

| Render | Mean FLIP | Mean absolute error (nits) |
| --- | --- | --- |
| GCC 16.2, release (the reference's own render) | 0 | 0 |
| GCC 16.2, debug with sanitizers | 0 | 0 |
| Clang 23.1, release | 0 | 0 |
| Clang 23.1, debug with sanitizers | 0 | 0 |
| GCC release, Embree forced to AVX2 | 0.00091 | 0.00031 |
| GCC release, Embree forced to AVX or SSE4.2 or SSE2 | 0.0012 | 0.00042 |
| CI: MSVC 19.44, release | 0 | 0 |
| CI: Clang 18.1, debug with sanitizers | 0 | 0 |
| CI: GCC 13.3, release, on a runner without AVX-512 | 0.00091 | 0.00031 |

The compilers agree exactly, debug builds included, and MSVC with them; Embree's kernels are what
differ. Forcing its AVX2 or older kernels on the reference's machine made a couple of paths in the
whole image diverge: two specks in the FLIP heat map. The CI rows come from a run with tolerances
of 0, before the runner rounded renders to half floats. MSVC and Clang matched the reference
exactly then too. GCC's numbers then (0.00153 and 0.00155) were the forced AVX2 run's to every
digit, so its runner had no AVX-512, and the row gives the AVX2 run's numbers now. (Embree 4.4
takes its kernels only from the device's configuration string, so forcing them took a throwaway
build, not part of the code.)

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

**The tolerances are 0.02 mean FLIP and 0.02 nits.** They are 17 and 48 times the largest
differences measured above (0.0012 and 0.00042, Embree's older kernels), which leaves room for a
platform whose paths diverge more often. Both stay below what the spot made 1% brighter gives, and
the 10% change is 7.6 and 35 times above them. 64 samples per pixel are enough for that, so the
test stays fast: about 8 s in CI's debug job.

## skeleton-standard

The same scene and viewpoint through the Standard tier, at 256 x 144: one frame, its radiance in
nits before tone mapping. It has no noise, so its references are small and exact:

| Reference | Written by | Size |
| --- | --- | --- |
| `ref.lavapipe.exr` | The GPU workflow's release job (GCC 13.3): Mesa 26.2.6 with LLVM 18.1.3, as `gpu.yml` pins them | 43,031 bytes |
| `ref.radv.exr` | The owner's Radeon 780M: Mesa 26.2.7 (Arch Linux), GCC 16.2, release | 42,995 bytes |

The tier draws the same picture every time on one driver. Every render below matched its
reference exactly, mean FLIP 0 and 0 nits: radv in release, twice, and in debug with sanitizers
and validation; lavapipe in CI's debug (Clang 18.1) and release (GCC 13.3) jobs, and on the
owner's machine with Mesa 26.2.7 and LLVM 23.1, in release and debug. All three lavapipe
candidates were the same file, byte for byte.

What the tolerances must allow and what they must catch, against the references:

| Render | Mean FLIP | Mean absolute error (nits) |
| --- | --- | --- |
| The same driver, any build, run again | 0 | 0 |
| lavapipe against the radv reference: a different driver altogether | 0.00103 | 0.0071 |
| The spot's lumens 1% higher (both drivers alike) | 0.0085 | 0.060 |
| 2% higher | 0.0141 | 0.120 |
| 5% higher | 0.0274 | 0.300 |
| 10% higher | 0.0448 | 0.600 |

**The tolerances are 0.003 mean FLIP and 0.02 nits.** No driver update is measured yet, so the
whole difference between two drivers stands in for one: the tolerances are 2.9 and 2.8 times it.
The spot 1% brighter is 2.8 and 3.0 times above them, so a change of 1% in the light fails. A
Mesa upgrade that moves the picture by more than one driver differs from another is worth
looking at anyway.

**Against the engine.** Milestone 1 asks the Standard tier to be within a mean FLIP of 0.08 of
the engine's direct light from the same view (exit criterion 7; M2-26's shadows bring it to
0.05). It is 0.072 on both drivers, against
`cs-render tests/scenes/skeleton --tier path --direct-only --size 256x144 --spp 64 --seed 1`.
Three differences make it, by measurement: replacing one at a time, until the two agree
(radv; lavapipe gives the same numbers to three digits):

| Step | Mean FLIP |
| --- | --- |
| The Standard tier as it is: one sample at each pixel's centre | 0.072 |
| Anti-aliased: drawn at 4 x 4 samples per pixel and averaged, as the engine averages its 64 | 0.050 |
| And the 796 pixels the frame shadows in the engine (no shadows until M2-26) made the tier's | 0.015 |
| And the chart's pixels too (Lambert against the engine's painting BRDF) | 0.010 |

The rest is at the spot's sharp cut-off, where a grid of 16 samples and the engine's 64 random
ones still disagree pixel by pixel. Where nothing differs by design the two agree: on the lit
wall, the engine's luminance over the tier's has a median of 1.0000, and 90% of pixels lie
within 1.7%. On the chart's patches the median is 1.000, with the patches' edges from 0.86 to
1.13. The gap closes as the tier gains jittered frames (ADR 0002) and shadows (M2-26).

## Checks of the runner

Three more tests with the `golden` label check `cs-render golden` itself (`runner_checks.cmake`).
They work on copies in the build directory:

- **The spot 10% brighter:** skeleton-path, with the spot's lumens raised by 10%, fails. It writes
  its six images, and leaves its test directory untouched.
- **A missing reference, and `--update`:** on a small copy of skeleton-path, a missing reference
  skips and writes only the candidate reference to the out folder; `--update` writes the same
  file as the reference; the next run passes without writing the test directory; a different
  seed fails and writes the candidate too; the next passing run removes the failure images and
  the candidate.
- **Mistakes:** a mistake on the command line exits with 2. A `test.json` that is not valid exits
  with 1: an unknown key, a missing scene, a bad size or tier, the Standard tier with more than
  one frame, or text that is not JSON. Neither writes anything.

Two tests with the `gpu` and `cs-render` labels check cs-render's GPU commands
(`apps/cs-render/checks.cmake`): `--device-report` prints the device report, with validation on
in a debug build under `CS_REQUIRE_GPU=1`, and `--tier standard` writes the skeleton's EXR, PNG
and lux report.
