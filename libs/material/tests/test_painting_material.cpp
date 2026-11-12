// SPDX-License-Identifier: Apache-2.0

#include <cs/core/random.hpp>
#include <cs/core/sampling.hpp>
#include <cs/material/painting_material.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <numbers>

using Catch::Matchers::WithinAbs;

namespace {

bool equal(const glm::dvec3& a, const glm::dvec3& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

} // namespace

TEST_CASE("PaintingMaterial has the defaults of ADR 0003", "[material]") {
    const cs::material::PaintingMaterial m;

    // Base: oil paint, roughness of presets 1 to 5; colour and specular weight from OpenPBR.
    CHECK(equal(m.base.color, glm::dvec3(0.8)));
    CHECK(m.base.roughness == 0.45);
    CHECK(m.base.specular_weight == 1.0);

    // Coat: off; gloss varnish roughness (preset 1); IOR 1.52.
    CHECK(m.coat.weight == 0.0);
    CHECK(m.coat.roughness == 0.18);
    CHECK(m.coat.ior == 1.52);

    // Metal: off; gold F0 and edge tint; burnished roughness.
    CHECK(m.metal.weight == 0.0);
    CHECK(equal(m.metal.f0, {1.059, 0.773, 0.307}));
    CHECK(equal(m.metal.edge_tint, {1.001, 0.985, 0.523}));
    CHECK(m.metal.roughness == 0.21);

    // Fuzz: off; OpenPBR's colour and roughness.
    CHECK(m.fuzz.weight == 0.0);
    CHECK(equal(m.fuzz.color, glm::dvec3(1.0)));
    CHECK(m.fuzz.roughness == 0.5);

    // Glazing: none; float glass when switched on.
    CHECK_FALSE(m.glazing.present);
    CHECK(m.glazing.ior == 1.52);
    CHECK(m.glazing.reflectance_scale == 1.0);
    CHECK(equal(m.glazing.transmittance, glm::dvec3(0.89)));
}

TEST_CASE("The glazing IOR gives the ADR's 8.2% reflectance for float glass",
          "[material][accuracy]") {
    // Two surfaces at normal incidence, with the light bouncing between them: R = 2 r / (1 + r),
    // where r = ((n - 1) / (n + 1))^2 for one surface. Tolerance 0.0005: the ADR gives 8.2%, to
    // one decimal.
    const double n = cs::material::PaintingMaterial{}.glazing.ior;
    const double r = (n - 1.0) * (n - 1.0) / ((n + 1.0) * (n + 1.0));
    CHECK_THAT(2.0 * r / (1.0 + r), WithinAbs(0.082, 0.0005));
}

TEST_CASE("PaintingBrdf evaluates only the base", "[material][accuracy]") {
    cs::material::PaintingMaterial m;
    m.base.color = {0.6, 0.3, 0.1};
    // Every other layer switched on: none of them may change the result yet.
    m.coat.weight = 1.0;
    m.metal.weight = 1.0;
    m.fuzz.weight = 1.0;
    m.glazing.present = true;
    m.glazing.reflectance_scale = 0.12;

    const cs::material::PaintingBrdf brdf(m);
    const cs::material::LambertianBrdf base(m.base.color);

    cs::Rng rng(cs::hash_seed(5, 0, 0, 0, 0));
    for (int i = 0; i < 1000; ++i) {
        const glm::dvec3 wo =
            cs::sample_uniform_sphere({rng.next_double(), rng.next_double()}).direction;
        const glm::dvec3 wi =
            cs::sample_uniform_sphere({rng.next_double(), rng.next_double()}).direction;
        const glm::dvec2 u{rng.next_double(), rng.next_double()};
        CHECK(equal(brdf.eval(wo, wi), base.eval(wo, wi)));
        CHECK(brdf.pdf(wo, wi) == base.pdf(wo, wi));
        const cs::material::BrdfSample a = brdf.sample(wo, u);
        const cs::material::BrdfSample b = base.sample(wo, u);
        CHECK(equal(a.wi, b.wi));
        CHECK(a.pdf == b.pdf);
        CHECK(equal(a.weight, b.weight));
    }
    const glm::dvec3 up{0.0, 0.0, 1.0};
    // One division by pi: equal to rounding.
    CHECK_THAT(brdf.eval(up, up).g, WithinAbs(0.3 * std::numbers::inv_pi, 1e-15));
}
