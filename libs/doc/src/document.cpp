// SPDX-License-Identifier: Apache-2.0

#include <cs/doc/document.hpp>

namespace cs::doc {

std::string_view component_key(ComponentKind kind) {
    static constexpr std::array<std::string_view, component_kind_count> keys = {
        "wall",  "opening",   "room",      "exhibit",   "framing",   "placement",
        "track", "luminaire", "luxSensor", "viewpoint", "dimension",
    };
    return keys[static_cast<std::size_t>(kind)];
}

bool Entity::has(ComponentKind kind) const {
    switch (kind) {
    case ComponentKind::wall:
        return wall.has_value();
    case ComponentKind::opening:
        return opening.has_value();
    case ComponentKind::room:
        return room.has_value();
    case ComponentKind::exhibit:
        return exhibit.has_value();
    case ComponentKind::framing:
        return framing.has_value();
    case ComponentKind::placement:
        return placement.has_value();
    case ComponentKind::track:
        return track.has_value();
    case ComponentKind::luminaire:
        return luminaire.has_value();
    case ComponentKind::lux_sensor:
        return lux_sensor.has_value();
    case ComponentKind::viewpoint:
        return viewpoint.has_value();
    case ComponentKind::dimension:
        return dimension.has_value();
    }
    return false;
}

const Entity* Document::find_entity(const Uuid& id) const {
    const auto it = data_.entities.find(id);
    return it == data_.entities.end() ? nullptr : &it->second;
}

const Work* Document::find_work(const Uuid& id) const {
    const auto it = data_.works.find(id);
    return it == data_.works.end() ? nullptr : &it->second;
}

} // namespace cs::doc
