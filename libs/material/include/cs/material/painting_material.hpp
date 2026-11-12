// SPDX-License-Identifier: Apache-2.0

#pragma once

// The material of a painting's surface (ADR 0003)
// -----------------------------------------------
// One parameter set for every tier: the paint (base), its varnish (coat), gilding (metal), the
// powdery surface of pastel and charcoal (fuzz), and the glass in front of the work (glazing).
// The engine evaluates it as a subset of OpenPBR Surface v1.1; the real-time tiers as the
// glTF-equivalent layers.
//
// For now only the base is evaluated, as a Lambertian layer: PaintingBrdf ignores every other
// parameter. GGX, varnish, gilding and glazing arrive in milestone 3.
//
// Each layer is off by default (weight 0, no glazing), so a default PaintingMaterial is plain
// unvarnished paint; its other values are the ADR's, ready for when the layer is switched on.
// Where the ADR gives no value, OpenPBR's default is used. Every number is marked as in the ADR:
// [B] derived from measurements through a model, [C] an educated guess, [OpenPBR] its default.
// Values the ADR states without a source are marked [C] until one is found.
//
// Roughness is GGX perceptual roughness r in [0, 1] (alpha = r^2). Colours are linear Rec. 709.

#include <cs/material/brdf.hpp>

#include <glm/vec3.hpp>

namespace cs::material {

struct PaintingMaterial {
    // The paint. Its colour comes from the photograph, converted to linear albedo (clamped to
    // 0.02 to 0.90 on import).
    struct Base {
        glm::dvec3 color{0.8};   // [OpenPBR]
        double roughness = 0.45; // [B] oil paint (presets 1, 2 and 4)
        // The paint's own highlight. Set it to 0 under a coat: the paint/varnish boundary is
        // index-matched, and leaving it on adds a false second highlight.
        double specular_weight = 1.0; // [OpenPBR]
    } base;

    // Varnish.
    struct Coat {
        double weight = 0.0;     // 0: unvarnished
        double roughness = 0.18; // [B] gloss varnish, preset 1
        // 1.5 in the real-time tier, as glTF fixes it. Darkening is always off: the photograph
        // already shows the varnish's saturation and yellowing.
        double ior = 1.52; // [C]
    } coat;

    // Gilding, inside a mask.
    struct Metal {
        double weight = 0.0;                       // the mask: 1 on gilding
        glm::dvec3 f0{1.059, 0.773, 0.307};        // [C] gold, reflectance at normal incidence
        glm::dvec3 edge_tint{1.001, 0.985, 0.523}; // [C] gold, at grazing angles
        double roughness = 0.21;                   // [C] burnished; oil-gilt is 0.29
    } metal;

    // Pastel and charcoal.
    struct Fuzz {
        double weight = 0.0;
        glm::dvec3 color{1.0};  // [OpenPBR]
        double roughness = 0.5; // [OpenPBR]
    } fuzz;

    // A thin-walled glass sheet in front of the work, part of the frame. The lux meter reads
    // behind it, at the work's surface.
    struct Glazing {
        bool present = false;
        double ior = 1.52; // [B] float glass: 8.2% reflectance from its two surfaces
        // 1 for plain glass; 0.12 [C] for anti-reflective glass, under 1% reflectance.
        double reflectance_scale = 1.0;
        glm::dvec3 transmittance{0.89}; // [C] float glass, a vendor figure; slightly green
    } glazing;
};

// The BRDF of a painting's surface at one point, built from its material. The engine makes one
// per shading point, with the base colour read from the photograph at that point.
class PaintingBrdf final : public Brdf {
  public:
    explicit PaintingBrdf(const PaintingMaterial& material) : base_(material.base.color) {}

    glm::dvec3 eval(const glm::dvec3& wo, const glm::dvec3& wi) const override {
        return base_.eval(wo, wi);
    }
    BrdfSample sample(const glm::dvec3& wo, glm::dvec2 u) const override {
        return base_.sample(wo, u);
    }
    double pdf(const glm::dvec3& wo, const glm::dvec3& wi) const override {
        return base_.pdf(wo, wi);
    }

  private:
    LambertianBrdf base_;
};

} // namespace cs::material
