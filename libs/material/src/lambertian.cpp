// SPDX-License-Identifier: Apache-2.0

#include <cs/core/sampling.hpp>
#include <cs/material/brdf.hpp>

#include <numbers>

namespace cs::material {

namespace {

// Both directions strictly above the surface. A direction in the surface plane (z == 0) is
// excluded too: it carries no projected area, and the sampler never returns one.
bool same_side_above(const glm::dvec3& wo, const glm::dvec3& wi) {
    return wo.z > 0.0 && wi.z > 0.0;
}

} // namespace

glm::dvec3 LambertianBrdf::eval(const glm::dvec3& wo, const glm::dvec3& wi) const {
    if (!same_side_above(wo, wi)) {
        return glm::dvec3(0.0);
    }
    return albedo_ * std::numbers::inv_pi;
}

// Cosine-weighted directions: pdf = cos(theta_i) / pi, so the weight f cos / pdf is the albedo
// for every sample (PBRT 4th edition, section 9.2).
BrdfSample LambertianBrdf::sample(const glm::dvec3& wo, glm::dvec2 u) const {
    if (wo.z <= 0.0) {
        return {};
    }
    const DirectionSample s = sample_cosine_hemisphere(u);
    return {.wi = s.direction, .pdf = s.pdf, .weight = albedo_};
}

double LambertianBrdf::pdf(const glm::dvec3& wo, const glm::dvec3& wi) const {
    if (!same_side_above(wo, wi)) {
        return 0.0;
    }
    return cosine_hemisphere_pdf(wi.z);
}

} // namespace cs::material
