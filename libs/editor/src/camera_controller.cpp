// SPDX-License-Identifier: Apache-2.0

#include <cs/editor/camera_controller.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <tuple>
#include <utility>

namespace cs::editor {

namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;

// The longest step a walk takes before keeping clear of the walls. Shorter than the clearance, so
// that a step from a clear place cannot reach a wall, let alone cross it. [C]
constexpr double max_step_m = 0.1;

double finite_or_zero(double value) {
    return std::isfinite(value) ? value : 0.0;
}

double clamp_pitch(double pitch_rad) {
    return std::clamp(pitch_rad, -max_pitch_rad, max_pitch_rad);
}

double clamp_distance(double distance_m) {
    return std::clamp(distance_m, OrbitController::min_distance_m, OrbitController::max_distance_m);
}

// Right turns right, which lowers the yaw with z up; down turns down.
void turn(double& yaw_rad, double& pitch_rad, glm::dvec2 delta_px) {
    yaw_rad = std::remainder(yaw_rad - turn_rad_per_px * finite_or_zero(delta_px.x), two_pi);
    pitch_rad = clamp_pitch(pitch_rad - turn_rad_per_px * finite_or_zero(delta_px.y));
}

// As world::compile makes a viewpoint's camera.
world::Camera camera_looking(glm::dvec3 position_m, double yaw_rad, double pitch_rad,
                             double vertical_fov_rad) {
    const double cos_pitch = std::cos(pitch_rad);
    const double sin_pitch = std::sin(pitch_rad);
    const double cos_yaw = std::cos(yaw_rad);
    const double sin_yaw = std::sin(yaw_rad);
    world::Camera camera;
    camera.projection = world::Projection::perspective;
    camera.position_m = position_m;
    camera.forward = {cos_pitch * cos_yaw, cos_pitch * sin_yaw, sin_pitch};
    camera.up = {-sin_pitch * cos_yaw, -sin_pitch * sin_yaw, cos_pitch};
    camera.vertical_fov_rad = vertical_fov_rad;
    return camera;
}

// A camera's view as (yaw, pitch), the pitch clamped. With forward and up as camera_looking makes
// them, up.z forward - forward.z up is (cos yaw, sin yaw, 0) and (forward.z, up.z) is
// (sin pitch, cos pitch): both stay defined for a camera looking straight up or down.
std::pair<double, double> view_angles(const world::Camera& camera) {
    const glm::dvec3 forward = camera.forward;
    const glm::dvec3 up = camera.up;
    const glm::dvec3 heading = up.z * forward - forward.z * up;
    return {std::atan2(heading.y, heading.x), clamp_pitch(std::atan2(forward.z, up.z))};
}

double field_of_view(const world::Camera& camera) {
    const double fov_rad = camera.vertical_fov_rad;
    const bool usable = camera.projection == world::Projection::perspective && fov_rad > 0.0 &&
                        fov_rad < std::numbers::pi;
    return usable ? fov_rad : default_vertical_fov_rad;
}

} // namespace

// ---- Orbit -------------------------------------------------------------------------------------

OrbitController::OrbitController(glm::dvec3 target_m, double distance_m, double yaw_rad,
                                 double pitch_rad, double vertical_fov_rad)
    : target_m_(target_m), distance_m_(clamp_distance(distance_m)),
      yaw_rad_(std::remainder(yaw_rad, two_pi)), pitch_rad_(clamp_pitch(pitch_rad)),
      vertical_fov_rad_(vertical_fov_rad) {}

OrbitController::OrbitController(const world::Camera& camera, double distance_m)
    : OrbitController(camera.position_m + clamp_distance(distance_m) * camera.forward, distance_m,
                      0.0, 0.0, field_of_view(camera)) {
    std::tie(yaw_rad_, pitch_rad_) = view_angles(camera);
}

void OrbitController::update(const CameraInput& input) {
    turn(yaw_rad_, pitch_rad_, input.mouse_delta_px);
    distance_m_ =
        clamp_distance(distance_m_ * std::pow(zoom_per_notch, finite_or_zero(input.wheel_notches)));
}

world::Camera OrbitController::camera() const {
    world::Camera camera = camera_looking({}, yaw_rad_, pitch_rad_, vertical_fov_rad_);
    camera.position_m = target_m_ - distance_m_ * camera.forward;
    return camera;
}

// ---- Walk --------------------------------------------------------------------------------------

WalkRoom walk_room(const doc::Room& room) {
    WalkRoom walk;
    walk.floor_z_m = room.floor_height_m;
    walk.outline_m.reserve(room.outline_m.size());
    for (const doc::Vec2& point : room.outline_m) {
        walk.outline_m.emplace_back(point.x, point.y);
    }
    return walk;
}

WalkController::WalkController(const WalkRoom& room, const doc::ProjectSettings& settings,
                               const world::Camera& camera)
    : eye_z_m_(room.floor_z_m + settings.eye_height_m), position_m_(camera.position_m),
      vertical_fov_rad_(field_of_view(camera)) {
    std::tie(yaw_rad_, pitch_rad_) = view_angles(camera);

    // Which way round the outline runs, from the sign of its area (the shoelace formula).
    const std::vector<glm::dvec2>& outline = room.outline_m;
    double twice_area = 0.0;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::dvec2 a = outline[i];
        const glm::dvec2 b = outline[(i + 1) % outline.size()];
        twice_area += a.x * b.y - b.x * a.y;
    }
    if (!(twice_area > 0.0 || twice_area < 0.0)) {
        return; // no area, or not finite: no walls
    }
    const double anticlockwise = twice_area > 0.0 ? 1.0 : -1.0;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::dvec2 along = outline[(i + 1) % outline.size()] - outline[i];
        const double length = glm::length(along);
        if (length > 0.0) {
            walls_.push_back(
                {outline[i], along, anticlockwise * glm::dvec2(-along.y, along.x) / length});
        }
    }

    if (inside(position_m_)) {
        position_m_ = keep_clear(position_m_);
        return;
    }
    // From outside the room: of the places wall_clearance_m in from each wall's nearest point and
    // clear of the other walls, the nearest that is inside. A sharp corner can leave the place
    // off one wall outside, behind the next.
    double nearest_m = std::numeric_limits<double>::infinity();
    const glm::dvec2 from = position_m_;
    for (const Wall& wall : walls_) {
        const glm::dvec2 place = keep_clear(wall.nearest_to(from) + wall_clearance_m * wall.inward);
        if (const double distance_m = glm::length(place - from);
            distance_m < nearest_m && inside(place)) {
            nearest_m = distance_m;
            position_m_ = place;
        }
    }
}

void WalkController::update(const CameraInput& input) {
    turn(yaw_rad_, pitch_rad_, input.mouse_delta_px);

    const glm::dvec2 heading{std::cos(yaw_rad_), std::sin(yaw_rad_)};
    const glm::dvec2 right{heading.y, -heading.x};
    const glm::dvec2 direction = (double(input.forward) - double(input.back)) * heading +
                                 (double(input.right) - double(input.left)) * right;
    const double elapsed_s = std::clamp(finite_or_zero(input.elapsed_s), 0.0, max_elapsed_s);
    if (direction == glm::dvec2(0.0) || elapsed_s == 0.0) {
        return;
    }
    const glm::dvec2 walk_m = glm::normalize(direction) * (walk_speed_m_per_s * elapsed_s);
    if (walls_.empty()) {
        position_m_ += walk_m;
        return;
    }
    // In steps short enough not to cross a wall, each pushed back off the walls it nears, which
    // slides the eye along a wall it walks into.
    const int steps = static_cast<int>(std::ceil(glm::length(walk_m) / max_step_m));
    for (int step = 0; step < steps; ++step) {
        const glm::dvec2 next = keep_clear(position_m_ + walk_m / double(steps));
        if (!inside(next) && inside(position_m_)) {
            break; // a gap narrower than twice the clearance: the eye stops before it
        }
        position_m_ = next;
    }
}

glm::dvec3 WalkController::eye_m() const {
    return {position_m_.x, position_m_.y, eye_z_m_};
}

world::Camera WalkController::camera() const {
    return camera_looking(eye_m(), yaw_rad_, pitch_rad_, vertical_fov_rad_);
}

glm::dvec2 WalkController::Wall::nearest_to(glm::dvec2 p) const {
    const double t = glm::dot(p - start_m, along_m) / glm::dot(along_m, along_m);
    return start_m + std::clamp(t, 0.0, 1.0) * along_m;
}

// Pushes p out to wall_clearance_m from every wall nearer than that, straight away from the
// wall's nearest point, which rounds the inside of a corner that juts into the room; a point on a
// wall goes in along its normal. It moves p only by what it lacks, so p must be in front of the
// walls near it, as a short step from a clear place is. A push off one wall can bring p nearer
// the next at a corner, so it goes round a few times.
glm::dvec2 WalkController::keep_clear(glm::dvec2 p) const {
    for (int pass = 0; pass < 4; ++pass) {
        for (const Wall& wall : walls_) {
            const glm::dvec2 nearest = wall.nearest_to(p);
            const glm::dvec2 away = p - nearest;
            const double distance_m = glm::length(away);
            if (distance_m < wall_clearance_m) {
                p = nearest +
                    wall_clearance_m * (distance_m > 0.0 ? away / distance_m : wall.inward);
            }
        }
    }
    return p;
}

// Even-odd: whether a ray from p along +x crosses the walls an odd number of times.
bool WalkController::inside(glm::dvec2 p) const {
    bool in = false;
    for (const Wall& wall : walls_) {
        const glm::dvec2 a = wall.start_m;
        const glm::dvec2 b = wall.start_m + wall.along_m;
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < a.x + (p.y - a.y) * wall.along_m.x / wall.along_m.y) {
            in = !in;
        }
    }
    return in;
}

} // namespace cs::editor
