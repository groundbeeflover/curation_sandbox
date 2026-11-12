// SPDX-License-Identifier: Apache-2.0

#include <cs/geom/box_frame.hpp>

#include "build.hpp"

#include <glm/geometric.hpp>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace cs::geom {

namespace {

glm::dvec2 unchecked_sight_opening(const BoxFrame& frame) {
    return glm::dvec2(frame.work_width_m, frame.work_height_m) - 2.0 * frame.rebate_m;
}

glm::dvec2 unchecked_outer_size(const BoxFrame& frame) {
    return unchecked_sight_opening(frame) + 2.0 * frame.member_width_m;
}

// Throws std::invalid_argument, naming the function, unless the frame can be built.
void check(const BoxFrame& frame, const char* function) {
    detail::require_positive(frame.work_width_m, function, "the work's width");
    detail::require_positive(frame.work_height_m, function, "the work's height");
    detail::require_positive(frame.member_width_m, function, "the member width");
    detail::require_positive(frame.depth_m, function, "the depth");
    detail::require_non_negative(frame.rebate_m, function, "the rebate");

    const glm::dvec2 sight = unchecked_sight_opening(frame);
    if (!(sight.x > 0.0 && sight.y > 0.0)) {
        throw std::invalid_argument(
            std::string("geom::") + function +
            ": the rebate leaves no sight opening; it must be less than half the work's width "
            "and height");
    }
    detail::require_finite_result({unchecked_outer_size(frame), frame.depth_m}, function,
                                  "the outer size");
}

} // namespace

glm::dvec2 sight_opening(const BoxFrame& frame) {
    check(frame, "sight_opening");
    return unchecked_sight_opening(frame);
}

glm::dvec2 outer_size(const BoxFrame& frame) {
    check(frame, "outer_size");
    return unchecked_outer_size(frame);
}

Mesh box_frame(const BoxFrame& frame) {
    check(frame, "box_frame");
    const glm::dvec2 inner = unchecked_sight_opening(frame) / 2.0;
    const glm::dvec2 outer = unchecked_outer_size(frame) / 2.0;

    // The corners of the opening and of the outside, anticlockwise seen from the front from the
    // bottom left, computed once so that the faces share them bit for bit.
    const std::array<glm::dvec2, 4> in{
        {{-inner.x, -inner.y}, {inner.x, -inner.y}, {inner.x, inner.y}, {-inner.x, inner.y}}};
    const std::array<glm::dvec2, 4> out{
        {{-outer.x, -outer.y}, {outer.x, -outer.y}, {outer.x, outer.y}, {-outer.x, outer.y}}};
    const double back = 0.0;
    const double front = frame.depth_m;

    const glm::dvec3 origin{0.0, 0.0, 0.0};
    const glm::dvec3 x_axis{1.0, 0.0, 0.0};
    const glm::dvec3 y_axis{0.0, 1.0, 0.0};
    const glm::dvec3 z_axis{0.0, 0.0, 1.0};

    Mesh mesh;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t j = (i + 1) % 4;
        // The member from corner i to corner j: a trapezoid on the front and on the back, mitred
        // at the corners.
        detail::add_planar_quad(
            mesh, {{{out[i], front}, {out[j], front}, {in[j], front}, {in[i], front}}}, x_axis,
            y_axis, origin);
        detail::add_planar_quad(mesh,
                                {{{in[i], back}, {in[j], back}, {out[j], back}, {out[i], back}}},
                                -x_axis, y_axis, origin);

        // Its outer side faces away from the opening, its inner side into it. Both have u along
        // the member, one way or the other, and v along z, from the back.
        const glm::dvec3 along{glm::normalize(out[j] - out[i]), 0.0};
        detail::add_planar_quad(
            mesh, {{{out[i], back}, {out[j], back}, {out[j], front}, {out[i], front}}}, along,
            z_axis, origin);
        detail::add_planar_quad(mesh,
                                {{{in[j], back}, {in[i], back}, {in[i], front}, {in[j], front}}},
                                -along, z_axis, origin);
    }
    return mesh;
}

} // namespace cs::geom
