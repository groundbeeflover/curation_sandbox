#include <cs/core/image.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>

namespace cs {

Image::Image(int width, int height) : width_(width), height_(height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("image size must be positive, got " + std::to_string(width) +
                                    " x " + std::to_string(height));
    }
    pixels_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
}

Rgb& Image::at(int x, int y) {
    assert(x >= 0 && x < width_ && y >= 0 && y < height_);
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                   static_cast<std::size_t>(x)];
}

const Rgb& Image::at(int x, int y) const {
    assert(x >= 0 && x < width_ && y >= 0 && y < height_);
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                   static_cast<std::size_t>(x)];
}

void Image::write_ppm(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path.string() + " for writing");
    }

    // Header: "P6", the size, and the largest channel value. Then raw RGB bytes, row by row.
    file << "P6\n" << width_ << ' ' << height_ << "\n255\n";
    for (const Rgb& pixel : pixels_) {
        const unsigned char bytes[3] = {linear_to_srgb8(pixel.r), linear_to_srgb8(pixel.g),
                                        linear_to_srgb8(pixel.b)};
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    if (!file) {
        throw std::runtime_error("failed while writing " + path.string());
    }
}

unsigned char linear_to_srgb8(float linear) {
    // Clamp to [0, 1]. NaN fails every comparison, so it also ends up as 0 here.
    // Path tracers produce NaN when something divides by zero, so a stray black
    // pixel in a render is worth investigating.
    float v = linear > 0.0f ? std::min(linear, 1.0f) : 0.0f;

    // The sRGB transfer function: screens expect this curve, not linear values.
    v = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;

    return static_cast<unsigned char>(std::lround(v * 255.0f));
}

}  // namespace cs
