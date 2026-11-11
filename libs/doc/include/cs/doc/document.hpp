// SPDX-License-Identifier: Apache-2.0

#pragma once

// The document (ADR 0008): project settings, the project's copies of the library definitions it
// uses, the works, and the entities of the space, each a fixed set of optional components.
//
// A Document is read-only. It is made whole, from DocumentData (by load_project_folder, or by a
// test), and changes only through the mutation API (ADR 0007).

#include <cs/doc/components.hpp>
#include <cs/doc/library.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cs::doc {

// How lengths are shown in the interface; the document itself is always in metres. In
// project.json, "centimetres" or "inches".
enum class DisplayUnits { centimetres, inches };

struct ProjectSettings {
    double centre_line_m = 1.50;            // > 0: the height of hung works' centres
    double eye_height_m = 1.60;             // > 0: for walking and viewpoints
    double opening_hours_per_year = 2184.0; // 0 to 8784: for yearly light exposure
    DisplayUnits display_units = DisplayUnits::centimetres;
    bool operator==(const ProjectSettings&) const = default;
};

// The components, in the order they are declared in Entity and written in project.json.
enum class ComponentKind {
    wall,
    opening,
    room,
    exhibit,
    framing,
    placement,
    track,
    luminaire,
    lux_sensor,
    viewpoint,
    dimension,
};

inline constexpr std::array all_component_kinds = {
    ComponentKind::wall,      ComponentKind::opening,   ComponentKind::room,
    ComponentKind::exhibit,   ComponentKind::framing,   ComponentKind::placement,
    ComponentKind::track,     ComponentKind::luminaire, ComponentKind::lux_sensor,
    ComponentKind::viewpoint, ComponentKind::dimension,
};

inline constexpr std::size_t component_kind_count = all_component_kinds.size();

static_assert(static_cast<std::size_t>(ComponentKind::dimension) + 1 == component_kind_count,
              "all_component_kinds lists every ComponentKind");

// The component's key in project.json: "wall", ..., "luxSensor", "viewpoint", "dimension".
std::string_view component_key(ComponentKind kind);

// A thing in the space: a display name and any subset of the components. Its id is its key in
// the document's map of entities. In project.json, an object with "id", "name" and one key per
// component present, in this order.
struct Entity {
    std::string name;
    std::optional<Wall> wall;
    std::optional<Opening> opening;
    std::optional<Room> room;
    std::optional<Exhibit> exhibit;
    std::optional<Framing> framing;
    std::optional<Placement> placement;
    std::optional<Track> track;
    std::optional<Luminaire> luminaire;
    std::optional<LuxSensor> lux_sensor;
    std::optional<Viewpoint> viewpoint;
    std::optional<Dimension> dimension;

    bool has(ComponentKind kind) const;
    bool operator==(const Entity&) const = default;
};

// Everything a project file holds, as plain values. Maps are ordered by id, which is the order
// they are written in.
struct DocumentData {
    ProjectSettings settings;
    Library library; // the project's copies of the definitions it uses
    std::map<Uuid, Work> works;
    std::map<Uuid, Entity> entities;
    bool operator==(const DocumentData&) const = default;
};

class Document {
  public:
    // An empty project with the default settings.
    Document() = default;

    // A document holding `data`. It is not checked here; load_project_folder checks what it
    // reads, and save_project_folder refuses to write a document that would not read back.
    explicit Document(DocumentData data) : data_(std::move(data)) {}

    const ProjectSettings& settings() const { return data_.settings; }
    const Library& library() const { return data_.library; }
    const std::map<Uuid, Work>& works() const { return data_.works; }
    const std::map<Uuid, Entity>& entities() const { return data_.entities; }
    const DocumentData& data() const { return data_; }

    // nullptr when there is none with that id. Library definitions: library().find_...().
    const Entity* find_entity(const Uuid& id) const;
    const Work* find_work(const Uuid& id) const;

    bool operator==(const Document&) const = default;

  private:
    DocumentData data_;
};

} // namespace cs::doc
