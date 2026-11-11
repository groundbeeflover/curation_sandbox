# 0010. GPU tests and golden images in CI: a pinned lavapipe on Linux and Windows

- Status: proposed
- Date: 2026-11-11
- Decides: how GPU tests and golden images run in GitHub Actions (spike M1-03, issue #38), so
  that M1-29 can add `.github/workflows/gpu.yml` without guesswork
- Depends on: [0002](0002-rendering-tiers.md) (Testing in CI), [0009](0009-platforms-licence-distribution.md) (Windows and Linux)

## Context

ADR 0002 puts image tests on Mesa lavapipe (software Vulkan with ray queries) with the Mesa
version pinned, and leaves two questions to this spike: where a pinned lavapipe comes from, and
whether Windows runs image tests at all. GitHub's runner images ship no Vulkan driver, and the
Ubuntu archive's Mesa moves with stable-release updates. Every validation message must fail a
test, so the validation layers need a source and a version too.

## How it was measured

A throwaway program (`spike/lvpbench` on the branch
[`spike/lavapipe`](https://github.com/groundbeeflover/curation_sandbox/tree/spike/lavapipe),
never merged) creates a headless Vulkan 1.3 device with validation on, renders a gallery-like
room (a painting in a frame on a wall, a bench, a spotlight) and reads the image back into host
memory. It has two modes:

- **raster**: Standard-tier-like work, with dynamic rendering, a depth buffer, and eight
  spotlights evaluated per pixel. One sample is one jittered frame accumulated by blending.
- **rq**: path-tracer-like work. A compute shader traces with `VK_KHR_ray_query` against a
  BLAS and a TLAS built on the device: a camera ray, then two bounces, each with a shadow ray
  to the light. One sample is one dispatch.

It prints the time for device creation, scene upload and acceleration-structure build,
pipeline creation, the first frame (which includes lavapipe's lazy shader compilation), the
render, and the readback. It also prints the number of validation messages. All runs used
GitHub's standard runners for a private repository: **2 vCPUs** (AMD EPYC 9V74 on Linux,
Xeon Platinum 8370C on Windows) and 7 GB of memory. Public repositories get 4 vCPUs, so these
times roughly halve once the repository is published (ADR 0009).

| Run | What it showed |
| --- | --- |
| [1](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42930469113) | Ubuntu names the manifest `lvp_icd.json`; the Linux SDK tarball has no loader; a Mesa build with shared LLVM takes 5 min 10 s; the Windows SDK installs in 3 min when not cached |
| [2](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42931512392) | The Ubuntu archive option, fully measured; the elevated Windows runner ignores `VK_DRIVER_FILES`; Ubuntu's static LLVM needs Polly |
| [3](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42932309706) | The pinned build, fully measured; the three Windows drivers, measured |
| [4](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42933585580) | The candidate `gpu.yml`: lavapipe built without a window-system platform lacks `VK_EXT_headless_surface`; on an SDK cache hit, the Windows loader is not installed system-wide |
| [5](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42934657548) | The candidate, green on both platforms, building lavapipe with the X11 platform when the cache misses (5 min 48 s) |
| [6](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42935610251) | On a cache hit, the restored lavapipe needed `libxcb-keysyms`, which runners lack; the `ldd` guard failed the job as intended |
| [7](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42936187579) | The final candidate, green on both platforms, with warm timings (Mesa's shader cache on). [Attempt 1](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42936187579/attempts/1) misses the cache and builds lavapipe in 3 min 29 s. [Attempt 2](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42936187579/attempts/2) hits every cache: the whole Linux job took 46 s, 26 s of them the spike's benchmark. The Windows job took 4 min 37 s, 2 min 33 s of them the build |
| [8](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42937457847) | After M1-01 merged: the build follows `ci.yml` through presets and vcpkg. The test steps fail because the presets now treat "no tests found" as an error, a guard M1-29 keeps |
| [9](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42938198397) | The final candidate, after the code review: LLVM pinned by package version, caches keyed on the workflow's hash, the device report checking each required extension, and placeholder `gpu` and `golden` tests (spike only). [Attempt 1](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42938198397/attempts/1) misses the cache (lavapipe built in 4 min 47 s); [attempt 2](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42938198397/attempts/2) hits it, and the whole Linux job took 2 min, 74 s of them configuring through vcpkg. The Windows job took 3 min 16 s |

## Linux: where Mesa comes from

| Source | Version on 2026-11-11 | Pinned | Time to install |
| --- | --- | --- | --- |
| Ubuntu 24.04 archive (`mesa-vulkan-drivers`, noble-updates) | 25.2.8-0ubuntu0.24.04.4 with LLVM 20.1.2 | **No.** Noble released with 24.0.5; stable-release updates replaced it with 25.2.8, and superseded builds leave the mirrors | 11 to 24 s, with `apt-get update`, vulkan-tools and the archive's validation layers |
| kisak-mesa PPA | 26.2.6~kisak1 | **No.** The PPA keeps only its latest build, and has a single maintainer | about the same as the archive |
| **The Mesa release tarball, built for lavapipe only (chosen)** | **26.2.6** (SHA-256 checked), LLVM 18.1.3 (`llvm-18-dev=1:18.1.3-1ubuntu1` from noble's release pocket, pinned), linked statically | **Yes**, by version and checksum | 3 min 29 s to 5 min 48 s when the cache misses (dependencies 17 to 20 s, the build the rest); **1 s** to restore the 22 MB cache entry |

The build configures `-Dvulkan-drivers=swrast -Dgallium-drivers=llvmpipe -Dplatforms=x11` and
disables everything else. Two details matter:

- **The X11 platform is required.** Lavapipe exposes `VK_EXT_headless_surface`,
  `VK_KHR_surface` and `VK_KHR_swapchain` only when a window-system platform is built
  (`LVP_USE_WSI_PLATFORM` in `lvp_device.c`). Run 4 shows the extension missing without it.
- **LLVM is linked statically.** The cached library then does not depend on the LLVM versions
  preinstalled on the runner image (16 to 20 today), which change over time. Its remaining
  dependencies (zlib, zstd, tinfo, libdrm, libxcb) are on the runner image. In run 6, one more
  (`libxcb-keysyms`, which Mesa links when its development package happens to be installed)
  made the loader skip the driver. The build no longer installs that package, and the job now
  runs `ldd` on the library first and fails with the missing library's name.

Mesa 26.2.6 is also the version on the owner's Arch Linux machine and in mesa-dist-win. That
matters for the golden images (see below).

### vulkaninfo on the pinned lavapipe ([run 7](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42936187579))

```text
Vulkan Instance Version: 1.3.275
VK_EXT_headless_surface                : extension revision 1
VK_LAYER_KHRONOS_validation Khronos Validation Layer 1.4.363  version 1
GPU0:
	apiVersion         = 1.4.354
	driverVersion      = 26.2.6
	deviceType         = PHYSICAL_DEVICE_TYPE_CPU
	deviceName         = llvmpipe (LLVM 18.1.3, 256 bits)
	driverName         = llvmpipe
	driverInfo         = Mesa 26.2.6 (LLVM 18.1.3)
	VK_EXT_headless_surface                : extension revision 1
	VK_KHR_ray_query                                   : extension revision 1
```

The device is Vulkan 1.4.354 (so 1.3 is covered) and also has `VK_KHR_acceleration_structure`
and `VK_KHR_ray_tracing_pipeline` (run 3). The instance version is that of the runner's own
loader (`libvulkan1` 1.3.275); volk opens it at run time, so no other loader is needed.

## Validation layers

| Source | Version | Verdict |
| --- | --- | --- |
| Ubuntu archive `vulkan-validationlayers` | 1.3.275 (January 2024) | Rejected: older than every driver it would check (1.4.318 to 1.4.354), so it does not know their newer extensions' rules |
| **LunarG Vulkan SDK 1.4.363.1 tarball (chosen)** | 1.4.363 | `libVkLayer_khronos_validation.so`, its manifest and `vulkaninfo` are extracted from the 367 MB tarball. The SHA-256 matches LunarG's published one. The layer is stripped to 33 MB and cached as a 13 MB entry. Download and extraction take 28 to 45 s when the cache misses; restoring takes 1 to 3 s |
| vcpkg `vulkan-validationlayers` | follows the vcpkg baseline | Not measured: it builds from source, which would add a long cold build to every vcpkg cache miss |

On Windows, the SDK comes from `jakoch/install-vulkan-sdk-action` v1.8.1, pinned to 1.4.363.1.
It installs the loader, the same layer version and `vulkaninfo` in 3 min when not cached, and
restores from its 345 MB cache entry in 8 to 14 s.

In 199 runs of the program on five drivers (lavapipe 25.2.8, 26.2.4 and three builds of
26.2.6, plus SwiftShader), 19 of them with synchronisation validation, the layers reported
**no messages**. The only failures were SwiftShader's ray-query runs, because it has no ray
queries. The layers do catch mistakes: the spike's first draft set a buffer usage flag from an
extension it had not enabled, and the layer reported it locally.

## Timings

All times are on the 2-vCPU runners described above. A **sample** is one accumulated frame
in raster mode, or one dispatch in rq mode. Times for the pinned lavapipe are from runs 3 and 7;
they varied by up to 35% between runners.

### Rendering and reading back a 256×256 image

| | raster, 1 sample | rq, 1 sample |
| --- | --- | --- |
| Instance and device with validation | 16 ms warm; 39 to 56 ms cold | 16 ms warm; 39 to 56 ms cold |
| Scene upload and acceleration structures | 0.2 ms | **95 ms** warm; **1.06 to 1.58 s** cold |
| Pipeline creation | 2 ms | 7 ms warm; 15 to 20 ms cold |
| First frame (includes lazy shader compilation) | 8 ms warm; 31 to 43 ms cold | 19 ms warm; 79 to 112 ms cold |
| One sample | 1.5 to 2.6 ms | 18 to 26 ms |
| Readback into host memory (`vkCmdCopyImageToBuffer`, wait, `memcpy`) | 0.8 ms | 0.8 ms |
| Whole process | **34 ms** warm; 78 to 102 ms cold | **157 ms** warm; 1.2 to 1.8 s cold |

"Cold" means Mesa's on-disk shader cache is empty, as on a fresh runner. Lavapipe compiles its
internal acceleration-structure build shaders and the test's own shaders on first use, and
caches them on disk. The cache warms during the first test of a job and stays warm for the rest
of it, so the GPU job leaves it on.

### Time per sample by image size (pinned lavapipe, validation on)

| Size | raster | rq (2 bounces) |
| --- | --- | --- |
| 128×128 | 0.45 to 0.57 ms | 5.0 to 6.9 ms |
| 256×256 | 1.5 to 2.1 ms | 18 to 26 ms |
| 512×512 | 7.2 ms | 98 ms |

The time scales with the number of pixels; it does not depend on the sample count. Validation
adds about 4 ms to device creation and nothing measurable to rendering; synchronisation
validation adds nothing measurable either. Mesa 25.2.8 from the archive with LLVM 20 ran
within the same spread (1.4 ms raster and 20 ms rq per sample at 256×256).

### Are lavapipe images stable?

The spike compared the 8-bit renders at 256×256 with 64 samples:

- Mesa 26.2.6 on Linux with LLVM 18, on Windows with LLVM 23 (mesa-dist-win), and on the
  owner's Arch machine with LLVM 23 produced **identical** images in both modes. Mesa 26.2.4
  (jakoch/rasterizers) was identical too.
- On one machine, the images were identical with 1, 3 and 16 lavapipe threads, and across
  repeated runs.
- Mesa 25.2.8 differed from 26.2.6 on 0.01% of pixels in rq mode (at most 5 code values).
- RADV on the Radeon 780M differed from lavapipe on 3% of pixels in raster mode (at most 3
  code values) and on 0.03% in rq mode (at most 8). SwiftShader differed on 3% (at most 2).

So one set of lavapipe references can serve Linux CI, Windows and a developer's machine, as
long as they run the same Mesa version. A Mesa upgrade can move a few pixels, so it is a
deliberate pull request that updates the lavapipe references.

### The largest goldens that keep the GPU job under 10 minutes

The budget for the Linux job, with every cache hit:

| Part | Measured or assumed |
| --- | --- |
| Checkout, restoring lavapipe, layers and ccache | about 15 s (measured; runs 7 and 9, attempt 2) |
| Configure and build in release | 75 to 80 s today, almost all of it vcpkg installing three ports without a binary cache (measured, run 9). With all of milestone 1's dependencies and M1-30's binary cache: **assumed at most 4 min**, the main uncertainty |
| GPU unit tests (`-L gpu`): one process per Catch2 test case, about 0.05 to 0.2 s each warm | at most 1 min |
| Safety margin: runners varied by 35%; cache misses add about 6 min, but only after a version bump | 2 min |
| **Golden images (`-L golden`)** | **at most 2 min** |

A golden at N×N with S samples costs about 0.2 s plus S × (N/256)² × t, where t is 2 ms
(raster) or 25 ms (rq) on the spike's scene. Real tiers do more work per pixel: shadow maps,
IES lookups and the bake in the Standard tier; deeper paths and a larger BVH in a path tracer.
Assume five times the spike's cost.

| Golden | Spike's cost | Assumed real cost | Fits in 2 min |
| --- | --- | --- | --- |
| Standard tier, 256×256, 64 frames | 0.13 s | 0.9 s | about 100 |
| Standard tier, 512×512, 256 frames | 1.9 s | 9 s | about 12 |
| Ray queries or the GPU path tracer, 256×256, 64 spp | 1.6 s | 8 s | about 14 |
| Ray queries or the GPU path tracer, 128×128, 256 spp | 1.7 s | 9 s | about 13 |
| Ray queries or the GPU path tracer, 512×512, 256 spp | 25 s | 2 min | one, alone |

On time alone, the largest goldens that fit are 512×512 with 256 frames for the Standard tier
(about 9 s each, a dozen of them), and 256×256 with 64 spp or 128×128 with 256 spp for
ray-query tiers (8 to 9 s each, about 14). A 512×512 ray-query golden at 256 spp would use the
whole budget alone. The blueprint also caps goldens at 128 to 256 pixels, to keep references at
tens of kilobytes. The limits are therefore:

- **Size: 128×128 to 256×256**, as the blueprint says; 256×256 by default.
- **Samples: at most 64 per pixel at 256×256, and at most 256 at 128×128.** That means
  accumulated frames for the Standard tier and spp for ray-query tiers.
- **Time: under 10 s per golden on CI.** This is the binding limit when a tier's work per
  pixel turns out heavier than assumed. ctest's timeout of 120 s catches a runaway test, and
  M1-29 states the measured time of `-L golden` in its pull request.

On Windows, lavapipe is slower (2.4 ms raster and 42 ms rq per sample at 256×256). Building
acceleration structures takes **2 to 5 s per process**, and Mesa's shader cache did not
shorten it there (run 6). Tests that build acceleration structures should therefore stay few on
Windows.

## Windows options

| Option | What was measured | Verdict |
| --- | --- | --- |
| **lavapipe from [mesa-dist-win](https://github.com/pal1000/mesa-dist-win) 26.2.6 (MSVC build)** | Vulkan 1.4.354, `VK_KHR_ray_query`, no `VK_EXT_headless_surface` (Mesa leaves it out on Win32). A 56 MB archive (SHA-256 checked) downloads and extracts in 4 to 6 s. Images identical to Linux. No validation messages | **Chosen** |
| lavapipe from [jakoch/rasterizers](https://github.com/jakoch/rasterizers) (26.2.4) | The same driver two patch releases behind. The zip has no ICD manifest (the spike wrote one). Releases are named by date, and the SDK action always takes the latest | Rejected: older and harder to pin than mesa-dist-win, with no gain |
| SwiftShader 5.0.0.1 (the jakoch/rasterizers build) | Vulkan 1.3.0, **no ray query**, has a headless surface. Raster is 2.2× slower than lavapipe (5.3 ms at 256×256). Its images differ from lavapipe's, so it would need references of its own | Rejected: it cannot run ray-query tests and doubles the reference sets |
| None (CPU tests only on Windows) | No cost | Rejected: Windows-only problems (MSVC builds of GPU code, the loader, the registry) would surface only on the owner's machines |

Two Windows details for M1-29:

- **The runner is elevated**, so the loader ignores `VK_DRIVER_FILES`, `VK_ICD_FILENAMES` and
  `VK_LAYER_PATH`. The driver and the layer are registered under
  `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` and `...\ExplicitLayers` instead.
- **On a cache hit, the SDK action does not install the loader system-wide.** The workflow
  adds `%VULKAN_SDK%\runtime\x64` to `PATH`.

## Decision

- **Linux** runs `ctest -L gpu` and `ctest -L golden` on every pull request and every push to
  main. It uses lavapipe built from the Mesa 26.2.6 tarball with static LLVM 18.1.3 and the X11
  platform, cached by version. It uses validation layers 1.4.363 from the LunarG SDK tarball,
  cached by version, and the runner's own Vulkan loader. Mesa's shader cache stays on.
- **Windows** runs `ctest -L gpu` with lavapipe from mesa-dist-win 26.2.6 and the SDK action's
  layers 1.4.363. It does not run goldens: the spike found its lavapipe images identical to
  Linux's, so running them again would only cost a second, slower build. Tests that need
  `VK_EXT_headless_surface` or a swapchain skip when the device lacks the extension.
- **A green job means the tests ran.** The presets already fail when a label matches no
  tests (M1-01). The workflow sets `CS_REQUIRE_GPU=1`; M1-19's test harness must then fail,
  not skip, when it finds no Vulkan device. The device report fails the job when lavapipe or
  the layers lack an extension the tests rely on. Goldens run even when a GPU unit test failed,
  so a pull request shows both.
- **Golden references for lavapipe** are one set, named for lavapipe and its Mesa version, as
  `tests/golden/README.md` (M1-18) lays out. **CI's build is the authority.** References are
  taken from the job's `golden-out` artefact. The owner's Arch Linux machine produced identical
  images for the spike's scene, but it runs LLVM 23 rather than 18 and moves on its own
  schedule, so a local regeneration that disagrees with CI loses. Mesa, LLVM and the layers
  change only together with a deliberate pull request that bumps the versions below and
  updates the lavapipe references.
- Goldens follow the limits above: at most 256×256, at most 64 samples at that size, under
  10 s each, and `-L golden` under 2 min.
- Every action is pinned to a commit SHA, with the release as a comment.

## The workflow for M1-29

This is the candidate that ran green in run 9, with three differences:

- The trigger is the project's usual one instead of the spike branch.
- The spike-only steps that ran `lvpbench` are removed.
- So is the spike's `cmake/CsSpikeTests.cmake`, which registered one placeholder test per
  label. On today's main, `-L gpu` and `-L golden` find no tests, and the presets turn that
  into a failure. M1-29 starts only after M1-18, M1-19 and M1-22 have added real tests.

The build steps mirror
`ci.yml` as M1-01 left it (presets, vcpkg from the runner image, the MSVC environment on
Windows), and should follow it when M1-30 adds vcpkg's binary cache.

```yaml
# GPU tests and golden images on software Vulkan (Mesa lavapipe), as decided in
# docs/adr/0010-gpu-tests-in-ci.md. Linux runs `-L gpu` and `-L golden`; Windows runs `-L gpu`.
name: GPU

on:
  push:
    branches: [main]
  pull_request:

# A newer push to the same pull request cancels the older run.
concurrency:
  group: ${{ github.workflow }}-${{ github.event.pull_request.number || github.ref }}
  cancel-in-progress: true

permissions:
  contents: read

env:
  # Bumping any of these changes the cache keys below and, for Mesa, may change golden images.
  MESA_VERSION: "26.2.6"
  MESA_SHA256: b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92
  MESA_DIST_WIN_SHA256: b2e00615feaba45942343c265cc8c5f576142ca682c1ac6661922d4b72492877
  VULKAN_SDK_VERSION: "1.4.363.1"
  VULKAN_SDK_SHA256: b7867d8ca658563813af666f34b93db87f1b1c7d9d69d97e924c210c7ba33eb8
  # The Ubuntu 24.04 package lavapipe's static LLVM comes from (exact version, release pocket).
  LLVM_PACKAGE_VERSION: "1:18.1.3-1ubuntu1"
  # The GPU test harness (M1-19) fails instead of skipping when there is no Vulkan device.
  CS_REQUIRE_GPU: "1"

jobs:
  linux:
    name: lavapipe (Linux)
    runs-on: ubuntu-24.04
    timeout-minutes: 20
    env:
      VK_DRIVER_FILES: /home/runner/mesa/share/vulkan/icd.d/lvp_icd.x86_64.json
      VK_LAYER_PATH: /home/runner/vulkan-layers/share/vulkan/explicit_layer.d
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1

      # Mesa: only lavapipe, built from the pinned release tarball with LLVM linked statically
      # (LLVM 18.1.3 from the Ubuntu 24.04 release pocket), so the cached build does not depend
      # on the LLVM versions preinstalled on the runner image. About 5 minutes when the cache
      # misses. The key hashes this file, so any change to the build below rebuilds it.
      - name: Restore lavapipe
        id: mesa-cache
        uses: actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0
        with:
          path: ~/mesa
          key: lavapipe-${{ env.MESA_VERSION }}-ubuntu-24.04-${{ hashFiles('.github/workflows/gpu.yml') }}
      # The X11 platform is what makes lavapipe expose VK_EXT_headless_surface and VK_KHR_swapchain.
      - name: Build lavapipe
        if: steps.mesa-cache.outputs.cache-hit != 'true'
        run: |
          sudo apt-get update -qq
          sudo apt-get install -y -qq --no-install-recommends \
            bison flex glslang-tools libdrm-dev libedit-dev libelf-dev libexpat1-dev libffi-dev \
            "libpolly-18-dev=${LLVM_PACKAGE_VERSION}" "llvm-18-dev=${LLVM_PACKAGE_VERSION}" \
            libncurses-dev libxml2-dev libzstd-dev ninja-build \
            python3-mako python3-packaging python3-yaml zlib1g-dev \
            libx11-dev libx11-xcb-dev libxcb1-dev libxcb-dri3-dev libxcb-present-dev \
            libxcb-randr0-dev libxcb-shm0-dev libxcb-sync-dev libxcb-xfixes0-dev libxrandr-dev libxshmfence-dev
          test "$(llvm-config-18 --version)" = 18.1.3
          pip install --break-system-packages meson==1.9.1
          curl -sSfL --retry 3 -o mesa.tar.xz "https://archive.mesa3d.org/mesa-${MESA_VERSION}.tar.xz"
          echo "${MESA_SHA256}  mesa.tar.xz" | sha256sum -c -
          tar xf mesa.tar.xz
          cd "mesa-${MESA_VERSION}"
          meson setup build --prefix="$HOME/mesa" --libdir=lib --buildtype=release -Db_ndebug=true \
            -Dplatforms=x11 -Dgallium-drivers=llvmpipe -Dvulkan-drivers=swrast -Dvulkan-layers= \
            -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
            -Dllvm=enabled -Dshared-llvm=disabled -Dvalgrind=disabled \
            -Dlibunwind=disabled -Dlmsensors=disabled -Dxmlconfig=disabled -Dzstd=enabled \
            -Dbuild-tests=false -Dvideo-codecs= -Dtools= -Dgallium-va=disabled -Dgallium-rusticl=false \
            -Dteflon=false -Dperfetto=false
          meson install -C build

      # Validation layers and vulkaninfo from the pinned LunarG SDK; the loader is the system's
      # libvulkan1, which volk opens at run time.
      - name: Restore the validation layers
        id: layers-cache
        uses: actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0
        with:
          path: ~/vulkan-layers
          key: vulkan-layers-${{ env.VULKAN_SDK_VERSION }}-linux-${{ hashFiles('.github/workflows/gpu.yml') }}
      - name: Extract the validation layers from the Vulkan SDK
        if: steps.layers-cache.outputs.cache-hit != 'true'
        run: |
          curl -sSfL --retry 3 -o sdk.tar.xz "https://sdk.lunarg.com/sdk/download/${VULKAN_SDK_VERSION}/linux/vulkansdk-linux-x86_64-${VULKAN_SDK_VERSION}.tar.xz"
          echo "${VULKAN_SDK_SHA256}  sdk.tar.xz" | sha256sum -c -
          mkdir -p ~/vulkan-layers
          tar xf sdk.tar.xz -C ~/vulkan-layers --strip-components=2 \
            "${VULKAN_SDK_VERSION}/x86_64/lib/libVkLayer_khronos_validation.so" \
            "${VULKAN_SDK_VERSION}/x86_64/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" \
            "${VULKAN_SDK_VERSION}/x86_64/bin/vulkaninfo"
          strip --strip-unneeded ~/vulkan-layers/lib/libVkLayer_khronos_validation.so
          rm sdk.tar.xz

      - name: Device report
        run: |
          if ldd ~/mesa/lib/libvulkan_lvp.so | grep 'not found'; then exit 1; fi
          ~/vulkan-layers/bin/vulkaninfo --summary
          ~/vulkan-layers/bin/vulkaninfo > vulkaninfo.txt 2>&1 || true
          for name in VK_KHR_ray_query VK_EXT_headless_surface VK_KHR_swapchain VK_LAYER_KHRONOS_validation; do
            grep -q "$name" vulkaninfo.txt || { echo "::error::lavapipe or the layers lack $name"; exit 1; }
          done

      - name: Compiler cache
        uses: hendrikmuhs/ccache-action@f09c25b45002a07be2955cbe52e8cee55643f89d # v1.2.24
        with:
          key: ubuntu-24.04-gpu-release

      # The build mirrors ci.yml's Linux release job (and M1-30's vcpkg binary cache once it exists).
      - name: vcpkg
        run: |
          echo "VCPKG_ROOT=$VCPKG_INSTALLATION_ROOT" >> "$GITHUB_ENV"
          git -C "$VCPKG_INSTALLATION_ROOT" fetch --quiet origin
      - name: Configure
        run: cmake --preset release -DCS_WARNINGS_AS_ERRORS=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
      - name: Build
        id: build
        run: cmake --build --preset release

      # lavapipe uses every core, so tests run one at a time. Mesa's shader cache stays on: it is
      # empty at the start of the job and saves about a second per test process after the first.
      - name: GPU tests
        run: ctest --preset release -L gpu --timeout 120
      # Goldens run even when a GPU test failed, so the pull request shows both.
      - name: Golden images
        if: ${{ !cancelled() && steps.build.outcome == 'success' }}
        run: ctest --preset release -L golden --timeout 120
      - name: Upload golden differences
        if: failure()
        uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9 # v7.0.2
        with:
          name: golden-out-linux
          path: build/*/golden-out/
          if-no-files-found: ignore

  windows:
    name: lavapipe (Windows)
    runs-on: windows-2022
    timeout-minutes: 20
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1

      - name: Vulkan SDK (loader, validation layers, vulkaninfo)
        uses: jakoch/install-vulkan-sdk-action@7fe84289ddbf750f7eff9f8e4588e600e1dd06c8 # v1.8.1
        with:
          vulkan_version: ${{ env.VULKAN_SDK_VERSION }}
          install_runtime: true
          cache: true
          stripdown: true

      # lavapipe from mesa-dist-win, the same Mesa version as Linux. The runner is elevated, so
      # the loader ignores VK_DRIVER_FILES and VK_LAYER_PATH: both are registered in HKLM instead.
      # On a cache hit the SDK action does not install the loader system-wide: put it on PATH.
      - name: Install lavapipe
        shell: bash
        run: |
          echo "$VULKAN_SDK\\runtime\\x64" >> "$GITHUB_PATH"
          curl -sSfL --retry 3 -o mesa.7z "https://github.com/pal1000/mesa-dist-win/releases/download/${MESA_VERSION}/mesa3d-${MESA_VERSION}-release-msvc.7z"
          echo "${MESA_DIST_WIN_SHA256}  mesa.7z" | sha256sum -c -
          7z x -y -o"$RUNNER_TEMP/mesa" mesa.7z x64/vulkan_lvp.dll x64/lvp_icd.x86_64.json > /dev/null
          export MSYS2_ARG_CONV_EXCL='*'
          reg add 'HKLM\SOFTWARE\Khronos\Vulkan\Drivers' /v "$(cygpath -w "$RUNNER_TEMP/mesa/x64/lvp_icd.x86_64.json")" /t REG_DWORD /d 0 /f
          reg add 'HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers' /v "$(cygpath -w "$VULKAN_SDK/Bin/VkLayer_khronos_validation.json")" /t REG_DWORD /d 0 /f

      - name: Device report
        shell: bash
        run: |
          "$VULKAN_SDK/Bin/vulkaninfoSDK.exe" --summary
          "$VULKAN_SDK/Bin/vulkaninfoSDK.exe" > vulkaninfo.txt 2>&1 || true
          # lavapipe on Windows has no VK_EXT_headless_surface; tests that need it skip there.
          for name in llvmpipe VK_KHR_ray_query VK_LAYER_KHRONOS_validation; do
            grep -q "$name" vulkaninfo.txt || { echo "::error::lavapipe or the layers lack $name"; exit 1; }
          done

      # The build mirrors ci.yml's Windows release job.
      - name: vcpkg
        shell: bash
        run: |
          echo "VCPKG_ROOT=$VCPKG_INSTALLATION_ROOT" >> "$GITHUB_ENV"
          git -C "$VCPKG_INSTALLATION_ROOT" fetch --quiet origin
      - name: MSVC environment
        uses: ilammy/msvc-dev-cmd@0b201ec74fa43914dc39ae48a89fd1d8cb592756 # v1.13.0
      - name: Configure
        run: cmake --preset release -DCS_WARNINGS_AS_ERRORS=ON
      - name: Build
        run: cmake --build --preset release
      - name: GPU tests
        run: ctest --preset release -L gpu --timeout 120
```

## Consequences

- The GPU job's results depend only on versions written in the workflow, not on what Ubuntu or
  GitHub ship that week. The cost is a cold Mesa build of about 5 minutes after each version
  bump. The Linux caches take about 35 MB; the Windows SDK action keeps its own 345 MB entry.
- The cache keys hash `gpu.yml`, so a pull request that edits the workflow rebuilds lavapipe
  once (about 5 minutes). That is the price of never restoring a build that does not match the
  workflow's own instructions.
- Lavapipe on CI and on the owner's machine produced the same images for the same Mesa. A
  developer can therefore check goldens locally, but references come from CI.
- The lavapipe manifest is `lvp_icd.json` on Ubuntu and on Arch, not the `lvp_icd.x86_64.json`
  that CLAUDE.md and several task verifications name. On those systems the documented command
  loads no driver, and the GPU tests would skip. A follow-up fixes the commands.
- Windows catches loader, registry and MSVC problems in GPU code, but it does not check
  goldens, and it cannot test anything that needs a surface.
- Upgrading Mesa is a deliberate pull request: bump `MESA_VERSION` and both checksums,
  regenerate the lavapipe references, and say in the pull request why the images moved.
- `docs/dependencies.md` (M1-02) should list Mesa (MIT), the validation layers (Apache-2.0)
  and the SDK action as test-time tools, with these versions. None of them ship with the app.

## Revisit

- When M1-30 lands, measure the real build time in the GPU job. Today the GPU job builds the
  release tree again, as `ci.yml` does. If the 10-minute budget gets tight, or the duplicate
  builds cost too many minutes, run `-L gpu` and `-L golden` as steps of `ci.yml`'s release
  jobs instead. M1-31 touches both files.
- When the repository becomes public (4 vCPUs), loosen the golden limits.
- When the GPU path tracer arrives (1.0), its goldens are the ones that hit the limits first.
- If mesa-dist-win stops publishing, or lags the Linux version by more than a minor release:
  build lavapipe for Windows the same way as on Linux, or drop GPU tests on Windows.

## Sources

- Mesa release tarballs and checksums: https://archive.mesa3d.org/
- Lavapipe ray tracing (Mesa 24.1): https://phoronix.com/news/Lavapipe-RT-Pipelines-Mesa-24.1
- Lavapipe's extension list, including the WSI condition: `src/gallium/frontends/lavapipe/lvp_device.c` in Mesa 26.2.6
- ubuntu-24.04 runner image: https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md
- Runner sizes for private and public repositories: https://docs.github.com/en/actions/reference/runners/github-hosted-runners
- LunarG Vulkan SDK downloads and checksums: https://vulkan.lunarg.com/sdk/home
- Vulkan loader, elevated processes and driver discovery on Windows: https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md
- Vulkan SDK action: https://github.com/jakoch/install-vulkan-sdk-action
- mesa-dist-win: https://github.com/pal1000/mesa-dist-win
- jakoch/rasterizers (lavapipe and SwiftShader for Windows): https://github.com/jakoch/rasterizers
- The throwaway program, workflows and runs: https://github.com/groundbeeflover/curation_sandbox/tree/spike/lavapipe
