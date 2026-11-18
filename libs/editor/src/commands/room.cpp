// SPDX-License-Identifier: Apache-2.0

#include <cs/editor/commands/room.hpp>

#include "commands/common.hpp"

#include <glm/vec2.hpp>

#include <algorithm>
#include <set>
#include <utility>

namespace cs::editor {

std::string wall_label(std::size_t index) {
    // Bijective base 26: the last letter is index % 26, and the letters before it label
    // index / 26 - 1, so that "Z" (25) is followed by "AA" (26).
    std::string label;
    while (true) {
        label.push_back(static_cast<char>('A' + index % 26));
        if (index < 26) {
            break;
        }
        index = index / 26 - 1;
    }
    std::ranges::reverse(label);
    return label;
}

std::vector<std::string> unused_wall_labels(const doc::Document& document, std::size_t count) {
    std::set<std::string> used;
    for (const auto& [id, entity] : document.entities()) {
        if (entity.wall) {
            used.insert(entity.wall->label);
        }
    }
    std::vector<std::string> labels;
    labels.reserve(count);
    for (std::size_t index = 0; labels.size() < count; ++index) {
        if (std::string label = wall_label(index); !used.contains(label)) {
            labels.push_back(std::move(label));
        }
    }
    return labels;
}

std::optional<doc::Uuid> find_wall_labelled(const doc::Document& document, std::string_view label) {
    for (const auto& [id, entity] : document.entities()) {
        if (entity.wall && entity.wall->label == label) {
            return id;
        }
    }
    return std::nullopt;
}

NewRoom new_room(const doc::Document& document, doc::IdGenerator& ids, double width_m,
                 double depth_m, double height_m) {
    detail::check_length("new_room", "the width", width_m);
    detail::check_length("new_room", "the depth", depth_m);
    detail::check_length("new_room", "the height", height_m);

    doc::Room room;
    room.outline_m = {{0.0, 0.0}, {width_m, 0.0}, {width_m, depth_m}, {0.0, depth_m}};
    room.floor_height_m = 0.0;
    room.ceiling_height_m = height_m;

    doc::Transaction transaction(document, "New room");
    const doc::Uuid room_id = ids.next();
    doc::Entity room_entity;
    room_entity.name = "Room";
    room_entity.room = room;
    transaction.add_entity(room_id, std::move(room_entity));

    // The outline runs anticlockwise, so the room is on the left of each side, and face A, on the
    // left of the wall's line, faces into it when the line is half a thickness out: to the right
    // of the side. The sides run along +x, +y, -x and -y, so their right is -y, +x, +y and -x, by
    // the axes rather than by normalising, which a size near the limits of a double would spoil.
    constexpr double half = default_wall_thickness_m / 2.0;
    constexpr std::array<glm::dvec2, 4> outward{
        {{0.0, -half}, {half, 0.0}, {0.0, half}, {-half, 0.0}}};
    const std::vector<std::string> labels = unused_wall_labels(document, 4);
    std::array<doc::Uuid, 4> wall_ids;
    for (std::size_t i = 0; i < 4; ++i) {
        const glm::dvec2 a = detail::to_glm(room.outline_m[i]);
        const glm::dvec2 b = detail::to_glm(room.outline_m[(i + 1) % 4]);
        const glm::dvec2 out = outward[i];

        doc::Wall wall;
        wall.start_m = {a.x + out.x, a.y + out.y};
        wall.end_m = {b.x + out.x, b.y + out.y};
        wall.height_m = height_m;
        wall.thickness_m = default_wall_thickness_m;
        wall.label = labels[i];

        wall_ids[i] = ids.next();
        doc::Entity wall_entity;
        wall_entity.name = "Wall " + labels[i];
        wall_entity.wall = std::move(wall);
        transaction.add_entity(wall_ids[i], std::move(wall_entity));
    }
    return {std::move(transaction), room_id, wall_ids};
}

} // namespace cs::editor
