# NVIDIA FLIP

The C++ implementation of FLIP, an image difference evaluator, which the golden-image tests and
benchmarks use through `cs::core`'s image comparison. Vendored because it has no vcpkg port.

| | |
| --- | --- |
| Upstream | https://github.com/NVlabs/flip |
| Version | 1.7 (`pyproject.toml` at the commit below; the repository has no tags) |
| Commit | `d3ab02a4425eb167d8a4021b382a2bafc6e6c838` (2026-10-13) |
| Licence | BSD-3-Clause, `LICENSE` |
| Retrieved | 2026-11-11 |

| File | From | SHA-256 |
| --- | --- | --- |
| `FLIP.h` | `src/cpp/FLIP.h` | `c6dfe508adb7802a9f602114ece3d822dccb739ba389b97320c91cf2b1ddbc07` |
| `LICENSE` | `LICENSE` | `13b955078ffb4a3215757038ab2e5fcf0cc66349d0990faec9adba8ad034e578` |

Both files are byte-identical to upstream: never edit them here. `CMakeLists.txt` is ours and
defines the header-only target `flip::flip`, which `libs/core/CMakeLists.txt` adds and links.
Code includes the header as `<FLIP.h>`; `FLIP_ENABLE_CUDA` stays undefined.

To update: download both files at a new commit from
`https://raw.githubusercontent.com/NVlabs/flip/<commit>/<path>`, check that the licence is
unchanged, and update this table and the FLIP row of `docs/dependencies.md` in the same pull
request. Check that the hashes match with `sha256sum third_party/flip/FLIP.h third_party/flip/LICENSE`.
