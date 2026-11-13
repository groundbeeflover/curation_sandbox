// SPDX-License-Identifier: Apache-2.0

// Feeds arbitrary bytes to the EXR reader: it may refuse them with std::runtime_error, but must
// never crash, leak, read out of bounds or allocate beyond its limits.

#include <cs/core/image_io.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        const cs::Image image =
            cs::read_exr_from_memory(std::span(reinterpret_cast<const std::byte*>(data), size));
        // Touch every pixel, so a reader that returns an image larger than its data is caught.
        volatile float sum = 0.0f;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                sum = sum + image.at(x, y).r + image.at(x, y).g + image.at(x, y).b;
            }
        }
    } catch (const std::runtime_error&) {
        // Refused, as hostile input should be.
    }
    return 0;
}
