// SPDX-License-Identifier: Apache-2.0

#include <cs/world/ids.hpp>

#include <cstddef>

namespace cs::world {

std::string Id::to_string() const {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text += '-';
        }
        text += digits[bytes_[i] >> 4];
        text += digits[bytes_[i] & 0x0f];
    }
    return text;
}

} // namespace cs::world
