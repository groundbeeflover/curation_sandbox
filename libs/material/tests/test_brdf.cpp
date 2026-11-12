// SPDX-License-Identifier: Apache-2.0

#include <cs/core/random.hpp>
#include <cs/core/sampling.hpp>
#include <cs/material/brdf.hpp>
#include <cs/material/painting_material.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double inv_pi = std::numbers::inv_pi;

glm::dvec2 next_u(cs::Rng& rng) {
    return {rng.next_double(), rng.next_double()};
}

// A direction spread evenly over the whole sphere: above and below the surface.
glm::dvec3 any_direction(cs::Rng& rng) {
    return cs::sample_uniform_sphere(next_u(rng)).direction;
}

// Values that are a single multiplication or division by pi agree to a few rounding errors of a
// double (about 1e-17 at these sizes); 1e-15 leaves room for that and still fails any real error.
constexpr double rounding = 1e-15;

// Different values per channel, the darkest at the ADR's import clamp, so a mixed-up channel
// shows.
const glm::dvec3 test_albedo{0.8, 0.45, 0.02};

void check_rgb(const glm::dvec3& actual, const glm::dvec3& expected, double tolerance) {
    CHECK_THAT(actual.r, WithinAbs(expected.r, tolerance));
    CHECK_THAT(actual.g, WithinAbs(expected.g, tolerance));
    CHECK_THAT(actual.b, WithinAbs(expected.b, tolerance));
}

double max_component(const glm::dvec3& v) {
    return std::max({std::abs(v.r), std::abs(v.g), std::abs(v.b)});
}

// The interface's own consistency, for any BRDF: on 10^4 random wo and u, sample's pdf is pdf()
// of the direction it returns and its weight is f cos / pdf; on 10^4 random pairs, f is
// reciprocal (Helmholtz). The directions cover the whole sphere, so the cases below the surface
// are exercised too.
void check_consistency(const cs::material::Brdf& brdf, std::uint64_t seed) {
    constexpr int n = 10'000;
    cs::Rng rng(cs::hash_seed(seed, 0, 0, 0, 0));
    double max_pdf_error = 0.0;
    double max_weight_error = 0.0;
    double max_reciprocity_error = 0.0;
    int sampled = 0;
    for (int i = 0; i < n; ++i) {
        const glm::dvec3 wo = any_direction(rng);
        const cs::material::BrdfSample s = brdf.sample(wo, next_u(rng));
        if (wo.z <= 0.0) {
            // Nothing to sample for a viewer below the surface.
            CHECK(s.pdf == 0.0);
            CHECK(max_component(s.weight) == 0.0);
        } else {
            ++sampled;
            REQUIRE(s.pdf > 0.0);
            REQUIRE_THAT(glm::length(s.wi), WithinAbs(1.0, 1e-12));
            max_pdf_error = std::max(max_pdf_error, std::abs(s.pdf - brdf.pdf(wo, s.wi)));
            const glm::dvec3 expected_weight = brdf.eval(wo, s.wi) * s.wi.z / s.pdf;
            max_weight_error =
                std::max(max_weight_error, max_component(s.weight - expected_weight));
        }

        const glm::dvec3 a = any_direction(rng);
        const glm::dvec3 b = any_direction(rng);
        max_reciprocity_error =
            std::max(max_reciprocity_error, max_component(brdf.eval(a, b) - brdf.eval(b, a)));
    }
    // About half of wo lie above the surface; make sure the loop really sampled.
    CHECK(sampled > n / 3);
    // 1e-6 is the acceptance criterion's tolerance; the two are computed from the same cosine, so
    // they agree to rounding.
    CHECK(max_pdf_error <= 1e-6);
    CHECK(max_weight_error <= 1e-9);
    CHECK(max_reciprocity_error <= 1e-12);
}

} // namespace

TEST_CASE("Lambert evaluates to albedo over pi above the surface", "[material][accuracy]") {
    const cs::material::LambertianBrdf lambert(test_albedo);
    cs::Rng rng(cs::hash_seed(1, 0, 0, 0, 0));
    for (int i = 0; i < 1000; ++i) {
        const glm::dvec3 wo = any_direction(rng);
        const glm::dvec3 wi = any_direction(rng);
        const glm::dvec3 f = lambert.eval(wo, wi);
        if (wo.z > 0.0 && wi.z > 0.0) {
            check_rgb(f, test_albedo * inv_pi, rounding);
        } else {
            // Reflection only: no light passes through, and none comes from below.
            check_rgb(f, glm::dvec3(0.0), 0.0);
        }
    }
    // Exactly at the normal, both ways.
    check_rgb(lambert.eval({0, 0, 1}, {0, 0, 1}), test_albedo * inv_pi, rounding);
}

TEST_CASE("Lambert reflects its albedo: integral of f cos over the hemisphere",
          "[material][accuracy]") {
    const cs::material::LambertianBrdf lambert(test_albedo);
    const glm::dvec3 wo = glm::normalize(glm::dvec3{0.3, -0.2, 0.8});
    constexpr int n = 100'000;

    SECTION("independent of the BRDF's sampler: uniform over the hemisphere") {
        // Uniform sphere directions folded onto the upper half: pdf 1 / (2 pi). The estimate per
        // channel is 2 rho cos(theta), whose standard deviation is rho / sqrt(3), so the
        // standard error of 10^5 samples is 0.18% of rho: 0.5%, the acceptance criterion, is
        // 2.7 of them, and the fixed seed makes the result repeatable.
        cs::Rng rng(cs::hash_seed(2, 0, 0, 0, 0));
        glm::dvec3 sum(0.0);
        for (int i = 0; i < n; ++i) {
            glm::dvec3 wi = any_direction(rng);
            wi.z = std::abs(wi.z);
            sum += lambert.eval(wo, wi) * wi.z * (2.0 * std::numbers::pi);
        }
        const glm::dvec3 estimate = sum / static_cast<double>(n);
        CHECK_THAT(estimate.r, WithinRel(test_albedo.r, 0.005));
        CHECK_THAT(estimate.g, WithinRel(test_albedo.g, 0.005));
        CHECK_THAT(estimate.b, WithinRel(test_albedo.b, 0.005));
    }

    SECTION("with the BRDF's own sampler: f cos / pdf") {
        // Importance sampling the cosine makes every sample equal rho, so this agrees to
        // rounding; it checks that sample, eval and pdf describe the same distribution.
        cs::Rng rng(cs::hash_seed(3, 0, 0, 0, 0));
        glm::dvec3 sum(0.0);
        glm::dvec3 weight_sum(0.0);
        for (int i = 0; i < n; ++i) {
            const cs::material::BrdfSample s = lambert.sample(wo, next_u(rng));
            REQUIRE(s.pdf > 0.0);
            sum += lambert.eval(wo, s.wi) * s.wi.z / lambert.pdf(wo, s.wi);
            weight_sum += s.weight;
        }
        // Every term is rho to rounding, so the mean is too, but for the rounding of 10^5
        // additions (about n epsilon, 2e-11): 1e-9 rather than the 0.5% of a noisy estimate, so
        // that a pdf slightly off from sample's would show.
        check_rgb(sum / static_cast<double>(n), test_albedo, 1e-9);
        check_rgb(weight_sum / static_cast<double>(n), test_albedo, 1e-9);
    }
}

TEST_CASE("Lambert's pdf is the cosine over pi, and zero below the surface",
          "[material][accuracy]") {
    const cs::material::LambertianBrdf lambert(test_albedo);
    const glm::dvec3 wo{0.0, 0.0, 1.0};
    CHECK_THAT(lambert.pdf(wo, {0, 0, 1}), WithinAbs(inv_pi, rounding));
    const glm::dvec3 wi = glm::normalize(glm::dvec3{1.0, 0.0, 1.0});
    CHECK_THAT(lambert.pdf(wo, wi), WithinAbs(std::numbers::sqrt2 / 2.0 * inv_pi, rounding));
    CHECK(lambert.pdf(wo, {0, 0, -1}) == 0.0);
    CHECK(lambert.pdf({0, 0, -1}, {0, 0, 1}) == 0.0);
    CHECK(lambert.pdf(wo, {1, 0, 0}) == 0.0); // grazing: in the surface plane
}

TEST_CASE("Lambert's sample, pdf and eval agree, and eval is reciprocal", "[material][accuracy]") {
    check_consistency(cs::material::LambertianBrdf(test_albedo), 4);
}

TEST_CASE("Lambert's sample takes the edges of u", "[material]") {
    const cs::material::LambertianBrdf lambert(test_albedo);
    const glm::dvec3 wo{0.0, 0.0, 1.0};
    for (const glm::dvec2 u :
         {glm::dvec2{0.0, 0.0}, glm::dvec2{0xffffffffp-32, 0xffffffffp-32}, glm::dvec2{1.0, 1.0}}) {
        const cs::material::BrdfSample s = lambert.sample(wo, u);
        CHECK(s.pdf > 0.0);
        CHECK(s.wi.z > 0.0);
        check_rgb(s.weight, test_albedo, 1e-12);
    }
}
