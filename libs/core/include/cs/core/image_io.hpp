// SPDX-License-Identifier: Apache-2.0

#pragma once

// Image files: linear OpenEXR for data (renders, golden references) and 8-bit sRGB PNG for people
// to look at. An Image holds linear Rec. 709 RGB, in nits for a render; EXR keeps those values as
// they are, and only a PNG goes through exposure, tone mapping and the sRGB curve (colour.hpp), or
// through the sRGB curve alone for an image that is already display-referred, such as a heat map.
//
// EXR files from outside are hostile: read_exr checks the header against the limits below before
// it decodes any pixels, and every failure is a std::runtime_error saying what was wrong.

#include <cs/core/image.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace cs {

// The largest width or height read_exr accepts, and the most pixels in all. 16384 is the largest
// texture side Vulkan guarantees on desktop GPUs; 2^26 pixels (8192 x 8192) is 768 MiB of floats.
inline constexpr int max_image_side = 16384;
inline constexpr std::int64_t max_image_pixels = std::int64_t{1} << 26;
// The largest EXR file read_exr opens: more than any image within the limits above needs.
inline constexpr std::uintmax_t max_exr_file_bytes = std::uintmax_t{1} << 30;
// The most channels an EXR file may have, and the most bytes its decoded channels may take:
// every channel is decoded, as 32 bits a sample, before R, G and B are picked out.
inline constexpr int max_exr_channels = 64;
inline constexpr std::int64_t max_exr_decoded_bytes = std::int64_t{1} << 30;

// How write_exr stores each channel.
enum class ExrChannels {
    // 32-bit float: bit-exact, for renders that are read back as data.
    float32,
    // 16-bit half float, half the size: for golden references. About 3 significant digits
    // (relative error at most 2^-11 from 6.1e-5 up); values beyond the largest half, 65504, are
    // stored as 65504 (or -65504) so a reference stays finite. NaN stays NaN.
    half,
};

// Writes the image as a single-part, scanline OpenEXR file with channels R, G and B, compressed
// losslessly with ZIP (images smaller than 16 x 16 are stored uncompressed). Throws
// std::runtime_error if the file cannot be written, or if the image is larger than read_exr
// reads (max_image_side, max_image_pixels).
void write_exr(const Image& image, const std::filesystem::path& path,
               ExrChannels channels = ExrChannels::float32);

// Reads the R, G and B channels of a single-part, scanline OpenEXR file (half or float), ignoring
// any others, such as alpha. The image is the file's data window, whatever its origin; the display
// window is ignored, as every file write_exr writes has the two equal. Throws std::runtime_error if
// the file cannot be read, is larger than max_exr_file_bytes, is not such an EXR file (tiled,
// multi-part, deep, subsampled, or without R, G and B), or exceeds max_image_side,
// max_image_pixels, max_exr_channels or max_exr_decoded_bytes.
Image read_exr(const std::filesystem::path& path);

// The same, from the bytes of a file in memory. read_exr reads the file and calls this; the fuzz
// target calls it directly.
Image read_exr_from_memory(std::span<const std::byte> bytes);

// Writes an 8-bit sRGB PNG for display: every channel is multiplied by exposure_scale(ev100),
// tone mapped with pbr_neutral and encoded with linear_to_srgb8, the pipeline of colour.hpp.
// ev100 is the exposure of a physical camera (EV100 8 maps 307.2 nits to 1 before tone mapping).
// Throws std::invalid_argument if ev100 is not finite, and std::runtime_error if the file cannot
// be written.
void write_png(const Image& image, const std::filesystem::path& path, double ev100);

// Writes an 8-bit sRGB PNG of an image that is already display-referred linear RGB, such as a heat
// map (flip_heat_map, image_compare.hpp): every channel is encoded with linear_to_srgb8 alone,
// which clamps it to [0, 1] and takes NaN as 0, with no exposure or tone mapping. Throws
// std::runtime_error if the file cannot be written.
void write_display_png(const Image& image, const std::filesystem::path& path);

} // namespace cs
