// SPDX-License-Identifier: Apache-2.0

#include <cs/core/image_io.hpp>

#include <cs/core/colour.hpp>

#include <tinyexr.h>

// STATIC keeps stb_image_write's functions inside this file, so another module that also
// implements it cannot collide with these symbols at link time.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cs {
namespace {

// The largest finite half float.
constexpr float max_half = 65504.0f;

// tinyexr reports errors as strings it allocates; this frees one and returns it as a message.
std::string take_error(const char* error, std::string_view fallback) {
    if (error == nullptr) {
        return std::string(fallback);
    }
    std::string message(error);
    FreeEXRErrorMessage(error);
    return message;
}

void write_bytes(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path.string() + " for writing");
    }
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file) {
        throw std::runtime_error("failed while writing " + path.string());
    }
}

// Frees what tinyexr allocated for a parsed header and a loaded image, however reading ends.
struct ExrHeader {
    EXRHeader header{};
    ExrHeader() { InitEXRHeader(&header); }
    ~ExrHeader() { FreeEXRHeader(&header); }
    ExrHeader(const ExrHeader&) = delete;
    ExrHeader& operator=(const ExrHeader&) = delete;
};

struct ExrImage {
    EXRImage image{};
    ExrImage() { InitEXRImage(&image); }
    ~ExrImage() { FreeEXRImage(&image); }
    ExrImage(const ExrImage&) = delete;
    ExrImage& operator=(const ExrImage&) = delete;
};

[[noreturn]] void refuse(std::string_view reason) {
    throw std::runtime_error("not a readable EXR image: " + std::string(reason));
}

} // namespace

void write_exr(const Image& image, const std::filesystem::path& path, ExrChannels channels) {
    const int width = image.width();
    const int height = image.height();
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    // EXR readers expect channels in alphabetical order, so B, G, R. A half file is clamped
    // to the half range here; tinyexr then converts each float to the nearest half.
    const bool half = channels == ExrChannels::half;
    std::array<std::vector<float>, 3> planes;
    for (auto& plane : planes) {
        plane.resize(pixels);
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Rgb& rgb = image.at(x, y);
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
            const std::array<float, 3> bgr = {rgb.b, rgb.g, rgb.r};
            for (std::size_t c = 0; c < 3; ++c) {
                // std::clamp would keep NaN as NaN too, but its result is unspecified for NaN.
                const float v = bgr[c];
                planes[c][i] = half && !std::isnan(v) ? std::clamp(v, -max_half, max_half) : v;
            }
        }
    }

    std::array<unsigned char*, 3> plane_pointers{};
    for (std::size_t c = 0; c < 3; ++c) {
        plane_pointers[c] = reinterpret_cast<unsigned char*>(planes[c].data());
    }

    EXRImage exr_image;
    InitEXRImage(&exr_image);
    exr_image.num_channels = 3;
    exr_image.images = plane_pointers.data();
    exr_image.width = width;
    exr_image.height = height;

    std::array<EXRChannelInfo, 3> infos{};
    constexpr std::array<const char*, 3> names = {"B", "G", "R"};
    const int stored = half ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
    std::array<int, 3> input_types{};
    std::array<int, 3> stored_types{};
    for (std::size_t c = 0; c < 3; ++c) {
        std::strncpy(infos[c].name, names[c], sizeof(infos[c].name) - 1);
        infos[c].pixel_type = stored;
        infos[c].x_sampling = 1;
        infos[c].y_sampling = 1;
        input_types[c] = TINYEXR_PIXELTYPE_FLOAT;
        stored_types[c] = stored;
    }

    EXRHeader header;
    InitEXRHeader(&header);
    header.num_channels = 3;
    header.channels = infos.data();
    header.pixel_types = input_types.data();
    header.requested_pixel_types = stored_types.data();
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

    unsigned char* memory = nullptr;
    const char* error = nullptr;
    const std::size_t size = SaveEXRImageToMemory(&exr_image, &header, &memory, &error);
    // tinyexr allocates the file with malloc.
    const std::unique_ptr<unsigned char, decltype(&std::free)> owned(memory, &std::free);
    if (size == 0) {
        throw std::runtime_error("cannot encode " + path.string() +
                                 " as EXR: " + take_error(error, "unknown error"));
    }
    write_bytes(path, memory, size);
}

Image read_exr(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("cannot read " + path.string() + ": " + ec.message());
    }
    if (size > max_exr_file_bytes) {
        throw std::runtime_error("cannot read " + path.string() + ": " + std::to_string(size) +
                                 " bytes is more than the limit of " +
                                 std::to_string(max_exr_file_bytes));
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path.string() + " for reading");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("failed while reading " + path.string());
    }

    try {
        return read_exr_from_memory(bytes);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
}

Image read_exr_from_memory(std::span<const std::byte> bytes) {
    const auto* memory = reinterpret_cast<const unsigned char*>(bytes.data());

    EXRVersion version;
    if (bytes.empty() || ParseEXRVersionFromMemory(&version, memory, bytes.size()) != 0) {
        refuse("no EXR signature");
    }
    if (version.multipart != 0 || version.non_image != 0 || version.tiled != 0) {
        refuse("only single-part scanline files are read, not multi-part, deep or tiled ones");
    }

    ExrHeader header;
    const char* error = nullptr;
    if (ParseEXRHeaderFromMemory(&header.header, &version, memory, bytes.size(), &error) != 0) {
        refuse(take_error(error, "the header cannot be parsed"));
    }
    const EXRHeader& h = header.header;
    if (h.tiled != 0 || h.multipart != 0 || h.non_image != 0) {
        refuse("only single-part scanline files are read, not multi-part, deep or tiled ones");
    }

    // The size, before anything is decoded. 64-bit, as a hostile window can overflow an int.
    const std::int64_t width =
        static_cast<std::int64_t>(h.data_window.max_x) - h.data_window.min_x + 1;
    const std::int64_t height =
        static_cast<std::int64_t>(h.data_window.max_y) - h.data_window.min_y + 1;
    if (width <= 0 || height <= 0 || width > max_image_side || height > max_image_side ||
        width * height > max_image_pixels) {
        refuse("its size, " + std::to_string(width) + " x " + std::to_string(height) +
               ", is outside 1 to " + std::to_string(max_image_side) + " a side and " +
               std::to_string(max_image_pixels) + " pixels");
    }

    // tinyexr decodes every channel at 32 bits a sample (with halves widened, below).
    if (h.num_channels <= 0 || h.num_channels > max_exr_channels ||
        h.num_channels * width * height * 4 > max_exr_decoded_bytes) {
        refuse("its " + std::to_string(h.num_channels) + " channels of " + std::to_string(width) +
               " x " + std::to_string(height) + " pixels are more than " +
               std::to_string(max_exr_channels) + " channels or " +
               std::to_string(max_exr_decoded_bytes) + " bytes decoded");
    }

    // Find R, G and B; decode them (and any other half channel) as float. tinyexr does not
    // handle subsampled channels, so none may be.
    std::array<int, 3> rgb_channel = {-1, -1, -1};
    for (int c = 0; c < h.num_channels; ++c) {
        const EXRChannelInfo& info = h.channels[c];
        if (info.x_sampling != 1 || info.y_sampling != 1) {
            refuse("a channel is subsampled");
        }
        if (h.pixel_types[c] == TINYEXR_PIXELTYPE_HALF) {
            h.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
        }

        // A channel name is at most 255 characters; never trust the terminator.
        const std::string_view name(info.name, strnlen(info.name, sizeof(info.name)));
        const int index = name == "R" ? 0 : name == "G" ? 1 : name == "B" ? 2 : -1;
        if (index < 0) {
            continue;
        }
        if (rgb_channel[index] >= 0) {
            refuse("channel " + std::string(name) + " appears twice");
        }
        if (h.pixel_types[c] != TINYEXR_PIXELTYPE_HALF &&
            h.pixel_types[c] != TINYEXR_PIXELTYPE_FLOAT) {
            refuse("channel " + std::string(name) + " is not half or float");
        }
        rgb_channel[index] = c;
    }
    if (std::ranges::any_of(rgb_channel, [](int c) { return c < 0; })) {
        refuse("it has no R, G and B channels");
    }

    ExrImage exr;
    error = nullptr;
    if (LoadEXRImageFromMemory(&exr.image, &h, memory, bytes.size(), &error) != 0) {
        refuse(take_error(error, "the pixels cannot be decoded"));
    }
    if (exr.image.images == nullptr || exr.image.width != width || exr.image.height != height ||
        exr.image.num_channels != h.num_channels) {
        refuse("the decoded pixels do not match the header");
    }

    Image image(static_cast<int>(width), static_cast<int>(height));
    std::array<const float*, 3> planes{};
    for (std::size_t i = 0; i < 3; ++i) {
        planes[i] = reinterpret_cast<const float*>(exr.image.images[rgb_channel[i]]);
    }
    // Row 0 is the top of the data window, as in an Image.
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
            image.at(x, y) = Rgb{planes[0][i], planes[1][i], planes[2][i]};
        }
    }
    return image;
}

void write_png(const Image& image, const std::filesystem::path& path, double ev100) {
    const int width = image.width();
    const int height = image.height();
    const float scale = static_cast<float>(exposure_scale(ev100));

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height) * 3);
    std::size_t i = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Rgb& rgb = image.at(x, y);
            const glm::vec3 display = pbr_neutral(glm::vec3(rgb.r, rgb.g, rgb.b) * scale);
            pixels[i++] = linear_to_srgb8(display.r);
            pixels[i++] = linear_to_srgb8(display.g);
            pixels[i++] = linear_to_srgb8(display.b);
        }
    }

    std::vector<unsigned char> file;
    const auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(context);
        const auto* begin = static_cast<const unsigned char*>(data);
        out->insert(out->end(), begin, begin + size);
    };
    if (stbi_write_png_to_func(append, &file, width, height, 3, pixels.data(), width * 3) == 0) {
        throw std::runtime_error("cannot encode " + path.string() + " as PNG");
    }
    write_bytes(path, file.data(), file.size());
}

} // namespace cs
