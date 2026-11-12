// SPDX-License-Identifier: Apache-2.0

#include <cs/core/photometry.hpp>
#include <cs/core/random.hpp>
#include <cs/core/sampling.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double pi = std::numbers::pi;

// Two uniform numbers in [0, 1) for one sample.
glm::dvec2 next_u(cs::Rng& rng) {
    return {rng.next_double(), rng.next_double()};
}

double radians_from_degrees(double angle_deg) {
    return angle_deg * pi / 180.0;
}

// The smallest and largest u the generator can produce, exactly 1 (which rounding in a caller
// can produce), and some in between: the samplers must return unit directions with a finite,
// positive pdf at all of them.
constexpr double u_max = 0xffffffffp-32;
constexpr std::array<glm::dvec2, 6> edge_u = {glm::dvec2{0.0, 0.0},   glm::dvec2{u_max, u_max},
                                              glm::dvec2{0.0, u_max}, glm::dvec2{u_max, 0.0},
                                              glm::dvec2{1.0, 1.0},   glm::dvec2{0.5, 0.5}};

// Unit length within a few rounding errors of a double.
constexpr double unit_tolerance = 1e-12;

} // namespace

TEST_CASE("cosine-weighted hemisphere sampling", "[core][sampling][accuracy]") {
    constexpr int n = 1'000'000;
    cs::Rng rng(cs::hash_seed(11, 0, 0, 0, 0));

    double sum_cos = 0.0;
    double sum_cos_squared = 0.0;
    glm::dvec3 sum_direction(0.0);
    double min_z = 1.0;
    double max_pdf_error = 0.0;
    double max_length_error = 0.0;
    for (int i = 0; i < n; ++i) {
        const cs::DirectionSample s = cs::sample_cosine_hemisphere(next_u(rng));
        const double cos_theta = s.direction.z; // the normal is +z
        sum_cos += cos_theta;
        sum_cos_squared += cos_theta * cos_theta;
        sum_direction += s.direction;
        min_z = std::min(min_z, cos_theta);
        max_pdf_error = std::max(max_pdf_error, std::abs(s.pdf - cos_theta / pi));
        max_length_error = std::max(max_length_error, std::abs(glm::length(s.direction) - 1.0));
    }

    // E[cos theta] = integral of cos theta * (cos theta / pi) over the hemisphere = 2/3. The
    // standard deviation of cos theta is sqrt(1/18) = 0.236, so the standard error of the mean
    // of 10^6 samples is 2.4e-4; 0.005 is the tolerance of the acceptance criterion, 20 of them.
    CHECK_THAT(sum_cos / n, WithinAbs(2.0 / 3.0, 0.005));

    // Many densities have that mean; under this one cos^2(theta) is uniform on (0, 1], so
    // E[cos^2 theta] = 1/2 with a variance of 1/12: standard error 2.9e-4, and 0.0015 is 5 of them.
    CHECK_THAT(sum_cos_squared / n, WithinAbs(0.5, 0.0015));

    // The azimuth is uniform: the mean direction has no sideways part. x = sin(theta) cos(phi)
    // has variance E[sin^2 theta] / 2 = 1/4, so the standard error is 5e-4; 0.005 is 10 of them.
    CHECK_THAT(sum_direction.x / n, WithinAbs(0.0, 0.005));
    CHECK_THAT(sum_direction.y / n, WithinAbs(0.0, 0.005));

    // Every direction is above the surface, has unit length and the density cos(theta)/pi.
    CHECK(min_z > 0.0);
    CHECK(max_length_error < unit_tolerance);
    CHECK(max_pdf_error <= 1e-6);

    // The pdf on its own, for a BRDF's pdf(wo, wi), agrees with the sampler, and is zero below
    // the surface.
    CHECK_THAT(cs::cosine_hemisphere_pdf(0.5), WithinRel(0.5 / pi, 1e-15));
    CHECK(cs::cosine_hemisphere_pdf(-0.5) == 0.0);
    for (const glm::dvec2& u : edge_u) {
        const cs::DirectionSample s = cs::sample_cosine_hemisphere(u);
        CHECK(s.direction.z > 0.0);
        CHECK_THAT(glm::length(s.direction), WithinAbs(1.0, unit_tolerance));
        CHECK(s.pdf > 0.0);
        CHECK(std::isfinite(s.pdf));
        CHECK(s.pdf == cs::cosine_hemisphere_pdf(s.direction.z));
    }
}

TEST_CASE("uniform cone sampling", "[core][sampling][accuracy]") {
    const double half_angle_deg = GENERATE(0.5, 5.0, 15.0, 30.0, 60.0, 90.0, 135.0, 180.0);
    const double half_angle_rad = radians_from_degrees(half_angle_deg);
    const double cos_max = std::cos(half_angle_rad);
    INFO("half angle " << half_angle_deg << " degrees");

    constexpr int n = 100'000;
    cs::Rng rng(cs::hash_seed(12, 0, 0, 0, 0));

    double sum_cos = 0.0;
    glm::dvec2 sum_sideways(0.0);
    double min_cos_margin = 1.0;
    double max_length_error = 0.0;
    double min_pdf = INFINITY;
    double max_pdf = 0.0;
    for (int i = 0; i < n; ++i) {
        const cs::DirectionSample s = cs::sample_uniform_cone(next_u(rng), half_angle_rad);
        const double cos_theta = s.direction.z; // the axis is +z
        sum_cos += cos_theta;
        sum_sideways += glm::dvec2(s.direction.x, s.direction.y);
        min_cos_margin = std::min(min_cos_margin, cos_theta - cos_max);
        max_length_error = std::max(max_length_error, std::abs(glm::length(s.direction) - 1.0));
        min_pdf = std::min(min_pdf, s.pdf);
        max_pdf = std::max(max_pdf, s.pdf);
    }

    // Every direction lies inside the cone. 1e-12 allows for cos(half_angle) being rounded
    // differently here and in the sampler; it is far smaller than any angle a light cares about.
    CHECK(min_cos_margin >= -1e-12);
    CHECK(max_length_error < unit_tolerance);

    // The density is uniform, 1 / solid angle: pdf * 2 pi (1 - cos(half_angle)) = 1. 1e-9 is the
    // acceptance criterion; it leaves room for the cancellation in 1 - cos at 0.5 degrees, which
    // costs about 3e-12 here.
    const double solid_angle = 2.0 * pi * (1.0 - cos_max);
    CHECK(min_pdf == max_pdf);
    CHECK_THAT(min_pdf * solid_angle, WithinAbs(1.0, 1e-9));
    CHECK_THAT(cs::uniform_cone_pdf(half_angle_rad) * solid_angle, WithinAbs(1.0, 1e-9));

    // Uniform over solid angle means cos(theta) is uniform on [cos(half_angle), 1], with mean
    // (1 + cos(half_angle)) / 2 and standard deviation (1 - cos(half_angle)) / sqrt(12). The
    // standard error of 10^5 samples is then 9.1e-4 of the cone's width in cos(theta); 0.005 of
    // the width is more than 5 of them.
    const double width = 1.0 - cos_max;
    CHECK_THAT(sum_cos / n, WithinAbs((1.0 + cos_max) / 2.0, 0.005 * width));

    // The azimuth covers the whole circle evenly, so the mean direction has no sideways part.
    // With t = 1 - cos(theta) uniform on [0, width), E[sin^2 theta] = E[t (2 - t)] =
    // width - width^2 / 3, and x = sin(theta) cos(phi) has half that variance. 5 standard errors.
    const double sideways_se = std::sqrt((width - width * width / 3.0) / 2.0 / n);
    CHECK_THAT(sum_sideways.x / n, WithinAbs(0.0, 5.0 * sideways_se));
    CHECK_THAT(sum_sideways.y / n, WithinAbs(0.0, 5.0 * sideways_se));

    for (const glm::dvec2& u : edge_u) {
        const cs::DirectionSample s = cs::sample_uniform_cone(u, half_angle_rad);
        CHECK(s.direction.z >= cos_max - 1e-12);
        CHECK_THAT(glm::length(s.direction), WithinAbs(1.0, unit_tolerance));
        CHECK(std::isfinite(s.pdf));
        CHECK(s.pdf == min_pdf);
    }
}

TEST_CASE("the cone sampler covers the solid angle of a uniform cone light",
          "[core][sampling][accuracy]") {
    // A light of 1 lm spread evenly over the cone has an intensity of 1 / solid angle candela,
    // which is the sampler's density: lights sampled with it carry the lumens they are rated for.
    // 1e-9 relative, as above.
    const double half_angle_deg = GENERATE(0.5, 15.0, 60.0, 90.0, 180.0);
    const double half_angle_rad = radians_from_degrees(half_angle_deg);
    INFO("half angle " << half_angle_deg << " degrees");
    CHECK_THAT(cs::uniform_cone_pdf(half_angle_rad),
               WithinRel(cs::photometry::uniform_cone_intensity(1.0, half_angle_rad), 1e-9));
}

TEST_CASE("uniform sphere sampling", "[core][sampling][accuracy]") {
    constexpr int n = 1'000'000;
    cs::Rng rng(cs::hash_seed(13, 0, 0, 0, 0));

    glm::dvec3 sum_direction(0.0);
    double max_length_error = 0.0;
    double sum_x_squared = 0.0;
    double sum_z_squared = 0.0;
    for (int i = 0; i < n; ++i) {
        const cs::DirectionSample s = cs::sample_uniform_sphere(next_u(rng));
        sum_direction += s.direction;
        sum_x_squared += s.direction.x * s.direction.x;
        sum_z_squared += s.direction.z * s.direction.z;
        max_length_error = std::max(max_length_error, std::abs(glm::length(s.direction) - 1.0));
        REQUIRE(s.pdf == cs::uniform_sphere_pdf());
    }

    // Each component of a uniform direction has variance 1/3, so the mean of 10^6 has a standard
    // error of 5.8e-4 per axis and a typical length of 1e-3; 0.005 is the acceptance criterion.
    CHECK(glm::length(sum_direction / static_cast<double>(n)) < 0.005);

    // Not only balanced but uniform: E[z^2] = 1/3 for the polar angle, and E[x^2] = 1/3 for the
    // azimuth (standard error 3e-4 each).
    CHECK_THAT(sum_z_squared / n, WithinAbs(1.0 / 3.0, 0.003));
    CHECK_THAT(sum_x_squared / n, WithinAbs(1.0 / 3.0, 0.003));

    CHECK(max_length_error < unit_tolerance);
    CHECK_THAT(cs::uniform_sphere_pdf(), WithinRel(1.0 / (4.0 * pi), 1e-15));

    for (const glm::dvec2& u : edge_u) {
        const cs::DirectionSample s = cs::sample_uniform_sphere(u);
        CHECK_THAT(glm::length(s.direction), WithinAbs(1.0, unit_tolerance));
    }
}
