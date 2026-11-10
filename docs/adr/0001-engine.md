# 0001. Build the renderer: C++20, Vulkan, Slang and Embree

- Status: proposed
- Date: 2026-11-10
- Decides: question 1 of the planning brief (build or buy the engine)

## Context

The product's selling points are true scale and trustworthy light: fixtures in candela with real
IES beam shapes, a layered material for paintings, and a lux meter that reads the renderer's own
light, checked against a path-traced reference (ADR 0002). It must also run at 60 fps on
integrated GPUs, build for Windows and Linux, and be released under Apache-2.0.

All code is written by Claude Code sessions and reviewed by one person. So the stack must be
code-first and text-based, build and test from the command line, and run headless in GitHub
Actions. Editors with binary scene files are hard for an agent to change and for a reviewer to
read.

## Options weighed

Each scored 1 (poor) to 5 (excellent) on: time to MVP / quality ceiling / control over physical
accuracy / integrated-GPU performance / ray tracing / licence fit / cross-platform /
agent-friendliness.

| Candidate | Scores | Total | Deciding facts |
| --- | --- | --- | --- |
| **Custom renderer: C++20, Vulkan 1.3, Slang, Embree** | 2/5/5/4/5/5/4/4 | **34** | One light model drives the image, the lux meter and the reference. Vulkan ray tracing runs on the 780M (RADV), RTX cards, Intel Arc and lavapipe in CI. The most scope. |
| Custom renderer: Rust, wgpu | 3/4/5/4/3/5/5/3 | 32 | wgpu 30's ray tracing is still marked experimental; better macOS story. |
| Diligent Engine + own renderer | 2/4/5/4/4/5/4/3 | 31 | A thinner abstraction than writing Vulkan directly, but less known to agents. |
| Godot 4.7 | 5/3/2/4/2/5/5/4 | 30 | Fastest MVP, text scenes, lumens and lux; but no candela, no IES, no path tracer, lightmaps baked in the editor. |
| Blender 5.3 as host | 4/5/3/3/4/2/5/4 | 30 | Cycles is excellent; GPL, and Blender's UI is not a curator's tool. |
| three.js or Babylon.js on WebGPU, in Electron or Tauri | 5/3/3/3/1/5/3/5 | 28 | No hardware ray tracing in WebGPU; WebGPU on Linux webviews is unverified. |
| Bevy 0.20 | 3/3/3/3/3/5/5/3 | 28 | Breaking changes every ~4 months; its ray-traced lighting has no spot or point lights and denoises with DLSS. |
| Wicked Engine | 3/5/2/3/4/5/4/2 | 28 | Strong renderer, one maintainer, little documentation. |
| Filament 1.80 | 3/2/3/5/1/5/4/4 | 27 | Excellent physical units; no ray tracing and no GI. |
| O3DE | 2/4/4/2/3/5/4/2 | 26 | Heavy, editor-centred. |
| Unreal Engine 5.8 | 4/5/4/2/3/1/3/1 | 23 | Licence forbids publishing engine source; binary assets; RADV crashes reported. |
| Unity 6 HDRP | 4/4/4/2/2/1/3/2 | 22 | Proprietary; HDRP in maintenance mode since 2026. |
| NVIDIA Falcor, Omniverse Kit | 2/5/4/1/2/3/2/3 and 3/5/4/1/2/1/2/3 | 22, 21 | Research or NVIDIA-only. |

**Runner-up: Godot.** It would win if lux could come from a separate calculator rather than the
renderer's own light, IES could be faked with projector textures, and speed to market mattered
more than the accuracy claim. None of those is true for this product. Rust and wgpu would win if
wgpu's ray tracing stabilises and macOS becomes a launch requirement.

The custom renderer's weakest score, time to MVP, is the one the working method improves most:
three parallel agent sessions, established libraries for everything that is not the product's
own physics, and a walking skeleton first.

## Decision

Build the renderer and the application in **C++20** with **CMake**, on **Vulkan 1.3** (dynamic
rendering, synchronisation 2, buffer device address; ray queries where available), with shaders
in **Slang** compiled to SPIR-V at build time, and **Embree 4** for the CPU calculation engine.

### Libraries

Versions are pinned in [`docs/dependencies.md`](../dependencies.md), which a task in milestone
1 writes. All are permissive (MIT, BSD, zlib, Apache-2.0) except Qt (LGPL-3.0, ADR 0006).

| Purpose | Library | Licence |
| --- | --- | --- |
| Vulkan loader and headers | volk, Vulkan-Headers | MIT, Apache-2.0 |
| Instance and device selection, including headless | vk-bootstrap | MIT |
| GPU memory | Vulkan Memory Allocator | MIT |
| Shaders | Slang (`slangc`) | Apache-2.0 with LLVM exception |
| CPU ray tracing for the engine | Embree 4 | Apache-2.0 |
| Denoising | Open Image Denoise 2 | Apache-2.0 |
| Application shell and UI | Qt 6 Widgets (ADR 0006) | LGPL-3.0, dynamically linked |
| Image input and output | stb_image, libpng, libjpeg-turbo, libtiff, tinyexr, lcms2 | public domain, zlib, BSD-style, MIT |
| GPU texture compression and containers | KTX-Software (Basis Universal) | Apache-2.0 |
| glTF (later exhibits, test meshes) | fastgltf | MIT |
| JSON; zip container | nlohmann/json; miniz | MIT; MIT |
| Vector and matrix maths | glm | MIT |
| Tests | Catch2 3 | BSL-1.0 |
| Image comparison in tests | NVIDIA FLIP | BSD-3 |
| Debug overlays only | Dear ImGui | MIT |

Dependencies come from **vcpkg in manifest mode** with a pinned baseline and a binary cache, so
that a fresh worktree builds in minutes; Qt is the exception, installed from Qt's binaries (a
pinned version in CI, the system package or the Qt installer on developer machines), because
building it from source in vcpkg takes hours.

### What this asks of the code

- Vulkan validation layers are on in debug builds and in CI, and a validation message fails the
  test that caused it. Synchronisation and lifetime bugs are the custom renderer's main risk.
- The renderer can run without a window (offscreen), so every tier is testable headless on
  lavapipe.
- Slang's version is pinned (it releases monthly) and shaders are compiled at build time, never
  at run time, so a shader error fails the build.

## Consequences

- The most code of any option. The roadmap's first milestone is a walking skeleton that proves
  the whole path (document, world, engine, Vulkan, window) early, rather than any one layer.
- Full control of units, materials, IES and the lux meter, which is the product's reason to
  exist.
- Agents work in a well-known language and API, with everything in text and every check on the
  command line.
- macOS later goes through MoltenVK or KosmicKrisp; ray tracing there is unverified and would be
  a spike when macOS is planned.

## Revisit

If the walking skeleton (milestone 1) overruns its estimate by more than half, compare again
with Godot for the display tiers while keeping the calculation engine. Otherwise at the 1.0
planning, for wgpu's ray-tracing status if macOS is on the table.

## Sources

- Godot physical light units: https://docs.godotengine.org/en/stable/tutorials/3d/physical_light_and_camera_units.html
- Bevy 0.20 release notes: https://bevy.org/news/bevy-0-20/
- wgpu changelog: https://github.com/gfx-rs/wgpu/blob/trunk/CHANGELOG.md
- Lavapipe ray tracing pipelines (Mesa 24.1): https://phoronix.com/news/Lavapipe-RT-Pipelines-Mesa-24.1
- Ubuntu noble Mesa packages: https://packages.ubuntu.com/noble-updates/mesa-vulkan-drivers
- Filament light units: https://github.com/google/filament/blob/main/filament/include/filament/LightManager.h
- Unity render pipelines in 2026: https://unity.com/topics/render-pipelines-strategy-for-2026
- Unreal Engine licence FAQ: https://www.unrealengine.com/en-US/faq
- O3DE lights: https://docs.o3de.org/docs/user-guide/components/reference/atom/light/
- Wicked Engine features: https://github.com/turanszkij/WickedEngine/blob/master/features.txt
- volk https://github.com/zeux/volk, vk-bootstrap https://github.com/charles-lunarg/vk-bootstrap,
  fastgltf https://github.com/spnda/fastgltf
