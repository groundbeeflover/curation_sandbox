// SPDX-License-Identifier: Apache-2.0

#pragma once

// The reference BRDF interface
// ----------------------------
// A BRDF f(wo, wi) says how much of the light arriving from direction wi leaves towards wo, per
// steradian: radiance out = integral of f(wo, wi) L(wi) cos(theta_i) over the incoming
// directions. The calculation engine samples it; the GPU tiers mirror it and are tested against
// it.
//
// Directions are in a local shading frame: +z is the surface normal, and both wo (towards the
// viewer) and wi (towards the light) point away from the surface and are unit length. The caller
// rotates them in from the world. Only reflection exists here: a direction below the surface
// (z <= 0) gets no value and no density. Colours are linear Rec. 709 RGB.
//
// Physically Based Rendering, 4th edition, chapter 9, defines the quantities.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace cs::material {

struct BrdfSample {
    glm::dvec3 wi{0.0, 0.0, 1.0}; // the sampled incoming direction, unit length
    double pdf = 0.0;             // its density per steradian; 0 when no direction was sampled
    // f(wo, wi) cos(theta_i) / pdf: what one sample contributes to an estimate of the reflected
    // radiance, per unit of incoming radiance. Zero when pdf is zero.
    glm::dvec3 weight{0.0};
};

class Brdf {
  public:
    virtual ~Brdf() = default;

    // f(wo, wi), per steradian, for each colour channel.
    virtual glm::dvec3 eval(const glm::dvec3& wo, const glm::dvec3& wi) const = 0;

    // An incoming direction for wo from two uniform numbers u in [0, 1]^2 (cs::Rng), with
    // pdf == pdf(wo, wi). When wo is below the surface, nothing is sampled: pdf and weight are 0.
    virtual BrdfSample sample(const glm::dvec3& wo, glm::dvec2 u) const = 0;

    // The density with which sample(wo, u) picks wi, per steradian.
    virtual double pdf(const glm::dvec3& wo, const glm::dvec3& wi) const = 0;

  protected:
    Brdf() = default;
    Brdf(const Brdf&) = default;
    Brdf& operator=(const Brdf&) = default;
};

// A perfectly matte surface: it scatters the same radiance in every direction, f = albedo / pi.
// The albedo is the fraction of the light it reflects, per channel, in [0, 1]; above 1 it would
// create energy. Sampled by cos(theta) / pi, so every sample's weight is the albedo.
class LambertianBrdf final : public Brdf {
  public:
    explicit LambertianBrdf(const glm::dvec3& albedo) : albedo_(albedo) {}

    const glm::dvec3& albedo() const { return albedo_; }

    glm::dvec3 eval(const glm::dvec3& wo, const glm::dvec3& wi) const override;
    BrdfSample sample(const glm::dvec3& wo, glm::dvec2 u) const override;
    double pdf(const glm::dvec3& wo, const glm::dvec3& wi) const override;

  private:
    glm::dvec3 albedo_;
};

} // namespace cs::material
