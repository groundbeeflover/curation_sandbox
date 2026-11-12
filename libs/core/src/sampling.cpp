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

// The largest double below 1.
constexpr double one_minus_epsilon = 0x1.fffffffffffffp-1;

// 1 - cos(half_angle), the cone's solid angle over 2 pi, written as 2 sin^2(half_angle / 2) so
// that narrow beams do not lose their digits to cancellation. The angle must lie in (0, pi].
// It is checked only in debug builds: angles from files are validated where they are parsed.
double cone_width(double half_angle_rad) {
    assert(half_angle_rad > 0.0 && half_angle_rad <= std::numbers::pi);
    const double s = std::sin(0.5 * half_angle_rad);
    return 2.0 * s * s;
}

double cone_pdf(double width) {
    return 1.0 / (two_pi * width);
}

} // namespace

// Malley's method: points spread evenly over the unit disk, lifted straight up onto the
// hemisphere, are distributed as cos(theta) / pi. Here the disk point has radius sqrt(u.x), so
// sin^2(theta) = u.x and cos(theta) = sqrt(1 - u.x). Keeping u.x below 1 keeps cos(theta), and
// so the pdf, above 0: a direction in the surface plane would make f / pdf divide by zero.
DirectionSample sample_cosine_hemisphere(glm::dvec2 u) {
    const double r_squared = std::min(u.x, one_minus_epsilon);
    const double sin_theta = std::sqrt(r_squared);
    const double cos_theta = std::sqrt(1.0 - r_squared);
    return {spherical_direction(sin_theta, cos_theta, two_pi * u.y),
            cosine_hemisphere_pdf(cos_theta)};
}

// Uniform over solid angle means uniform in cos(theta). With t = 1 - cos(theta) uniform on
// [0, width), sin(theta) = sqrt(t (2 - t)) avoids the cancellation of sqrt(1 - cos^2) near the
// axis.
DirectionSample sample_uniform_cone(glm::dvec2 u, double half_angle_rad) {
    const double width = cone_width(half_angle_rad);
    const double t = u.x * width;
    const double sin_theta = std::sqrt(std::max(t * (2.0 - t), 0.0));
    return {spherical_direction(sin_theta, 1.0 - t, two_pi * u.y), cone_pdf(width)};
}

double uniform_cone_pdf(double half_angle_rad) {
    return cone_pdf(cone_width(half_angle_rad));
}

// The cone with a half-angle of pi, written out: cos(theta) = 1 - 2 u.x, uniform on (-1, 1].
DirectionSample sample_uniform_sphere(glm::dvec2 u) {
    const double cos_theta = 1.0 - 2.0 * u.x;
    const double sin_theta = 2.0 * std::sqrt(u.x * (1.0 - u.x));
    return {spherical_direction(sin_theta, cos_theta, two_pi * u.y), uniform_sphere_pdf()};
}

} // namespace cs
