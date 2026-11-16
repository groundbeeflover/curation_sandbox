// SPDX-License-Identifier: Apache-2.0

#include <cs/engine/work_lux.hpp>

#include <cs/core/random.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace cs::engine {

namespace {

bool finite(glm::dvec2 v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

bool finite(glm::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void check(const world::Exhibit& exhibit) {
    const world::FaceRect& visible = exhibit.visible;
    if (!finite(visible.min_m) || !finite(visible.max_m) || !(visible.max_m.x > visible.min_m.x) ||
        !(visible.max_m.y > visible.min_m.y)) {
        throw std::invalid_argument("work lux: the exhibit's visible area is empty or not finite");
    }
    if (!finite(exhibit.face_centre_m) || !finite(exhibit.face_u) || !finite(exhibit.face_v) ||
        !finite(exhibit.face_normal)) {
        throw std::invalid_argument("work lux: the exhibit's face is not finite");
    }
}

// The summary of one part of the 25 readings.
template <typename Part>
LuxSummary summarise(const std::array<WorkLuxPoint, work_lux_grid_points>& points, Part part) {
    LuxSummary summary;
    summary.minimum = std::numeric_limits<double>::infinity();
    summary.maximum = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    double variance_sum = 0.0;
    for (const WorkLuxPoint& point : points) {
        const Estimate& estimate = part(point.reading);
        sum += estimate.value;
        variance_sum += estimate.standard_error * estimate.standard_error;
        summary.minimum = std::min(summary.minimum, estimate.value);
        summary.maximum = std::max(summary.maximum, estimate.value);
    }
    const double n = static_cast<double>(work_lux_grid_points);
    summary.mean = sum / n;
    summary.standard_error = std::sqrt(variance_sum) / n;
    return summary;
}

} // namespace

std::array<glm::dvec2, work_lux_grid_points> work_lux_points(const world::Exhibit& exhibit) {
    check(exhibit);
    const world::FaceRect& visible = exhibit.visible;
    const glm::dvec2 size = visible.max_m - visible.min_m;
    std::array<glm::dvec2, work_lux_grid_points> points;
    for (int row = 0; row < work_lux_grid_side; ++row) {
        for (int column = 0; column < work_lux_grid_side; ++column) {
            // Cell centres: from the left along u, and from the top, against v.
            const double u = visible.min_m.x + size.x * (column + 0.5) / work_lux_grid_side;
            const double v = visible.max_m.y - size.y * (row + 0.5) / work_lux_grid_side;
            points[static_cast<std::size_t>(row * work_lux_grid_side + column)] = {u, v};
        }
    }
    return points;
}

WorkLux work_lux_grid(const Scene& scene, const world::Exhibit& exhibit,
                      const LuxOptions& options) {
    const std::array<glm::dvec2, work_lux_grid_points> face_points = work_lux_points(exhibit);
    WorkLux result;
    for (std::size_t k = 0; k < work_lux_grid_points; ++k) {
        WorkLuxPoint& point = result.points[k];
        point.face_m = face_points[k];
        point.point_m = exhibit.face_point(point.face_m);
        LuxOptions point_options = options;
        point_options.seed = hash_seed(options.seed, static_cast<std::uint32_t>(k),
                                       exhibit.painting_instance, 0, work_lux_dimension);
        point.reading = lux_at(scene, point.point_m, exhibit.face_normal, point_options);
    }
    result.direct_lux = summarise(
        result.points, [](const LuxReading& r) -> const Estimate& { return r.direct_lux; });
    result.total_lux = summarise(
        result.points, [](const LuxReading& r) -> const Estimate& { return r.total_lux; });
    return result;
}

} // namespace cs::engine
