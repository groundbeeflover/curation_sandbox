// SPDX-License-Identifier: Apache-2.0

#include "room_walls.hpp"

#include <cs/geom/wall_face.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace cs::world::detail {

geom::Mesh room_walls(std::vector<glm::dvec2> outline, double floor_z_m, double ceiling_z_m) {
    if (!std::isfinite(floor_z_m) || !std::isfinite(ceiling_z_m) || !(ceiling_z_m > floor_z_m)) {
        throw std::invalid_argument("room_walls: the ceiling must be above the floor");
    }
    // Twice the signed area (the shoelace formula): positive for an anticlockwise outline.
    double twice_area = 0.0;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::dvec2 a = outline[i];
        const glm::dvec2 b = outline[(i + 1) % outline.size()];
        twice_area += a.x * b.y - b.x * a.y;
    }
    if (outline.size() < 3 || !std::isfinite(twice_area) || twice_area == 0.0) {
        throw std::invalid_argument("room_walls: the outline encloses no area");
    }
    if (twice_area < 0.0) {
        std::ranges::reverse(outline); // anticlockwise, so that the inside is on each side's left
    }

    geom::Mesh mesh;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::dvec2 start = outline[i];
        const glm::dvec2 end = outline[(i + 1) % outline.size()];
        // Face A of a wall along this side faces the room, and its left end is the side's end.
        // Throws for equal or non-finite points.
        const geom::FaceFrame face = geom::wall_face_frame(start, end, 0.0, geom::WallFace::a);
        // Corners straight from the outline, so neighbouring walls share them bit for bit.
        const auto first = static_cast<std::uint32_t>(mesh.positions.size());
        mesh.positions.insert(mesh.positions.end(),
                              {glm::dvec3(end, floor_z_m), glm::dvec3(start, floor_z_m),
                               glm::dvec3(start, ceiling_z_m), glm::dvec3(end, ceiling_z_m)});
        mesh.normals.insert(mesh.normals.end(), 4, face.normal);
        mesh.uvs.insert(mesh.uvs.end(),
                        {glm::dvec2(0.0, floor_z_m), glm::dvec2(face.length_m, floor_z_m),
                         glm::dvec2(face.length_m, ceiling_z_m), glm::dvec2(0.0, ceiling_z_m)});
        mesh.indices.insert(mesh.indices.end(),
                            {first, first + 1, first + 2, first, first + 2, first + 3});
    }
    return mesh;
}

} // namespace cs::world::detail
