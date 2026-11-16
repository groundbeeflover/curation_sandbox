// SPDX-License-Identifier: Apache-2.0

#pragma once

// What cs-render's two commands share: reading numbers from the command line, loading a project
// folder, and rendering its first viewpoint with the path tracer.

#include <cs/core/image.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/doc/document.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/textures.hpp>
#include <cs/world/world.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cs_render {

// Exit codes. A usage error also prints the usage text.
inline constexpr int exit_failure = 1;
inline constexpr int exit_usage = 2;

// Bounds on what the command line and a golden test may ask for.
inline constexpr std::uint32_t max_spp = 1u << 20;
inline constexpr unsigned max_threads = 1024;

// A mistake on the command line: exit code 2 and the usage text.
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <typename T> T parse_number(std::string_view flag, std::string_view text, T low, T high) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || value < low || value > high) {
        throw UsageError(std::string(flag) + " takes a whole number from " + std::to_string(low) +
                         " to " + std::to_string(high) + ", not \"" + std::string(text) + "\"");
    }
    return value;
}

// The threads to render on: the number asked for, or the machine's when that is 0.
unsigned thread_count(unsigned requested);

// A project folder, compiled and with its photographs loaded.
struct Project {
    cs::doc::Document document;
    cs::world::World world;
    cs::engine::LoadedTextures textures;

    // The viewpoint cs-render renders: the first.
    const cs::world::Camera& camera() const { return world.cameras.front(); }
};

// Loads and compiles the project folder and loads the photographs in its assets/, printing each
// of the world's and the photographs' problems as a warning on stderr. Throws if the folder cannot
// be loaded or the project has no viewpoint.
Project load_project(const std::filesystem::path& folder);

struct ImageSettings {
    int width = 256;  // pixels
    int height = 144; // pixels
    std::uint32_t samples_per_pixel = 64;
    std::uint64_t seed = 1;
};

// The first viewpoint, path traced: linear Rec. 709 radiance in nits. The scene is the project's.
cs::Image render_view(const Project& project, const cs::engine::Scene& scene,
                      const ImageSettings& settings, cs::ThreadPool& pool);

// The mean luminance of the image's finite pixels, in nits, for automatic exposure: one NaN or
// infinite pixel would otherwise decide the exposure of the whole image.
double mean_luminance(const cs::Image& image);

// The exposure a render is shown at: its viewpoint's EV100, or metered from the image's mean
// luminance when the viewpoint has none.
double display_ev100(const cs::world::Camera& camera, const cs::Image& image);

} // namespace cs_render
