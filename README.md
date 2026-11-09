# Curation Sandbox

A desktop "digital maquette" for exhibitions: build a room, hang true-to-scale artworks in
parametric frames, and light them with physically accurate light. It is a learning project
first, written from scratch in C++ and Vulkan, and will become free, open-source software if
it goes well.

**Status:** milestone 1, a CPU path tracer. So far it renders a colour gradient.

## Build and run

You need a C++20 compiler (GCC 13+, Clang 16+ or Visual Studio 2022), CMake 3.25+, Ninja and
Git. The first configure downloads the test framework, so it needs a network connection.

```sh
cmake --preset debug           # configure a debug build with sanitizers
cmake --build --preset debug   # compile
ctest --preset debug           # run the unit tests
./build/debug/apps/pt/pt       # writes renders/first.ppm
```

Write code with the `debug` preset: it is slow, but it catches memory bugs and undefined
behaviour. Render with the `release` preset, which is many times faster. The `profile`
preset is optimised but keeps debug information, for profilers and for debugging slow code.

On Windows, open the folder in Visual Studio 2022, which reads `CMakePresets.json`, or build
without presets:

```sh
cmake -B build
cmake --build build --config Release
ctest --test-dir build --build-config Release
```

## Layout

```text
core/      GPU-free library: images, photometry and, soon, the path tracer
apps/pt/   command-line path tracer
tests/     unit tests (Catch2)
docs/      milestone guides and learning notes
```

`core` never depends on Vulkan or window code, so it builds and tests anywhere. Every later
renderer is checked against it.

## Roadmap

1. **C++ and a CPU path tracer** (now). See the [milestone 1 guide](docs/milestone-1.md).
2. **First Vulkan renderer.** Checkpoint on 31 January 2027: a painting lit in Vulkan that
   matches the path tracer.
3. **Curation tools:** rooms, frames, hanging, lights, a catalogue of CC0 works.
4. **Baked global illumination.** This completes the learning MVP.
5. **Hardware ray tracing.**
6. **Open-source release.**

## Licence

Not chosen yet: MIT or Apache-2.0, before the first public release. Every dependency must
allow redistribution, and catalogue images must be CC0.
