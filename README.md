# Curation Sandbox

A desktop application for museum and gallery curators. Build an exhibition space, hang
artworks at their true size in frames, light them with real museum fixtures, and see the result
rendered with physically based light, with the illuminance in lux on every work, ready to hold
against conservation limits. It replaces paper maquettes, elevation drawings and guesswork about
light with one model that is to scale, lit in real units and quick to change.

Free, open-source software under the Apache-2.0 licence, for Windows and Linux.

**Status:** planning complete; milestone 1 (the walking skeleton) is about to start. The code in
this repository is the starting skeleton: a C++20 and CMake project with a small GPU-free core
library and tests. Nothing is usable yet. Target for a first build curators can try: March 2027.

## Where to read more

- [Product blueprint](docs/product/blueprint.md): who it is for, the journeys that matter, scope,
  quality tiers and performance budgets, architecture, risks.
- [Roadmap](docs/roadmap.md): milestones with exit criteria, epics, the tasks of the first two
  milestones, lanes for parallel work, the timeline.
- [Decision records](docs/adr/): the engine, rendering tiers, painting materials, the catalogue,
  machine learning, the editor, project files, the data model, platforms and licence.
- [CLAUDE.md](CLAUDE.md): how the code is written (by Claude Code sessions, one task per
  session) and the rules every change follows.

## How it is built

C++20 with CMake; a custom Vulkan 1.3 renderer with shaders in Slang; a CPU path tracer built on
Embree that computes every lux reading and serves as the reference for the real-time tiers; Qt 6
Widgets for the application. Work is planned as GitHub issues, built by Claude Code sessions and
reviewed by the owner; see the roadmap.

## Build and test

You need a C++20 compiler (GCC 13+, Clang 16+ or Visual Studio 2022), CMake 3.25+, Ninja,
Python 3.11+, Git and [vcpkg](https://learn.microsoft.com/vcpkg/), which builds the libraries
listed in `vcpkg.json` on the first configure and caches them. Install vcpkg once and point
`VCPKG_ROOT` at it:

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics      # Windows: bootstrap-vcpkg.bat
export VCPKG_ROOT=~/vcpkg                       # also in your shell profile
```

On Windows, run the commands below from a Visual Studio developer prompt, so Ninja finds the
compiler. Each preset configures, builds and tests in `build/<preset>/`:

```sh
cmake --preset debug           # configure: debug, with AddressSanitizer and UBSan
cmake --build --preset debug   # compile
ctest --preset debug           # run every test; `-L core` runs one module's
python3 scripts/format.py --check                  # formatting (clang-format 23)
python3 -m unittest discover -s scripts/tests      # tests of the planning scripts
```

| Preset | For |
| --- | --- |
| `debug` | writing code: sanitizers catch memory errors and undefined behaviour |
| `release` | rendering and the accuracy and golden-image tests |
| `profile` | profiling: optimised, with debug information |
| `app` | the desktop app: `debug` plus the Qt user interface (needs Qt 6, installed outside vcpkg) |
| `fuzz` | Linux and Clang: every `libs/<module>/fuzz/*.cpp` as a libFuzzer target |

The code is a set of modules under `libs/`, one `cs_add_module(...)` call each
(`cmake/CsModule.cmake`): sources, tests and fuzz targets are found by glob, so adding a file
never edits a CMake list, and a module sees only the modules it declares.

## Licence

Apache-2.0: see [LICENSE](LICENSE) and [NOTICE](NOTICE). Every dependency must allow
redistribution, and catalogue images must be public domain or CC0 with a credit line; see the
decision records.
