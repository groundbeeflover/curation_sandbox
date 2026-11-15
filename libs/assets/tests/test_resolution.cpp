// SPDX-License-Identifier: Apache-2.0

// Tests of resolution.hpp (M1-14): pixels a metre at true size, and how close a viewer with
// one-arcminute acuity can stand before the image looks soft (ADR 0005).

#include <cs/assets/resolution.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>

TEST_CASE("effective_resolution: 4000 px across 800 mm", "[assets][resolution][accuracy]") {
    const auto r = cs::assets::effective_resolution(4000, 0.8);
    // 5.0 px/mm, exactly representable after the division; 1e-12 is double rounding.
    CHECK(r.pixels_per_m / 1000.0 == Catch::Approx(5.0).margin(1e-12));
    // 3437.75 / 5 mm = 687.55 mm; the issue's tolerance is 1 mm.
    CHECK(r.sharp_distance_m == Catch::Approx(0.688).margin(0.001));
}

TEST_CASE("effective_resolution matches ADR 0005's examples", "[assets][resolution][accuracy]") {
    // ADR 0005: 6.9 px/mm is sharp from 0.5 m, 3.4 px/mm from 1 m (rounded to 0.1 px/mm, so the
    // distance is checked to 2%).
    CHECK(cs::assets::effective_resolution(6900, 1.0).sharp_distance_m ==
          Catch::Approx(0.5).epsilon(0.02));
    CHECK(cs::assets::effective_resolution(3400, 1.0).sharp_distance_m ==
          Catch::Approx(1.0).epsilon(0.02));
    // Twice the pixels on the same work: twice as close.
    const double d1 = cs::assets::effective_resolution(1000, 0.5).sharp_distance_m;
    const double d2 = cs::assets::effective_resolution(2000, 0.5).sharp_distance_m;
    CHECK(d2 == Catch::Approx(d1 / 2.0).epsilon(1e-12));
}

TEST_CASE("effective_resolution refuses sizes that are not positive", "[assets][resolution]") {
    CHECK_THROWS_AS(cs::assets::effective_resolution(0, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(cs::assets::effective_resolution(-5, 1.0), std::invalid_argument);
    CHECK_THROWS_AS(cs::assets::effective_resolution(100, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(cs::assets::effective_resolution(100, -1.0), std::invalid_argument);
    CHECK_THROWS_AS(cs::assets::effective_resolution(100, std::numeric_limits<double>::quiet_NaN()),
                    std::invalid_argument);
    CHECK_THROWS_AS(cs::assets::effective_resolution(100, std::numeric_limits<double>::infinity()),
                    std::invalid_argument);
}
