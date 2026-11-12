// SPDX-License-Identifier: Apache-2.0

#pragma once

// The inside of a room: the surfaces a visitor sees. Conventions as in mesh.hpp.

#include <cs/geom/mesh.hpp>

namespace cs::geom {

// The floor, ceiling and four walls of a box room width_m along x, depth_m along y and height_m
// high, from (0, 0, 0) to (width_m, depth_m, height_m): the outline (0, 0), (width_m, 0),
// (width_m, depth_m), (0, depth_m) anticlockwise, with the floor at z = 0, as in the skeleton
// scene. The normals face into the room, so the mesh is closed and its volume() is
// -width_m * depth_m * height_m.
//
// UVs are in metres: on the floor, (x, y); on the ceiling, which faces down, (x, depth_m - y); on
// each wall, the face's (u, v) of wall_face.hpp, which are the coordinates of face A of a wall
// standing outside that side of the outline, running anticlockwise.
//
// Throws std::invalid_argument if a size is not positive or not finite.
Mesh room_mesh(double width_m, double depth_m, double height_m);

} // namespace cs::geom
