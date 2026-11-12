// SPDX-License-Identifier: Apache-2.0

#pragma once

// The painted face of a flat work, at its true size, hung on a wall. Conventions as in mesh.hpp.

#include <cs/geom/mesh.hpp>
#include <cs/geom/wall_face.hpp>

namespace cs::geom {

// The size of a flat work (doc::FlatForm), unframed.
struct FlatWork {
    double width_m = 0.0;  // > 0
    double height_m = 0.0; // > 0
    double depth_m = 0.0;  // >= 0: a canvas's stretcher or a panel's thickness; 0 for paper
};

// The front of the work hung at `at` on `face`, in world coordinates: a rectangle width_m wide and
// height_m high, centred at (u_m, v_m) on the face, depth_m in front of the work's back, which is
// stand_off_m out from the face. Its normal is the face's: it points into the room. It is the
// rectangle at z = depth_m in the work's own coordinates, taken to the world by
// on_wall_transform(face, at).
//
// Four vertices, the bottom-left, bottom-right, top-right and top-left corners seen from the
// front, in that order, and two triangles. Its UVs are not metres but image coordinates: (0, 0)
// at the top-left corner of the image and (1, 1) at the bottom-right, u to the right and v down,
// as images are stored (core/image.hpp) and Vulkan samples them.
//
// Throws std::invalid_argument if the width or height is not positive, the depth or stand-off is
// negative, or a value or a corner is not finite.
Mesh painting_quad(const FaceFrame& face, const FlatWork& work, const WallPlacement& at);

} // namespace cs::geom
