// SPDX-License-Identifier: Apache-2.0

#pragma once

// The document (ADR 0008): project settings, the project's copies of the library definitions it
// uses, the works, and the entities of the space, each a fixed set of optional components.
//
// A Document is read-only. It is made whole, from DocumentData (by load_project_folder, or by a
// test), and changes only through the mutation API (ADR 0007).

#include <cs/doc/components.hpp>
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

// A fixture type: the light a kind of fitting emits (ADR 0008, Library definitions). A generic
// spot is a cone given by lumens and two full angles, as in IES files; IES distributions arrive
// with milestone 3.
struct FixtureType {
    std::string name;
    double flux_lm = 0.0;                 // > 0: at dimming 1
    double beam_angle_rad = 0.0;          // > 0: full angle at 50% of the peak intensity
    double field_angle_rad = 0.0;         // >= beam_angle_rad: full angle at 10% of the peak
    double colour_temperature_k = 3000.0; // > 0
    bool operator==(const FixtureType&) const = default;
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

inline constexpr std::size_t component_kind_count = 11;

inline constexpr std::array<ComponentKind, component_kind_count> all_component_kinds = {
    ComponentKind::wall,      ComponentKind::opening,   ComponentKind::room,
    ComponentKind::exhibit,   ComponentKind::framing,   ComponentKind::placement,
    ComponentKind::track,     ComponentKind::luminaire, ComponentKind::lux_sensor,
    ComponentKind::viewpoint, ComponentKind::dimension,
};

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
    std::map<LibraryId, FixtureType> fixture_types;
    std::map<Uuid, Work> works;
    std::map<Uuid, Entity> entities;
    bool operator==(const DocumentData&) const = default;
};

class Document {
  public:
    // An empty project with the default settings.
    Document() = default;

    // A document holding `data`. It is not checked here: load_project_folder checks what it
    // reads, and the mutation API keeps a document valid.
    explicit Document(DocumentData data) : data_(std::move(data)) {}

    const ProjectSettings& settings() const { return data_.settings; }
    const std::map<LibraryId, FixtureType>& fixture_types() const { return data_.fixture_types; }
    const std::map<Uuid, Work>& works() const { return data_.works; }
    const std::map<Uuid, Entity>& entities() const { return data_.entities; }
    const DocumentData& data() const { return data_; }

    // nullptr when there is none with that id.
    const Entity* find_entity(const Uuid& id) const;
    const Work* find_work(const Uuid& id) const;
    const FixtureType* find_fixture_type(const LibraryId& id) const;

    bool operator==(const Document&) const = default;

  private:
    DocumentData data_;
};

} // namespace cs::doc
