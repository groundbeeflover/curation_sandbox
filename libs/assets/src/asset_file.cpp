// SPDX-License-Identifier: Apache-2.0

#include <cs/assets/asset_file.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace cs::assets {

namespace {

// The extensions of the images load_texture decodes, PNG and JPEG: the only ones asset_file
// accepts, for every tier. TIFF decoding (M2-32) adds "tif" and "tiff".
constexpr std::array<std::string_view, 3> texture_extensions = {"png", "jpg", "jpeg"};

bool is_sha256(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
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
        std::string accepted; // "png, jpg, jpeg", from the list itself
        for (std::string_view known : texture_extensions) {
            accepted.append(accepted.empty() ? "" : ", ").append(known);
        }
        throw std::invalid_argument("asset: the extension is not one the image import reads (" +
                                    accepted + ")");
    }
    std::string name;
    name.reserve(sha256.size() + 1 + extension.size());
    name.append(sha256).append(".").append(extension);
    return folder / name;
}

} // namespace cs::assets
