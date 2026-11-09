#include <cs/core/photometry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

using Catch::Matchers::WithinRel;
namespace ph = cs::photometry;

TEST_CASE("illuminance falls with the square of the distance", "[photometry]") {
    // A 1000 cd light 2 m away, shining straight at the surface: 1000 / 2^2 = 250 lux.
    CHECK_THAT(ph::point_illuminance(1000.0, 2.0, 1.0), WithinRel(250.0, 1e-12));

    // Twice as far away, a quarter of the light.
    CHECK_THAT(ph::point_illuminance(1000.0, 4.0, 1.0), WithinRel(62.5, 1e-12));
}

TEST_CASE("illuminance follows Lambert's cosine law", "[photometry]") {
    // Light arriving 60 degrees from the normal spreads over twice the area.
    const double cos60 = std::cos(std::numbers::pi / 3.0);
    CHECK_THAT(ph::point_illuminance(1000.0, 2.0, cos60), WithinRel(125.0, 1e-12));

    // Light from behind the surface does not count.
    CHECK(ph::point_illuminance(1000.0, 2.0, -0.5) == 0.0);
}

TEST_CASE("lumen ratings convert to candela", "[photometry]") {
    // A cone with a half-angle of 180 degrees is the full sphere.
    CHECK_THAT(ph::uniform_cone_intensity(1000.0, std::numbers::pi),
               WithinRel(ph::isotropic_intensity(1000.0), 1e-12));

    // 1000 lm squeezed into a 30-degree beam (15-degree half-angle) gives about
    // 4,671 cd in the centre: 59 times brighter than spreading it everywhere.
    const double half_angle = 15.0 * std::numbers::pi / 180.0;
    CHECK_THAT(ph::uniform_cone_intensity(1000.0, half_angle), WithinRel(4670.84, 1e-5));
}
