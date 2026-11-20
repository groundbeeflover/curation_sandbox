// SPDX-License-Identifier: Apache-2.0
#include "viewport/exposure.hpp"

#include <algorithm>
#include <cmath>

namespace cs::ui {

void Exposure::view_from(const world::Camera& viewpoint) {
    if (viewpoint.ev100 && std::isfinite(*viewpoint.ev100)) {
        ev100_ = std::clamp(*viewpoint.ev100, -max_ev100_magnitude, max_ev100_magnitude);
    }
}

} // namespace cs::ui
