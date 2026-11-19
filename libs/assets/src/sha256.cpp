// SPDX-License-Identifier: Apache-2.0

#include "sha256.hpp"

#include <algorithm>
#include <bit>

namespace cs::assets {

namespace {

// K, the first 32 bits of the fractional parts of the cube roots of the first 64 primes
// (FIPS 180-4 section 4.2.2).
constexpr std::array<std::uint32_t, 64> k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// The functions of section 4.1.2.
constexpr std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (~x & z);
}
constexpr std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
constexpr std::uint32_t big_sigma0(std::uint32_t x) {
    return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}
constexpr std::uint32_t big_sigma1(std::uint32_t x) {
    return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}
constexpr std::uint32_t small_sigma0(std::uint32_t x) {
    return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
}
constexpr std::uint32_t small_sigma1(std::uint32_t x) {
    return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10);
}

} // namespace

// The computation of section 6.2.2 on one block: the message schedule, then 64 rounds.
void Sha256::compress(const std::byte* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t t = 0; t < 16; ++t) { // the words are big-endian
        w[t] = std::to_integer<std::uint32_t>(block[4 * t]) << 24 |
               std::to_integer<std::uint32_t>(block[4 * t + 1]) << 16 |
               std::to_integer<std::uint32_t>(block[4 * t + 2]) << 8 |
               std::to_integer<std::uint32_t>(block[4 * t + 3]);
    }
    for (std::size_t t = 16; t < 64; ++t) {
        w[t] = small_sigma1(w[t - 2]) + w[t - 7] + small_sigma0(w[t - 15]) + w[t - 16];
    }

    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t t = 0; t < 64; ++t) {
        const std::uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + k[t] + w[t];
        const std::uint32_t t2 = big_sigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    const std::array<std::uint32_t, 8> working = {a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < 8; ++i) {
        state_[i] += working[i];
    }
}

void Sha256::update(std::span<const std::byte> bytes) {
    message_bytes_ += bytes.size();
    while (!bytes.empty()) {
        const std::size_t taken = std::min(bytes.size(), block_.size() - block_bytes_);
        std::copy_n(bytes.begin(), taken,
                    block_.begin() + static_cast<std::ptrdiff_t>(block_bytes_));
        block_bytes_ += taken;
        bytes = bytes.subspan(taken);
        if (block_bytes_ == block_.size()) {
            compress(block_.data());
            block_bytes_ = 0;
        }
    }
}

std::string Sha256::digest() const {
    // Padding (section 5.1.1), on a copy so that the message may go on: a 1 bit, zeros up to 8
    // bytes short of a block, then the length in bits, 64 bits big-endian.
    Sha256 padded = *this;
    const std::uint64_t message_bits = message_bytes_ * 8;
    const std::byte one{0x80};
    padded.update(std::span(&one, 1));
    const std::byte zero{0};
    while (padded.block_bytes_ != 56) {
        padded.update(std::span(&zero, 1));
    }
    std::array<std::byte, 8> length{};
    for (std::size_t i = 0; i < 8; ++i) {
        length[i] = static_cast<std::byte>(message_bits >> (56 - 8 * i));
    }
    padded.update(length);

    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const std::uint32_t word : padded.state_) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            text.push_back(hex[(word >> shift) & 0xf]);
        }
    }
    return text;
}

std::string sha256(std::span<const std::byte> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.digest();
}

} // namespace cs::assets
