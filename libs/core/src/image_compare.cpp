// SPDX-License-Identifier: Apache-2.0

#include <cs/core/image_compare.hpp>

#include <FLIP.h>

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
// evaluate() would end the process), so then the range comes from the other image.
bool find_exposures(FLIP::image<FLIP::color3>& image, FLIP::Parameters& parameters) {
    float start = 0.0f;
    float stop = 0.0f;
    image.computeExposures(parameters.tonemapper, start, stop);
    if (!std::isfinite(start) || !std::isfinite(stop) || start > stop) {
        return false;
    }
    parameters.startExposure = start;
    parameters.stopExposure = stop;
    return true;
}

double mean_flip(const Image& reference, const Image& test) {
    FLIP::image<FLIP::color3> flip_reference = to_flip(reference);
    FLIP::image<FLIP::color3> flip_test = to_flip(test);

    FLIP::Parameters parameters; // The tool's defaults: 67 pixels per degree, ACES.
    if (!find_exposures(flip_reference, parameters) && !find_exposures(flip_test, parameters)) {
        // Both are black once FLIP's inputs are made light, so they look the same.
        return 0.0;
    }

    FLIP::image<float> error_map(reference.width(), reference.height(), 0.0f);
    FLIP::evaluate(flip_reference, flip_test, true, parameters, error_map);

    double sum = 0.0;
    for (int y = 0; y < error_map.getHeight(); ++y) {
        for (int x = 0; x < error_map.getWidth(); ++x) {
            sum += error_map.get(x, y);
        }
    }
    return sum / (static_cast<double>(error_map.getWidth()) * error_map.getHeight());
}

} // namespace

ImageComparison compare(const Image& reference, const Image& test) {
    if (reference.width() != test.width() || reference.height() != test.height()) {
        throw std::invalid_argument("cannot compare images of different sizes: the reference is " +
                                    size_text(reference) + ", the test " + size_text(test));
    }

    // In double, so that sums of float differences are exact for small images and NaN carries.
    double sum = 0.0;
    double max = 0.0;
    bool any_nan = false;
    for (int y = 0; y < reference.height(); ++y) {
        for (int x = 0; x < reference.width(); ++x) {
            const Rgb& a = reference.at(x, y);
            const Rgb& b = test.at(x, y);
            for (const double d :
                 {std::abs(double{a.r} - double{b.r}), std::abs(double{a.g} - double{b.g}),
                  std::abs(double{a.b} - double{b.b})}) {
                sum += d;
                any_nan = any_nan || std::isnan(d);
                max = d > max ? d : max;
            }
        }
    }

    ImageComparison result;
    const double samples = 3.0 * reference.width() * reference.height();
    result.mean_absolute_error = sum / samples;
    result.max_error = any_nan ? std::numeric_limits<double>::quiet_NaN() : max;
    result.mean_flip = mean_flip(reference, test);
    return result;
}

} // namespace cs
