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
Python 3.11+ and Git. Milestone 1 moves dependencies to vcpkg and changes these commands; the
README is updated with it.

```sh
cmake --preset debug           # configure a debug build with sanitizers
cmake --build --preset debug   # compile
ctest --preset debug           # run the unit tests
python3 scripts/format.py --check                  # formatting (clang-format 23)
python3 -m unittest discover -s scripts/tests      # tests of the planning scripts
```

## Licence

Apache-2.0 (the licence file arrives with milestone 1). Every dependency must allow
redistribution, and catalogue images must be public domain or CC0 with a credit line; see the
decision records.
