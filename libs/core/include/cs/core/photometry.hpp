// SPDX-License-Identifier: Apache-2.0

#pragma once

// Photometric units used throughout the project
// ---------------------------------------------
//   lumen (lm)      luminous flux: all the visible light a source emits
//   candela (cd)    luminous intensity: lumens per steradian, in one direction
//   lux (lx)        illuminance: lumens arriving per square metre of surface
//   nit (cd/m^2)    luminance: how bright a surface looks from one direction
//
// Convention: the renderer works in linear RGB whose luminance (the Y value)
// is in nits. Point lights and spotlights are then given in candela, and a
// virtual lux meter adds up luminance over the hemisphere above a surface.

#include <cassert>
#include <cmath>
#include <numbers>

namespace cs::photometry {

// Illuminance (lux) on a surface lit by a point source of intensity_cd candela,
// distance_m metres away. cos_incidence is the cosine of the angle between the
// surface normal and the direction towards the light.
// This is the inverse-square law combined with Lambert's cosine law.
constexpr double point_illuminance(double intensity_cd, double distance_m, double cos_incidence) {
    if (distance_m <= 0.0 || cos_incidence <= 0.0) {
        return 0.0; // no light reaches the back of a surface
    }
    return intensity_cd * cos_incidence / (distance_m * distance_m);
}

// Intensity (candela) of an ideal point source that spreads flux_lm lumens
// evenly in all directions, over the full sphere of 4 pi steradians.
constexpr double isotropic_intensity(double flux_lm) {
    return flux_lm / (4.0 * std::numbers::pi);
}

// Intensity (candela) of an ideal spotlight that spreads flux_lm lumens evenly
// over a cone with the given half-angle in radians, which must lie in (0, pi].
// The cone covers a solid angle of 2 pi (1 - cos(half_angle)) steradians.
// Real fixtures are not this even; IES files describe how their intensity
// actually varies with direction.
inline double uniform_cone_intensity(double flux_lm, double half_angle_rad) {
    assert(half_angle_rad > 0.0 && half_angle_rad <= std::numbers::pi);
    const double solid_angle = 2.0 * std::numbers::pi * (1.0 - std::cos(half_angle_rad));
    return flux_lm / solid_angle;
}

} // namespace cs::photometry
