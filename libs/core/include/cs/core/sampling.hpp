// SPDX-License-Identifier: Apache-2.0

#pragma once

// Sampling directions for Monte Carlo integration
// -----------------------------------------------
// Each sampler turns two uniform numbers u in [0, 1)^2 (from cs::Rng) into a unit direction and
// the probability density of choosing it, per steradian. An estimate of an integral over
// directions is then the average of f(direction) / pdf.
//
// Directions are in a local frame: +z is the surface normal for the hemisphere, the axis for
// the cone. The caller rotates them into the world.
//
// Physically Based Rendering, 4th edition, appendix A.5, derives each mapping.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <numbers>

namespace cs {

struct DirectionSample {
    glm::dvec3 direction{0.0, 0.0, 1.0}; // unit length
    double pdf = 0.0;                    // per steradian
};

// Directions above a surface, more of them near the normal: pdf = cos(theta) / pi. Sampling a
// Lambertian (matte) surface this way makes every sample carry the same weight.
DirectionSample sample_cosine_hemisphere(glm::dvec2 u);

// The density of sample_cosine_hemisphere for a direction with that cosine to the normal; zero
// below the surface.
constexpr double cosine_hemisphere_pdf(double cos_theta) {
    return std::max(cos_theta, 0.0) * std::numbers::inv_pi;
}

// Directions spread evenly over a cone around +z with the given half-angle in radians, which
// must lie in (0, pi]: pdf = 1 / (2 pi (1 - cos(half_angle))), one over the solid angle that
// cs::photometry::uniform_cone_intensity divides the lumens by.
DirectionSample sample_uniform_cone(glm::dvec2 u, double half_angle_rad);

// The density of sample_uniform_cone inside the cone (it is zero outside).
double uniform_cone_pdf(double half_angle_rad);

// Directions spread evenly over the whole sphere: pdf = 1 / (4 pi).
DirectionSample sample_uniform_sphere(glm::dvec2 u);

constexpr double uniform_sphere_pdf() {
    return 0.25 * std::numbers::inv_pi;
}

} // namespace cs
