// SPDX-License-Identifier: Apache-2.0

#pragma once

// The lux on a work
// -----------------
// work_lux_grid reads the lux meter at 5 x 5 points on the face of one work and summarises them:
// the reading cs-render's lux report, the app and the lux service give for an exhibit.
//
// The points measure what a visitor can see, the exhibit's visible area (world::Exhibit::visible):
// the frame's sight opening when the work is framed (the work less its rebate on every side,
// geom::sight_opening), the whole work when it is not. The area is divided into 5 x 5 equal
// cells and each point sits at the centre of its cell, so no point lies on the edge of the area:
// the outer points are a tenth of the area's width and height in from its edges. A point under
// the frame's lip would read about 0 lux and pull the minimum and the mean down.
//
// Each point lies on the work's face, facing along its normal, as a curator holds a meter flat
// against the work: behind any glazing, which therefore filters the light the points read.
//
// Point k (k = 0 to 24) is in column k % 5 from the left of the face (along face_u) and row
// k / 5 from the top (against face_v), as a visitor reads the work. It is measured with
// lux_at and the caller's options, except for its seed: hash_seed(options.seed, k,
// painting_instance, 0, work_lux_dimension), so that the 25 indirect estimates are independent of
// one another and of other exhibits'.

#include <cs/engine/lux.hpp>
#include <cs/world/world.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace cs::engine {

// The grid's points along each side.
inline constexpr int work_lux_grid_side = 5;
inline constexpr std::size_t work_lux_grid_points = 25;

// Keeps the grid's seeds apart from other uses of the same seed, as hash_seed's `dimension`.
inline constexpr std::uint32_t work_lux_dimension = 0x776c'7801; // "wlx" and 1

// The 25 points of an exhibit's grid, in face coordinates (metres from face_centre_m along
// face_u and face_v), in the order above. Exhibit::face_point takes them to the world.
std::array<glm::dvec2, work_lux_grid_points> work_lux_points(const world::Exhibit& exhibit);

// A summary of one part of the reading (direct or total) over the 25 points, in lux.
struct LuxSummary {
    double mean = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    // The Monte Carlo standard error of the mean: sqrt(sum of the points' squared standard
    // errors) / 25, the points' estimates being independent. 0 for the direct part. It says how
    // far the mean may be from the exact mean over the 25 points, not how even the light is.
    double standard_error = 0.0;
};

struct WorkLuxPoint {
    glm::dvec2 face_m{0.0};  // in face coordinates
    glm::dvec3 point_m{0.0}; // in the world
    LuxReading reading;
};

struct WorkLux {
    std::array<WorkLuxPoint, work_lux_grid_points> points;
    LuxSummary direct_lux;
    LuxSummary total_lux;
};

// Reads the lux meter at the exhibit's 25 points. Throws std::invalid_argument as lux_at does,
// or if the exhibit's visible area is empty or not finite, or its face is not finite.
WorkLux work_lux_grid(const Scene& scene, const world::Exhibit& exhibit,
                      const LuxOptions& options = {});

} // namespace cs::engine
