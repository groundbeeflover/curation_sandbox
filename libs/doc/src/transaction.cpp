// SPDX-License-Identifier: Apache-2.0

#include <cs/doc/transaction.hpp>

#include <cstddef>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace cs::doc {

namespace {

// Entity's component members, in the order of ComponentKind.
constexpr auto component_members =
    std::make_tuple(&Entity::wall, &Entity::opening, &Entity::room, &Entity::exhibit,
                    &Entity::framing, &Entity::placement, &Entity::track, &Entity::luminaire,
                    &Entity::lux_sensor, &Entity::viewpoint, &Entity::dimension);

static_assert(std::tuple_size_v<decltype(component_members)> == component_kind_count,
              "component_members lists every component");

// Calls f(kind, member pointer) for every component, in the order of ComponentKind.
template <class F> void for_each_component(F&& f) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (f(static_cast<ComponentKind>(I), std::get<I>(component_members)), ...);
    }(std::make_index_sequence<component_kind_count>{});
}

// Starts an edit of the value at `id` in `values`, or returns the one already started.
template <class Key, class T>
detail::Edit<T>& edit_of(std::map<Key, detail::Edit<T>>& edits, const std::map<Key, T>& values,
                         const Key& id) {
    const auto [it, inserted] = edits.try_emplace(id);
    if (inserted) {
        if (const auto value = values.find(id); value != values.end()) {
            it->second.before = value->second;
            it->second.after = value->second;
        }
    }
    return it->second;
}

// The value as the edits leave it, else as the document holds it; nullptr when there is none.
template <class Key, class T>
const T* find_in(const std::map<Key, detail::Edit<T>>& edits, const std::map<Key, T>& values,
                 const Key& id) {
    if (const auto edit = edits.find(id); edit != edits.end()) {
        return edit->second.after ? &*edit->second.after : nullptr;
    }
    const auto value = values.find(id);
    return value == values.end() ? nullptr : &value->second;
}

ChangeKind kind_of(bool before, bool after) {
    return !before ? ChangeKind::added : !after ? ChangeKind::removed : ChangeKind::changed;
}

// An entry for each edit whose value differs, `make(id, kind)` building it.
template <class Key, class T, class Change, class Make>
void add_changes(const std::map<Key, detail::Edit<T>>& edits, bool forward,
                 std::vector<Change>& out, Make make) {
    for (const auto& [id, edit] : edits) {
        const std::optional<T>& from = forward ? edit.before : edit.after;
        const std::optional<T>& to = forward ? edit.after : edit.before;
        if (from != to) {
            out.push_back(make(id, kind_of(from.has_value(), to.has_value())));
        }
    }
}

} // namespace

namespace detail {

ChangeList changes_of(const Edits& edits, bool forward) {
    ChangeList changes;
    if (edits.settings) {
        changes.settings = edits.settings->before != edits.settings->after;
    }
    add_changes(edits.fixture_types, forward, changes.fixture_types,
                [](const LibraryId& id, ChangeKind kind) { return FixtureTypeChange{id, kind}; });
    add_changes(edits.works, forward, changes.works,
                [](const Uuid& id, ChangeKind kind) { return WorkChange{id, kind}; });
    for (const auto& [id, edit] : edits.entities) {
        const std::optional<Entity>& from = forward ? edit.before : edit.after;
        const std::optional<Entity>& to = forward ? edit.after : edit.before;
        if (from == to) {
            continue;
        }
        if (!from || !to || from->name != to->name) {
            changes.entities.push_back({id, kind_of(from.has_value(), to.has_value())});
        }
        for_each_component([&](ComponentKind kind, auto member) {
            const auto* a = from ? &((*from).*member) : nullptr;
            const auto* b = to ? &((*to).*member) : nullptr;
            const bool had = a && a->has_value();
            const bool has = b && b->has_value();
            if (had != has || (had && **a != **b)) {
                changes.components.push_back({id, kind, kind_of(had, has)});
            }
        });
    }
    return changes;
}

} // namespace detail

Transaction::Transaction(const Document& document, std::string name)
    : document_(&document), revision_(document.revision()), name_(std::move(name)) {}

detail::Edit<Entity>& Transaction::existing_entity(const Uuid& id) {
    if (!find_entity(id)) {
        throw std::invalid_argument("no entity has the id " + id.to_string());
    }
    return edit_of(edits_.entities, document_->entities(), id);
}

void Transaction::add_entity(const Uuid& id, Entity entity) {
    if (id.is_nil()) {
        throw std::invalid_argument("an entity's id must not be nil");
    }
    if (find_entity(id)) {
        throw std::invalid_argument("an entity already has the id " + id.to_string());
    }
    edit_of(edits_.entities, document_->entities(), id).after = std::move(entity);
}

void Transaction::remove_entity(const Uuid& id) {
    existing_entity(id).after.reset();
}

void Transaction::rename_entity(const Uuid& id, std::string name) {
    existing_entity(id).after->name = std::move(name);
}

template <class T>
void Transaction::set_component(const Uuid& entity, std::optional<T> Entity::* member, T value) {
    (*existing_entity(entity).after).*member = std::move(value);
}

void Transaction::set(const Uuid& entity, Wall wall) {
    set_component(entity, &Entity::wall, std::move(wall));
}
void Transaction::set(const Uuid& entity, Opening opening) {
    set_component(entity, &Entity::opening, std::move(opening));
}
void Transaction::set(const Uuid& entity, Room room) {
    set_component(entity, &Entity::room, std::move(room));
}
void Transaction::set(const Uuid& entity, Exhibit exhibit) {
    set_component(entity, &Entity::exhibit, std::move(exhibit));
}
void Transaction::set(const Uuid& entity, Framing framing) {
    set_component(entity, &Entity::framing, std::move(framing));
}
void Transaction::set(const Uuid& entity, Placement placement) {
    set_component(entity, &Entity::placement, std::move(placement));
}
void Transaction::set(const Uuid& entity, Track track) {
    set_component(entity, &Entity::track, std::move(track));
}
void Transaction::set(const Uuid& entity, Luminaire luminaire) {
    set_component(entity, &Entity::luminaire, std::move(luminaire));
}
void Transaction::set(const Uuid& entity, LuxSensor lux_sensor) {
    set_component(entity, &Entity::lux_sensor, std::move(lux_sensor));
}
void Transaction::set(const Uuid& entity, Viewpoint viewpoint) {
    set_component(entity, &Entity::viewpoint, std::move(viewpoint));
}
void Transaction::set(const Uuid& entity, Dimension dimension) {
    set_component(entity, &Entity::dimension, std::move(dimension));
}

void Transaction::remove(const Uuid& entity, ComponentKind component) {
    const Entity* current = find_entity(entity);
    if (current && !current->has(component)) {
        throw std::invalid_argument("the entity " + entity.to_string() + " has no " +
                                    std::string(component_key(component)));
    }
    Entity& after = *existing_entity(entity).after;
    for_each_component([&](ComponentKind kind, auto member) {
        if (kind == component) {
            (after.*member).reset();
        }
    });
}

void Transaction::set_work(const Uuid& id, Work work) {
    if (id.is_nil()) {
        throw std::invalid_argument("a work's id must not be nil");
    }
    edit_of(edits_.works, document_->works(), id).after = std::move(work);
}

void Transaction::remove_work(const Uuid& id) {
    if (!find_work(id)) {
        throw std::invalid_argument("no work has the id " + id.to_string());
    }
    edit_of(edits_.works, document_->works(), id).after.reset();
}

void Transaction::set_fixture_type(const LibraryId& id, FixtureType fixture_type) {
    edit_of(edits_.fixture_types, document_->library().fixture_types, id).after =
        std::move(fixture_type);
}

void Transaction::remove_fixture_type(const LibraryId& id) {
    if (!find_fixture_type(id)) {
        throw std::invalid_argument("the library has no fixture type \"" + id + '"');
    }
    edit_of(edits_.fixture_types, document_->library().fixture_types, id).after.reset();
}

void Transaction::set_settings(ProjectSettings settings) {
    if (!edits_.settings) {
        edits_.settings =
            detail::Edit<ProjectSettings>{document_->settings(), document_->settings()};
    }
    edits_.settings->after = settings;
}

const Entity* Transaction::find_entity(const Uuid& id) const {
    return find_in(edits_.entities, document_->entities(), id);
}

const Work* Transaction::find_work(const Uuid& id) const {
    return find_in(edits_.works, document_->works(), id);
}

const FixtureType* Transaction::find_fixture_type(const LibraryId& id) const {
    return find_in(edits_.fixture_types, document_->library().fixture_types, id);
}

const ProjectSettings& Transaction::settings() const {
    return edits_.settings ? *edits_.settings->after : document_->settings();
}

ChangeList Transaction::changes() const {
    return detail::changes_of(edits_, true);
}

} // namespace cs::doc
