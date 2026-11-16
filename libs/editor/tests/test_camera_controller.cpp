// SPDX-License-Identifier: Apache-2.0

// The orbit and walk camera controllers (#70), driven by input values without a window: the
// numbers of ADR 0006's Views and the issue's acceptance criteria.

#include <cs/editor/camera_controller.hpp>

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using namespace cs::editor;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using glm::dvec2;
using glm::dvec3;

namespace doc = cs::doc;
namespace world = cs::world;

namespace {

constexpr double pi = std::numbers::pi;
constexpr double deg = pi / 180.0;

// A camera at `position_m` looking along (yaw, pitch), as world::compile makes a viewpoint's.
world::Camera camera_at(dvec3 position_m, double yaw_rad, double pitch_rad,
                        double vertical_fov_rad = default_vertical_fov_rad) {
    world::Camera camera;
    camera.position_m = position_m;
    camera.forward = {std::cos(pitch_rad) * std::cos(yaw_rad),
                      std::cos(pitch_rad) * std::sin(yaw_rad), std::sin(pitch_rad)};
    camera.up = {-std::sin(pitch_rad) * std::cos(yaw_rad), -std::sin(pitch_rad) * std::sin(yaw_rad),
                 std::cos(pitch_rad)};
    camera.vertical_fov_rad = vertical_fov_rad;
    return camera;
}

// M1-27's default new room: 6 m by 4 m in plan, its floor at z = 0.
WalkRoom rectangle_room() {
    return {.outline_m = {{0.0, 0.0}, {6.0, 0.0}, {6.0, 4.0}, {0.0, 4.0}}, .floor_z_m = 0.0};
}

// An L: 6 m by 2 m along x, and 2 m by 4 m up the left side, run clockwise; its inner corner,
// at (2, 2), juts into the room.
WalkRoom l_room() {
    return {.outline_m = {{0.0, 0.0}, {0.0, 4.0}, {2.0, 4.0}, {2.0, 2.0}, {6.0, 2.0}, {6.0, 0.0}},
            .floor_z_m = 0.0};
}

double distance_to_segment(dvec2 p, dvec2 a, dvec2 b) {
    const dvec2 ab = b - a;
    const double t = std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0, 1.0);
    return glm::length(p - (a + t * ab));
}

double distance_to_walls(dvec2 p, const WalkRoom& room) {
    double nearest = std::numeric_limits<double>::infinity();
    const auto& outline = room.outline_m;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        nearest = std::min(nearest,
                           distance_to_segment(p, outline[i], outline[(i + 1) % outline.size()]));
    }
    return nearest;
}

// Even-odd: whether a ray from p along +x crosses the outline an odd number of times.
bool inside(dvec2 p, const WalkRoom& room) {
    bool in = false;
    const auto& outline = room.outline_m;
    for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
        const dvec2 a = outline[i];
        const dvec2 b = outline[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y)) {
            in = !in;
        }
    }
    return in;
}

// The angle between two directions, accurate for small angles.
double angle_between(dvec3 a, dvec3 b) {
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

// A camera's frame is what the renderers expect of World's: unit forward and up, perpendicular,
// and the image's right (forward x up) level, since neither controller rolls.
void check_frame(const world::Camera& camera) {
    CHECK(camera.projection == world::Projection::perspective);
    CHECK_THAT(glm::length(camera.forward), WithinAbs(1.0, 1e-12));
    CHECK_THAT(glm::length(camera.up), WithinAbs(1.0, 1e-12));
    CHECK_THAT(glm::dot(camera.forward, camera.up), WithinAbs(0.0, 1e-12));
    CHECK_THAT(glm::cross(camera.forward, camera.up).z, WithinAbs(0.0, 1e-12));
    CHECK(camera.up.z > 0.0);
}

CameraInput hold_forward(double elapsed_s) {
    CameraInput input;
    input.forward = true;
    input.elapsed_s = elapsed_s;
    return input;
}

CameraInput drag(dvec2 delta_px) {
    CameraInput input;
    input.mouse_delta_px = delta_px;
    return input;
}

CameraInput wheel(double notches) {
    CameraInput input;
    input.wheel_notches = notches;
    return input;
}

} // namespace

// ---- Input to a camera -------------------------------------------------------------------------

TEST_CASE("camera controller: an orbit gives a camera at its distance from the target, looking "
          "at it, with its field of view",
          "[editor]") {
    const dvec3 target{3.0, 2.0, 1.5};
    const OrbitController orbit(target, 4.0, 30.0 * deg, -20.0 * deg, 0.7);
    const world::Camera camera = orbit.camera();

    check_frame(camera);
    CHECK(camera.vertical_fov_rad == 0.7);
    CHECK_THAT(glm::length(camera.position_m - target), WithinAbs(4.0, 1e-12));
    CHECK_THAT(angle_between(camera.forward, target - camera.position_m), WithinAbs(0.0, 1e-12));
    // Yaw 30 degrees from +x towards +y, looking 20 degrees down.
    CHECK_THAT(camera.forward.z, WithinAbs(std::sin(-20.0 * deg), 1e-12));
    CHECK_THAT(std::atan2(camera.forward.y, camera.forward.x), WithinAbs(30.0 * deg, 1e-12));
}

TEST_CASE("camera controller: a walk gives a camera at the eye, looking where it started looking, "
          "with the starting camera's field of view",
          "[editor]") {
    const WalkController walk(rectangle_room(), doc::ProjectSettings{},
                              camera_at({3.0, 2.0, 5.0}, 120.0 * deg, 10.0 * deg, 0.9));
    const world::Camera camera = walk.camera();

    check_frame(camera);
    CHECK(camera.vertical_fov_rad == 0.9);
    CHECK(camera.position_m == walk.eye_m());
    CHECK_THAT(angle_between(camera.forward, camera_at({}, 120.0 * deg, 10.0 * deg).forward),
               WithinAbs(0.0, 1e-12));
}

TEST_CASE("camera controller: moving the pointer right and down turns both views right and down",
          "[editor]") {
    OrbitController orbit({0.0, 0.0, 1.0}, 3.0, 0.0, 0.0);
    WalkController walk(rectangle_room(), doc::ProjectSettings{},
                        camera_at({3.0, 2.0, 0.0}, 0.0, 0.0));
    const dvec3 orbit_before = orbit.camera().forward;
    const dvec3 walk_before = walk.camera().forward;

    orbit.update(drag({20.0, 0.0}));
    walk.update(drag({20.0, 0.0}));
    // Right of a view along +x, with z up, is -y.
    CHECK(orbit.camera().forward.y < 0.0);
    CHECK(walk.camera().forward.y < 0.0);
    CHECK_THAT(orbit.yaw_rad(), WithinAbs(-20.0 * turn_rad_per_px, 1e-12));
    CHECK_THAT(walk.yaw_rad(), WithinAbs(-20.0 * turn_rad_per_px, 1e-12));

    orbit.update(drag({0.0, 30.0}));
    walk.update(drag({0.0, 30.0}));
    CHECK(orbit.camera().forward.z < orbit_before.z);
    CHECK(walk.camera().forward.z < walk_before.z);
    CHECK_THAT(orbit.pitch_rad(), WithinAbs(-30.0 * turn_rad_per_px, 1e-12));
    CHECK_THAT(walk.pitch_rad(), WithinAbs(-30.0 * turn_rad_per_px, 1e-12));
}

TEST_CASE("camera controller: input that is not finite is ignored", "[editor]") {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    OrbitController orbit({1.0, 1.0, 1.0}, 3.0, 0.5, 0.2);
    WalkController walk(rectangle_room(), doc::ProjectSettings{},
                        camera_at({3.0, 2.0, 0.0}, 0.5, 0.2));
    const world::Camera orbit_before = orbit.camera();
    const world::Camera walk_before = walk.camera();

    CameraInput input;
    input.mouse_delta_px = {nan, inf};
    input.wheel_notches = inf;
    input.forward = true;
    input.elapsed_s = nan;
    orbit.update(input);
    walk.update(input);
    input.elapsed_s = inf;
    walk.update(input);

    CHECK(orbit.camera() == orbit_before);
    CHECK(walk.camera() == walk_before);
}

// ---- Walk --------------------------------------------------------------------------------------

TEST_CASE("camera controller: the walking eye is at the project's eye height above the floor",
          "[editor]") {
    // Wherever the starting camera is in height.
    const doc::ProjectSettings defaults;
    REQUIRE(defaults.eye_height_m == 1.60);
    const WalkController walk(rectangle_room(), defaults, camera_at({3.0, 2.0, 7.0}, 0.0, 0.0));
    CHECK_THAT(walk.eye_m().z, WithinAbs(1.60, 1e-12));
    CHECK_THAT(walk.eye_m().x, WithinAbs(3.0, 1e-12));
    CHECK_THAT(walk.eye_m().y, WithinAbs(2.0, 1e-12));

    WalkRoom raised = rectangle_room();
    raised.floor_z_m = 0.45;
    doc::ProjectSettings tall;
    tall.eye_height_m = 1.75;
    const WalkController walk_raised(raised, tall, camera_at({3.0, 2.0, 0.0}, 0.0, 0.0));
    CHECK_THAT(walk_raised.eye_m().z, WithinAbs(2.20, 1e-12));

    // Walking and looking down do not change it.
    WalkController moving(rectangle_room(), defaults, camera_at({3.0, 2.0, 0.0}, 0.0, -40.0 * deg));
    moving.update(hold_forward(0.5));
    CHECK_THAT(moving.eye_m().z, WithinAbs(1.60, 1e-12));
}

TEST_CASE("camera controller: holding forward for 1.0 s walks 1.4 m within 1 mm", "[editor]") {
    // Along the view's heading on the floor, however far up or down it looks.
    const double pitch_rad = GENERATE(0.0, 30.0 * deg, -60.0 * deg);
    const double yaw_rad = 0.4;
    const dvec2 heading{std::cos(yaw_rad), std::sin(yaw_rad)};
    const dvec3 start{1.0, 1.0, 0.0};

    SECTION("in one update") {
        WalkController walk(rectangle_room(), doc::ProjectSettings{},
                            camera_at(start, yaw_rad, pitch_rad));
        walk.update(hold_forward(1.0));
        const dvec2 moved = dvec2(walk.eye_m()) - dvec2(start);
        CHECK_THAT(glm::length(moved), WithinAbs(1.4, 1e-3));
        CHECK_THAT(glm::dot(moved, heading), WithinAbs(1.4, 1e-3));
    }
    SECTION("in 60 frames") {
        WalkController walk(rectangle_room(), doc::ProjectSettings{},
                            camera_at(start, yaw_rad, pitch_rad));
        for (int frame = 0; frame < 60; ++frame) {
            walk.update(hold_forward(1.0 / 60.0));
        }
        const dvec2 moved = dvec2(walk.eye_m()) - dvec2(start);
        CHECK_THAT(glm::length(moved), WithinAbs(1.4, 1e-3));
        CHECK_THAT(glm::dot(moved, heading), WithinAbs(1.4, 1e-3));
    }
}

TEST_CASE("camera controller: back, left and right walk the other ways, and diagonally no faster",
          "[editor]") {
    const dvec3 start{3.0, 2.0, 0.0};
    const auto walked = [&](bool forward, bool back, bool left, bool right) {
        WalkController walk(rectangle_room(), doc::ProjectSettings{}, camera_at(start, 0.0, 0.0));
        CameraInput input;
        input.forward = forward;
        input.back = back;
        input.left = left;
        input.right = right;
        input.elapsed_s = 0.5;
        walk.update(input);
        return dvec2(walk.eye_m()) - dvec2(start);
    };
    // Looking along +x: left is +y.
    const dvec2 back = walked(false, true, false, false);
    const dvec2 left = walked(false, false, true, false);
    const dvec2 right = walked(false, false, false, true);
    const dvec2 diagonal = walked(true, false, true, false);
    const dvec2 nowhere = walked(true, true, false, false);
    CHECK_THAT(back.x, WithinAbs(-0.7, 1e-9));
    CHECK_THAT(back.y, WithinAbs(0.0, 1e-9));
    CHECK_THAT(left.y, WithinAbs(0.7, 1e-9));
    CHECK_THAT(left.x, WithinAbs(0.0, 1e-9));
    CHECK_THAT(right.y, WithinAbs(-0.7, 1e-9));
    CHECK_THAT(glm::length(diagonal), WithinAbs(0.7, 1e-9));
    CHECK_THAT(diagonal.x, WithinAbs(diagonal.y, 1e-9));
    CHECK(glm::length(nowhere) == 0.0);
}

TEST_CASE("camera controller: an update longer than a second walks only a second", "[editor]") {
    WalkController walk(rectangle_room(), doc::ProjectSettings{},
                        camera_at({0.5, 2.0, 0.0}, 0.0, 0.0));
    walk.update(hold_forward(30.0));
    CHECK_THAT(walk.eye_m().x, WithinAbs(0.5 + WalkController::walk_speed_m_per_s, 1e-9));
}

TEST_CASE("camera controller: walking pitch is clamped to plus or minus 85 degrees", "[editor]") {
    REQUIRE_THAT(max_pitch_rad, WithinAbs(85.0 * deg, 1e-15));
    WalkController walk(rectangle_room(), doc::ProjectSettings{},
                        camera_at({3.0, 2.0, 0.0}, 0.0, 0.0));

    walk.update(drag({0.0, -100000.0})); // far up
    CHECK_THAT(walk.pitch_rad(), WithinAbs(85.0 * deg, 1e-12));
    CHECK_THAT(walk.camera().forward.z, WithinAbs(std::sin(85.0 * deg), 1e-12));
    check_frame(walk.camera());

    walk.update(drag({0.0, 100000.0})); // far down
    CHECK_THAT(walk.pitch_rad(), WithinAbs(-85.0 * deg, 1e-12));
    CHECK_THAT(walk.camera().forward.z, WithinAbs(-std::sin(85.0 * deg), 1e-12));
    check_frame(walk.camera());

    // A starting camera that looks straight down keeps its heading, from its up.
    const WalkController down(rectangle_room(), doc::ProjectSettings{},
                              camera_at({3.0, 2.0, 0.0}, 1.0, -90.0 * deg));
    CHECK_THAT(down.pitch_rad(), WithinAbs(-85.0 * deg, 1e-12));
    CHECK_THAT(down.yaw_rad(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("camera controller: the walking eye stays 0.25 m from the room's walls", "[editor]") {
    const WalkRoom room = rectangle_room();
    REQUIRE(WalkController::wall_clearance_m == 0.25);

    SECTION("walking straight into a wall stops 0.25 m from it") {
        WalkController walk(room, doc::ProjectSettings{}, camera_at({3.0, 2.0, 0.0}, 0.0, 0.0));
        for (int frame = 0; frame < 300; ++frame) {
            walk.update(hold_forward(1.0 / 60.0));
        }
        CHECK_THAT(walk.eye_m().x, WithinAbs(6.0 - 0.25, 1e-9));
        CHECK_THAT(walk.eye_m().y, WithinAbs(2.0, 1e-9));
    }
    SECTION("walking into a wall at an angle slides along it, into the corner") {
        WalkController walk(room, doc::ProjectSettings{},
                            camera_at({3.0, 2.0, 0.0}, 20.0 * deg, 0.0));
        for (int frame = 0; frame < 600; ++frame) {
            walk.update(hold_forward(1.0 / 60.0));
            REQUIRE(distance_to_walls(dvec2(walk.eye_m()), room) >= 0.25 - 1e-9);
        }
        CHECK_THAT(walk.eye_m().x, WithinAbs(6.0 - 0.25, 1e-9));
        CHECK_THAT(walk.eye_m().y, WithinAbs(4.0 - 0.25, 1e-9));
    }
    SECTION("one long update does not pass through a wall") {
        WalkController walk(room, doc::ProjectSettings{}, camera_at({5.0, 2.0, 0.0}, 0.0, 0.0));
        walk.update(hold_forward(1.0));
        CHECK_THAT(walk.eye_m().x, WithinAbs(6.0 - 0.25, 1e-9));
    }
    SECTION("a camera outside the room, or too near a wall, stands at the nearest clear place") {
        const WalkController outside(room, doc::ProjectSettings{},
                                     camera_at({9.0, 1.0, 3.0}, pi, 0.0));
        CHECK_THAT(outside.eye_m().x, WithinAbs(6.0 - 0.25, 1e-9));
        CHECK_THAT(outside.eye_m().y, WithinAbs(1.0, 1e-9));

        const WalkController beyond_corner(room, doc::ProjectSettings{},
                                           camera_at({-2.0, -3.0, 3.0}, 0.5, 0.0));
        CHECK_THAT(beyond_corner.eye_m().x, WithinAbs(0.25, 1e-9));
        CHECK_THAT(beyond_corner.eye_m().y, WithinAbs(0.25, 1e-9));

        const WalkController near_wall(room, doc::ProjectSettings{},
                                       camera_at({3.0, 3.9, 1.6}, 0.0, 0.0));
        CHECK_THAT(near_wall.eye_m().x, WithinAbs(3.0, 1e-9));
        CHECK_THAT(near_wall.eye_m().y, WithinAbs(4.0 - 0.25, 1e-9));
    }
}

TEST_CASE("camera controller: in an L-shaped room the eye stays inside and clear of every wall, "
          "round the inner corner",
          "[editor]") {
    const WalkRoom room = l_room();
    WalkController walk(room, doc::ProjectSettings{}, camera_at({5.0, 1.0, 0.0}, pi, 0.0));

    // Walk west along the bottom arm, turn north up the left arm, then wander round the bend and
    // its inner corner with the keys and the pointer together.
    const auto walk_for = [&](CameraInput input, int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            walk.update(input);
            const dvec2 eye{walk.eye_m()};
            REQUIRE(inside(eye, room));
            REQUIRE(distance_to_walls(eye, room) >= 0.25 - 1e-9);
        }
    };
    CameraInput input = hold_forward(1.0 / 30.0);
    walk_for(input, 120);
    CHECK(walk.eye_m().x < 2.0);

    // A quarter turn right, from west to north, over 30 frames.
    input.mouse_delta_px = {90.0 * deg / turn_rad_per_px / 30.0, 0.0};
    walk_for(input, 30);
    input.mouse_delta_px = {};
    walk_for(input, 120);
    CHECK(walk.eye_m().y > 2.0);

    input = hold_forward(1.0 / 30.0);
    input.right = true;
    input.mouse_delta_px = {3.0, 0.0};
    walk_for(input, 300);
    input.forward = false;
    input.back = true;
    input.mouse_delta_px = {-7.0, 0.0};
    walk_for(input, 300);
}

TEST_CASE("camera controller: one long update does not step through a thin wall into another part "
          "of the room",
          "[editor]") {
    // A U: two arms either side of a notch 0.3 m wide, from y = 1 m to the top. A second's walk
    // east from 0.75 m short of the notch would land 0.35 m past it, clear of every wall.
    const WalkRoom room{.outline_m = {{0.0, 0.0},
                                      {5.0, 0.0},
                                      {5.0, 4.0},
                                      {2.65, 4.0},
                                      {2.65, 1.0},
                                      {2.35, 1.0},
                                      {2.35, 4.0},
                                      {0.0, 4.0}},
                        .floor_z_m = 0.0};
    WalkController walk(room, doc::ProjectSettings{}, camera_at({1.6, 3.0, 0.0}, 0.0, 0.0));
    walk.update(hold_forward(1.0));
    CHECK_THAT(walk.eye_m().x, WithinAbs(2.35 - 0.25, 1e-9));
    CHECK_THAT(walk.eye_m().y, WithinAbs(3.0, 1e-9));

    // Standing clear in the other arm, behind the notch's far side, the eye stays where it is.
    WalkController other_arm(room, doc::ProjectSettings{},
                             camera_at({3.0, 2.0, 0.0}, pi / 2.0, 0.0));
    CHECK_THAT(other_arm.eye_m().x, WithinAbs(3.0, 1e-12));
    other_arm.update(hold_forward(0.5));
    CHECK_THAT(other_arm.eye_m().x, WithinAbs(3.0, 1e-9));
    CHECK_THAT(other_arm.eye_m().y, WithinAbs(2.7, 1e-9));
}

TEST_CASE("camera controller: an outline that encloses no area has no walls", "[editor]") {
    WalkRoom room;
    room.floor_z_m = 0.2;
    WalkController no_room(room, doc::ProjectSettings{}, camera_at({30.0, -4.0, 0.0}, 0.0, 0.0));
    no_room.update(hold_forward(1.0));
    CHECK_THAT(no_room.eye_m().x, WithinAbs(31.4, 1e-9));
    CHECK_THAT(no_room.eye_m().z, WithinAbs(1.80, 1e-12));

    room.outline_m = {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}};
    WalkController flat(room, doc::ProjectSettings{}, camera_at({30.0, -4.0, 0.0}, 0.0, 0.0));
    flat.update(hold_forward(1.0));
    CHECK_THAT(flat.eye_m().x, WithinAbs(31.4, 1e-9));
}

TEST_CASE("camera controller: a document's room becomes the walk's", "[editor]") {
    doc::Room room;
    room.outline_m = {{0.0, 0.0}, {6.0, 0.0}, {6.0, 4.0}, {0.0, 4.0}};
    room.floor_height_m = 0.3;
    room.ceiling_height_m = 3.8;
    const WalkRoom walk = walk_room(room);
    CHECK(walk.outline_m == rectangle_room().outline_m);
    CHECK(walk.floor_z_m == 0.3);
}

// ---- Orbit -------------------------------------------------------------------------------------

TEST_CASE("camera controller: dragging orbits the target at a constant distance within 1e-6",
          "[editor]") {
    const dvec3 target{3.0, 2.0, 1.5};
    OrbitController orbit(target, 5.0, 0.3, 0.2);
    const double yaw_before = orbit.yaw_rad();
    const double pitch_before = orbit.pitch_rad();

    // Thousands of small drags, turning round many times and against the pitch limits.
    for (int step = 0; step < 4000; ++step) {
        const double t = step * 0.01;
        orbit.update(drag({37.0 * std::sin(1.3 * t) + 5.0, 23.0 * std::cos(0.7 * t)}));
        const world::Camera camera = orbit.camera();
        REQUIRE_THAT(glm::length(camera.position_m - target), WithinAbs(5.0, 1e-6));
        REQUIRE_THAT(angle_between(camera.forward, target - camera.position_m),
                     WithinAbs(0.0, 1e-6));
        REQUIRE(std::abs(orbit.pitch_rad()) <= max_pitch_rad);
    }
    CHECK(orbit.target_m() == target);

    // One drag turns by turn_rad_per_px a pixel, in both yaw and pitch.
    OrbitController fresh(target, 5.0, 0.3, 0.2);
    fresh.update(drag({-40.0, -10.0}));
    CHECK_THAT(fresh.yaw_rad() - yaw_before, WithinAbs(40.0 * turn_rad_per_px, 1e-12));
    CHECK_THAT(fresh.pitch_rad() - pitch_before, WithinAbs(10.0 * turn_rad_per_px, 1e-12));
    CHECK_THAT(glm::length(fresh.camera().position_m - target), WithinAbs(5.0, 1e-6));
}

TEST_CASE("camera controller: orbit pitch stays within plus or minus 85 degrees", "[editor]") {
    OrbitController orbit({0.0, 0.0, 0.0}, 2.0, 0.0, 0.0);
    orbit.update(drag({0.0, 100000.0}));
    CHECK_THAT(orbit.pitch_rad(), WithinAbs(-85.0 * deg, 1e-12));
    orbit.update(drag({0.0, -100000.0}));
    CHECK_THAT(orbit.pitch_rad(), WithinAbs(85.0 * deg, 1e-12));
    check_frame(orbit.camera());

    const OrbitController steep({0.0, 0.0, 0.0}, 2.0, 0.0, 1.5);
    CHECK_THAT(steep.pitch_rad(), WithinAbs(85.0 * deg, 1e-12));
}

TEST_CASE("camera controller: each wheel notch zooms by 10%, down to 0.3 m", "[editor]") {
    const dvec3 target{3.0, 2.0, 1.5};
    OrbitController orbit(target, 4.0, 0.3, 0.2);
    const dvec3 direction = orbit.camera().forward;

    orbit.update(wheel(1.0));
    CHECK_THAT(orbit.distance_m(), WithinRel(3.6, 1e-12));
    orbit.update(wheel(2.0));
    CHECK_THAT(orbit.distance_m(), WithinRel(4.0 * 0.9 * 0.9 * 0.9, 1e-12));
    // Zooming out undoes it, a notch at a time; half notches go half the way, geometrically.
    orbit.update(wheel(-3.0));
    CHECK_THAT(orbit.distance_m(), WithinRel(4.0, 1e-12));
    orbit.update(wheel(0.5));
    orbit.update(wheel(0.5));
    CHECK_THAT(orbit.distance_m(), WithinRel(3.6, 1e-12));
    // The view's direction and target do not change.
    CHECK_THAT(angle_between(orbit.camera().forward, direction), WithinAbs(0.0, 1e-12));
    CHECK(orbit.target_m() == target);
    CHECK_THAT(glm::length(orbit.camera().position_m - target), WithinRel(3.6, 1e-12));

    // Zooming in stops at 0.3 m.
    for (int notch = 0; notch < 100; ++notch) {
        orbit.update(wheel(1.0));
    }
    CHECK(orbit.distance_m() == OrbitController::min_distance_m);
    CHECK(OrbitController::min_distance_m == 0.3);
    CHECK_THAT(glm::length(orbit.camera().position_m - target), WithinAbs(0.3, 1e-12));

    const OrbitController too_near(target, 0.1, 0.0, 0.0);
    CHECK(too_near.distance_m() == 0.3);
}

TEST_CASE("camera controller: an orbit made from a camera keeps its position and view",
          "[editor]") {
    const world::Camera camera = camera_at({1.0, 2.0, 1.6}, 2.0, -0.3, 0.8);
    const OrbitController orbit(camera, 3.0);
    CHECK_THAT(glm::length(orbit.camera().position_m - camera.position_m), WithinAbs(0.0, 1e-12));
    CHECK_THAT(angle_between(orbit.camera().forward, camera.forward), WithinAbs(0.0, 1e-12));
    CHECK_THAT(glm::length(orbit.target_m() - (camera.position_m + 3.0 * camera.forward)),
               WithinAbs(0.0, 1e-12));
    CHECK(orbit.camera().vertical_fov_rad == 0.8);
}

// ---- Switching ---------------------------------------------------------------------------------

TEST_CASE("camera controller: switching from orbit to walk keeps the view direction", "[editor]") {
    const WalkRoom room = rectangle_room();
    // From inside the room and from outside it, looking down and up, at the pitch limit too.
    const double pitch_rad = GENERATE(-85.0 * deg, -35.0 * deg, 0.0, 20.0 * deg, 85.0 * deg);
    const double yaw_rad = GENERATE(-2.5, 0.0, 1.0, 3.1);
    const double distance_m = GENERATE(1.0, 12.0);
    const OrbitController orbit({3.0, 2.0, 1.2}, distance_m, yaw_rad, pitch_rad, 0.75);
    const world::Camera before = orbit.camera();

    const WalkController walk(room, doc::ProjectSettings{}, before);
    const world::Camera after = walk.camera();
    CHECK_THAT(angle_between(after.forward, before.forward), WithinAbs(0.0, 1e-12));
    CHECK_THAT(angle_between(after.up, before.up), WithinAbs(0.0, 1e-12));
    CHECK(after.vertical_fov_rad == before.vertical_fov_rad);
    CHECK_THAT(after.position_m.z, WithinAbs(1.60, 1e-12));
    const dvec2 eye{after.position_m};
    CHECK(inside(eye, room));
    CHECK(distance_to_walls(eye, room) >= 0.25 - 1e-9);

    // And back to orbit, which keeps it again.
    const OrbitController again(after, 3.0);
    CHECK_THAT(angle_between(again.camera().forward, before.forward), WithinAbs(0.0, 1e-12));
}
