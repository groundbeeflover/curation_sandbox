// SPDX-License-Identifier: Apache-2.0
//
// The viewport's exposure (ADR 0002; issue #82). The viewport shows the active viewpoint's EV100.
// When no viewpoint is active (the curator walks away from it), or the viewpoint asks for
// automatic exposure, it keeps the last EV100 it had; a new project starts at EV100 8. Automatic
// exposure itself arrives with M2-39.
#pragma once

#include <cs/world/world.hpp>

namespace cs::ui {

class Exposure {
  public:
    // A new project's EV100: a lit gallery at about 100 nits shows mid-grey near the middle.
    static constexpr double default_ev100 = 8.0;
    // The tone-mapping pass takes EV100 within this, either way (ToneMapPass::max_ev100_magnitude).
    static constexpr double max_ev100_magnitude = 100.0;

    [[nodiscard]] double ev100() const { return ev100_; }

    // A viewpoint became active: its EV100, clamped to +-max_ev100_magnitude. A viewpoint without
    // one, or with one that is not finite, keeps the last EV100.
    void view_from(const world::Camera& viewpoint);
    // A new project: back to default_ev100.
    void reset() { ev100_ = default_ev100; }

  private:
    double ev100_ = default_ev100;
};

} // namespace cs::ui
