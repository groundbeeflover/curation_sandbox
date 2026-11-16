// SPDX-License-Identifier: Apache-2.0

#pragma once

// Camera control without a toolkit (ADR 0006, Views): orbiting a target, and walking at eye
// height inside a room. A controller takes what the user did since its last update, as plain
// values, and gives a world::Camera, so that it is tested without a window and the UI (M1-27)
// only translates its events.
//
// Conventions: world coordinates in metres, z up. A view's direction is a yaw about +z (0 looks
// along +x, pi/2 along +y, as doc::Viewpoint's) and a pitch above the horizontal, within
// max_pitch_rad. Moving the pointer right turns the view right, and down turns it down, in both
// controllers: walking, as looking round; orbiting, as the camera travels round the target.

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/world/world.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <numbers>
#include <vector>

namespace cs::editor {

// What the user did since the last update. Values that are not finite are ignored.
struct CameraInput {
    // How far the pointer moved, in logical pixels, x right and y down. The UI passes it while a
    // button is held in orbit, and while the cursor is captured in walk; zero otherwise.
    glm::dvec2 mouse_delta_px{0.0};
    // Wheel notches turned (Qt's angleDelta / 120), > 0 away from the user: zooming in. Fine
    // wheels and touchpads give fractions.
    double wheel_notches = 0.0;
    // The movement keys (W, S, A, D) held through the elapsed time.
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    double elapsed_s = 0.0; // since the last update
};

// How far the view turns per pixel the pointer moves: about 0.29 degrees. [C]
inline constexpr double turn_rad_per_px = 0.005;
// How far up or down a view may look, so that it never looks straight up or down, where its yaw
// would be lost. Orbit shares walk's limit, so that switching between them keeps the view.
inline constexpr double max_pitch_rad = 85.0 * std::numbers::pi / 180.0;
// The vertical field of view of a controller that does not start from a camera: the skeleton
// viewpoint's 50 degrees. [C]
inline constexpr double default_vertical_fov_rad = 50.0 * std::numbers::pi / 180.0;

// Orbiting a target at a distance: dragging turns the camera round it, the wheel zooms.
class OrbitController {
  public:
    static constexpr double zoom_per_notch = 0.9; // each notch in: 10% nearer
    static constexpr double min_distance_m = 0.3;
    static constexpr double max_distance_m = 1000.0; // [C]

    // Looking at `target_m` from `distance_m` away, in the direction (yaw_rad, pitch_rad), all
    // finite; the distance and pitch are clamped to their limits, and a field of view outside
    // (0, pi) becomes default_vertical_fov_rad.
    OrbitController(glm::dvec3 target_m, double distance_m, double yaw_rad, double pitch_rad,
                    double vertical_fov_rad = default_vertical_fov_rad);

    // Orbiting the point `distance_m` ahead of `camera` (finite, with up perpendicular to forward,
    // as World's are), from where it is, with its field of view if it has one. A camera that
    // looks steeper than max_pitch_rad moves round the point to that pitch.
    OrbitController(const world::Camera& camera, double distance_m);

    // Turns with the pointer and zooms with the wheel; the movement keys do nothing.
    void update(const CameraInput& input);

    world::Camera camera() const;

    glm::dvec3 target_m() const { return target_m_; }
    double distance_m() const { return distance_m_; }
    double yaw_rad() const { return yaw_rad_; }
    double pitch_rad() const { return pitch_rad_; }

  private:
    glm::dvec3 target_m_;
    double distance_m_;
    double yaw_rad_;
    double pitch_rad_;
    double vertical_fov_rad_;
};

// The room a walk stays inside: its outline in plan, of any shape and either way round, and the
// height of its floor. An outline that encloses no area has no walls.
struct WalkRoom {
    std::vector<glm::dvec2> outline_m;
    double floor_z_m = 0.0;
};

WalkRoom walk_room(const doc::Room& room);

// Walking at eye height: the pointer looks round, the keys walk along the floor at
// walk_speed_m_per_s, and the eye stays inside the room, wall_clearance_m from its walls, sliding
// along a wall it walks into. Where the room is narrower than twice that, the eye keeps as clear
// as it can, and it does not walk into such a gap. Free walls and openings are not walls here
// until M2-14.
class WalkController {
  public:
    static constexpr double walk_speed_m_per_s = 1.4;
    static constexpr double wall_clearance_m = 0.25;
    // An update longer than this (a stall, a debugger) walks only this long. [C]
    static constexpr double max_elapsed_s = 1.0;

    // Standing where `camera` is in plan, with the eye at the settings' eye height above the
    // floor, looking where `camera` looks (finite, with up perpendicular to forward, as World's
    // are) with its field of view if it has one. A camera nearer a wall than wall_clearance_m
    // stands that far from it, where the room is wide enough; one outside the room stands that
    // far in from the nearest point of a wall where that is inside.
    WalkController(const WalkRoom& room, const doc::ProjectSettings& settings,
                   const world::Camera& camera);

    // Looks round with the pointer and walks with the keys; the wheel does nothing.
    void update(const CameraInput& input);

    world::Camera camera() const;

    glm::dvec3 eye_m() const;
    double yaw_rad() const { return yaw_rad_; }
    double pitch_rad() const { return pitch_rad_; }

  private:
    // One side of the room's outline, in plan.
    struct Wall {
        glm::dvec2 start_m;
        glm::dvec2 along_m; // from its start to its end, not zero
        glm::dvec2 inward;  // unit, perpendicular to it, into the room
        glm::dvec2 nearest_to(glm::dvec2 p) const;
    };

    glm::dvec2 keep_clear(glm::dvec2 p) const;
    bool inside(glm::dvec2 p) const;

    std::vector<Wall> walls_; // none: nothing stops a walk
    double eye_z_m_;
    glm::dvec2 position_m_; // the eye, in plan
    double yaw_rad_ = 0.0;
    double pitch_rad_ = 0.0;
    double vertical_fov_rad_;
};

} // namespace cs::editor
