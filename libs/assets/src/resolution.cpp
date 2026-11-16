// SPDX-License-Identifier: Apache-2.0

#include <cs/assets/resolution.hpp>

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace cs::assets {

EffectiveResolution effective_resolution(int width_px, double width_m) {
    if (width_px <= 0 || !std::isfinite(width_m) || width_m <= 0.0) {
        throw std::invalid_argument("effective_resolution needs a positive width in pixels and in "
                                    "metres, got " +
                                    std::to_string(width_px) + " px and " +
                                    std::to_string(width_m) + " m");
    }
    // One arcminute: the detail a viewer with normal acuity resolves.
    const double arcminute_rad = std::numbers::pi / (180.0 * 60.0);
    const double pixels_per_m = width_px / width_m;
    // Sharp while one pixel subtends at most an arcminute: d tan(1') >= 1 / pixels_per_m.
    return {pixels_per_m, 1.0 / (pixels_per_m * std::tan(arcminute_rad))};
}

} // namespace cs::assets
