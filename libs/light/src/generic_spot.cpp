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

} // namespace

GenericSpot::GenericSpot(double lumens, double beam_angle_rad, double field_angle_rad)
    : lumens_(lumens), beam_angle_rad_(beam_angle_rad), field_angle_rad_(field_angle_rad),
      exponent_(0.0), peak_cd_(0.0) {
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

    const double half_beam = beam_angle_rad / 2.0;
    const double log_ratio = std::log(field_angle_rad / beam_angle_rad);
    if (!(log_ratio > 0.0)) { // equal angles, or so close that their ratio rounds to 1
        peak_cd_ = photometry::uniform_cone_intensity(lumens, half_beam);
        check_peak();
        return;
    }
    // 2^-(r^p) = 1/10 at r = field / beam.
    exponent_ = std::log(std::log2(10.0)) / log_ratio;

    // The solid angle the spot would cover at its peak intensity, 2 pi times the integral of
    // relative_intensity(theta) sin(theta) over theta. Inside the half beam angle, Simpson's rule
    // in theta; beyond it, in ln(theta), where the falloff is smooth and evenly resolved for any
    // ratio of the angles. The integral stops at pi, or sooner where the intensity has fallen
    // below 2^-1000 of the peak and what is left cannot be told from rounding.
    const auto in_theta = [this](double theta) {
        return relative_intensity(theta) * std::sin(theta);
    };
    const auto in_log_theta = [&in_theta](double u) {
        const double theta = std::exp(u);
        return in_theta(theta) * theta;
    };
    const double end = std::min(pi, half_beam * std::pow(1000.0, 1.0 / exponent_));
    double integral = simpson(in_theta, 0.0, half_beam, 1024);
    if (end > half_beam) {
        integral += simpson(in_log_theta, std::log(half_beam), std::log(end), 4096);
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
    const double half_beam = beam_angle_rad_ / 2.0;
    if (exponent_ == 0.0) {
        return theta_rad <= half_beam ? 1.0 : 0.0;
    }
    return std::exp2(-std::pow(theta_rad / half_beam, exponent_));
}

double GenericSpot::intensity_cd(glm::dvec3 direction) const {
    const double theta = std::atan2(std::hypot(direction.x, direction.y), direction.z);
    return peak_cd_ * relative_intensity(theta);
}

} // namespace cs::light
