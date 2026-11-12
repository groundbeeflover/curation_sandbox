// SPDX-License-Identifier: Apache-2.0

#include <cs/core/photometry.hpp>
#include <cs/light/distribution.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace cs::light {

namespace {

constexpr double pi = std::numbers::pi;

// Composite Simpson's rule for f over [a, b] with n intervals (n even).
template <typename F> double simpson(F f, double a, double b, int n) {
    const double h = (b - a) / n;
    double sum = f(a) + f(b);
    for (int i = 1; i < n; ++i) {
        sum += f(a + i * h) * (i % 2 == 1 ? 4.0 : 2.0);
    }
    return sum * h / 3.0;
}

// Where the taper beyond the half field angle reaches zero (see the header): 1.5 times the half
// field angle, but never past 90 degrees for a field angle of 180 degrees or less; for wider
// fields the taper grows again from nothing at 90 degrees, three times as fast as the half field
// angle passes it, until it rejoins 1.5 times at 108 degrees, and stops at 180 degrees.
double taper_cutoff_rad(double half_field_rad) {
    if (half_field_rad <= pi / 2.0) {
        return std::min(1.5 * half_field_rad, pi / 2.0);
    }
    return std::min({1.5 * half_field_rad, pi, half_field_rad + 3.0 * (half_field_rad - pi / 2.0)});
}

} // namespace

GenericSpot::GenericSpot(double lumens, double beam_angle_rad, double field_angle_rad)
    : lumens_(lumens), beam_angle_rad_(beam_angle_rad), field_angle_rad_(field_angle_rad),
      half_beam_rad_(beam_angle_rad / 2.0), half_field_rad_(field_angle_rad / 2.0),
      cutoff_rad_(0.0), exponent_(0.0), peak_cd_(0.0) {
    // Written so that NaN fails every test.
    if (!(std::isfinite(lumens) && lumens > 0.0)) {
        throw std::invalid_argument("GenericSpot: lumens must be positive and finite");
    }
    if (!(std::isfinite(beam_angle_rad) && beam_angle_rad > 0.0)) {
        throw std::invalid_argument("GenericSpot: the beam angle must be positive and finite");
    }
    if (!(field_angle_rad >= beam_angle_rad && field_angle_rad <= 2.0 * pi)) {
        throw std::invalid_argument(
            "GenericSpot: the field angle must lie between the beam angle and 2 pi");
    }

    const double half_beam = half_beam_rad_;
    const double log_ratio = std::log(field_angle_rad / beam_angle_rad);
    if (!(log_ratio > 0.0)) { // equal angles, or so close that their ratio rounds to 1
        peak_cd_ = photometry::uniform_cone_intensity(lumens, half_beam);
        check_peak();
        return;
    }
    // 2^-(r^p) = 1/10 at r = field / beam.
    exponent_ = std::log(std::log2(10.0)) / log_ratio;
    cutoff_rad_ = taper_cutoff_rad(half_field_rad_);

    // The solid angle the spot would cover at its peak intensity, 2 pi times the integral of
    // relative_intensity(theta) sin(theta) over theta, in three parts. Near the axis, where the
    // profile is almost flat (above 2^-(1/1000)), Simpson's rule in theta; from there to the
    // half field angle, in ln(theta), where the falloff is smooth and evenly resolved for any
    // ratio of the angles, however sharp the edge; and the taper, in theta again, from the
    // half field angle to the cut-off, beyond which nothing is emitted.
    const auto in_theta = [this](double theta) {
        return relative_intensity(theta) * std::sin(theta);
    };
    const auto in_log_theta = [&in_theta](double u) {
        const double theta = std::exp(u);
        return in_theta(theta) * theta;
    };
    // For small p the flat part shrinks towards nothing; below 1e-6 of the half beam angle the
    // area is too small to matter, and the floor keeps ln(start) finite.
    const double start = half_beam * std::max(std::pow(1000.0, -1.0 / exponent_), 1e-6);
    double integral = simpson(in_theta, 0.0, start, 1024);
    integral += simpson(in_log_theta, std::log(start), std::log(half_field_rad_), 4096);
    if (cutoff_rad_ > half_field_rad_) {
        integral += simpson(in_theta, half_field_rad_, cutoff_rad_, 4096);
    }
    peak_cd_ = lumens / (2.0 * pi * integral);
    check_peak();
}

void GenericSpot::check_peak() const {
    // Angles so small, or lumens so large, that the peak overflows a double.
    if (!std::isfinite(peak_cd_)) {
        throw std::invalid_argument("GenericSpot: the peak intensity is too large to represent");
    }
}

double GenericSpot::relative_intensity(double theta_rad) const {
    if (exponent_ == 0.0) {
        return theta_rad <= half_beam_rad_ ? 1.0 : 0.0;
    }
    const double profile = std::exp2(-std::pow(theta_rad / half_beam_rad_, exponent_));
    if (theta_rad <= half_field_rad_) {
        return profile;
    }
    if (theta_rad >= cutoff_rad_) {
        return 0.0;
    }
    // 1 - smoothstep from the half field angle to the cut-off: flat at both ends, so the
    // intensity and its slope are continuous.
    const double s = (theta_rad - half_field_rad_) / (cutoff_rad_ - half_field_rad_);
    return profile * (1.0 - s * s * (3.0 - 2.0 * s));
}

double GenericSpot::intensity_cd(glm::dvec3 direction) const {
    // A zero vector is the axis even when its z is -0.0, which atan2 would read as pi.
    const double sin_part = std::hypot(direction.x, direction.y);
    const double theta =
        sin_part == 0.0 && direction.z == 0.0 ? 0.0 : std::atan2(sin_part, direction.z);
    return peak_cd_ * relative_intensity(theta);
}

} // namespace cs::light
