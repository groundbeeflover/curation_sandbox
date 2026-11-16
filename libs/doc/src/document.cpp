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
    bool present = false;
    for_each_component([&](ComponentKind k, auto member) {
        if (k == kind) {
            present = (this->*member).has_value();
        }
    });
    return present;
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
