// SPDX-License-Identifier: Apache-2.0

#include <cs/assets/asset_file.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace cs::assets {

namespace {

bool is_sha256(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// "png, jpg or jpeg", from the list itself.
std::string accepted_extensions() {
    std::string text;
    for (std::size_t i = 0; i < texture_extensions.size(); ++i) {
        if (i > 0) {
            text += i + 1 == texture_extensions.size() ? " or " : ", ";
        }
        text += texture_extensions[i];
    }
    return text;
}

} // namespace

std::filesystem::path asset_file(const std::filesystem::path& folder, std::string_view sha256,
                                 std::string_view extension) {
    // The names are not quoted in the messages: they may hold anything, a NUL or a newline.
    if (!is_sha256(sha256)) {
        throw std::invalid_argument(
            "asset: the name is not a SHA-256 of 64 lowercase hexadecimal digits");
    }
    if (std::ranges::find(texture_extensions, extension) == texture_extensions.end()) {
        throw std::invalid_argument("asset: the extension is not one the image import reads (" +
                                    accepted_extensions() + ")");
    }
    std::string name;
    name.reserve(sha256.size() + 1 + extension.size());
    name.append(sha256).append(".").append(extension);
    return folder / name;
}

} // namespace cs::assets
