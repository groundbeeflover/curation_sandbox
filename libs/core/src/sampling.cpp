// SPDX-License-Identifier: Apache-2.0

#include <cs/core/sampling.hpp>

#include <cassert>
#include <cmath>

namespace cs {

namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;

// The unit direction at polar angle theta from +z and azimuth phi.
glm::dvec3 spherical_direction(double sin_theta, double cos_theta, double phi) {
    return {sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta};
}

// 1 - cos(angle), written as 2 sin^2(angle / 2) so that narrow beams do not lose their digits
// to cancellation.
double one_minus_cos(double angle_rad) {
    const double s = std::sin(0.5 * angle_rad);
    return 2.0 * s * s;
}

} // namespace

// Malley's method: points spread evenly over the unit disk, lifted straight up onto the
// hemisphere, are distributed as cos(theta) / pi. Here the disk point has radius sqrt(u.x), so
// sin^2(theta) = u.x and cos(theta) = sqrt(1 - u.x), which stays above 0 because u.x < 1.
DirectionSample sample_cosine_hemisphere(glm::dvec2 u) {
    const double sin_theta = std::sqrt(u.x);
    const double cos_theta = std::sqrt(1.0 - u.x);
    return {spherical_direction(sin_theta, cos_theta, two_pi * u.y),
            cosine_hemisphere_pdf(cos_theta)};
}

// Uniform over solid angle means uniform in cos(theta). With t = 1 - cos(theta) uniform on
// [0, 1 - cos(half_angle)), sin(theta) = sqrt(t (2 - t)) avoids the cancellation of
// sqrt(1 - cos^2) near the axis.
DirectionSample sample_uniform_cone(glm::dvec2 u, double half_angle_rad) {
    assert(half_angle_rad > 0.0 && half_angle_rad <= std::numbers::pi);
    const double width = one_minus_cos(half_angle_rad);
    const double t = u.x * width;
    const double sin_theta = std::sqrt(std::max(t * (2.0 - t), 0.0));
    return {spherical_direction(sin_theta, 1.0 - t, two_pi * u.y), 1.0 / (two_pi * width)};
}

double uniform_cone_pdf(double half_angle_rad) {
    assert(half_angle_rad > 0.0 && half_angle_rad <= std::numbers::pi);
    return 1.0 / (two_pi * one_minus_cos(half_angle_rad));
}

// The cone with a half-angle of pi, written out: cos(theta) = 1 - 2 u.x, uniform on (-1, 1].
DirectionSample sample_uniform_sphere(glm::dvec2 u) {
    const double cos_theta = 1.0 - 2.0 * u.x;
    const double sin_theta = 2.0 * std::sqrt(u.x * (1.0 - u.x));
    return {spherical_direction(sin_theta, cos_theta, two_pi * u.y), uniform_sphere_pdf()};
}

} // namespace cs
