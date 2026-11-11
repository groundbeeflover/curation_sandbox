// SPDX-License-Identifier: Apache-2.0

#include <cs/doc/document.hpp>

namespace cs::doc {

std::string_view component_key(ComponentKind kind) {
    switch (kind) {
    case ComponentKind::wall:
        return "wall";
    case ComponentKind::opening:
        return "opening";
    case ComponentKind::room:
        return "room";
    case ComponentKind::exhibit:
        return "exhibit";
    case ComponentKind::framing:
        return "framing";
    case ComponentKind::placement:
        return "placement";
    case ComponentKind::track:
        return "track";
    case ComponentKind::luminaire:
        return "luminaire";
    case ComponentKind::lux_sensor:
        return "luxSensor";
    case ComponentKind::viewpoint:
        return "viewpoint";
    case ComponentKind::dimension:
        return "dimension";
    }
    return {};
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

namespace {

template <class Map, class Key>
const typename Map::mapped_type* find_in(const Map& map, const Key& key) {
    const auto it = map.find(key);
    return it == map.end() ? nullptr : &it->second;
}

} // namespace

const Entity* Document::find_entity(const Uuid& id) const {
    return find_in(data_.entities, id);
}

const Work* Document::find_work(const Uuid& id) const {
    return find_in(data_.works, id);
}

const FixtureType* Document::find_fixture_type(const LibraryId& id) const {
    return find_in(data_.fixture_types, id);
}

} // namespace cs::doc
