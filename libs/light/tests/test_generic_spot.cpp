// SPDX-License-Identifier: Apache-2.0

// The generic spotlight of distribution.hpp, checked against the definitions of its datasheet
// values: beam angle where the intensity is 50% of the peak, field angle where it is 10%, and
// lumens as the intensity integrated over the sphere.

#include <cs/core/photometry.hpp>
#include <cs/light/distribution.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::light::GenericSpot;

namespace {

constexpr double pi = std::numbers::pi;
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();

double deg(double degrees) {
    return degrees * pi / 180.0;
}

// The unit direction theta from the axis (+z), at azimuth phi.
glm::dvec3 direction(double theta, double phi) {
    return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
}

// The intensity integrated over the sphere, in lumens, by the midpoint rule in theta and phi.
// It only calls intensity_cd, so it does not assume the symmetry the implementation relies on.
// 100,000 steps in theta put about 280 inside the half beam of a 1-degree spot; its error is
// about 1e-5 for smooth profiles and 1e-4 for the step of a uniform cone.
double integrate_over_sphere(const GenericSpot& spot) {
    constexpr int n_theta = 100'000;
    constexpr int n_phi = 4;
    const double d_theta = pi / n_theta;
    const double d_phi = 2.0 * pi / n_phi;
    double sum = 0.0;
    for (int i = 0; i < n_theta; ++i) {
        const double theta = (i + 0.5) * d_theta;
        double ring = 0.0;
        for (int j = 0; j < n_phi; ++j) {
            ring += spot.intensity_cd(direction(theta, (j + 0.5) * d_phi));
        }
        sum += ring * d_phi * std::sin(theta) * d_theta;
    }
    return sum;
}

// The flux in lumens between two angles from the axis, by the midpoint rule in theta, for a
// spot symmetric about its axis.
double flux_between(const GenericSpot& spot, double from_theta, double to_theta) {
    constexpr int n = 100'000;
    const double d_theta = (to_theta - from_theta) / n;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double theta = from_theta + (i + 0.5) * d_theta;
        sum += spot.intensity_cd(direction(theta, 0.0)) * std::sin(theta);
    }
    return 2.0 * pi * sum * d_theta;
}

struct Angles {
    double beam_deg;
    double field_deg;
};

} // namespace

TEST_CASE("generic spot: 50% of the peak at half the beam angle, 10% at half the field angle",
          "[light][accuracy]") {
    // Tolerance 1% (the issue's); the profile meets both points exactly by construction, so
    // this only allows for rounding.
    const auto [beam_deg, field_deg] =
        GENERATE(Angles{10, 20}, Angles{24, 36}, Angles{30, 60}, Angles{60, 100}, Angles{15, 90},
                 Angles{1, 2}, Angles{120, 360}, Angles{60, 180}, Angles{100, 200});
    CAPTURE(beam_deg, field_deg);
    const GenericSpot spot(800.0, deg(beam_deg), deg(field_deg));
    const double peak = spot.peak_intensity_cd();

    CHECK_THAT(spot.intensity_cd({0.0, 0.0, 1.0}), WithinRel(peak, 1e-12));
    for (const double phi : {0.0, 1.0, 2.5, 4.0}) {
        CHECK_THAT(spot.intensity_cd(direction(deg(beam_deg) / 2.0, phi)),
                   WithinRel(0.5 * peak, 0.01));
        CHECK_THAT(spot.intensity_cd(direction(deg(field_deg) / 2.0, phi)),
                   WithinRel(0.1 * peak, 0.01));
    }
}

TEST_CASE("generic spot: the intensity falls steadily away from the axis", "[light]") {
    const GenericSpot spot(800.0, deg(24), deg(36));
    double previous = spot.intensity_cd({0.0, 0.0, 1.0});
    for (int i = 1; i <= 180; ++i) {
        const double current = spot.intensity_cd(direction(deg(i), 0.3));
        CHECK(current <= previous);
        CHECK(current >= 0.0);
        previous = current;
    }
}

TEST_CASE("generic spot: integrating over the sphere returns the lumens", "[light][accuracy]") {
    // Tolerance 0.1%, inside the issue's 0.5%: the implementation is good to 1e-5 or better, and
    // the test's own midpoint rule to about 1e-4 (its error for a hard edge).
    const auto [beam_deg, field_deg] =
        GENERATE(Angles{10, 20}, Angles{24, 36}, Angles{30, 60}, Angles{60, 100}, Angles{15, 90},
                 Angles{1, 2}, Angles{30, 30.5}, Angles{120, 360}, Angles{40, 40}, Angles{10, 40},
                 Angles{10, 60}, Angles{60, 180}, Angles{100, 200}, Angles{5, 120},
                 Angles{30, 30.0003}, Angles{359, 360}, Angles{30, 170});
    CAPTURE(beam_deg, field_deg);
    const double lumens = 680.0;
    const GenericSpot spot(lumens, deg(beam_deg), deg(field_deg));

    CHECK(spot.total_lumens() == lumens);
    CHECK_THAT(integrate_over_sphere(spot), WithinRel(lumens, 0.001));
}

struct Spill {
    double beam_deg;
    double field_deg;
    double inside_field; // the share of the lumens inside half the field angle
};

TEST_CASE("generic spot: how much light spills beyond the field angle", "[light][accuracy]") {
    // Pins the profile's shape, not only its two defining points: the share of the lumens inside
    // half the field angle, with the taper the owner chose on #135. The shares are this
    // implementation's, and agree with an independent model of the same rule to within 1%.
    // Tolerance 0.1% of the lumens: flux_between is good to about 1e-6 here.
    const auto [beam_deg, field_deg, inside_field] =
        GENERATE(Spill{15, 25, 0.9427}, Spill{24, 36, 0.9575}, Spill{10, 40, 0.8461},
                 Spill{10, 60, 0.8196}, Spill{15, 90, 0.8279}, Spill{60, 100, 0.9484});
    CAPTURE(beam_deg, field_deg);
    const double lumens = 680.0;
    const GenericSpot spot(lumens, deg(beam_deg), deg(field_deg));
    CHECK_THAT(flux_between(spot, 0.0, deg(field_deg) / 2.0) / lumens,
               WithinAbs(inside_field, 0.001));
}

TEST_CASE("generic spot: nothing beyond the cut-off, nothing behind", "[light]") {
    // The cut-off is 1.5 times the half field angle, at most 90 degrees when the field angle is
    // 180 degrees or less, and at most 180 degrees otherwise.
    const auto [beam_deg, field_deg] =
        GENERATE(Angles{10, 20}, Angles{10, 60}, Angles{15, 90}, Angles{60, 100}, Angles{30, 170},
                 Angles{60, 180}, Angles{100, 200}, Angles{120, 360});
    CAPTURE(beam_deg, field_deg);
    const GenericSpot spot(800.0, deg(beam_deg), deg(field_deg));
    const double half_field = deg(field_deg) / 2.0;
    const double cutoff =
        half_field <= pi / 2.0
            ? std::min(1.5 * half_field, pi / 2.0)
            : std::min({1.5 * half_field, pi, half_field + 3.0 * (half_field - pi / 2.0)});

    if (cutoff > half_field) {
        CHECK(spot.intensity_cd(direction(0.5 * (half_field + cutoff), 0.4)) > 0.0);
    }
    for (int i = 0; i <= 1000; ++i) {
        const double theta = cutoff + (pi - cutoff) * i / 1000.0;
        if (theta > cutoff) {
            CHECK(spot.intensity_cd(direction(theta, 0.4)) == 0.0);
        }
    }
    if (field_deg <= 180) {
        CHECK(flux_between(spot, pi / 2.0, pi) == 0.0);
    }
}

TEST_CASE("generic spot: no jump where the taper starts", "[light][accuracy]") {
    // Just inside and just outside half the field angle, the intensity and its slope agree. With
    // a step of h = 1e-6 rad, the values differ by about the slope times h (tolerance 1e-5 of
    // the peak) and the one-sided slopes by the taper's curvature times h (tolerance 1%).
    const auto [beam_deg, field_deg] = GENERATE(Angles{10, 20}, Angles{24, 36}, Angles{10, 60},
                                                Angles{15, 90}, Angles{60, 100}, Angles{30, 170});
    CAPTURE(beam_deg, field_deg);
    const GenericSpot spot(800.0, deg(beam_deg), deg(field_deg));
    const double half_field = deg(field_deg) / 2.0;
    const double h = 1e-6;
    const auto at = [&spot](double theta) { return spot.intensity_cd(direction(theta, 1.1)); };

    const double inside = at(half_field - h);
    const double edge = at(half_field);
    const double outside = at(half_field + h);
    CHECK_THAT(inside, WithinAbs(edge, 1e-5 * spot.peak_intensity_cd()));
    CHECK_THAT(outside, WithinAbs(edge, 1e-5 * spot.peak_intensity_cd()));
    CHECK_THAT((outside - edge) / h, WithinRel((edge - inside) / h, 0.01));
}

TEST_CASE("generic spot: no jump as the field angle passes 180 degrees", "[light][accuracy]") {
    // The cut-off moves continuously through a 180-degree field (physics review of the taper):
    // a hundredth of a degree wider must not move a share of the lumens behind the fixture.
    // Tolerance 0.1% of the peak.
    const GenericSpot at_180(1.0, deg(60), deg(180));
    for (const double field_deg : {179.99, 180.01}) {
        CAPTURE(field_deg);
        const GenericSpot near(1.0, deg(60), deg(field_deg));
        CHECK_THAT(near.peak_intensity_cd(), WithinRel(at_180.peak_intensity_cd(), 0.001));
    }
}

TEST_CASE("generic spot: a narrow spot is finite everywhere", "[light][accuracy]") {
    // The owner's addition to the issue: a 1-degree beam in a 2-degree field still conserves its
    // light (checked above) and gives no infinite or NaN intensity in any direction.
    const GenericSpot spot(680.0, deg(1), deg(2));
    CHECK(std::isfinite(spot.peak_intensity_cd()));
    CHECK(spot.peak_intensity_cd() > 0.0);
    for (int i = 0; i <= 3600; ++i) {
        const double theta = deg(i * 0.05);
        const double intensity = spot.intensity_cd(direction(theta, 0.7));
        CHECK(std::isfinite(intensity));
        CHECK(intensity >= 0.0);
    }
    CHECK(std::isfinite(spot.intensity_cd({0.0, 0.0, 0.0})));
    CHECK(std::isfinite(spot.intensity_cd({0.0, 0.0, -1.0})));
}

TEST_CASE("generic spot: equal beam and field angles give a uniform cone", "[light][accuracy]") {
    // Tolerance 1e-6 relative (the issue's): both sides compute the same formula.
    const double lumens = 1000.0;
    const double angle = GENERATE(deg(1), deg(30), deg(90), deg(200), 2.0 * pi);
    CAPTURE(angle);
    const GenericSpot spot(lumens, angle, angle);
    const double expected = cs::photometry::uniform_cone_intensity(lumens, angle / 2.0);

    CHECK_THAT(spot.peak_intensity_cd(), WithinRel(expected, 1e-6));
    for (const double fraction : {0.0, 0.25, 0.5, 0.9, 0.999}) {
        CHECK_THAT(spot.intensity_cd(direction(fraction * angle / 2.0, 1.2)),
                   WithinRel(expected, 1e-6));
    }
    if (angle < 2.0 * pi) {
        CHECK(spot.intensity_cd(direction(1.001 * angle / 2.0, 1.2)) == 0.0);
        CHECK(spot.intensity_cd({0.0, 0.0, -1.0}) == 0.0);
    }
}

TEST_CASE("generic spot: refuses lumens and angles it cannot represent", "[light]") {
    // The owner's addition to the issue: checked in release builds too, because a zero or
    // non-finite angle would otherwise give an infinite intensity and NaN pixels.
    const double beam = deg(20);
    const double field = deg(30);

    SECTION("lumens must be positive and finite") {
        for (const double lumens : {0.0, -1.0, not_a_number, infinity}) {
            CAPTURE(lumens);
            CHECK_THROWS_AS(GenericSpot(lumens, beam, field), std::invalid_argument);
        }
    }
    SECTION("the beam angle must be positive and finite") {
        for (const double bad : {0.0, -deg(10), not_a_number, infinity}) {
            CAPTURE(bad);
            CHECK_THROWS_AS(GenericSpot(500.0, bad, field), std::invalid_argument);
        }
    }
    SECTION("the field angle must be finite, at least the beam angle and at most 2 pi") {
        for (const double bad : {deg(10), 0.0, not_a_number, infinity, 2.0 * pi + 1e-9, -field}) {
            CAPTURE(bad);
            CHECK_THROWS_AS(GenericSpot(500.0, beam, bad), std::invalid_argument);
        }
    }
    SECTION("the peak intensity must be finite") {
        CHECK_THROWS_AS(GenericSpot(500.0, 1e-200, 1e-200), std::invalid_argument);
        CHECK_THROWS_AS(GenericSpot(500.0, 1e-200, 2e-200), std::invalid_argument);
        CHECK_THROWS_AS(GenericSpot(1e308, deg(1), deg(2)), std::invalid_argument);
    }
    SECTION("the limits themselves are accepted") {
        CHECK_NOTHROW(GenericSpot(500.0, beam, beam));
        CHECK_NOTHROW(GenericSpot(500.0, 2.0 * pi, 2.0 * pi));
        CHECK_NOTHROW(GenericSpot(1e-9, 1e-6, 2.0 * pi));
    }
}

TEST_CASE("generic spot: an example from ADR 0003", "[light][accuracy]") {
    // "A 6-degree spot of about 680 lm reaches about 40,000 cd". The ADR's figure is for a real
    // fixture; this generic one, with a typical 12-degree field, lands in the same range.
    const GenericSpot spot(680.0, deg(6), deg(12));
    CHECK(spot.peak_intensity_cd() > 30'000.0);
    CHECK(spot.peak_intensity_cd() < 60'000.0);
}
