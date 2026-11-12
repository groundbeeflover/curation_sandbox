// SPDX-License-Identifier: Apache-2.0

#include <cs/geom/painting.hpp>

#include "build.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace cs::geom {

Mesh painting_quad(const FaceFrame& face, const FlatWork& work, const WallPlacement& at) {
    constexpr const char* function = "painting_quad";
    detail::require_positive(work.width_m, function, "the work's width");
    detail::require_positive(work.height_m, function, "the work's height");
    detail::require_non_negative(work.depth_m, function, "the work's depth");
    const glm::dmat4 to_world = on_wall_transform(face, at);

    // The front of the work in its own coordinates, at z = depth, taken to the world.
    const double x = work.width_m / 2.0;
    const double y = work.height_m / 2.0;
    const auto world = [&](double corner_x, double corner_y) {
        return glm::dvec3(to_world * glm::dvec4(corner_x, corner_y, work.depth_m, 1.0));
    };

    Mesh mesh;
    detail::add_quad(mesh, {world(-x, -y), world(x, -y), world(x, y), world(-x, y)}, face.normal,
                     {{{0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}}});
    detail::require_finite_positions(mesh, function);
    return mesh;
}

} // namespace cs::geom
