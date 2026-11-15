// SPDX-License-Identifier: Apache-2.0

// The compiled skeleton under the lux meter: the document loaded from tests/scenes/skeleton,
// compiled to a world and read by the engine. It belongs here because the engine may not depend
// on the document (blueprint section 7), so its own tests cannot load a project.

#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("the compiled skeleton's chart reads the spot's I cos / d^2 at its centre",
          "[integration][accuracy]") {
    const cs::doc::Document document = cs::doc::load_project_folder(
        std::filesystem::path(CS_SOURCE_DIR) / "tests/scenes/skeleton");
    const cs::world::World world = cs::world::compile(document);
    REQUIRE(world.problems.empty());
    REQUIRE(world.exhibits.size() == 1);
    REQUIRE(world.lights.size() == 1);

    const cs::world::Exhibit& chart = world.exhibits.front();
    const cs::light::Luminaire& spot = world.lights.front().luminaire;

    // The chart's centre, facing out of the wall, as a curator would hold a meter against it.
    const glm::dvec3 point_m = chart.face_point({0.0, 0.0});
    const glm::dvec3 normal = chart.face_normal;

    // The analytic answer, from the compiled world's own light and exhibit.
    const glm::dvec3 to_light = spot.position_m() - point_m;
    const double distance_m = glm::length(to_light);
    const double cos_theta = glm::dot(normal, to_light / distance_m);
    const double expected_lux =
        spot.intensity_cd_towards(point_m) * cos_theta / (distance_m * distance_m);

    // The scene README's geometry: the light arrives from 2.309 m at cos 0.4577. The README
    // rounds both to four figures, so 1 mm and 1e-3 hold them; a compile that moved the work or
    // the spot by more would fail here before the lux comparison could hide it.
    CHECK_THAT(distance_m, WithinAbs(2.309, 1e-3));
    CHECK_THAT(cos_theta, WithinAbs(0.4577, 1e-3));

    cs::engine::LuxOptions options;
    options.samples = 16; // only the direct part is checked; it does not depend on samples
    const cs::engine::Scene scene(world);
    const cs::engine::LuxReading reading = cs::engine::lux_at(scene, point_m, normal, options);

    // The direct part is a sum over point lights, not an estimate, so the engine evaluates the
    // same I cos / d^2 and agrees to rounding; its shadow ray starts ray_offset_m (1e-5 m) off
    // the face, far below 1%. The 1% (the owner's, on #47) leaves room for nothing else: a light
    // shadowed by the frame or the ceiling, a wrong intensity or a flipped normal all miss it.
    CHECK(reading.direct_lux.standard_error == 0.0);
    REQUIRE(reading.direct_lux.value > 0.0);
    CHECK_THAT(reading.direct_lux.value, WithinRel(expected_lux, 0.01));
}
