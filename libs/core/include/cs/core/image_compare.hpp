// SPDX-License-Identifier: Apache-2.0

#pragma once

// How different two images are, for golden-image tests and benchmarks: the plain numeric errors
// on the linear values, and NVIDIA FLIP, which estimates how different they look to a person
// flipping between them on a screen (Andersson et al., HPG 2020 and HDR-FLIP, 2021).

#include <cs/core/image.hpp>

namespace cs {

struct ImageComparison {
    // The mean of |reference - test| over every pixel and all three channels, in the images'
    // units (nits for a render).
    double mean_absolute_error = 0.0;
    // The largest |reference - test| of any channel of any pixel.
    double max_error = 0.0;
    // The mean of FLIP's per-pixel error, in [0, 1]: 0 is indistinguishable.
    double mean_flip = 0.0;
};

// Compares a test image with a reference of the same size. FLIP runs in its HDR mode with the
// defaults of the reference implementation's tool for EXR files: 67 pixels per degree (a 4K
// monitor 0.7 m wide seen from 0.7 m), the ACES tone mapper, and exposures found from the
// reference; when the reference is black they are found from the test image instead. FLIP sees
// negative and non-finite values as 0; the two numeric errors see them as they are, so a NaN in
// either image makes those NaN, and a tolerance check on them fail.
// Throws std::invalid_argument, saying both sizes, if the images differ in size.
ImageComparison compare(const Image& reference, const Image& test);

} // namespace cs
