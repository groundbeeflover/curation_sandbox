// SPDX-License-Identifier: Apache-2.0

// Feeds arbitrary bytes to the PNG and JPEG importer: it may refuse them with std::runtime_error,
// but must never crash, leak, read out of bounds or allocate beyond its limits.

#include <cs/assets/texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        const cs::assets::Texture texture = cs::assets::load_texture_from_memory(
            std::span(reinterpret_cast<const std::byte*>(data), size));
        // Touch the first and last level's pixels, so an image larger than its data is caught.
        volatile float sum = 0.0f;
        for (const cs::Image* level : {&texture.levels.front(), &texture.levels.back()}) {
            for (int y = 0; y < level->height(); ++y) {
                for (int x = 0; x < level->width(); ++x) {
                    sum = sum + level->at(x, y).r + level->at(x, y).g + level->at(x, y).b;
                }
            }
        }
    } catch (const std::runtime_error&) {
        // Refused, as hostile input should be.
    }
    return 0;
}
