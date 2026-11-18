// SPDX-License-Identifier: Apache-2.0

#include <cs/editor/commands/lighting.hpp>

#include "commands/common.hpp"

#include <cs/doc/components.hpp>
#include <cs/doc/work.hpp>
#include <cs/geom/wall_face.hpp>
#include <cs/world/compile.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cs::editor {

namespace {

constexpr double degree = std::numbers::pi / 180.0;

// How near an existing track must be to the one add_spot_for would make for the spot to go on it.
constexpr double same_track_m = 0.001;

using detail::to_glm;

[[noreturn]] void refuse(const std::string& problem) {
    detail::refuse("add_spot_for", problem);
}

// Whether `point` is inside the polygon `outline` (even-odd rule), in plan.
bool inside(const std::vector<doc::Vec2>& outline, glm::dvec2 point) {
    bool in = false;
    for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
        const glm::dvec2 a = to_glm(outline[i]);
        const glm::dvec2 b = to_glm(outline[j]);
        if ((a.y > point.y) != (b.y > point.y) &&
            point.x < a.x + (b.x - a.x) * (point.y - a.y) / (b.y - a.y)) {
            in = !in;
        }
    }
    return in;
}

// A track the spot can go on: one already in the document within same_track_m of `wanted`, else
// nullopt.
std::optional<doc::Uuid> matching_track(const doc::Document& document, const doc::Track& wanted) {
    for (const auto& [id, entity] : document.entities()) {
        if (!entity.track || entity.track->points_m.size() != 2 ||
            std::abs(entity.track->height_m - wanted.height_m) > same_track_m) {
            continue;
        }
        const auto& points = entity.track->points_m;
        if (glm::distance(to_glm(points[0]), to_glm(wanted.points_m[0])) <= same_track_m &&
            glm::distance(to_glm(points[1]), to_glm(wanted.points_m[1])) <= same_track_m) {
            return id;
        }
    }
    return std::nullopt;
}

} // namespace

double cci_track_distance_m(double ceiling_height_m) {
    if (!std::isfinite(ceiling_height_m) || !(ceiling_height_m > cci_reference_height_m)) {
        // cci_reference_height_m, written as a curator reads it.
        detail::refuse("cci_track_distance_m",
                       "the ceiling must be finite and more than 1.6 m above the floor, not " +
                           std::to_string(ceiling_height_m) + " m");
    }
    return (ceiling_height_m - cci_reference_height_m) * std::tan(cci_aim_from_vertical_rad);
}

doc::FixtureType generic_spot() {
    return {.name = "Generic spot 800 lm",
            .flux_lm = 800.0,
            .beam_angle_rad = 15.0 * degree,
            .field_angle_rad = 25.0 * degree,
            .colour_temperature_k = 3000.0};
}

AddedSpot add_spot_for(const doc::Document& document, doc::IdGenerator& ids,
                       const doc::Uuid& exhibit) {
    // What is lit, and where the centre of its face is.
    const doc::Entity* entity = document.find_entity(exhibit);
    if (entity == nullptr || !entity->exhibit) {
        refuse("no entity with an exhibit has the id " + exhibit.to_string());
    }
    const doc::Work* work = document.find_work(entity->exhibit->work);
    if (work == nullptr) {
        refuse("the exhibit's work is not in the document");
    }
    const auto* on_wall =
        entity->placement ? std::get_if<doc::OnWall>(&*entity->placement) : nullptr;
    const doc::Entity* wall_entity = on_wall ? document.find_entity(on_wall->wall) : nullptr;
    if (wall_entity == nullptr || !wall_entity->wall) {
        refuse("the exhibit is not hung on a wall");
    }
    const doc::Wall& wall = *wall_entity->wall;
    const geom::FaceFrame face = detail::face_frame(wall, on_wall->face);
    // As world::compile puts the work's face.
    const double face_out_m =
        std::max(on_wall->stand_off_m + work->form.depth_m.value_or(0.0), world::min_face_offset_m);
    const glm::dvec3 target_m =
        face.point(on_wall->distance_m, on_wall->centre_height_m, face_out_m);

    // The ceiling over it.
    double floor_z_m = 0.0;
    double ceiling_z_m = wall.height_m;
    for (const auto& [id, other] : document.entities()) {
        if (other.room && inside(other.room->outline_m, glm::dvec2(target_m))) {
            floor_z_m = other.room->floor_height_m;
            ceiling_z_m = other.room->ceiling_height_m;
            break;
        }
    }
    const double track_out_m = cci_track_distance_m(ceiling_z_m - floor_z_m);
    if (!(track_out_m > face_out_m)) {
        refuse("the track would be no further from the wall than the work's face");
    }
    const double drop_m = ceiling_z_m - target_m.z;
    if (!(drop_m > 0.0)) {
        refuse("the centre of the work's face is not below the ceiling");
    }

    // The track, along the face from its left end, and where the spot goes on it: aiming at the
    // target from `reach_m` away horizontally is 30 degrees from vertical, and the track passes
    // `across_m` from it, so the spot is slid sqrt(reach^2 - across^2) along from the point
    // opposite.
    const glm::dvec3 first_m = face.point(0.0, ceiling_z_m, track_out_m);
    const glm::dvec3 last_m = face.point(face.length_m, ceiling_z_m, track_out_m);
    doc::Track track;
    track.points_m = {{first_m.x, first_m.y}, {last_m.x, last_m.y}};
    track.height_m = ceiling_z_m;
    const double reach_m = drop_m * std::tan(cci_aim_from_vertical_rad);
    const double across_m = track_out_m - face_out_m;
    const double slide_m =
        reach_m > across_m ? std::sqrt(reach_m * reach_m - across_m * across_m) : 0.0;
    const double opposite_m = on_wall->distance_m;
    const auto on_track = [&](double u_m) { return u_m >= 0.0 && u_m <= face.length_m; };
    double along_m = opposite_m - slide_m;
    if (!on_track(along_m)) {
        along_m = on_track(opposite_m + slide_m) ? opposite_m + slide_m
                                                 : std::clamp(along_m, 0.0, face.length_m);
    }

    doc::Transaction transaction(document, "Add spot");
    if (document.library().find_fixture_type(std::string(generic_spot_id)) == nullptr) {
        transaction.set_fixture_type(std::string(generic_spot_id), generic_spot());
    }
    std::optional<doc::Uuid> track_id = matching_track(document, track);
    if (!track_id) {
        track_id = ids.next();
        doc::Entity track_entity;
        track_entity.name = wall.label.empty() ? "Track" : "Track " + wall.label;
        track_entity.track = std::move(track);
        transaction.add_entity(*track_id, std::move(track_entity));
    }

    const auto luminaires = std::ranges::count_if(
        document.entities(), [](const auto& pair) { return pair.second.luminaire.has_value(); });
    doc::Entity spot;
    spot.name = "Spot " + std::to_string(luminaires + 1);
    spot.placement = doc::OnTrack{.track = *track_id, .distance_m = along_m};
    doc::Luminaire& luminaire = spot.luminaire.emplace();
    luminaire.fixture_type = generic_spot_id;
    luminaire.dimming = 1.0;
    luminaire.aim = doc::AimAtEntity{exhibit};
    const doc::Uuid spot_id = ids.next();
    transaction.add_entity(spot_id, std::move(spot));
    return {std::move(transaction), spot_id, *track_id};
}

} // namespace cs::editor
