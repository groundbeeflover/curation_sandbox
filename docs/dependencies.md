# Dependencies

Every library, tool and shipped data asset the project uses, with its version and licence. The
policy comes from [ADR 0009](adr/0009-platforms-licence-distribution.md): everything we ship
must allow redistribution under Apache-2.0. The libraries are chosen in
[ADR 0001](adr/0001-engine.md), the data rules in [ADR 0003](adr/0003-painting-materials.md)
and [ADR 0004](adr/0004-catalogue.md).

`python3 scripts/check-dependencies.py` checks this file against `vcpkg.json`: every dependency
has a row in [Libraries from vcpkg](#libraries-from-vcpkg) and none without one, and every
licence in the vcpkg, pulled-in and vendored tables is allowed by
[Allowed licences](#allowed-licences). Given a configured build
(`--installed build/debug/vcpkg_installed`), it also compares the versions with what vcpkg
installed and finds installed ports missing here. The script tests run it on the repository, so
CI fails when the names or licences here and `vcpkg.json` disagree; versions are compared only
when the script is given `--installed`, which CI does not do yet.

## Adding or changing a dependency

1. A new library needs a task that says so (`CLAUDE.md`); never vendor or download code a task
   did not ask for.
2. Add it to `vcpkg.json` and a row here, with the version vcpkg installs at the pinned
   `builtin-baseline`, the licence as an SPDX expression from the port's `vcpkg.json` (or the
   upstream licence file when the port has none), what it is for and which modules link it.
3. A port it pulls in goes under [Pulled in by other ports](#pulled-in-by-other-ports).
4. Run `python3 scripts/check-dependencies.py --installed build/debug/vcpkg_installed`.

Moving the baseline changes versions: update the rows in the same pull request. Versions are
upstream versions; vcpkg's port revisions (`#1`) are left out.

When a port is missing or broken, a later task may fetch a pinned prebuilt archive from CMake
with a SHA-256 instead, and documents it here. None does today: every library in the vcpkg
tables comes from a port, and the only other code is FLIP, [vendored](#vendored). Slang's port is
itself a download of Slang's release binaries, checked with SHA-512.

## Allowed licences

For code we link, vendor or ship. Licence expressions follow SPDX: in `A OR B` one allowed choice
is enough, in `A AND B` both must be allowed, and `A WITH <exception>` is judged by `A`, since an
exception only adds permissions.

| SPDX | Licence | When we ship binaries |
| --- | --- | --- |
| `Apache-2.0` | Apache License 2.0 | include the licence and the library's NOTICE file, if any |
| `MIT` | MIT License | include the copyright notice and licence |
| `BSD-2-Clause` | BSD 2-Clause | include the copyright notice and licence |
| `BSD-3-Clause` | BSD 3-Clause | include the copyright notice and licence |
| `Zlib` | zlib License | nothing required; we include it anyway |
| `BSL-1.0` | Boost Software License 1.0 | nothing required for binaries |
| `libtiff` | libtiff License (BSD-style) | include the copyright notice and licence |

Not allowed: GPL and AGPL of any version; LGPL, except Qt, dynamically linked and installed
outside vcpkg (below); non-commercial, research-only or no-derivatives terms; NVIDIA RTX SDKs,
whose licence forbids use under an open-source licence. Adding a licence to this table changes
the policy of ADR 0009, so the owner decides it, in a pull request of its own.

## Libraries from vcpkg

Manifest mode, pinned by the `builtin-baseline` in `vcpkg.json`. Modules link these through
`cs_add_module(... EXTERNAL ...)` in `libs/<module>/CMakeLists.txt`; a module's external
libraries are visible to the modules that declare it.

| Port | Version | Licence | Purpose | Module |
| --- | --- | --- | --- | --- |
| `catch2` | 3.16.1 | `BSL-1.0 AND BSD-3-Clause` | Unit tests (Catch2 3); never shipped | tests of every module |
| `embree` | 4.4.1 | `Apache-2.0` | CPU ray tracing for the calculation engine. Only triangles, instances, filter functions and ray masks are built, with Embree's own task system instead of TBB: a smaller build and one dependency fewer | engine |
| `glm` | 1.0.3 | `MIT` | Vectors and matrices | core |
| `lcms` | 2.19.1 | `MIT` | Little CMS 2: embedded ICC profiles converted to linear Rec. 709 on import | assets |
| `miniz` | 3.1.2 | `MIT` | The zip container of `.curation` files; also tinyexr's compression | doc |
| `nlohmann-json` | 3.12.0 | `MIT` | JSON of project files and manifests | doc |
| `shader-slang` | 2026.21 | `Apache-2.0 WITH LLVM-exception` | `slangc`, which compiles the shaders to SPIR-V at build time. A build tool, declared `"host": true` so that vcpkg installs it for the machine that builds: its library is never linked or shipped, so the licences of what its binaries bundle (LLVM, glslang, SPIRV-Tools) do not reach our packages | gpu (build only) |
| `stb` | 2026-08-02 | `MIT OR Unlicense` | `stb_image` (PNG and JPEG import), `stb_image_write` (PNG output), later `stb_truetype` (the overlay's font atlas) | core; assets and gpu through core |
| `tiff` | 4.7.2 | `libtiff` | TIFF import. Only the `zip` feature (Deflate; LZW is built in): JPEG, LZMA and other codecs are left out, which keeps the code that reads hostile files small | assets |
| `tinyexr` | 3.2.0 | `BSD-3-Clause` | EXR read and write (it includes parts of OpenEXR, also BSD-3-Clause) | core |
| `vk-bootstrap` | 1.4.363 | `MIT` | Vulkan instance and device selection, including headless | gpu |
| `volk` | 1.4.363.0 | `MIT` | Loads the Vulkan loader and its functions at run time, so no loader is linked. Its header-only target: gpu compiles volk itself, with the window-system defines its surfaces need | gpu |
| `vulkan-headers` | 1.4.363.0 | `Apache-2.0 OR MIT` | The Vulkan API headers | gpu |
| `vulkan-memory-allocator` | 3.5.0 | `MIT` | GPU memory allocation (VMA) | gpu |

Added later by the tasks that first use them: Open Image Denoise (M3) and KTX-Software (M4).

### Pulled in by other ports

| Port | Version | Licence | Needed by |
| --- | --- | --- | --- |
| `zlib` | 1.3.2 | `Zlib` | tiff (Deflate) |

Ports named `vcpkg-*` (`vcpkg-cmake`, `vcpkg-cmake-config`, `vcpkg-cmake-get-vars`,
`vcpkg-tool-meson`) only build other ports; nothing of them is linked or shipped, and they are
not listed.

### How the ports are built

`vcpkg-configuration.json` puts the overlay triplets in `triplets/` before vcpkg's own. The only
one, `x64-windows.cmake`, is vcpkg's `x64-windows` plus `/Ob2` in Embree's debug flags.

- **Why:** vcpkg compiles every port's debug configuration with `/Ob0`, which leaves Embree's
  per-ISA SIMD helpers out of line. The linker then keeps their SSE2 copies, which Embree's
  AVX-512 kernels call and crash in, on a CPU with AVX-512 (M1-46, #168). Linux needs nothing:
  GCC and Clang always inline Embree's `__forceinline`.
- **Cost:** a change to a triplet changes the ABI hash of every package built with it, so the
  Windows jobs rebuild them all once, Embree taking one to two hours. The CI cache keys hash
  `triplets/`, so the rebuilt packages are saved.

## Vendored

Code copied into `third_party/`, byte-identical to upstream, with its licence beside it. The
README in each directory gives the commit, the SHA-256 of each file and how to update it.

| Library | Version | Licence | Purpose | Module |
| --- | --- | --- | --- | --- |
| NVIDIA FLIP | 1.7, commit d3ab02a | `BSD-3-Clause` | Image comparison for golden images and benchmarks; a single C++ header with no vcpkg port, in `third_party/flip/` | core |

## Outside vcpkg

Not checked by the script: these are installed separately, and only Qt is shipped.

| What | Version | Licence | Where it comes from | Used for |
| --- | --- | --- | --- | --- |
| Qt 6 (Core, Gui, Widgets, Test; the Wayland client on Linux) | **6.10.3** in CI and releases; local builds may use 6.10.3 or newer | `LGPL-3.0-only` | Qt's binaries through aqtinstall in CI (M1-31); the distribution's package or the Qt installer on developer machines | The app and the `ui` module, dynamically linked; obligations below |
| libwayland (client) | the system's | `MIT` | The distribution's package (`libwayland-dev` on Debian and Ubuntu); Qt's Wayland client links it | The Linux app's pointer lock for walking (M1-45); not shipped, the system's is used |
| wayland-protocols | the system's | `MIT` | The distribution's package; `cmake/CsQt.cmake` generates client code from its pointer-constraints and relative-pointer XML at build time | The Linux app's pointer lock for walking (M1-45); a build input, the generated code is ours to ship |
| Vulkan loader | the system's | `Apache-2.0` | Installed with the GPU driver; volk loads it at run time | Every Vulkan program; not shipped |
| Vulkan validation layers | 1.4.363 (LunarG Vulkan SDK 1.4.363.1) | `Apache-2.0` | The LunarG SDK, pinned in CI by ADR 0010; locally the SDK or the distribution's package | Debug builds and tests, where a validation message fails the test; never shipped |
| Mesa lavapipe | 26.2.6 | `MIT` | Built from the Mesa tarball on Linux CI, mesa-dist-win on Windows (ADR 0010) | GPU tests and golden images without a GPU |
| clang-format | 23.1.4 | `Apache-2.0 WITH LLVM-exception` | `pip install clang-format==23.1.4` | `scripts/format.py`; CI uses the same version |
| CMake, Ninja, vcpkg, Python 3 | CMake 3.25 or newer | `BSD-3-Clause`, `Apache-2.0`, `MIT`, `PSF-2.0` | The system, or Visual Studio on Windows | Building and the scripts; never shipped |

Qt 6.10.3 is the newest version that aqtinstall, which CI uses, can install on both Windows and
Linux on 2026-11-11 (spike M1-04, ADR 0011): move the pin to the newest version both can install
once it catches up.

### Qt's LGPL-3.0 obligations

What shipping Qt under the LGPL asks of us ([Qt's summary](https://www.qt.io/development/open-source-lgpl-obligations)):

- Link Qt dynamically, as its own unmodified shared libraries, so that a user can replace them
  with a compatible build; never link it statically.
- Use only Qt modules offered under the LGPL. Some are GPL-3.0 only (for example Qt Charts,
  Qt Graphs and Qt Virtual Keyboard) and are not allowed.
- Ship the LGPL-3.0 and GPL-3.0 texts and Qt's copyright notices with the app, and say in the
  app's About box and documentation that it uses Qt under the LGPL-3.0.
- Provide the source of the exact Qt version shipped, or a written offer to provide it; the
  pinned version makes that one archive.
- If we ever change Qt itself, publish those changes under the LGPL. We do not change it.

## Data assets

Data shipped with the app or committed to the repository. ADR 0003 and ADR 0004 set the rules;
nothing below is checked by the script.

### Allowed licences for shipped data

| Licence | For |
| --- | --- |
| `CC0-1.0` | Any data: catalogue images, material values, test images |
| Public domain, marked by the source | Catalogue images whose source record says so (ADR 0004 stores the raw rights field) |
| `OFL-1.1` | Fonts only |
| `Apache-2.0` | Data the project makes itself: test scenes, rendered test images, presets |
| A manufacturer's terms, per file | IES files, only when the terms allow redistribution with the app; recorded per file here |

Not allowed: CC BY-SA, non-commercial or research-only terms (MERL and UTIA fit parameters but
are never shipped, ADR 0003), and no-derivatives terms.

### Data

| Asset | Version | Licence | Source | Where | Status |
| --- | --- | --- | --- | --- | --- |
| UI font: Inter | 4.1 | `OFL-1.1` | [rsms/inter v4.1](https://github.com/rsms/inter/releases/tag/v4.1), the static TrueType files in `extras/ttf/` | `data/fonts/`, with its `OFL.txt` | Chosen here; bundled by M2-19 |
| Test images | | `Apache-2.0` if the project made them, otherwise `CC0-1.0` | Each committed image gets a row here | `libs/*/tests/data/`, `tests/golden/` | None yet |
| Catalogue images | | `CC0-1.0` or public domain, per image | Museums' open-access collections (ADR 0004) | `catalogue/`, images in releases | Later (M3); the manifest records each image's rights and credit line |
| IES files | | Per file | Fixture manufacturers (ADR 0003) | `data/fixtures/` | Later; each file gets a row here |
| Material values | | `CC0-1.0` or similar | physicallybased.info, parts of the EPFL RGL database (ADR 0003) | `data/finishes/`, `data/presets/` | Later; each source gets a row here |

**Why Inter.** It was drawn for user interfaces on screens and stays legible at small sizes; it
has tabular figures, so lux columns line up; it covers the accented Latin of artists' names; and
its static TrueType files have the outlines `stb_truetype` reads for the overlay's text. The OFL
lets the font be bundled and embedded with the app, provided its licence ships beside it and the
font is not sold on its own.
