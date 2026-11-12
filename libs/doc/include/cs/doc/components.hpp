// SPDX-License-Identifier: Apache-2.0

#pragma once

// The components of ADR 0008 that make up the space: walls, openings, rooms, exhibits and their
// framing, placements, tracks, luminaires, lux sensors, viewpoints and dimensions. Every entity of
// a document holds any subset of them (document.hpp).
//
// Conventions, for every type in this module:
// - Units are SI: metres, radians, lumens, kelvin. Plan positions are (x, y) in metres; z is up,
//   and heights are z values in metres above the project's ground level, z = 0.
// - Colours are linear Rec. 709 RGB.
// - References to other entities and works are by id; to library definitions (fixture types,
//   frame styles, finishes, material presets) by LibraryId.
// - In project.json, each member's key is its name in camelCase, keeping a unit suffix (_m, _rad,
//   _lm, _k) after an underscore: centre_height_m is "centreHeight_m", fixture_type is
//   "fixtureType", finish_a is "finishA". Kelvin is written with its symbol: colour_temperature_k
//   is "colourTemperature_K". Keys are written in the order the members are declared
//   (project_io.hpp).

#include <cs/doc/uuid.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cs::doc {

// The id of a library definition: a frame style ("box"), a finish ("wall-white"), a fixture type
// ("generic-spot-800"), a material preset ("oil-canvas-gloss"). 1 to 128 characters from a-z, 0-9,
// '.', '_' and '-', starting with a letter or digit.
using LibraryId = std::string;

// A point or direction in the plan; in project.json, [x, y].
struct Vec2 {
    double x = 0.0;
    double y = 0.0;
    bool operator==(const Vec2&) const = default;
};

// A point or direction in space, z up; in project.json, [x, y, z].
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool operator==(const Vec3&) const = default;
};

// A linear Rec. 709 colour, each channel at least 0; in project.json, [r, g, b].
struct LinearRgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    bool operator==(const LinearRgb&) const = default;
};

// A file in the project's assets/ folder, named by its content: assets/<sha256>.<extension>
// (ADR 0007). The hash is also the asset's id.
struct AssetRef {
    std::string sha256;    // 64 lowercase hexadecimal digits
    std::string extension; // 1 to 8 characters from a-z and 0-9: "png", "jpg", "tif", "exr", "ies"
    bool operator==(const AssetRef&) const = default;
};

// ---- Building --------------------------------------------------------------------------------

// The two faces of a wall. Face A is on the left of the line from the wall's start to its end,
// seen from above; face B is on the right. In project.json, "A" or "B".
enum class WallFace { a, b };

struct Wall {
    Vec2 start_m;                      // the ends of the wall's centre line, in plan
    Vec2 end_m;                        //
    double height_m = 0.0;             // > 0
    double thickness_m = 0.0;          // > 0
    std::optional<LibraryId> finish_a; // the finish of face A; absent: the default wall finish
    std::optional<LibraryId> finish_b; // the finish of face B; absent: the default wall finish
    std::string label;                 // the name curators use, "A", "B", ...; editable
    bool operator==(const Wall&) const = default;
};

// In project.json, "door", "passage" or "window".
enum class OpeningKind { door, passage, window };

// A hole in a wall.
struct Opening {
    Uuid wall;             // an entity with a Wall
    double offset_m = 0.0; // >= 0: along the wall's centre line, from its start to the near side
    double width_m = 0.0;  // > 0
    double height_m = 0.0; // > 0
    double sill_m = 0.0;   // >= 0: from the floor to the bottom; 0 for doors and passages
    OpeningKind kind = OpeningKind::door;
    bool operator==(const Opening&) const = default;
};

struct Room {
    std::vector<Vec2> outline_m;   // the floor's outline in plan, 3 to 1000 points, not repeated
    double floor_height_m = 0.0;   // z of the floor
    double ceiling_height_m = 0.0; // z of the ceiling, > floor_height_m
    std::optional<LibraryId> floor_finish;   // absent: the default floor finish
    std::optional<LibraryId> ceiling_finish; // absent: the default ceiling finish
    bool operator==(const Room&) const = default;
};

// ---- Exhibits --------------------------------------------------------------------------------

// Marks an entity as the showing of a work (work.hpp) in the space.
struct Exhibit {
    Uuid work; // a Work of the document
    bool operator==(const Exhibit&) const = default;
};

// Glass or acrylic in front of a work (ADR 0003). In project.json: "none", "floatGlass",
// "lowIronGlass", "acrylic", "antiReflectiveGlass", "antiReflectiveAcrylic", "nonGlareGlass".
enum class Glazing {
    none,
    float_glass,
    low_iron_glass,
    acrylic,
    anti_reflective_glass,
    anti_reflective_acrylic,
    non_glare_glass,
};

// A card border between a work and its frame (ADR 0003).
struct Mat {
    double border_m = 0.0;       // > 0: the width of the border at the top and sides
    double bottom_extra_m = 0.0; // >= 0: how much wider the bottom border is
    double overlap_m = 0.0;      // >= 0: how far the window overlaps the work
    LinearRgb colour;
    bool operator==(const Mat&) const = default;
};

// How an exhibit is framed: a frame style from the library, with overrides. Absent overrides take
// the style's values.
struct Framing {
    LibraryId style;
    std::optional<double> width_m;   // > 0: the frame's member width
    std::optional<LinearRgb> colour; // the frame's colour
    std::optional<Mat> mat;          // a mat, in place of the style's
    std::optional<Glazing> glazing;  // in place of the style's (and the material preset's)
    bool operator==(const Framing&) const = default;
};

// ---- Placement -------------------------------------------------------------------------------
// Where an entity is, as a curator thinks of it. Exactly one kind per entity. In project.json a
// placement is one object whose "kind" key comes first: "onWall", "onFloor", "onHost", "onTrack"
// or "onCeiling", followed by the members of that kind.

// On a face of a wall: paintings, drawings, labels, wall-mounted fixtures.
struct OnWall {
    Uuid wall; // an entity with a Wall
    WallFace face = WallFace::a;
    double distance_m = 0.0; // along the face, from its left end (seen facing it) to the centre
    double centre_height_m = 0.0; // z of the centre
    double stand_off_m = 0.0;     // >= 0: from the face to the back of the thing
    bool operator==(const OnWall&) const = default;
};

// On the floor of a room: plinths, benches, vitrines; sculpture in 1.0.
struct OnFloor {
    Uuid room;                 // an entity with a Room
    Vec2 position_m;           // in plan
    double rotation_rad = 0.0; // about the vertical, counter-clockwise seen from above
    bool operator==(const OnFloor&) const = default;
};

// On or in another entity: a sculpture on a plinth, an object in a vitrine (1.0). Hosts never
// form a cycle.
struct OnHost {
    Uuid host;                 // any other entity
    Vec3 offset_m;             // from the host's origin, in the host's frame
    double rotation_rad = 0.0; // about the host's vertical, counter-clockwise seen from above
    bool operator==(const OnHost&) const = default;
};

// On a lighting track: luminaires.
struct OnTrack {
    Uuid track;              // an entity with a Track
    double distance_m = 0.0; // >= 0: along the track's polyline from its first point
    bool operator==(const OnTrack&) const = default;
};

// In a room's ceiling: recessed luminaires.
struct OnCeiling {
    Uuid room;       // an entity with a Room
    Vec2 position_m; // in plan
    bool operator==(const OnCeiling&) const = default;
};

using Placement = std::variant<OnWall, OnFloor, OnHost, OnTrack, OnCeiling>;

// ---- Lighting --------------------------------------------------------------------------------

// A lighting track hung at one height.
struct Track {
    std::vector<Vec2> points_m; // the polyline in plan, 2 to 1000 points
    double height_m = 0.0;      // z of the track
    bool operator==(const Track&) const = default;
};

// Where a luminaire points. In project.json, an object whose "kind" comes first: "entity" with
// "target", or "point" with "point_m".
struct AimAtEntity {
    Uuid target; // any entity; for an exhibit, the centre of the work's face
    bool operator==(const AimAtEntity&) const = default;
};

struct AimAtPoint {
    Vec3 point_m;
    bool operator==(const AimAtPoint&) const = default;
};

using Aim = std::variant<AimAtEntity, AimAtPoint>;

// A light fitting. Its position comes from the entity's Placement.
struct Luminaire {
    LibraryId fixture_type; // a FixtureType in the document's library (document.hpp)
    double dimming = 1.0;   // 0 to 1: the fraction of the fixture type's flux it emits
    Aim aim;
    std::optional<double> colour_temperature_k; // > 0: in place of the fixture type's
    bool operator==(const Luminaire&) const = default;
};

// A persistent lux probe: the illuminance on a small surface at a point.
struct LuxSensor {
    Vec3 position_m;
    Vec3 normal; // the direction the surface faces; not zero, need not be unit length
    bool operator==(const LuxSensor&) const = default;
};

// ---- Views and annotations -------------------------------------------------------------------

// A saved camera.
struct Viewpoint {
    Vec3 position_m;
    double yaw_rad = 0.0;           // about the vertical: 0 looks along +x, pi/2 along +y
    double pitch_rad = 0.0;         // above the horizontal, -pi/2 to pi/2
    double field_of_view_rad = 0.0; // vertical, full angle, in (0, pi)
    std::optional<double> ev100;    // exposure as EV100; absent: automatic
    bool operator==(const Viewpoint&) const = default;
};

// One end of a dimension: a point in the world, or, when entity is set, a point relative to that
// entity's origin.
struct Anchor {
    std::optional<Uuid> entity; // any entity
    Vec3 point_m;
    bool operator==(const Anchor&) const = default;
};

// A measured distance shown in the views.
struct Dimension {
    Anchor from;
    Anchor to;
    bool operator==(const Dimension&) const = default;
};

} // namespace cs::doc
