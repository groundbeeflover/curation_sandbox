// SPDX-License-Identifier: Apache-2.0

// What each kind of entity becomes in the render world (compile.hpp), beyond the skeleton scene:
// rooms elsewhere and listed either way round, exhibits unframed, re-framed, deep or without an
// image, luminaires dimmed, aimed at points and other entities and placed on walls, ceilings and
// bent tracks, and viewpoints that look up. Each document is the skeleton's, changed in code, and
// passes the loader's checks (world_checks.hpp, accepted()).

#include "world_checks.hpp"

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/work.hpp>
#include <cs/geom/box_frame.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/geom/painting.hpp>
#include <cs/geom/wall_face.hpp>
#include <cs/light/distribution.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <utility>
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

// The skeleton spot's position: 2.465 m along the track from (1, 1.097) at z = 3.5.
const dvec3 spot_position(3.465, 1.097, 3.5);

// A document holding only a room with this outline, floor and ceiling.
doc::Document room_only(std::vector<doc::Vec2> outline, double floor_m, double ceiling_m) {
    doc::DocumentData data;
    doc::Entity room;
    room.name = "Room";
    room.room = doc::Room{.outline_m = std::move(outline),
                          .floor_height_m = floor_m,
                          .ceiling_height_m = ceiling_m,
                          .floor_finish = std::nullopt,
                          .ceiling_finish = std::nullopt};
    data.entities.emplace(uuid(gallery_text), room);
    return accepted(data);
}

doc::Luminaire& spot_of(doc::DocumentData& data) {
    auto& spot = entity(data, spot_text);
    REQUIRE(spot.luminaire.has_value());
    return *spot.luminaire;
}

} // namespace

// ---- Rooms -----------------------------------------------------------------------------------

TEST_CASE("compile rooms: a rectangular room anywhere, at any floor height, is a closed box "
          "facing in",
          "[world][accuracy]") {
    // A room 6 m along x and 4 m along y from (2, 1), its floor at 0.5 m and its ceiling at 3.0 m:
    // 2.5 m high. Listed anticlockwise, clockwise, or from another corner, its outline is the same
    // axis-aligned rectangle, so it is geom::room_mesh's box (compile.hpp), facing into the room
    // whichever way the outline runs.
    std::vector<doc::Vec2> outline;
    SECTION("anticlockwise") {
        outline = {{2.0, 1.0}, {8.0, 1.0}, {8.0, 5.0}, {2.0, 5.0}};
    }
    SECTION("anticlockwise, from another corner") {
        outline = {{8.0, 5.0}, {2.0, 5.0}, {2.0, 1.0}, {8.0, 1.0}};
    }
    SECTION("clockwise") {
        outline = {{2.0, 1.0}, {2.0, 5.0}, {8.0, 5.0}, {8.0, 1.0}};
    }

    const World world = compile(room_only(outline, 0.5, 3.0));
    check_well_formed(world);
    CHECK(world.problems.empty());
    REQUIRE(world.instances.size() == 1);
    const Instance& room = the_instance(world, id(gallery_text), Part::room);
    const geom::Mesh actual = in_world(world, room);

    const Box box = bounds(actual);
    CHECK_THAT(box.min, VecWithin(dvec3(2.0, 1.0, 0.5), tolerance_m));
    CHECK_THAT(box.max, VecWithin(dvec3(8.0, 5.0, 3.0), tolerance_m));

    // Floor and ceiling 2 x 6 x 4 = 48, walls 2 x 6 x 2.5 = 30 and 2 x 4 x 2.5 = 20: 98 m^2.
    // Facing in, the closed mesh's volume is negative: -6 x 4 x 2.5 = -60 m^3.
    CHECK(geom::is_closed(actual));
    CHECK_THAT(geom::area(actual), WithinRel(98.0, 1e-9));
    CHECK_THAT(geom::volume(actual), WithinRel(-60.0, 1e-9));

    const auto* lambert = std::get_if<LambertMaterial>(&material_of(world, room));
    REQUIRE(lambert != nullptr);
    CHECK_THAT(lambert->albedo, VecWithin(dvec3(0.8), 0.0));

    // A document with no luminaires and no viewpoints has no lights and no cameras from them.
    CHECK(world.lights.empty());
    CHECK(world.exhibits.empty());
}

TEST_CASE("compile rooms: an empty document compiles to an empty world", "[world]") {
    const World world = compile(doc::Document{});
    CHECK(world.instances.empty());
    CHECK(world.exhibits.empty());
    CHECK(world.lights.empty());
    CHECK(world.problems.empty());
}

// ---- Exhibits --------------------------------------------------------------------------------

TEST_CASE("compile exhibits: an unframed work has no frame instance and the whole work visible",
          "[world][accuracy]") {
    auto data = skeleton_data();
    entity(data, chart_text).framing.reset();
    const World world = compile(accepted(data));
    check_well_formed(world);
    CHECK(world.problems.empty());

    CHECK(world.instances.size() == 2);
    CHECK(instances_of(world, id(chart_text), Part::frame).empty());
    CHECK(instances_of(world, id(chart_text), Part::painting).size() == 1);

    // The owner's first comment: the whole work, 0.73 x 0.92 m, centred on the face centre.
    const Exhibit& exhibit = the_exhibit(world, id(chart_text));
    CHECK_FALSE(exhibit.frame_instance.has_value());
    CHECK_THAT(exhibit.visible.min_m, VecWithin(dvec2(-0.365, -0.46), tolerance_m));
    CHECK_THAT(exhibit.visible.max_m, VecWithin(dvec2(0.365, 0.46), tolerance_m));
    CHECK_THAT(exhibit.face_centre_m, VecWithin(dvec3(3.0, 0.04, 1.5), tolerance_m));
}

TEST_CASE("compile exhibits: a framing's width and colour replace the default frame's",
          "[world][accuracy]") {
    auto data = skeleton_data();
    auto& framing = entity(data, chart_text).framing;
    REQUIRE(framing.has_value());
    framing->width_m = 0.08;
    framing->colour = doc::LinearRgb{.r = 0.2, .g = 0.3, .b = 0.4};
    const World world = compile(accepted(data));
    check_well_formed(world);
    CHECK(world.problems.empty());

    const Instance& frame = the_instance(world, id(chart_text), Part::frame);
    const auto* lambert = std::get_if<LambertMaterial>(&material_of(world, frame));
    REQUIRE(lambert != nullptr);
    CHECK_THAT(lambert->albedo, VecWithin(dvec3(0.2, 0.3, 0.4), 0.0));

    // Members 8 cm wide around the 0.72 x 0.91 m sight opening: outside 0.88 x 1.07 m, centred on
    // (3, 1.5) in x and z; the default depth, 4 cm, from the stand-off at y = 0.04.
    const Box box = bounds(in_world(world, frame));
    CHECK_THAT(box.min, VecWithin(dvec3(2.56, 0.04, 0.965), tolerance_m));
    CHECK_THAT(box.max, VecWithin(dvec3(3.44, 0.08, 2.035), tolerance_m));

    // The rebate, and so the visible area, is the default frame's.
    const Exhibit& exhibit = the_exhibit(world, id(chart_text));
    CHECK_THAT(exhibit.visible.min_m, VecWithin(dvec2(-0.36, -0.455), issue_tolerance_m));
    CHECK_THAT(exhibit.visible.max_m, VecWithin(dvec2(0.36, 0.455), issue_tolerance_m));
}

TEST_CASE("compile exhibits: a deep work's face stands out by its depth, and the spot aims at it",
          "[world][accuracy]") {
    // A canvas on a 3 cm stretcher: its back at the 4 cm stand-off, its face at y = 0.07.
    auto data = skeleton_data();
    work(data, chart_work_text).form.depth_m = 0.03;
    const World world = compile(accepted(data));
    check_well_formed(world);

    const geom::Mesh expected =
        geom::painting_quad(skeleton_face_a(), {.width_m = 0.73, .height_m = 0.92, .depth_m = 0.03},
                            {.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.04});
    const geom::Mesh actual = in_world(world, the_instance(world, id(chart_text), Part::painting));
    CHECK(same_corners(actual.positions, expected.positions, issue_tolerance_m));

    const dvec3 face_centre(3.0, 0.07, 1.5);
    const Exhibit& exhibit = the_exhibit(world, id(chart_text));
    CHECK_THAT(exhibit.face_centre_m, VecWithin(face_centre, tolerance_m));

    const Light& light = the_light(world, id(spot_text));
    CHECK_THAT(light.luminaire.axis(),
               VecWithin(glm::normalize(face_centre - spot_position), unit_tolerance));
}

TEST_CASE("compile exhibits: a work without an image is a placeholder with no image", "[world]") {
    auto data = skeleton_data();
    work(data, chart_work_text).image.reset();
    const World world = compile(accepted(data));
    check_well_formed(world);
    CHECK(world.problems.empty());

    const Instance& painting = the_instance(world, id(chart_text), Part::painting);
    const auto* surface = std::get_if<PaintingSurface>(&material_of(world, painting));
    REQUIRE(surface != nullptr);
    CHECK_FALSE(surface->image.has_value());
}

// ---- Luminaires ------------------------------------------------------------------------------

TEST_CASE("compile luminaires: dimming scales the spot's lumens", "[world][accuracy]") {
    auto data = skeleton_data();
    spot_of(data).dimming = 0.5;
    const World world = compile(accepted(data));
    check_well_formed(world);

    // The fixture type's 800 lm times 0.5, with its beam and field angles: the peak of a
    // GenericSpot scales with its lumens. Tolerances as in the skeleton test.
    const Light& light = the_light(world, id(spot_text));
    REQUIRE(light.spot != nullptr);
    CHECK_THAT(light.spot->total_lumens(), WithinRel(400.0, 1e-12));
    const cs::light::GenericSpot reference(400.0, 15.0 * degrees, 25.0 * degrees);
    CHECK_THAT(light.spot->peak_intensity_cd(), WithinRel(reference.peak_intensity_cd(), 1e-9));
    CHECK_THAT(light.luminaire.intensity_cd(light.luminaire.axis()),
               WithinRel(reference.peak_intensity_cd(), 1e-9));
}

TEST_CASE("compile luminaires: one dimmed to 0 is left out without a problem", "[world]") {
    auto data = skeleton_data();
    spot_of(data).dimming = 0.0;
    const World world = compile(accepted(data));
    check_well_formed(world);
    CHECK(world.lights.empty());
    CHECK(world.problems.empty());
}

TEST_CASE("compile luminaires: a colour temperature overrides the fixture type's", "[world]") {
    auto data = skeleton_data();
    spot_of(data).colour_temperature_k = 4000.0;
    const World world = compile(accepted(data));
    CHECK(the_light(world, id(spot_text)).colour_temperature_k == 4000.0);
}

TEST_CASE("compile luminaires: aimed at a point, a viewpoint or a lux sensor", "[world]") {
    auto data = skeleton_data();
    dvec3 target(0.0);
    SECTION("a point") {
        spot_of(data).aim = doc::AimAtPoint{.point_m = {.x = 3.0, .y = 0.0, .z = 0.0}};
        target = dvec3(3.0, 0.0, 0.0);
    }
    SECTION("a viewpoint: its position") {
        spot_of(data).aim = doc::AimAtEntity{.target = uuid(viewpoint_text)};
        target = dvec3(3.0, 3.0, 1.6);
    }
    SECTION("a lux sensor: its position") {
        const auto sensor_id = uuid("01a1cd0d-dc00-7000-8000-00000000000b");
        doc::Entity sensor;
        sensor.name = "Sensor";
        sensor.lux_sensor = doc::LuxSensor{.position_m = {.x = 1.0, .y = 2.0, .z = 0.8},
                                           .normal = {.x = 0.0, .y = 0.0, .z = 1.0}};
        data.entities.emplace(sensor_id, sensor);
        spot_of(data).aim = doc::AimAtEntity{.target = sensor_id};
        target = dvec3(1.0, 2.0, 0.8);
    }
    const World world = compile(accepted(data));
    check_well_formed(world);
    CHECK(world.problems.empty());

    const Light& light = the_light(world, id(spot_text));
    CHECK_THAT(light.luminaire.position_m(), VecWithin(spot_position, tolerance_m));
    CHECK_THAT(light.luminaire.axis(),
               VecWithin(glm::normalize(target - spot_position), unit_tolerance));
}

TEST_CASE("compile luminaires: aimed at another luminaire, its position", "[world]") {
    // A second spot in the ceiling at (2, 2), aimed straight down, and the skeleton's spot aimed
    // at it.
    auto data = skeleton_data();
    const auto second_id = uuid("01a1cd0d-dc00-7000-8000-00000000000b");
    doc::Entity second;
    second.name = "Spot 2";
    second.placement = doc::OnCeiling{.room = uuid(gallery_text), .position_m = {2.0, 2.0}};
    second.luminaire = doc::Luminaire{.fixture_type = "generic-spot-800",
                                      .dimming = 1.0,
                                      .aim = doc::AimAtPoint{.point_m = {2.0, 2.0, 0.0}},
                                      .colour_temperature_k = std::nullopt};
    data.entities.emplace(second_id, second);
    spot_of(data).aim = doc::AimAtEntity{.target = second_id};
    const World world = compile(accepted(data));
    check_well_formed(world);

    const dvec3 target(2.0, 2.0, 3.5);
    CHECK_THAT(the_light(world, id(spot_text)).luminaire.axis(),
               VecWithin(glm::normalize(target - spot_position), unit_tolerance));
}

TEST_CASE("compile luminaires: in a ceiling, at the room's ceiling height", "[world]") {
    // Recessed in the gallery's ceiling (z = 3.5) at (2, 2) in plan, aimed straight down.
    auto data = skeleton_data();
    auto& spot = entity(data, spot_text);
    spot.placement = doc::OnCeiling{.room = uuid(gallery_text), .position_m = {2.0, 2.0}};
    spot.luminaire->aim = doc::AimAtPoint{.point_m = {2.0, 2.0, 0.0}};
    const World world = compile(accepted(data));
    check_well_formed(world);
    CHECK(world.problems.empty());

    const Light& light = the_light(world, id(spot_text));
    CHECK_THAT(light.luminaire.position_m(), VecWithin(dvec3(2.0, 2.0, 3.5), tolerance_m));
    CHECK_THAT(light.luminaire.axis(), VecWithin(dvec3(0.0, 0.0, -1.0), unit_tolerance));
}

TEST_CASE("compile luminaires: on a wall, at the placement's point on the face", "[world]") {
    // On face A of wall A, 1 m from its left end (x = 6, so x = 5), 3 m high, 0.1 m out from the
    // face at y = 0: where on_wall_transform puts the origin of a hung thing (geom/wall_face.hpp).
    auto data = skeleton_data();
    auto& spot = entity(data, spot_text);
    spot.placement = doc::OnWall{.wall = uuid(wall_a_text),
                                 .face = doc::WallFace::a,
                                 .distance_m = 1.0,
                                 .centre_height_m = 3.0,
                                 .stand_off_m = 0.1};
    const World world = compile(accepted(data));
    check_well_formed(world);
    CHECK(world.problems.empty());

    const dvec3 position(5.0, 0.1, 3.0);
    const Light& light = the_light(world, id(spot_text));
    CHECK_THAT(light.luminaire.position_m(), VecWithin(position, tolerance_m));
    CHECK_THAT(light.luminaire.axis(),
               VecWithin(glm::normalize(dvec3(3.0, 0.04, 1.5) - position), unit_tolerance));
}

TEST_CASE("compile luminaires: along a bent track, the distance is measured along the polyline",
          "[world]") {
    // The track from (1, 1) to (3, 1), then to (3, 3), at z = 3.2: 3 m along it is 1 m past the
    // corner, at (3, 2).
    auto data = skeleton_data();
    auto& track = entity(data, track_text).track;
    REQUIRE(track.has_value());
    track->points_m = {{1.0, 1.0}, {3.0, 1.0}, {3.0, 3.0}};
    track->height_m = 3.2;
    std::get<doc::OnTrack>(*entity(data, spot_text).placement).distance_m = 3.0;
    const World world = compile(accepted(data));
    check_well_formed(world);

    CHECK_THAT(the_light(world, id(spot_text)).luminaire.position_m(),
               VecWithin(dvec3(3.0, 2.0, 3.2), tolerance_m));
}

// ---- Viewpoints ------------------------------------------------------------------------------

TEST_CASE("compile viewpoints: yaw and pitch give the direction of view, up stays upright",
          "[world]") {
    auto data = skeleton_data();
    auto& viewpoint = entity(data, viewpoint_text).viewpoint;
    REQUIRE(viewpoint.has_value());
    viewpoint->yaw_rad = 0.0;
    viewpoint->pitch_rad = 30.0 * degrees;
    viewpoint->field_of_view_rad = 40.0 * degrees;
    viewpoint->ev100.reset();
    const World world = compile(accepted(data));
    REQUIRE(world.cameras.size() == 1);
    const Camera& camera = world.cameras.front();

    // Yaw 0 looks along +x; pitched 30 degrees up, forward is (cos 30, 0, sin 30). Up is the
    // world's up made perpendicular to it, with no roll: (-sin 30, 0, cos 30).
    const double c = std::cos(30.0 * degrees);
    const double s = std::sin(30.0 * degrees);
    CHECK(camera.projection == Projection::perspective);
    CHECK_THAT(camera.forward, VecWithin(dvec3(c, 0.0, s), unit_tolerance));
    CHECK_THAT(camera.up, VecWithin(dvec3(-s, 0.0, c), unit_tolerance));
    CHECK_THAT(camera.vertical_fov_rad, WithinAbs(40.0 * degrees, 1e-12));
    // Absent: automatic exposure.
    CHECK_FALSE(camera.ev100.has_value());
}

TEST_CASE("compile viewpoints: yaw pi/2 looks along +y", "[world]") {
    auto data = skeleton_data();
    entity(data, viewpoint_text).viewpoint->yaw_rad = std::numbers::pi / 2.0;
    const World world = compile(accepted(data));
    REQUIRE(world.cameras.size() == 1);
    CHECK_THAT(world.cameras.front().forward, VecWithin(dvec3(0.0, 1.0, 0.0), unit_tolerance));
    CHECK_THAT(world.cameras.front().up, VecWithin(dvec3(0.0, 0.0, 1.0), unit_tolerance));
}
