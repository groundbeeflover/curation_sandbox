// SPDX-License-Identifier: Apache-2.0

#pragma once

// The faces of a wall, and positions on them as the document gives them: ADR 0008's OnWall
// placement measures u along a face from its left end, seen facing it, and v as the height above
// z = 0.
//
// A face's u axis, its v axis (up) and its normal (out of the face) form a right-handed frame,
// like a screen seen by someone facing the wall: u to their right, v up, the normal towards them.
// For face A, on the left of the wall's line from start to end seen from above, the left end is
// therefore the wall's end, and u runs from the end towards the start; for face B it runs from the
// start towards the end.

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace cs::geom {

// The two faces of a wall, as doc::WallFace: face A is on the left of the line from the wall's
// start to its end, seen from above; face B is on the right.
enum class WallFace { a, b };

// A frame on one face of a wall.
struct FaceFrame {
    glm::dvec3 origin; // the face's left end, seen facing it, at z = 0
    glm::dvec3 u;      // unit and horizontal: along the face, to the right seen facing it
    glm::dvec3 v;      // unit: up, (0, 0, 1)
    glm::dvec3 normal; // unit and horizontal: out of the face; cross(u, v)
    double length_m;   // the face's length

    // The point u_m along the face from its left end, at height v_m, and w_m out from the face.
    glm::dvec3 point(double u_m, double v_m, double w_m = 0.0) const {
        return origin + u * u_m + v * v_m + normal * w_m;
    }
};

// The given face of the wall whose centre line runs from start_m to end_m in plan, thickness_m
// thick. The face lies thickness_m / 2 from the centre line and is as long as it: walls are not
// joined at corners yet (M2-01). Throws std::invalid_argument if the ends are equal, or the
// thickness is negative or a value is not finite.
FaceFrame wall_face_frame(glm::dvec2 start_m, glm::dvec2 end_m, double thickness_m, WallFace face);

// Where a thing hangs on a wall face (ADR 0008's OnWall placement).
struct WallPlacement {
    double u_m = 0.0;         // along the face, from its left end seen facing it, to the centre
    double v_m = 0.0;         // the height of the centre: its z
    double stand_off_m = 0.0; // >= 0: from the face to the back of the thing
};

// The transform from a hung thing's own coordinates to the world. In its own coordinates x runs
// to the right seen from the front, y up and z out of its front, and the origin is the centre of
// its back (box_frame.hpp). The transform takes them to the face's u, v and normal, and the origin
// to the point at (u_m, v_m) on the face, stand_off_m out from it. A rotation and a translation,
// so it maps normals as it maps directions. Throws std::invalid_argument if the stand-off is
// negative or a value is not finite.
glm::dmat4 on_wall_transform(const FaceFrame& face, const WallPlacement& at);

} // namespace cs::geom
