// SPDX-License-Identifier: Apache-2.0

#include <cs/light/luminaire.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace cs::light {

namespace {

bool is_finite(glm::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

Luminaire::Luminaire(glm::dvec3 position_m, glm::dvec3 aim_point_m,
                     std::shared_ptr<const Distribution> distribution)
    : position_m_(position_m), distribution_(std::move(distribution)) {
    if (!is_finite(position_m) || !is_finite(aim_point_m)) {
        throw std::invalid_argument("Luminaire: the position and aim point must be finite");
    }
    if (!distribution_) {
        throw std::invalid_argument("Luminaire: no distribution");
    }
    const glm::dvec3 aim = aim_point_m - position_m;
    const double distance = glm::length(aim);
    if (!(distance > 0.0)) {
        throw std::invalid_argument("Luminaire: the aim point is the position");
    }
    axis_z_ = aim / distance;

    // x level and to the left of the beam; for a vertical beam, the world's x axis.
    const glm::dvec3 up{0.0, 0.0, 1.0};
    const glm::dvec3 left = glm::cross(up, axis_z_);
    const double left_length = glm::length(left);
    axis_x_ = left_length > 1e-12 ? left / left_length
                                  : glm::normalize(glm::dvec3{1.0, 0.0, 0.0} - axis_z_ * axis_z_.x);
    axis_y_ = glm::cross(axis_z_, axis_x_);
}

glm::dvec3 Luminaire::to_fixture(glm::dvec3 world_direction) const {
    return {glm::dot(world_direction, axis_x_), glm::dot(world_direction, axis_y_),
            glm::dot(world_direction, axis_z_)};
}

double Luminaire::intensity_cd(glm::dvec3 world_direction) const {
    return distribution_->intensity_cd(to_fixture(world_direction));
}

} // namespace cs::light
