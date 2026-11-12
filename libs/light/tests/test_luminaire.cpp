// SPDX-License-Identifier: Apache-2.0

// A fixture placed in the room (luminaire.hpp): its beam axis points at its aim point, wherever
// that is, so the intensity towards the aim point is the peak.

#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::light::GenericSpot;
using cs::light::Luminaire;
using glm::dvec3;

namespace {

constexpr double pi = std::numbers::pi;

double deg(double degrees) {
    return degrees * pi / 180.0;
}

std::shared_ptr<const GenericSpot> spot() {
    return std::make_shared<const GenericSpot>(680.0, deg(24), deg(36));
}

struct Placement {
    dvec3 position_m;
    dvec3 aim_point_m;
};

} // namespace

TEST_CASE("luminaire: the intensity towards the aim point is the peak", "[light]") {
    const auto [position, aim] =
        GENERATE(Placement{{0.0, 0.0, 3.5}, {2.0, 1.0, 1.6}},  // from a ceiling track onto a wall
                 Placement{{1.0, 2.0, 3.0}, {1.0, 2.0, 0.0}},  // straight down
                 Placement{{1.0, 2.0, 0.0}, {1.0, 2.0, 3.0}},  // straight up
                 Placement{{0.0, 0.0, 2.0}, {-3.0, 0.0, 2.0}}, // level
                 Placement{{5.0, -4.0, 3.2}, {5.001, -4.0, -10.0}});
    CAPTURE(position.x, position.y, position.z, aim.x, aim.y, aim.z);
    const auto distribution = spot();
    const Luminaire luminaire(position, aim, distribution);

    CHECK_THAT(luminaire.intensity_cd_towards(aim),
               WithinRel(distribution->peak_intensity_cd(), 1e-9));
    // A point further along the beam axis is in the same direction.
    CHECK_THAT(luminaire.intensity_cd_towards(position + 3.0 * (aim - position)),
               WithinRel(distribution->peak_intensity_cd(), 1e-9));

    const dvec3 axis = luminaire.axis();
    CHECK_THAT(glm::length(axis), WithinAbs(1.0, 1e-12));
    const dvec3 local = luminaire.to_fixture(aim - position);
    CHECK_THAT(local.x, WithinAbs(0.0, 1e-9));
    CHECK_THAT(local.y, WithinAbs(0.0, 1e-9));
    CHECK_THAT(local.z, WithinRel(glm::length(aim - position), 1e-12));
}

TEST_CASE("luminaire: half the beam angle off the axis, half the peak", "[light][accuracy]") {
    // A spot 2 m above the floor aimed straight down; a point on the floor at a horizontal
    // distance of 2 tan(12 degrees) is 12 degrees off its axis: half its 24-degree beam.
    const auto distribution = spot();
    const Luminaire luminaire({0.0, 0.0, 2.0}, {0.0, 0.0, 0.0}, distribution);
    const double offset = 2.0 * std::tan(deg(12));
    for (const dvec3 point : {dvec3{offset, 0.0, 0.0}, dvec3{0.0, -offset, 0.0},
                              dvec3{offset / std::sqrt(2.0), offset / std::sqrt(2.0), 0.0}}) {
        CHECK_THAT(luminaire.intensity_cd_towards(point),
                   WithinRel(0.5 * distribution->peak_intensity_cd(), 0.01));
    }
}

TEST_CASE("luminaire: the fixture frame is right-handed, with x level", "[light]") {
    const Luminaire luminaire({0.0, 0.0, 3.0}, {2.0, 2.0, 1.0}, spot());
    const dvec3 x = luminaire.to_fixture({1.0, 0.0, 0.0});
    const dvec3 y = luminaire.to_fixture({0.0, 1.0, 0.0});
    const dvec3 z = luminaire.to_fixture({0.0, 0.0, 1.0});
    // to_fixture is a rotation: it keeps lengths and handedness.
    for (const dvec3 v : {x, y, z}) {
        CHECK_THAT(glm::length(v), WithinAbs(1.0, 1e-12));
    }
    CHECK_THAT(glm::dot(glm::cross(x, y), z), WithinAbs(1.0, 1e-12));
    // The world's vertical has no component along the fixture's x axis.
    CHECK_THAT(z.x, WithinAbs(0.0, 1e-12));
}

TEST_CASE("luminaire: the fixture's x points left of the beam and y up", "[light]") {
    // The documented roll, which IES files (milestone 3) will depend on.
    const auto near = [](dvec3 actual, dvec3 expected) {
        return glm::length(actual - expected) < 1e-12;
    };
    // Level, along world +x: left (seen from behind) is world +y, up is world +z.
    const Luminaire level({0.0, 0.0, 2.0}, {1.0, 0.0, 2.0}, spot());
    CHECK(near(level.to_fixture({0.0, 1.0, 0.0}), {1.0, 0.0, 0.0}));
    CHECK(near(level.to_fixture({0.0, 0.0, 1.0}), {0.0, 1.0, 0.0}));
    CHECK(near(level.to_fixture({1.0, 0.0, 0.0}), {0.0, 0.0, 1.0}));
    // Straight up and straight down: x is the world's x axis.
    const Luminaire up({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, spot());
    CHECK(near(up.to_fixture({1.0, 0.0, 0.0}), {1.0, 0.0, 0.0}));
    CHECK(near(up.to_fixture({0.0, 1.0, 0.0}), {0.0, 1.0, 0.0}));
    const Luminaire down({0.0, 0.0, 3.0}, {0.0, 0.0, 0.0}, spot());
    CHECK(near(down.to_fixture({1.0, 0.0, 0.0}), {1.0, 0.0, 0.0}));
    CHECK(near(down.to_fixture({0.0, -1.0, 0.0}), {0.0, 1.0, 0.0}));
}

TEST_CASE("luminaire: towards its own position, the peak", "[light]") {
    // A zero direction counts as the beam axis, even when the fixture frame turns it into
    // components of -0.0.
    const auto distribution = spot();
    const dvec3 position{1.0, 2.0, 3.0};
    const Luminaire luminaire(position, {0.0, 0.0, 0.0}, distribution);
    CHECK(luminaire.intensity_cd_towards(position) == distribution->peak_intensity_cd());
    CHECK(distribution->intensity_cd({0.0, 0.0, -0.0}) == distribution->peak_intensity_cd());
}

TEST_CASE("luminaire: refuses an aim it cannot point at", "[light]") {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    const dvec3 position{1.0, 2.0, 3.0};

    CHECK_THROWS_AS(Luminaire(position, position, spot()), std::invalid_argument);
    CHECK_THROWS_AS(Luminaire(position, {1.0, 2.0, 0.0}, nullptr), std::invalid_argument);
    CHECK_THROWS_AS(Luminaire({nan, 2.0, 3.0}, {0.0, 0.0, 0.0}, spot()), std::invalid_argument);
    CHECK_THROWS_AS(Luminaire(position, {0.0, infinity, 0.0}, spot()), std::invalid_argument);
}
