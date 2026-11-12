// SPDX-License-Identifier: Apache-2.0

#pragma once

// How a fixture spreads its light
// -------------------------------
// A Distribution gives a fixture's luminous intensity, in candela, in every direction of its own
// space: +z is the beam axis, the way the fixture points, as in cs::sample_uniform_cone. Every
// renderer and the lux meter read lights through this one definition, so that they agree.
// Positioning a fixture in the room is the job of Luminaire (luminaire.hpp).
//
// Beam and field angles follow the IES convention: full cone angles, measured across the beam,
// at which the intensity has fallen to 50% (beam) and 10% (field) of its peak.

#include <glm/vec3.hpp>

namespace cs::light {

class Distribution {
  public:
    virtual ~Distribution() = default;

    // Intensity in candela towards direction, in fixture space. The direction need not be of
    // unit length; a zero vector counts as the beam axis, and a NaN component gives NaN.
    virtual double intensity_cd(glm::dvec3 direction) const = 0;

    // All the light the fixture emits, in lumens: intensity_cd integrated over the sphere.
    virtual double total_lumens() const = 0;

  protected:
    Distribution() = default;
    Distribution(const Distribution&) = default;
    Distribution& operator=(const Distribution&) = default;
};

// A generic spotlight, defined by what a datasheet gives: lumens, beam angle and field angle
// (full cone angles in radians). Its intensity is symmetric about the axis and falls smoothly
// with the angle theta from it, as a generalised Gaussian,
//
//     I(theta) = I_peak * 2^-((theta / half_beam)^p),   p = ln(log2(10)) / ln(field / beam),
//
// which is 50% of the peak at half the beam angle and 10% at half the field angle exactly.
//
// Beyond half the field angle the profile is multiplied by a taper, 1 - smoothstep, that falls
// from 1 to 0 at a cut-off of 1.5 times the half field angle, but at most 90 degrees from the
// axis when the field angle is 180 degrees or less. The taper is flat at both ends, so the
// intensity and its slope are continuous; nothing is emitted beyond the cut-off, and nothing
// behind a fixture whose field angle is 180 degrees or less, as with a real spot whose housing
// blocks the back. Left alone, the Gaussian's tail would put 40 to 50% of the lumens outside
// the field at field / beam ratios of 4 to 6, some of them behind the fixture. The factor 1.5
// is a judgement, not a measurement: the spill of real spots ends about that far out, and
// milestone 3's IES files should be used to check it.
//
// Near a field angle of 180 degrees the taper narrows to nothing, from both sides, so that the
// light changes continuously as the field angle passes 180: below it the cut-off is held at 90
// degrees, above it the taper's width grows three times as fast as the half field angle passes
// 90 degrees, rejoining 1.5 times the half field angle at 108 degrees, and the cut-off never
// passes 180 degrees. Within a few degrees of a 180-degree field the edge is effectively hard.
//
// I_peak is chosen so that the fixture emits exactly its lumens. The share of them inside the
// field angle is about 96% for 24 / 36 degrees, 94% for 15 / 25, 85% for 10 / 40 and 82% for
// 10 / 60. Above a field / beam ratio of about 3.3, p < 1 and the profile comes to a point on
// the axis instead of a flat top.
//
// With the beam angle equal to the field angle the spot is a uniform cone: the same intensity
// everywhere inside the cone, cs::photometry::uniform_cone_intensity, and none outside.
class GenericSpot final : public Distribution {
  public:
    // Throws std::invalid_argument unless every value is finite, lumens > 0 and
    // 0 < beam_angle_rad <= field_angle_rad <= 2 pi, and the peak intensity they give is finite
    // (which refuses absurdly small angles). A zero angle (a delta light) is refused.
    GenericSpot(double lumens, double beam_angle_rad, double field_angle_rad);

    double intensity_cd(glm::dvec3 direction) const override;
    double total_lumens() const override { return lumens_; }

    // The intensity on the axis, the brightest direction.
    double peak_intensity_cd() const { return peak_cd_; }
    double beam_angle_rad() const { return beam_angle_rad_; }
    double field_angle_rad() const { return field_angle_rad_; }

  private:
    // The intensity as a fraction of the peak, theta radians from the axis.
    double relative_intensity(double theta_rad) const;
    void check_peak() const;

    double lumens_;
    double beam_angle_rad_;
    double field_angle_rad_;
    double half_beam_rad_;
    double half_field_rad_;
    double cutoff_rad_; // nothing is emitted beyond it; zero for a uniform cone
    double exponent_;   // p; zero for a uniform cone
    double peak_cd_;
};

} // namespace cs::light
