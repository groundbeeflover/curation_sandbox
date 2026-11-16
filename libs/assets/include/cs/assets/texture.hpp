// SPDX-License-Identifier: Apache-2.0

#pragma once

// Image import as a linear texture
// --------------------------------
// A work's image arrives as a PNG or a JPEG, whose values are sRGB-encoded for a screen. The
// renderer needs linear light (CLAUDE.md), so load_texture decodes the file with stb_image and
// converts every value with the exact sRGB curve (IEC 61966-2-1), not a 2.2 gamma: 8-bit 188
// becomes 0.50289. Colour profiles are not read yet (M2-09): every file is taken as sRGB. Alpha
// is dropped, grey becomes R = G = B.
//
// Then it builds the mip chain, each level half the size of the one before (rounded down, never
// below 1) until 1 x 1. A level is the area average of the one above, in linear light: a pixel
// of an odd-sized level that straddles two pixels of the next takes its share of each. Averaging
// sRGB values instead would darken every fine pattern: a checkerboard of black and white becomes
// linear 0.5, which is sRGB 188, not sRGB 128 (linear 0.216). Nothing is sharpened (ADR 0005).
//
// Files from outside are hostile: the size in the header is checked before anything is decoded.

#include <cs/core/image.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace cs::assets {

// The largest image side, in pixels, that load_texture accepts.
inline constexpr int max_texture_side = 16384;

// The largest decoded image, in bytes: the full-size level as linear float RGB, 12 bytes a pixel
// (the mips add a third more). It bounds stb_image's own buffers too, which hold at most 8 bytes
// a pixel each. 512 MiB is, for example, 8192 x 5461 pixels.
inline constexpr std::int64_t max_texture_decoded_bytes = std::int64_t{512} << 20;

// The largest file load_texture reads, in bytes. A PNG or JPEG of an image within the limits
// above is far smaller.
inline constexpr std::uintmax_t max_texture_file_bytes = std::uintmax_t{512} << 20;

// A linear RGB image and its mip chain. levels[0] is the image at full size; each next level is
// half the size, rounded down and at least 1, and the last is 1 x 1.
struct Texture {
    std::vector<Image> levels;

    int width() const { return levels.front().width(); }
    int height() const { return levels.front().height(); }
};

// Loads a PNG (8 or 16 bits a channel) or a JPEG, converts it to linear light and builds its
// mip chain. Throws std::runtime_error, with a message saying why, if the file cannot be read,
// is larger than max_texture_file_bytes, is not a PNG or JPEG stb_image can decode, or its image
// is wider or taller than max_texture_side or larger than max_texture_decoded_bytes decoded.
Texture load_texture(const std::filesystem::path& path);
Texture load_texture_from_memory(std::span<const std::byte> bytes);

// Converts one sRGB-encoded value in [0, 1] to linear light with the exact sRGB curve.
double srgb_to_linear(double encoded);

// The number of levels in the mip chain of a width x height image: floor(log2(max side)) + 1.
// A 1000 x 600 image has 10. Throws std::invalid_argument if a size is not positive.
int mip_level_count(int width, int height);

// The mip chain of a linear image, levels[0] being the image itself.
std::vector<Image> build_mip_chain(Image image);

} // namespace cs::assets
