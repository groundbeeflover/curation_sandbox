// SPDX-License-Identifier: Apache-2.0

#pragma once

// The mutation API (ADR 0007): the one way to change a document. A user action builds a
// Transaction, which records the value before and after of everything it touches, and a History
// (history.hpp) commits it: all of it at once or, when the result would hold a dangling
// reference, none of it. Undo and redo apply the before and after values, so no command writes
// undo code. Each commit, undo and redo reports a ChangeList, from which the render world, the
// bake and the lux readings update what changed.

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/library.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cs::doc {

enum class ChangeKind { added, changed, removed };

// An entity itself: added, removed, or renamed (changed). Its components report their own changes.
struct EntityChange {
    Uuid entity;
    ChangeKind kind = ChangeKind::changed;
    bool operator==(const EntityChange&) const = default;
};

// A component of an entity. An entity added or removed reports each of its components added or
// removed too.
struct ComponentChange {
    Uuid entity;
    ComponentKind component = ComponentKind::wall;
    ChangeKind kind = ChangeKind::changed;
    bool operator==(const ComponentChange&) const = default;
};

struct WorkChange {
    Uuid work;
    ChangeKind kind = ChangeKind::changed;
    bool operator==(const WorkChange&) const = default;
};

struct FixtureTypeChange {
    LibraryId fixture_type;
    ChangeKind kind = ChangeKind::changed;
    bool operator==(const FixtureTypeChange&) const = default;
};

// What a commit, undo or redo changed, compared with the document before it: one entry for each
// thing whose value differs, however many times the transaction set it. Each list is sorted by
// id; components of one entity are in the order of ComponentKind.
struct ChangeList {
    bool settings = false;
    std::vector<FixtureTypeChange> fixture_types;
    std::vector<WorkChange> works;
    std::vector<EntityChange> entities;
    std::vector<ComponentChange> components;

    bool empty() const {
        return !settings && fixture_types.empty() && works.empty() && entities.empty() &&
               components.empty();
    }
    bool operator==(const ChangeList&) const = default;
};

namespace detail {

// One thing's value before a transaction and after it; absent when it does not exist.
template <class T> struct Edit {
    std::optional<T> before;
    std::optional<T> after;
};

// Everything a transaction touched, by id.
struct Edits {
    std::optional<Edit<ProjectSettings>> settings;
    std::map<LibraryId, Edit<FixtureType>> fixture_types;
    std::map<Uuid, Edit<Work>> works;
    std::map<Uuid, Edit<Entity>> entities;
};

} // namespace detail

// The changes of one user action, made for one revision of one document and committed with
// History::commit. Each call sees the document as the calls before it left it. A call that
// breaks its rule throws std::invalid_argument and leaves the transaction as it was. References
// are not checked until the commit, so a transaction may remove a wall before the works on it.
//
//   Transaction move(history.document(), "Move Water Lilies");
//   move.set(water_lilies, OnWall{wall_a, WallFace::a, 2.4, 1.5, 0.04});
//   const ChangeList changes = history.commit(std::move(move));
class Transaction {
  public:
    // `name` is shown in the interface: "Undo Move Water Lilies". `document` must outlive the
    // transaction.
    Transaction(const Document& document, std::string name);

    const std::string& name() const { return name_; }
    const Document& document() const { return *document_; }

    // ---- Entities ----

    // Adds an entity. Its id must not be nil nor name an entity.
    void add_entity(const Uuid& id, Entity entity);
    // Removes an entity and its components. Every call below needs an entity with the id.
    void remove_entity(const Uuid& id);
    void rename_entity(const Uuid& id, std::string name);

    // Sets a component of the entity, adding it or replacing the one there.
    void set(const Uuid& entity, Wall wall);
    void set(const Uuid& entity, Opening opening);
    void set(const Uuid& entity, Room room);
    void set(const Uuid& entity, Exhibit exhibit);
    void set(const Uuid& entity, Framing framing);
    void set(const Uuid& entity, Placement placement);
    void set(const Uuid& entity, Track track);
    void set(const Uuid& entity, Luminaire luminaire);
    void set(const Uuid& entity, LuxSensor lux_sensor);
    void set(const Uuid& entity, Viewpoint viewpoint);
    void set(const Uuid& entity, Dimension dimension);

    // Removes a component the entity has.
    void remove(const Uuid& entity, ComponentKind component);

    // ---- Works, the library and the settings ----

    // Adds a work or replaces the one with the id, which must not be nil.
    void set_work(const Uuid& id, Work work);
    // Removes a work the document holds.
    void remove_work(const Uuid& id);

    // Adds a fixture type to the project's library or replaces the one with the id.
    void set_fixture_type(const LibraryId& id, FixtureType fixture_type);
    // Removes a fixture type the library holds.
    void remove_fixture_type(const LibraryId& id);

    void set_settings(ProjectSettings settings);

    // ---- The document as this transaction leaves it ----

    const Entity* find_entity(const Uuid& id) const;
    const Work* find_work(const Uuid& id) const;
    const FixtureType* find_fixture_type(const LibraryId& id) const;
    const ProjectSettings& settings() const;

    // What committing would change, before reference checks.
    ChangeList changes() const;
    // True when committing would change nothing.
    bool empty() const { return changes().empty(); }

  private:
    friend class History;

    const Document* document_;
    std::uint64_t revision_;
    std::string name_;
    detail::Edits edits_;

    detail::Edit<Entity>& existing_entity(const Uuid& id);
    template <class T>
    void set_component(const Uuid& entity, std::optional<T> Entity::* member, T value);
};

namespace detail {

// What applying `edits` changes: forward (before to after) for a commit or redo, backward for an
// undo.
ChangeList changes_of(const Edits& edits, bool forward);

} // namespace detail

} // namespace cs::doc
