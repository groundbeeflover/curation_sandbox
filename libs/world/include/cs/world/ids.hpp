// SPDX-License-Identifier: Apache-2.0

#pragma once

// The values the render world takes from the document, as its own types: the engine and the GPU
// tiers read the world and never see the document (blueprint section 7), so nothing here names a
// cs::doc type. compile() copies them.

#include <array>
#include <compare>
#include <cstdint>
#include <string>

namespace cs::world {

// The id of an entity or a work of the document: the same 128 bits, so that a result (a lux
// reading, a problem) can be traced back to what the curator made.
class Id {
  public:
    using Bytes = std::array<std::uint8_t, 16>;

    constexpr Id() = default;
    constexpr explicit Id(const Bytes& bytes) : bytes_(bytes) {}

    const Bytes& bytes() const { return bytes_; }

    // The canonical UUID text, lowercase: "01a1cd0d-dc00-7000-8000-000000000007".
    std::string to_string() const;

    // Byte by byte, as the document orders its ids.
    auto operator<=>(const Id&) const = default;

  private:
    Bytes bytes_{};
};

// A file in the project's assets/ folder, named by its content (ADR 0007): the hash is the
// asset's id. The world refers to images this way and never loads them; the renderers ask the
// texture cache (libs/assets) for them.
struct AssetRef {
    std::string sha256;    // 64 lowercase hexadecimal digits
    std::string extension; // "png", "jpg", "tif", "exr"
    bool operator==(const AssetRef&) const = default;
};

} // namespace cs::world
