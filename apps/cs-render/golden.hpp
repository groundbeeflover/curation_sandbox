// SPDX-License-Identifier: Apache-2.0

#pragma once

// cs-render golden: one golden-image test (tests/golden/README.md).
//
//   cs-render golden <test-folder> [--update] [--out <folder>] [--threads <n>]
//
// renders the scene <test-folder>/test.json describes, at its tier, size, samples and seed, and
// compares it with <test-folder>/ref.<driver>.exr, the reference for the driver that rendered it
// (cpu for the path tier, which the engine renders). It passes when the mean FLIP and the mean
// absolute error are both within test.json's tolerances. On a failure it writes the render, a
// difference image and a FLIP heat map to <folder>, by default build/<preset>/golden-out/<name>/.
// --update writes the render as the new reference instead; without it, nothing in the test folder
// is ever written.

namespace cs_render {

// The exit code of a test with no reference for the running driver, which CTest shows as skipped
// rather than passed (SKIP_RETURN_CODE in cmake/CsGolden.cmake). 77 is what Automake's test
// harness and Meson use for a skip.
inline constexpr int exit_skip = 77;

// Runs `cs-render golden ...`, where argv[0] is "golden", and returns the exit code: 0 within the
// tolerances (or the reference updated), 1 outside them or on an error, 2 for a usage error,
// exit_skip without a reference.
int golden_main(int argc, char* argv[]);

} // namespace cs_render
