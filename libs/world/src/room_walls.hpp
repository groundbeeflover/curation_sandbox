// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cs/geom/mesh.hpp>

#include <glm/vec2.hpp>

#include <vector>

namespace cs::world::detail {

// The walls of a room of any outline, in world coordinates: one quad per side of the outline,
// from floor_z_m to ceiling_z_m, facing into the room, with geom/mesh.hpp's conventions. Each
// side's UVs are (u, v) of face A of a wall standing outside it, as geom::room_mesh gives them:
// u from the side's left end seen from inside the room, v the height z. The outline may run
// either way round. Without a floor and ceiling the mesh is open.
//
// For rooms whose outline is not an axis-aligned rectangle, until M2-01 builds floors and
// ceilings of any shape. Throws std::invalid_argument if the outline encloses no area, two
// consecutive points are equal, the ceiling is not above the floor, or a value is not finite.
geom::Mesh room_walls(std::vector<glm::dvec2> outline, double floor_z_m, double ceiling_z_m);

} // namespace cs::world::detail
