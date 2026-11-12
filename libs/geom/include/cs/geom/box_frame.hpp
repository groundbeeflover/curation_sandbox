// SPDX-License-Identifier: Apache-2.0

#pragma once

// The plain frame of milestone 1: a rectangular ring of members with a square-cornered section,
// around a work. Profiled mouldings arrive with M2-05, mats and glazing with M2-07. Conventions as
// in mesh.hpp.

#include <cs/geom/mesh.hpp>

#include <glm/vec2.hpp>

namespace cs::geom {

struct BoxFrame {
    double work_width_m = 0.0;   // > 0: the unframed work
    double work_height_m = 0.0;  // > 0
    double member_width_m = 0.0; // > 0: the width of each member, seen from the front
    double depth_m = 0.0;        // > 0: from the frame's back to its front
    double rebate_m = 0.0;       // >= 0: how far the frame overlaps the work on each side; 5 to
                                 // 10 mm in practice (ADR 0003)
};

// The part of the work seen through the frame, as (width, height) in metres: the work less twice
// the rebate on each axis.
glm::dvec2 sight_opening(const BoxFrame& frame);

// The frame's outside, as (width, height) in metres: the sight opening plus twice the member
// width on each axis.
glm::dvec2 outer_size(const BoxFrame& frame);

// The frame, in its own coordinates (on_wall_transform in wall_face.hpp hangs it): x to the right
// seen from the front, y up and z out of the front, with the origin at the centre of its back,
// where the work's back lies too. So it spans z = 0 to depth_m, the outer size in x and y, less
// the sight opening.
//
// A closed mesh whose normals face outwards: the front and the back, each a ring of four members
// mitred at the corners, the four outer sides, and the four inner sides, which face into the sight
// opening. UVs are in metres.
//
// Throws std::invalid_argument if the work's size, the member width or the depth is not positive,
// the rebate is negative, the sight opening is empty (twice the rebate at least the work's width
// or height), or a value is not finite.
Mesh box_frame(const BoxFrame& frame);

} // namespace cs::geom
