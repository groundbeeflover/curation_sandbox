// SPDX-License-Identifier: Apache-2.0

#include <cs/geom/wall_face.hpp>

#include "build.hpp"

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <stdexcept>

namespace cs::geom {

FaceFrame wall_face_frame(glm::dvec2 start_m, glm::dvec2 end_m, double thickness_m, WallFace face) {
    constexpr const char* function = "wall_face_frame";
    detail::require_finite(start_m.x, function, "the start's x");
    detail::require_finite(start_m.y, function, "the start's y");
    detail::require_finite(end_m.x, function, "the end's x");
    detail::require_finite(end_m.y, function, "the end's y");
    detail::require_non_negative(thickness_m, function, "the thickness");

    const glm::dvec2 along = end_m - start_m;
    const double length_m = glm::length(along);
    if (!(length_m > 0.0 && std::isfinite(length_m))) {
        throw std::invalid_argument("geom::wall_face_frame: the wall's start and end must differ");
    }
    const glm::dvec2 direction = along / length_m;
    const glm::dvec2 left{-direction.y, direction.x}; // face A's side, seen from above
    const double half_thickness_m = thickness_m / 2.0;

    FaceFrame frame{};
    frame.v = {0.0, 0.0, 1.0};
    frame.length_m = length_m;
    if (face == WallFace::a) {
        // Seen facing face A, the wall's end is on the left, so u runs back towards the start.
        frame.origin = {end_m + left * half_thickness_m, 0.0};
        frame.u = {-direction, 0.0};
        frame.normal = {left, 0.0};
    } else {
        frame.origin = {start_m - left * half_thickness_m, 0.0};
        frame.u = {direction, 0.0};
        frame.normal = {-left, 0.0};
    }
    return frame;
}

glm::dmat4 on_wall_transform(const FaceFrame& face, const WallPlacement& at) {
    constexpr const char* function = "on_wall_transform";
    detail::require_finite(at.u_m, function, "u");
    detail::require_finite(at.v_m, function, "v");
    detail::require_non_negative(at.stand_off_m, function, "the stand-off");

    // Columns: where the thing's x, y and z axes go, then where its origin goes.
    return glm::dmat4(glm::dvec4(face.u, 0.0), glm::dvec4(face.v, 0.0),
                      glm::dvec4(face.normal, 0.0),
                      glm::dvec4(face.point(at.u_m, at.v_m, at.stand_off_m), 1.0));
}

} // namespace cs::geom
