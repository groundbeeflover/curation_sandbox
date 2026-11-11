// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <vector>

namespace cs {

// A colour in linear light: values are proportional to the amount of light,
// not gamma-encoded for a screen. 1.0 is not a maximum; light can be brighter.
struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// An image of linear RGB values, stored row by row from the top-left corner.
// The path tracer accumulates light into an Image; conversion to 8-bit values
// happens only when a file is written.
class Image {
  public:
    // Creates a black image. Throws std::invalid_argument if a size is not positive.
    Image(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    // The pixel in column x and row y. In debug builds, an assert checks the bounds.
    Rgb& at(int x, int y);
    const Rgb& at(int x, int y) const;

    // Writes a binary PPM file (P6), the simplest image format there is: a short
    // text header followed by raw bytes. Applies the sRGB curve and clamps to [0, 1].
    // Throws std::runtime_error if the file cannot be written.
    void write_ppm(const std::filesystem::path& path) const;

  private:
    int width_;
    int height_;
    std::vector<Rgb> pixels_;
};

// Converts one linear channel value to an 8-bit sRGB value for display.
unsigned char linear_to_srgb8(float linear);

} // namespace cs
