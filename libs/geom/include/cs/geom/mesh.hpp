// SPDX-License-Identifier: Apache-2.0

#pragma once

// Triangle meshes, as this module makes them and the world module (M1-13) reads them, and the
// measurements tests make of them.
//
// Conventions, for every mesh of this module:
// - Positions are in metres, z up, as in the document (doc/components.hpp).
// - Triangles wind counter-clockwise seen from their front, the side their normals point to.
// - Normals are per vertex and of unit length. Each flat face has its own vertices, so that the
//   edges between faces stay sharp.
// - UVs are in metres along the surface, unless a function says otherwise. Each face's u and v
//   axes form a right-handed frame with its normal (u x v = normal), so that a texture reads
//   unmirrored seen from the front.
// - Two vertices are the same point only when their positions are exactly equal. Corners that
//   faces share are computed once and repeated bit for bit, so that closed meshes are watertight
//   for a ray tracer, too.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace cs::geom {

struct Mesh {
    std::vector<glm::dvec3> positions;  // metres
    std::vector<glm::dvec3> normals;    // unit length; one per position
    std::vector<glm::dvec2> uvs;        // one per position
    std::vector<std::uint32_t> indices; // three per triangle, into positions
};

// Adds the vertices and triangles of `from` to `to`.
void append(Mesh& to, const Mesh& from);

// The total area of the mesh's triangles, in m^2.
double area(const Mesh& mesh);

// The volume the mesh encloses, in m^3, with a sign: positive when its triangles face outwards,
// negative when they face inwards (as the inside of a room does). Meaningful only for a closed
// mesh.
double volume(const Mesh& mesh);

// Whether the mesh is closed: every edge is shared by exactly two triangles, and they run along
// it in opposite directions, so that the surface has a consistent inside and outside. Vertices
// are matched by position, exactly, not by index. A mesh with no triangles, a degenerate triangle
// (two equal corners), an index out of range or a position that is not finite is not closed.
bool is_closed(const Mesh& mesh);

} // namespace cs::geom
