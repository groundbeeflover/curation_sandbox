// SPDX-License-Identifier: Apache-2.0

#pragma once

// The render world (ADR 0008): the scene every tier draws and the engine measures, compiled from
// the document (compile.hpp) so that they all see identical input. It holds meshes, instances of
// them, materials, lights in candela and cameras, plus the exhibits' visible areas for the lux
// meter. It is renderer-neutral: nothing here is Vulkan's or Embree's.
//
// Conventions:
// - Units are SI and coordinates are the document's: metres, z up, the floor of the skeleton
//   scene at z = 0. Meshes follow geom/mesh.hpp's conventions in their own coordinates, and each
//   instance's transform takes them to the world.
// - Colours are linear Rec. 709.
// - Everything is in a fixed order: entities by id, and within an entity, in the order of the
//   types below. Compiling the same document twice gives equal worlds (operator==).
// - Images are named by content hash (AssetRef); the world holds no pixels.

#include <cs/world/ids.hpp>

#include <cs/geom/mesh.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/material/painting_material.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cs::world {

// ---- Materials -------------------------------------------------------------------------------

// A matte surface: walls, floors, ceilings and frames until finishes arrive (M2-06, M2-31).
struct LambertMaterial {
    glm::dvec3 albedo{0.8};
    bool operator==(const LambertMaterial&) const = default;
};

// The face of a work (ADR 0003). Its base colour comes from the work's photograph, texel by
// texel, converted to linear albedo by the importer; parameters.base.color is used only where
// there is no image (a placeholder). The image's UVs are the painting mesh's.
struct PaintingSurface {
    material::PaintingMaterial parameters;
    std::optional<AssetRef> image; // absent: no photograph yet
};

using Material = std::variant<LambertMaterial, PaintingSurface>;

// The albedo of walls, floors and ceilings, until the project's finishes are read (M2-31): a
// white gallery paint. [C]
inline constexpr double default_wall_albedo = 0.8;

// ---- Instances -------------------------------------------------------------------------------

// What part of an entity an instance draws.
enum class Part {
    room,     // a room's walls, and its floor and ceiling when its outline allows (compile.hpp)
    painting, // the face of a work
    frame,    // a work's frame
};

// One mesh, placed in the world with one material.
struct Instance {
    Id entity; // the entity it belongs to
    Part part = Part::room;
    std::uint32_t mesh = 0;     // into World::meshes
    std::uint32_t material = 0; // into World::materials
    glm::dmat4 transform{1.0};  // the mesh's coordinates to the world: a rotation and translation
    bool operator==(const Instance&) const = default;
};

// ---- Exhibits --------------------------------------------------------------------------------

// A rectangle in a face's own coordinates, in metres.
struct FaceRect {
    glm::dvec2 min_m{0.0};
    glm::dvec2 max_m{0.0};
    bool operator==(const FaceRect&) const = default;
};

// A work as it hangs, for the lux meter and the per-work readings: where its face is and how much
// of it a visitor can see. The engine reads lux points from this, never from the document.
struct Exhibit {
    Id entity; // the entity with the Exhibit component
    Id work;   // the work it shows

    glm::dvec2 work_size_m{0.0}; // the unframed work, (width, height)

    // The work's face in world coordinates: its centre, u to the right and v up seen from the
    // front, and the normal out of the face, into the room. Unit and right-handed, u x v = normal.
    glm::dvec3 face_centre_m{0.0};
    glm::dvec3 face_u{1.0, 0.0, 0.0};
    glm::dvec3 face_v{0.0, 0.0, 1.0};
    glm::dvec3 face_normal{0.0, -1.0, 0.0};

    // The part of the face a visitor sees, in face coordinates (metres from face_centre_m along
    // face_u and face_v): the frame's sight opening when the work is framed (geom::sight_opening),
    // the whole work when it is not.
    FaceRect visible;

    std::uint32_t painting_instance = 0;         // into World::instances
    std::optional<std::uint32_t> frame_instance; // absent: unframed

    // The world point at (u_m, v_m) on the face.
    glm::dvec3 face_point(glm::dvec2 uv_m) const {
        return face_centre_m + face_u * uv_m.x + face_v * uv_m.y;
    }

    bool operator==(const Exhibit&) const = default;
};

// ---- Lights ----------------------------------------------------------------------------------

// A luminaire of the document, placed and aimed, with its distribution in candela.
struct Light {
    Id entity;

    // Its position, its orientation (fixture +z along the beam) and its distribution: what the
    // engine and the lux meter evaluate.
    light::Luminaire luminaire;

    // The same distribution as luminaire's, as a generic spot, for the renderers that evaluate it
    // in a shader. Its lumens already include the luminaire's dimming. IES distributions join it
    // in milestone 3.
    std::shared_ptr<const light::GenericSpot> spot;

    double colour_temperature_k = 3000.0;
};

// ---- Cameras ---------------------------------------------------------------------------------

enum class Projection { perspective, orthographic };

struct Camera {
    std::optional<Id> entity; // the viewpoint it comes from, if any
    Projection projection = Projection::perspective;
    glm::dvec3 position_m{0.0};
    glm::dvec3 forward{1.0, 0.0, 0.0}; // unit: the direction of view
    glm::dvec3 up{0.0, 0.0, 1.0};      // unit, perpendicular to forward: the image's up
    double vertical_fov_rad = 0.0;     // perspective: the full vertical angle, in (0, pi)
    double vertical_extent_m = 0.0;    // orthographic: the height of the view
    std::optional<double> ev100;       // absent: automatic exposure
    bool operator==(const Camera&) const = default;
};

// ---- Problems --------------------------------------------------------------------------------

// Something of the document the world could not show as the document says, and why: an entity
// left out, or shown in part. Compiling never throws for a document that loads (compile.hpp).
struct Problem {
    Id entity;
    std::string reason;
    bool operator==(const Problem&) const = default;
};

// ---- The world -------------------------------------------------------------------------------

struct World {
    std::vector<geom::Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Instance> instances;
    std::vector<Exhibit> exhibits;
    std::vector<Light> lights;
    std::vector<Camera> cameras;
    std::vector<Problem> problems;
};

// Equal when every value is the same, bit for bit; lights are equal when their position, axes,
// distribution (as a generic spot: lumens, beam and field angles) and colour temperature are.
bool operator==(const World& a, const World& b);

} // namespace cs::world
