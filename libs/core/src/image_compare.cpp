// SPDX-License-Identifier: Apache-2.0

#include <cs/core/image_compare.hpp>

#include <FLIP.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace cs {
namespace {

std::string size_text(const Image& image) {
    return std::to_string(image.width()) + " x " + std::to_string(image.height());
}

// FLIP's colour transforms and tone mapper assume light: negative, NaN and infinite values
// would spread through its filters, so it sees them as 0.
float light(float v) {
    return std::isfinite(v) && v > 0.0f ? v : 0.0f;
}

FLIP::image<FLIP::color3> to_flip(const Image& image) {
    FLIP::image<FLIP::color3> out(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const Rgb& rgb = image.at(x, y);
            out.set(x, y, FLIP::color3(light(rgb.r), light(rgb.g), light(rgb.b)));
        }
    }
    return out;
}

// HDR-FLIP's exposure range, as its evaluate() finds it from the reference when none is given:
// from the exposure that maps the brightest pixel to 0.85 after tone mapping to the one that
// does so for the median. A black image has no brightest pixel (the start is infinite, and
// evaluate() would end the process), so then the range comes from the other image. FLIP takes the
// median as at least FLT_EPSILON, so an image darker than that would get a stop before its start;
// it is given the single exposure of its brightest pixel instead.
bool find_exposures(FLIP::image<FLIP::color3>& image, FLIP::Parameters& parameters) {
    float start = 0.0f;
    float stop = 0.0f;
    image.computeExposures(parameters.tonemapper, start, stop);
    if (!std::isfinite(start) || !std::isfinite(stop)) {
        return false;
    }
    parameters.startExposure = start;
    parameters.stopExposure = std::max(start, stop);
    return true;
}

// FLIP's error at every pixel, in [0, 1]: what mean_flip averages and flip_heat_map shows.
FLIP::image<float> flip_errors(const Image& reference, const Image& test) {
    FLIP::image<FLIP::color3> flip_reference = to_flip(reference);
    FLIP::image<FLIP::color3> flip_test = to_flip(test);
    FLIP::image<float> errors(reference.width(), reference.height(), 0.0f);

    FLIP::Parameters parameters; // The tool's defaults: 67 pixels per degree, ACES.
    if (!find_exposures(flip_reference, parameters) && !find_exposures(flip_test, parameters)) {
        // Both are black once FLIP's inputs are made light, so they look the same.
        return errors;
    }
    FLIP::evaluate(flip_reference, flip_test, true, parameters, errors);
    return errors;
}

double mean_flip(const Image& reference, const Image& test) {
    const FLIP::image<float> errors = flip_errors(reference, test);
    double sum = 0.0;
    for (int y = 0; y < errors.getHeight(); ++y) {
        for (int x = 0; x < errors.getWidth(); ++x) {
            sum += errors.get(x, y);
        }
    }
    return sum / (static_cast<double>(errors.getWidth()) * errors.getHeight());
}

void check_same_size(const Image& reference, const Image& test) {
    if (reference.width() != test.width() || reference.height() != test.height()) {
        throw std::invalid_argument("cannot compare images of different sizes: the reference is " +
                                    size_text(reference) + ", the test " + size_text(test));
    }
}

// The sRGB transfer function's inverse, for FLIP's magma map, which is defined in sRGB.
float srgb_to_linear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

} // namespace

ImageComparison compare(const Image& reference, const Image& test) {
    check_same_size(reference, test);

    // In double, so that sums of float differences are exact for small images and NaN carries.
    double sum = 0.0;
    double max = 0.0;
    for (int y = 0; y < reference.height(); ++y) {
        for (int x = 0; x < reference.width(); ++x) {
            const Rgb& a = reference.at(x, y);
            const Rgb& b = test.at(x, y);
            for (const double d :
                 {std::abs(double{a.r} - double{b.r}), std::abs(double{a.g} - double{b.g}),
                  std::abs(double{a.b} - double{b.b})}) {
                sum += d; // Every d is >= 0 or NaN, so a NaN carries into the sum.
                max = d > max ? d : max;
            }
        }
    }

    ImageComparison result;
    const double samples = 3.0 * reference.width() * reference.height();
    result.mean_absolute_error = sum / samples;
    result.max_error = std::isnan(sum) ? std::numeric_limits<double>::quiet_NaN() : max;
    result.mean_flip = mean_flip(reference, test);
    return result;
}

Image flip_heat_map(const Image& reference, const Image& test) {
    check_same_size(reference, test);
    const FLIP::image<float> errors = flip_errors(reference, test);

    constexpr int entries = 256;
    Image heat_map(reference.width(), reference.height());
    for (int y = 0; y < heat_map.height(); ++y) {
        for (int x = 0; x < heat_map.width(); ++x) {
            // As FLIP's own image::colorMap picks the entry. FLIP's errors are in [0, 1]; the
            // clamp keeps anything else, NaN included, inside the table.
            const float error = errors.get(x, y);
            const float clamped = error > 0.0f ? std::min(error, 1.0f) : 0.0f;
            const int i = static_cast<int>(clamped * float(entries - 1) + 0.5f);
            const FLIP::color3& srgb = FLIP::MapMagma[i];
            heat_map.at(x, y) =
                Rgb{srgb_to_linear(srgb.r), srgb_to_linear(srgb.g), srgb_to_linear(srgb.b)};
        }
    }
    return heat_map;
}

} // namespace cs
