// SPDX-License-Identifier: Apache-2.0

// Documents the loader accepts but geom or light cannot build in full (the owner's second comment
// on #61): compile never throws for them; what cannot be built is left out of the world, or shown
// in part, and listed in World::problems with its entity's id and a reason.

#include "world_checks.hpp"

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/work.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/geom/painting.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace cs::world;
using namespace cs::world::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using glm::dvec2;
using glm::dvec3;

namespace doc = cs::doc;
namespace geom = cs::geom;

namespace {

// Whether the point lies inside the polygon in plan, by the even-odd rule: a horizontal ray from
// it crosses the outline an odd number of times. Used only for points a centimetre off an edge.
bool inside(const std::vector<dvec2>& polygon, dvec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const dvec2 a = polygon[i];
        const dvec2 b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            in = !in;
        }
    }
    return in;
}

double perimeter(const std::vector<dvec2>& polygon) {
    double length = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        length += glm::length(polygon[(i + 1) % polygon.size()] - polygon[i]);
    }
    return length;
}

} // namespace

TEST_CASE("problems: a framed work 7 mm wide compiles unframed, with one problem naming it",
          "[world][accuracy]") {
    // project.json accepts any positive size, but box_frame refuses a work no wider (or higher)
    // than twice the default frame's 5 mm rebate: its sight opening would be empty.
    auto data = skeleton_data();
    auto& form = work(data, chart_work_text).form;
    double width_m = 0.73;
    double height_m = 0.92;
    SECTION("7 mm wide") {
        width_m = 0.007;
    }
    SECTION("7 mm high") {
        height_m = 0.007;
    }
    form.width_m = width_m;
    form.height_m = height_m;
    const doc::Document document = accepted(data);

    World world;
    REQUIRE_NOTHROW(world = compile(document));
    check_well_formed(world);

    REQUIRE(world.problems.size() == 1);
    CHECK(world.problems.front().entity == id(chart_text));
    CHECK_FALSE(world.problems.front().reason.empty());

    // The work is still shown, at its true size, without its frame.
    CHECK(world.instances.size() == 2);
    CHECK(instances_of(world, id(chart_text), Part::frame).empty());
    const geom::Mesh expected =
        geom::painting_quad(skeleton_face_a(), {.width_m = width_m, .height_m = height_m},
                            {.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.04});
    const geom::Mesh actual = in_world(world, the_instance(world, id(chart_text), Part::painting));
    CHECK(same_corners(actual.positions, expected.positions, issue_tolerance_m));

    // Unframed, the whole work is visible.
    const Exhibit& exhibit = the_exhibit(world, id(chart_text));
    CHECK_FALSE(exhibit.frame_instance.has_value());
    CHECK_THAT(exhibit.visible.min_m,
               VecWithin(dvec2(-width_m / 2.0, -height_m / 2.0), tolerance_m));
    CHECK_THAT(exhibit.visible.max_m, VecWithin(dvec2(width_m / 2.0, height_m / 2.0), tolerance_m));

    // Everything else is as in the skeleton.
    CHECK(instances_of(world, id(gallery_text), Part::room).size() == 1);
    CHECK(world.lights.size() == 1);
    CHECK(world.cameras.size() == 1);
}

TEST_CASE("problems: a room whose outline is not an axis-aligned rectangle gets walls only, with "
          "one problem naming it",
          "[world][accuracy]") {
    // Until rooms of any shape get floors and ceilings (M2-01, M2-31), such a room is one quad per
    // side of its outline, from floor to ceiling, facing in (compile.hpp).
    // Each outline is made whole in one expression: assigning initializer lists to one vector in
    // several sections sets off a false -Wstringop-overflow in GCC 16's release builds.
    const auto outline_for = [](int shape) -> std::vector<dvec2> {
        switch (shape) {
        case 0:
            // The skeleton's 6 x 4 m room less its corner beyond x = 3, y = 2, anticlockwise.
            // Wall A and the chart on it stay on the outline's first side.
            return {{0.0, 0.0}, {6.0, 0.0}, {6.0, 2.0}, {3.0, 2.0}, {3.0, 4.0}, {0.0, 4.0}};
        case 1:
            return {{3.0, 0.0}, {5.0, 2.0}, {3.0, 4.0}, {1.0, 2.0}};
        default:
            return {{0.0, 0.0}, {6.0, 0.0}, {0.0, 4.0}};
        }
    };
    int shape = 0;
    SECTION("L-shaped") {
        shape = 0;
    }
    SECTION("a square turned 45 degrees") {
        shape = 1;
    }
    SECTION("a triangle") {
        shape = 2;
    }
    const std::vector<dvec2> outline = outline_for(shape);

    auto data = skeleton_data();
    auto& room = entity(data, gallery_text).room;
    REQUIRE(room.has_value());
    room->outline_m.clear();
    for (const auto& p : outline) {
        room->outline_m.push_back({.x = p.x, .y = p.y});
    }
    const doc::Document document = accepted(data);

    World world;
    REQUIRE_NOTHROW(world = compile(document));
    check_well_formed(world);

    REQUIRE(world.problems.size() == 1);
    CHECK(world.problems.front().entity == id(gallery_text));
    CHECK_FALSE(world.problems.front().reason.empty());

    const Instance& instance = the_instance(world, id(gallery_text), Part::room);
    const auto* lambert = std::get_if<LambertMaterial>(&material_of(world, instance));
    REQUIRE(lambert != nullptr);
    CHECK_THAT(lambert->albedo, VecWithin(dvec3(default_wall_albedo), 0.0));

    const geom::Mesh walls = in_world(world, instance);
    REQUIRE(walls.normals.size() == walls.positions.size());
    REQUIRE_FALSE(walls.indices.empty());

    // One quad, two triangles, per side.
    CHECK(walls.indices.size() == 6 * outline.size());

    // No floor, no ceiling: every normal is horizontal (to the unit tolerance), and the area is
    // the perimeter times the height, 3.5 m. For the L, 20 x 3.5 = 70 m^2.
    std::size_t not_horizontal = 0;
    for (const auto& n : walls.normals) {
        if (!(std::abs(n.z) <= unit_tolerance)) {
            ++not_horizontal;
        }
    }
    CHECK(not_horizontal == 0);
    CHECK_THAT(geom::area(walls), WithinRel(perimeter(outline) * 3.5, 1e-9));

    const Box box = bounds(walls);
    CHECK_THAT(box.min.z, WithinAbs(0.0, tolerance_m));
    CHECK_THAT(box.max.z, WithinAbs(3.5, tolerance_m));

    // Facing in: a centimetre along each triangle's normal from its centre is inside the outline,
    // a centimetre against it is outside.
    std::size_t facing_out = 0;
    for (std::size_t t = 0; t + 2 < walls.indices.size(); t += 3) {
        const dvec3 a = walls.positions[walls.indices[t]];
        const dvec3 b = walls.positions[walls.indices[t + 1]];
        const dvec3 c = walls.positions[walls.indices[t + 2]];
        const dvec3 centre = (a + b + c) / 3.0;
        const dvec3 n = walls.normals[walls.indices[t]];
        const dvec2 in_front = dvec2(centre + 0.01 * n);
        const dvec2 behind = dvec2(centre - 0.01 * n);
        if (!inside(outline, in_front) || inside(outline, behind)) {
            ++facing_out;
        }
        // And the triangle winds counter-clockwise seen from that side (geom/mesh.hpp).
        CHECK(glm::dot(glm::cross(b - a, c - a), n) > 0.0);
    }
    CHECK(facing_out == 0);
}

TEST_CASE("problems: a luminaire aimed at its own position is left out, with one problem naming "
          "it",
          "[world]") {
    // The loader does not know where a placement puts a spot, so it accepts an aim point at the
    // spot's own position; light::Luminaire refuses it, having no direction to point in. In the
    // gallery's ceiling (z = 3.5) at (2, 2), the position is exactly (2, 2, 3.5), whatever the
    // order of computation.
    auto data = skeleton_data();
    auto& spot = entity(data, spot_text);
    spot.placement = doc::OnCeiling{.room = uuid(gallery_text), .position_m = {2.0, 2.0}};
    spot.luminaire->aim = doc::AimAtPoint{.point_m = {.x = 2.0, .y = 2.0, .z = 3.5}};
    const doc::Document document = accepted(data);

    World world;
    REQUIRE_NOTHROW(world = compile(document));
    check_well_formed(world);

    CHECK(world.lights.empty());
    REQUIRE(world.problems.size() == 1);
    CHECK(world.problems.front().entity == id(spot_text));
    CHECK_FALSE(world.problems.front().reason.empty());

    // The rest of the scene is untouched.
    CHECK(world.instances.size() == 3);
    CHECK(world.exhibits.size() == 1);
    CHECK(world.cameras.size() == 1);
}

TEST_CASE("problems: a frame colour brighter than white is shown white, with one problem",
          "[world]") {
    // The document allows any colour of at least 0 in each channel; a Lambertian surface reflects
    // at most all the light it receives, and the engine's BRDF requires an albedo in [0, 1].
    auto data = skeleton_data();
    entity(data, chart_text).framing->colour = doc::LinearRgb{.r = 2.0, .g = 0.5, .b = 1.0};
    World world;
    REQUIRE_NOTHROW(world = compile(accepted(data)));
    check_well_formed(world);

    REQUIRE(world.problems.size() == 1);
    CHECK(world.problems.front().entity == id(chart_text));
    const Instance& frame = the_instance(world, id(chart_text), Part::frame);
    const auto* lambert = std::get_if<LambertMaterial>(&material_of(world, frame));
    REQUIRE(lambert != nullptr);
    CHECK_THAT(lambert->albedo, VecWithin(dvec3(1.0, 0.5, 1.0), 0.0));
}

TEST_CASE("problems: are ordered by entity id, whatever kind of entity they come from", "[world]") {
    // A second spot whose id sorts before the gallery's, aimed at its own position, and the
    // gallery made L-shaped: one problem each, the spot's first.
    constexpr std::string_view early_spot_text = "01a1cd0d-dc00-7000-8000-000000000000";
    auto data = skeleton_data();
    doc::Entity early_spot = entity(data, spot_text);
    early_spot.placement = doc::OnCeiling{.room = uuid(gallery_text), .position_m = {2.0, 2.0}};
    early_spot.luminaire->aim = doc::AimAtPoint{.point_m = {.x = 2.0, .y = 2.0, .z = 3.5}};
    data.entities.emplace(uuid(early_spot_text), early_spot);
    entity(data, gallery_text).room->outline_m = {{0.0, 0.0}, {6.0, 0.0}, {6.0, 2.0},
                                                  {3.0, 2.0}, {3.0, 4.0}, {0.0, 4.0}};
    World world;
    REQUIRE_NOTHROW(world = compile(accepted(data)));

    REQUIRE(world.problems.size() == 2);
    CHECK(world.problems[0].entity == id(early_spot_text));
    CHECK(world.problems[1].entity == id(gallery_text));
}
