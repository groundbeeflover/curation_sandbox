// SPDX-License-Identifier: Apache-2.0

#include <cs/assets/texture.hpp>

// STATIC keeps stb_image's functions inside this file, so a test or another module that also
// includes the implementation does not collide with it. Only the two decoders the import needs
// are compiled, and stb reads from memory only: the file is read here, with its size bounded.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace cs::assets {

namespace {

// A limit for a message, in MiB: the limits are whole MiB.
std::string mib(std::uintmax_t bytes) {
    return std::to_string(bytes >> 20) + " MiB";
}

struct StbiFree {
    void operator()(void* p) const { stbi_image_free(p); }
};

// Downsamples along one axis by area averaging: destination pixel i covers the source interval
// [i * scale, (i + 1) * scale), scale = src_size / dst_size, and each source pixel counts by how
// much of it lies inside. When the size halves exactly, that is the 2-pixel box filter.
Image downsample_axis(const Image& src, int dst_width, int dst_height) {
    const bool along_x = dst_width != src.width();
    const int src_size = along_x ? src.width() : src.height();
    const int dst_size = along_x ? dst_width : dst_height;
    const double scale = static_cast<double>(src_size) / dst_size;

    Image dst(dst_width, dst_height);
    for (int i = 0; i < dst_size; ++i) {
        const double begin = i * scale;
        const double end = (i + 1) * scale;
        const int first = static_cast<int>(begin);
        const int last = std::min(static_cast<int>(std::ceil(end)), src_size);
        for (int j = 0; j < (along_x ? dst_height : dst_width); ++j) {
            double r = 0.0;
            double g = 0.0;
            double b = 0.0;
            for (int s = first; s < last; ++s) {
                const double w =
                    (std::min<double>(s + 1, end) - std::max<double>(s, begin)) / scale;
                const Rgb& p = along_x ? src.at(s, j) : src.at(j, s);
                r += w * p.r;
                g += w * p.g;
                b += w * p.b;
            }
            Rgb& out = along_x ? dst.at(i, j) : dst.at(j, i);
            out = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
        }
    }
    return dst;
}

Image downsample(const Image& src) {
    const int w = std::max(src.width() / 2, 1);
    const int h = std::max(src.height() / 2, 1);
    if (w == src.width()) {
        return downsample_axis(src, w, h);
    }
    const Image half_x = downsample_axis(src, w, src.height());
    return h == src.height() ? half_x : downsample_axis(half_x, w, h);
}

// Takes ownership of stb_image's RGB pixels (null when decoding failed) and converts them.
template <typename T> Image to_linear(T* pixels, int width, int height) {
    const std::unique_ptr<T, StbiFree> owned(pixels);
    if (!owned) {
        throw std::runtime_error(std::string("cannot decode the image: ") + stbi_failure_reason());
    }
    constexpr int levels = T(~T(0)) + 1;
    // One table entry per code: 256 for 8 bits, 65536 for 16.
    static const auto table = [] {
        auto t = std::make_unique<std::array<float, levels>>();
        for (int v = 0; v < levels; ++v) {
            (*t)[static_cast<std::size_t>(v)] =
                static_cast<float>(srgb_to_linear(static_cast<double>(v) / (levels - 1)));
        }
        return t;
    }();

    Image image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const T* p = pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) *
                                      3;
            image.at(x, y) = {(*table)[p[0]], (*table)[p[1]], (*table)[p[2]]};
        }
    }
    return image;
}

} // namespace

double srgb_to_linear(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

int mip_level_count(int width, int height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("image size must be positive, got " + std::to_string(width) +
                                    " x " + std::to_string(height));
    }
    return std::bit_width(static_cast<unsigned>(std::max(width, height)));
}

std::vector<Image> build_mip_chain(Image image) {
    const int count = mip_level_count(image.width(), image.height());
    std::vector<Image> levels;
    levels.reserve(static_cast<std::size_t>(count));
    levels.push_back(std::move(image));
    for (int i = 1; i < count; ++i) {
        levels.push_back(downsample(levels.back()));
    }
    return levels;
}

Texture load_texture_from_memory(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > max_texture_file_bytes) {
        throw std::runtime_error("an image file must hold 1 byte to " +
                                 mib(max_texture_file_bytes) + ", this one holds " +
                                 std::to_string(bytes.size()) + " bytes");
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int size = static_cast<int>(bytes.size());

    // Check the size in the header before decoding anything.
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(data, size, &width, &height, &channels) == 0) {
        throw std::runtime_error(std::string("not a PNG or JPEG image that can be read: ") +
                                 stbi_failure_reason());
    }
    if (width > max_texture_side || height > max_texture_side) {
        throw std::runtime_error("the image is " + std::to_string(width) + " x " +
                                 std::to_string(height) + " pixels; at most " +
                                 std::to_string(max_texture_side) + " are allowed on a side");
    }
    const std::int64_t decoded = std::int64_t{width} * height * 3 * std::int64_t{sizeof(float)};
    if (decoded > max_texture_decoded_bytes) {
        throw std::runtime_error("the image is " + std::to_string(width) + " x " +
                                 std::to_string(height) + " pixels, " + std::to_string(decoded) +
                                 " bytes decoded as linear float RGB; at most " +
                                 mib(max_texture_decoded_bytes) + " are allowed");
    }

    // stb_image sets w and h; they are read in a statement of their own, after the call.
    int w = 0;
    int h = 0;
    if (stbi_is_16_bit_from_memory(data, size) != 0) {
        stbi_us* pixels = stbi_load_16_from_memory(data, size, &w, &h, &channels, 3);
        return Texture{build_mip_chain(to_linear(pixels, w, h))};
    }
    stbi_uc* pixels = stbi_load_from_memory(data, size, &w, &h, &channels, 3);
    return Texture{build_mip_chain(to_linear(pixels, w, h))};
}

Texture load_texture(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error("cannot read " + path.string() + ": " + error.message());
    }
    if (file_size > max_texture_file_bytes) {
        throw std::runtime_error(path.string() + " is " + std::to_string(file_size) +
                                 " bytes; image files of at most " + mib(max_texture_file_bytes) +
                                 " are read");
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read " + path.string());
    }
    try {
        return load_texture_from_memory(bytes);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
}

} // namespace cs::assets
