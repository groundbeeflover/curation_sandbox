// SPDX-License-Identifier: Apache-2.0

#include <cs/geom/room.hpp>

#include <cs/geom/wall_face.hpp>

#include "build.hpp"

#include <array>
#include <cstddef>

namespace cs::geom {

Mesh room_mesh(double width_m, double depth_m, double height_m) {
    constexpr const char* function = "room_mesh";
    detail::require_positive(width_m, function, "the width");
    detail::require_positive(depth_m, function, "the depth");
    detail::require_positive(height_m, function, "the height");

    // Every corner is built from these values alone, so faces share corners bit for bit.
    const double w = width_m;
    const double d = depth_m;
    const double h = height_m;
    const std::array<glm::dvec2, 4> outline{{{0.0, 0.0}, {w, 0.0}, {w, d}, {0.0, d}}};

    Mesh mesh;
    // The floor faces up; seen from above the outline runs anticlockwise.
    detail::add_planar_quad(
        mesh, {{{outline[0], 0.0}, {outline[1], 0.0}, {outline[2], 0.0}, {outline[3], 0.0}}},
        {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {outline[0], 0.0});
    // The ceiling faces down, so seen from below it runs the other way, and v runs along -y.
    detail::add_planar_quad(mesh,
                            {{{outline[0], h}, {outline[3], h}, {outline[2], h}, {outline[1], h}}},
                            {1.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {outline[3], h});
    // Each wall is face A of a wall along its side of the outline, whose inside is on the left.
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::dvec2 start = outline[i];
        const glm::dvec2 end = outline[(i + 1) % outline.size()];
        const FaceFrame face = wall_face_frame(start, end, 0.0, WallFace::a);
        // Seen from inside: the face's left end (the side's end) at the bottom left, anticlockwise.
        detail::add_planar_quad(mesh, {{{end, 0.0}, {start, 0.0}, {start, h}, {end, h}}}, face.u,
                                face.v, face.origin);
    }
    return mesh;
}

} // namespace cs::geom
