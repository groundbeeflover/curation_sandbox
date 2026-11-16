// SPDX-License-Identifier: Apache-2.0

#pragma once

// Effective resolution (ADR 0005)
// -------------------------------
// The app never invents detail an image does not have: no super-resolution, no sharpening. It
// says instead how close a visitor can stand before the image looks soft. Hung at true size, an
// image w pixels across a work W metres wide has w / W pixels a metre. A viewer with normal
// acuity resolves one arcminute, so at a distance d the smallest detail they see is
// d tan(1 arcminute) across: the image is sharp from d = 1 / (tan(1') w / W) away, which is
// 3438 / (pixels a millimetre) millimetres. 4000 pixels across 0.8 m is 5 px/mm, sharp from
// 0.688 m.

namespace cs::assets {

struct EffectiveResolution {
    double pixels_per_m = 0.0;
    // The closest a viewer with one-arcminute acuity can stand and still see the image sharp.
    double sharp_distance_m = 0.0;
};

// The effective resolution of an image width_px pixels across, hung on a work width_m metres
// wide. Throws std::invalid_argument if width_px is not positive or width_m is not a positive
// finite number.
EffectiveResolution effective_resolution(int width_px, double width_m);

} // namespace cs::assets
