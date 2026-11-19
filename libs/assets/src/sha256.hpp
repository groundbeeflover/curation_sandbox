// SPDX-License-Identifier: Apache-2.0

#pragma once

// SHA-256
// -------
// The hash that names an asset in a project (ADR 0007), as FIPS 180-4 defines it, written here
// rather than taken from a library (docs/dependencies.md). Private to libs/assets: store_image
// (project_assets.hpp) uses it. The message arrives in pieces of any size, so a file is hashed as
// it is read; the digest is the same however its bytes are split.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace cs::assets {

class Sha256 {
  public:
    // Adds the next bytes of the message.
    void update(std::span<const std::byte> bytes);

    // The digest of the bytes added so far, as 64 lowercase hexadecimal digits: the form
    // doc::AssetRef and asset_file take. Bytes may still be added afterwards.
    std::string digest() const;

  private:
    void compress(const std::byte* block); // one 64-byte block

    // H(0), FIPS 180-4 section 5.3.3.
    std::array<std::uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::byte, 64> block_{}; // the message bytes not yet compressed
    std::size_t block_bytes_ = 0;
    std::uint64_t message_bytes_ = 0;
};

// The digest of a whole message.
std::string sha256(std::span<const std::byte> bytes);

} // namespace cs::assets
