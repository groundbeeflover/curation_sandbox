// SPDX-License-Identifier: Apache-2.0

#pragma once

// The calculation engine's ray-traced scene
// -----------------------------------------
// A Scene is a world's triangles in an Embree BVH, for the engine's ray queries: the closest hit
// along a ray (intersect) and whether anything lies on a segment (occluded). It is built once
// from a world::World and then only read, so any number of threads may query it at once.
//
// Every instance of the world becomes its own triangle geometry, transformed into world
// coordinates, so a hit names the instance it belongs to (World::instances). Embree works in
// single precision; distances and points come back as doubles but are only as precise as floats
// at the scene's scale, about a micrometre in a room of ten metres. Rays that leave a surface
// must start a little off it (ray_offset_m) so that they do not hit it again.
//
// Triangles have two sides: a ray hits a triangle from behind as well as from the front.

#include <cs/world/world.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace cs::engine {

struct Hit {
    std::uint32_t instance = 0; // into World::instances
    std::uint32_t triangle = 0; // into that instance's mesh, as indices[3 * triangle + k]
    double distance_m = 0.0;    // along the ray, which is of unit length
    glm::dvec3 position_m{0.0}; // origin + distance_m * direction
    glm::dvec3 geometric_normal{0.0, 0.0, 1.0}; // unit, the triangle's front (counter-clockwise)
    glm::dvec3 normal{0.0, 0.0, 1.0}; // unit: the mesh's vertex normals, interpolated, in world
    glm::dvec2 uv{0.0};               // the mesh's uvs, interpolated
};

class Scene {
  public:
    // Copies what it needs from the world; the scene does not refer to it afterwards. Throws
    // std::invalid_argument if an instance names a mesh or material that is not in the world, or
    // a mesh has an index out of range, a position that is not finite, or not one normal and uv
    // per position; std::runtime_error if Embree fails.
    explicit Scene(const world::World& world);
    ~Scene();

    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;

    // The closest hit along the ray from origin in direction (unit length) with a distance in
    // [t_min, t_max], or none.
    std::optional<Hit> intersect(glm::dvec3 origin_m, glm::dvec3 direction, double t_min = 0.0,
                                 double t_max = std::numeric_limits<double>::infinity()) const;

    // Whether anything lies along the ray at a distance in [t_min, t_max]; faster than
    // intersect, for shadow rays.
    bool occluded(glm::dvec3 origin_m, glm::dvec3 direction, double t_min, double t_max) const;

    // The world's instances, materials and lights, as the scene was built from them.
    const world::World& world() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// How far a ray leaving a surface at point_m starts from it, along the surface's normal, so that
// single-precision rounding cannot make it hit the surface it leaves: 1e-5 times the point's
// largest coordinate, and at least 1e-5 m. That is about 100 float ulps at the point and 0.1 mm
// at 10 m from the origin, far below the smallest gap in the scenes (a work hung flat sits 1 mm
// off its wall, world/compile.hpp).
double ray_offset_m(glm::dvec3 point_m);

} // namespace cs::engine
