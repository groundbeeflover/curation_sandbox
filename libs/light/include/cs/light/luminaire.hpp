// SPDX-License-Identifier: Apache-2.0

#pragma once

// A fixture placed in the room
// ----------------------------
// A Luminaire is a Distribution at a position, aimed at a point: its beam axis (fixture +z)
// points from the position to the aim point. World coordinates are in metres, z up.
//
// The fixture's y axis points upwards for a level beam, and its x axis is horizontal: to the
// left of the beam seen from behind the fixture, so that x, y and z are right-handed. For a
// fixture aimed straight up or down, x is the world's x axis. Only distributions that are not
// symmetric about their axis (IES files, milestone 3) care about this roll.

#include <cs/light/distribution.hpp>

#include <glm/vec3.hpp>

#include <memory>

namespace cs::light {

class Luminaire {
  public:
    // Throws std::invalid_argument if a coordinate is not finite, the aim point is the
    // position, or the distribution is null.
    Luminaire(glm::dvec3 position_m, glm::dvec3 aim_point_m,
              std::shared_ptr<const Distribution> distribution);

    // Intensity in candela towards a world direction (need not be of unit length).
    double intensity_cd(glm::dvec3 world_direction) const;

    // Intensity in candela towards a point in the world, as the lux meter needs it.
    double intensity_cd_towards(glm::dvec3 point_m) const {
        return intensity_cd(point_m - position_m_);
    }

    // A world direction expressed in fixture space.
    glm::dvec3 to_fixture(glm::dvec3 world_direction) const;

    glm::dvec3 position_m() const { return position_m_; }
    glm::dvec3 axis() const { return axis_z_; } // unit beam axis, in the world
    const Distribution& distribution() const { return *distribution_; }

  private:
    glm::dvec3 position_m_;
    glm::dvec3 axis_x_; // the fixture's axes, in the world, orthonormal
    glm::dvec3 axis_y_;
    glm::dvec3 axis_z_;
    std::shared_ptr<const Distribution> distribution_;
};

} // namespace cs::light
