// SPDX-License-Identifier: Apache-2.0

// The skeleton scene (tests/scenes/skeleton/) compiled into a render world (#61): its room, its
// framed test chart, its spot and its viewpoint become three instances, one exhibit, one light and
// one camera, with the numbers the scene's README gives.

#include "world_checks.hpp"

#include <cs/geom/box_frame.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/geom/painting.hpp>
#include <cs/geom/wall_face.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

using namespace cs::world;
using namespace cs::world::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using glm::dvec2;
using glm::dvec3;

namespace geom = cs::geom;

namespace {

// The chart as geom builds it: 0.73 m wide, 0.92 m high, a print with no depth, centred at
// u = 3.0 m and 1.5 m high on face A of wall A, 0.04 m out from it.
constexpr geom::FlatWork chart_work{.width_m = 0.73, .height_m = 0.92, .depth_m = 0.0};
constexpr geom::WallPlacement chart_at{.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.04};

// The default box frame around the chart (compile.hpp's default_frame: the skeleton's framing sets
// no width or colour).
constexpr geom::BoxFrame chart_frame{.work_width_m = 0.73,
                                     .work_height_m = 0.92,
                                     .member_width_m = 0.06,
                                     .depth_m = 0.04,
                                     .rebate_m = 0.005};

dvec3 centroid(const std::vector<dvec3>& points) {
    dvec3 sum(0.0);
    for (const auto& p : points) {
        sum += p;
    }
    return sum / static_cast<double>(points.size());
}

} // namespace

TEST_CASE("skeleton world: compiles with three instances, one exhibit, light and camera, and no "
          "problems",
          "[world]") {
    const World world = compile(skeleton());
    check_well_formed(world);

    CHECK(world.instances.size() == 3);
    CHECK(instances_of(world, id(gallery_text), Part::room).size() == 1);
    CHECK(instances_of(world, id(chart_text), Part::painting).size() == 1);
    CHECK(instances_of(world, id(chart_text), Part::frame).size() == 1);
    CHECK(world.exhibits.size() == 1);
    CHECK(world.lights.size() == 1);
    CHECK(world.cameras.size() == 1);
    CHECK(world.problems.empty());
}

TEST_CASE("skeleton world: the spot's peak intensity is GenericSpot(800 lm, 15 deg, 25 deg)'s",
          "[world][accuracy]") {
    const World world = compile(skeleton());
    REQUIRE(world.lights.size() == 1);
    const Light& light = world.lights.front();
    CHECK(light.entity == id(spot_text));
    REQUIRE(light.spot != nullptr);

    // The fixture type of the scene: 800 lm, beam 15 degrees and field 25 degrees (full angles),
    // at dimming 1. The document stores the angles as the doubles nearest 15 and 25 degrees, which
    // may differ from 15 * pi / 180 by an ulp; the peak changes smoothly with the angles, so 1e-9
    // relative covers that and any order of computation, and still catches a wrong lumen count or
    // a beam and field swapped (both change the peak by percent).
    const cs::light::GenericSpot reference(800.0, 15.0 * degrees, 25.0 * degrees);
    const double peak_cd = reference.peak_intensity_cd();

    CHECK_THAT(light.spot->peak_intensity_cd(), WithinRel(peak_cd, 1e-9));
    CHECK_THAT(light.spot->total_lumens(), WithinRel(800.0, 1e-12));
    CHECK_THAT(light.spot->beam_angle_rad(), WithinAbs(15.0 * degrees, 1e-12));
    CHECK_THAT(light.spot->field_angle_rad(), WithinAbs(25.0 * degrees, 1e-12));

    // The luminaire evaluates the same distribution: along its own axis, the peak. Off the axis by
    // 1e-16 rad the profile 2^-((theta / half_beam)^p), p = 2.35, differs from 1 by far less than
    // 1e-9.
    CHECK_THAT(light.luminaire.intensity_cd(light.luminaire.axis()), WithinRel(peak_cd, 1e-9));
    CHECK_THAT(light.luminaire.distribution().total_lumens(), WithinRel(800.0, 1e-12));

    CHECK(light.colour_temperature_k == 3000.0);
}

TEST_CASE("skeleton world: the spot sits on the track and aims at the centre of the chart's face",
          "[world][accuracy]") {
    const World world = compile(skeleton());
    REQUIRE(world.lights.size() == 1);
    const auto& luminaire = world.lights.front().luminaire;

    // The track runs from (1, 1.097) to (5, 1.097) at z = 3.5; the spot is 2.465 m along it.
    const dvec3 position(3.465, 1.097, 3.5);
    CHECK_THAT(luminaire.position_m(), VecWithin(position, tolerance_m));

    // It aims at the exhibit: the centre of the chart's face, at u = 3 on face A of wall A (x = 3),
    // 1.5 m high, 0.04 m out from the face at y = 0, the work having no depth.
    const dvec3 face_centre(3.0, 0.04, 1.5);
    const dvec3 axis = glm::normalize(face_centre - position);
    CHECK_THAT(luminaire.axis(), VecWithin(axis, unit_tolerance));

    // The README's check: the aim drops 2.0 m over sqrt(1.057^2 + 0.465^2) = 1.15476 m
    // horizontally, atan(1.15476 / 2.0) = 30.0013 degrees from vertical. The spot is placed to the
    // millimetre, so the angle is the conservation guideline's 30 degrees to within 0.005 degrees.
    const double from_vertical_rad = std::acos(glm::dot(luminaire.axis(), dvec3(0.0, 0.0, -1.0)));
    CHECK_THAT(from_vertical_rad, WithinAbs(30.0 * degrees, 0.005 * degrees));
}

TEST_CASE("skeleton world: the painting's world corners equal geom::painting_quad's within 0.1 mm",
          "[world][accuracy]") {
    const World world = compile(skeleton());
    const Instance& painting = the_instance(world, id(chart_text), Part::painting);
    const geom::Mesh actual = in_world(world, painting);

    const geom::Mesh expected = geom::painting_quad(skeleton_face_a(), chart_work, chart_at);
    REQUIRE(expected.positions.size() == 4);
    REQUIRE(actual.positions.size() == 4);
    CHECK(actual.indices.size() == 6);

    // The issue's tolerance: 0.1 mm.
    CHECK(same_corners(actual.positions, expected.positions, issue_tolerance_m));

    // Each corner carries painting_quad's image UV (top-left (0, 0), bottom-right (1, 1)), so the
    // photograph reads unmirrored from the front; and the face's normal, +y into the room.
    for (std::size_t e = 0; e < expected.positions.size(); ++e) {
        for (std::size_t a = 0; a < actual.positions.size(); ++a) {
            if (near(actual.positions[a], expected.positions[e], issue_tolerance_m)) {
                INFO("corner " << e);
                CHECK_THAT(actual.uvs[a], VecWithin(expected.uvs[e], tolerance_m));
                CHECK_THAT(actual.normals[a], VecWithin(dvec3(0.0, 1.0, 0.0), unit_tolerance));
            }
        }
    }
}

TEST_CASE("skeleton world: the frame is the default box frame hung like the chart",
          "[world][accuracy]") {
    const World world = compile(skeleton());
    const Instance& frame = the_instance(world, id(chart_text), Part::frame);
    const geom::Mesh actual = in_world(world, frame);

    geom::Mesh expected = geom::box_frame(chart_frame);
    const auto hang = geom::on_wall_transform(skeleton_face_a(), chart_at);
    for (auto& p : expected.positions) {
        p = point_to_world(hang, p);
    }
    CHECK(actual.positions.size() == expected.positions.size());
    CHECK(same_corners(actual.positions, expected.positions, issue_tolerance_m));
    CHECK_THAT(geom::area(actual), WithinRel(geom::area(expected), 1e-9));

    // Analytically: the sight opening is 0.72 x 0.91 m (the work less 2 x 5 mm), the outside
    // 0.84 x 1.03 m (plus 2 x 6 cm), centred on (3, 1.5) in x and z; the frame's back is at the
    // stand-off, y = 0.04, and its front 4 cm further out.
    const Box box = bounds(actual);
    CHECK_THAT(box.min, VecWithin(dvec3(2.58, 0.04, 0.985), tolerance_m));
    CHECK_THAT(box.max, VecWithin(dvec3(3.42, 0.08, 2.015), tolerance_m));

    // Dark stained wood: Lambert albedo 0.1 (default_frame).
    const auto* lambert = std::get_if<LambertMaterial>(&material_of(world, frame));
    REQUIRE(lambert != nullptr);
    CHECK_THAT(lambert->albedo, VecWithin(dvec3(default_frame.albedo), 0.0));
}

TEST_CASE("skeleton world: the room is a closed box 6 x 4 x 3.5 m, Lambert 0.8, facing in",
          "[world][accuracy]") {
    const World world = compile(skeleton());
    const Instance& room = the_instance(world, id(gallery_text), Part::room);
    const geom::Mesh actual = in_world(world, room);

    const Box box = bounds(actual);
    CHECK_THAT(box.min, VecWithin(dvec3(0.0, 0.0, 0.0), tolerance_m));
    CHECK_THAT(box.max, VecWithin(dvec3(6.0, 4.0, 3.5), tolerance_m));

    // Floor and ceiling 2 x 6 x 4 = 48, walls along x 2 x 6 x 3.5 = 42, along y 2 x 4 x 3.5 = 28:
    // 118 m^2. Closed and facing in: volume -6 x 4 x 3.5 = -84 m^3 (geom/room.hpp). Sums of a few
    // dozen products of order 10, so 1e-9 relative is far above rounding.
    CHECK(geom::is_closed(actual));
    CHECK_THAT(geom::area(actual), WithinRel(118.0, 1e-9));
    CHECK_THAT(geom::volume(actual), WithinRel(-84.0, 1e-9));

    const auto* lambert = std::get_if<LambertMaterial>(&material_of(world, room));
    REQUIRE(lambert != nullptr);
    CHECK_THAT(lambert->albedo, VecWithin(dvec3(default_wall_albedo), 0.0));
    CHECK(default_wall_albedo == 0.8);
}

TEST_CASE("skeleton world: the painting's material names the chart's image by hash and extension",
          "[world]") {
    const World world = compile(skeleton());
    const Instance& painting = the_instance(world, id(chart_text), Part::painting);
    const auto* surface = std::get_if<PaintingSurface>(&material_of(world, painting));
    REQUIRE(surface != nullptr);
    REQUIRE(surface->image.has_value());
    CHECK(surface->image->sha256 == chart_sha256);
    CHECK(surface->image->extension == "png");
}

TEST_CASE("skeleton world: the viewpoint becomes a perspective camera at eye height looking at "
          "wall A",
          "[world]") {
    const World world = compile(skeleton());
    REQUIRE(world.cameras.size() == 1);
    const Camera& camera = world.cameras.front();

    REQUIRE(camera.entity.has_value());
    CHECK(*camera.entity == id(viewpoint_text));
    CHECK(camera.projection == Projection::perspective);
    CHECK_THAT(camera.position_m, VecWithin(dvec3(3.0, 3.0, 1.6), tolerance_m));

    // Yaw -pi/2 from +x and pitch 0: looking along -y, level, with the image's up the world's up.
    // cos(-pi/2) in doubles is 6e-17, well inside the unit tolerance.
    CHECK_THAT(camera.forward, VecWithin(dvec3(0.0, -1.0, 0.0), unit_tolerance));
    CHECK_THAT(camera.up, VecWithin(dvec3(0.0, 0.0, 1.0), unit_tolerance));
    CHECK_THAT(camera.vertical_fov_rad, WithinAbs(50.0 * degrees, 1e-12));
    REQUIRE(camera.ev100.has_value());
    CHECK(*camera.ev100 == 8.0);
}

TEST_CASE("skeleton world: the exhibit's face frame is the chart's face, unit and right-handed, "
          "facing into the room",
          "[world][accuracy]") {
    const World world = compile(skeleton());
    const Exhibit& exhibit = the_exhibit(world, id(chart_text));

    CHECK(exhibit.work == id(chart_work_text));
    CHECK_THAT(exhibit.work_size_m, VecWithin(dvec2(0.73, 0.92), tolerance_m));

    // Face A of wall A faces +y; seen from the room, right is -x and up is +z (geom/wall_face.hpp).
    CHECK_THAT(exhibit.face_centre_m, VecWithin(dvec3(3.0, 0.04, 1.5), tolerance_m));
    CHECK_THAT(exhibit.face_u, VecWithin(dvec3(-1.0, 0.0, 0.0), unit_tolerance));
    CHECK_THAT(exhibit.face_v, VecWithin(dvec3(0.0, 0.0, 1.0), unit_tolerance));
    CHECK_THAT(exhibit.face_normal, VecWithin(dvec3(0.0, 1.0, 0.0), unit_tolerance));

    CHECK_THAT(glm::length(exhibit.face_u), WithinAbs(1.0, unit_tolerance));
    CHECK_THAT(glm::length(exhibit.face_v), WithinAbs(1.0, unit_tolerance));
    CHECK_THAT(glm::length(exhibit.face_normal), WithinAbs(1.0, unit_tolerance));
    CHECK_THAT(glm::dot(exhibit.face_u, exhibit.face_v), WithinAbs(0.0, unit_tolerance));
    CHECK_THAT(glm::cross(exhibit.face_u, exhibit.face_v),
               VecWithin(exhibit.face_normal, unit_tolerance));

    // face_point follows the frame: one metre right and one up from the centre.
    CHECK_THAT(exhibit.face_point({1.0, 1.0}), VecWithin(dvec3(2.0, 0.04, 2.5), tolerance_m));
}

TEST_CASE("skeleton world: the exhibit's visible area is 0.72 x 0.91 m centred on the painting "
          "quad's centre",
          "[world][accuracy]") {
    const World world = compile(skeleton());
    const Exhibit& exhibit = the_exhibit(world, id(chart_text));

    // The owner's first comment: the 0.73 x 0.92 m work less the 5 mm rebate on each side, which
    // is geom::sight_opening of the default frame; within the issue's 0.1 mm.
    const dvec2 opening = geom::sight_opening(chart_frame);
    CHECK_THAT(opening, VecWithin(dvec2(0.72, 0.91), tolerance_m));
    CHECK_THAT(exhibit.visible.max_m - exhibit.visible.min_m,
               VecWithin(dvec2(0.72, 0.91), issue_tolerance_m));
    CHECK_THAT(exhibit.visible.min_m, VecWithin(dvec2(-0.36, -0.455), issue_tolerance_m));
    CHECK_THAT(exhibit.visible.max_m, VecWithin(dvec2(0.36, 0.455), issue_tolerance_m));

    // Its centre, in the world, is the centre of the painting quad.
    const geom::Mesh quad = geom::painting_quad(skeleton_face_a(), chart_work, chart_at);
    const dvec2 centre_uv = 0.5 * (exhibit.visible.min_m + exhibit.visible.max_m);
    CHECK_THAT(exhibit.face_point(centre_uv),
               VecWithin(centroid(quad.positions), issue_tolerance_m));

    // Its corners lie on the quad's plane and inside it.
    for (const dvec2 corner : {exhibit.visible.min_m, exhibit.visible.max_m}) {
        const dvec3 p = exhibit.face_point(corner);
        CHECK_THAT(p.y, WithinAbs(0.04, tolerance_m));
        CHECK(p.x >= 3.0 - 0.365 - tolerance_m);
        CHECK(p.x <= 3.0 + 0.365 + tolerance_m);
        CHECK(p.z >= 1.5 - 0.46 - tolerance_m);
        CHECK(p.z <= 1.5 + 0.46 + tolerance_m);
    }
}

TEST_CASE("skeleton world: the exhibit points at its painting and frame instances", "[world]") {
    const World world = compile(skeleton());
    const Exhibit& exhibit = the_exhibit(world, id(chart_text));

    REQUIRE(exhibit.painting_instance < world.instances.size());
    CHECK(world.instances[exhibit.painting_instance].part == Part::painting);
    REQUIRE(exhibit.frame_instance.has_value());
    REQUIRE(*exhibit.frame_instance < world.instances.size());
    CHECK(world.instances[*exhibit.frame_instance].part == Part::frame);
}
