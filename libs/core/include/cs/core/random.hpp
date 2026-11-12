// SPDX-License-Identifier: Apache-2.0

#pragma once

// Seeded random numbers that give the same render whatever the thread count
// -------------------------------------------------------------------------
// A render must not depend on which thread draws which pixel, or in which order. So nothing
// keeps one generator for a whole image: every pixel sample makes its own,
//
//     cs::Rng rng(cs::hash_seed(scene_seed, pixel_x, pixel_y, sample, dimension));
//
// and draws from it in a fixed order. The seed is a hash of where the numbers are used, a
// counter, rather than of when they are asked for. `dimension` keeps apart uses within one
// sample that must not share numbers, such as the camera and the lux meter.
//
// These are the only random numbers the project uses (CLAUDE.md, Testing rules); never
// std::random_device, the time or an address.

#include <bit>
#include <cstdint>

namespace cs {

namespace detail {

// The finaliser of SplitMix64 (Steele, Lea and Flood, 2014; constants from Stafford's
// "variant 13"): a bijection on 64 bits in which each input bit flips about half the output bits.
constexpr std::uint64_t mix64(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

// 2^64 divided by the golden ratio: keeps a zero input from mixing to zero.
inline constexpr std::uint64_t golden_gamma = 0x9e3779b97f4a7c15;

} // namespace detail

// The seed for one pixel sample's generator. For one scene seed, different pixels never share
// a seed, and within one pixel different (sample, dimension) pairs never do: each step below is
// a bijection of the value it adds.
constexpr std::uint64_t hash_seed(std::uint64_t scene_seed, std::uint32_t pixel_x,
                                  std::uint32_t pixel_y, std::uint32_t sample,
                                  std::uint32_t dimension) {
    std::uint64_t h = detail::mix64(scene_seed + detail::golden_gamma);
    h = detail::mix64(h + detail::golden_gamma + (std::uint64_t{pixel_y} << 32 | pixel_x));
    h = detail::mix64(h + detail::golden_gamma + (std::uint64_t{dimension} << 32 | sample));
    return h;
}

// 32 random bits as a float in [0, 1): the top 24 bits, which a float holds exactly, so the
// largest value is 1 - 2^-24 and never rounds up to 1.
constexpr float unit_float(std::uint32_t bits) {
    return static_cast<float>(bits >> 8) * 0x1p-24f;
}

// 32 random bits as a double in [0, 1), in steps of 2^-32. A double holds all 32 bits exactly,
// so the largest value is 1 - 2^-32.
constexpr double unit_double(std::uint32_t bits) {
    return static_cast<double>(bits) * 0x1p-32;
}

// PCG32 (O'Neill, 2014, pcg-random.org): 64 bits of state, 32-bit outputs by a xorshift and a
// random rotation (XSH RR), a period of 2^64 on each of 2^63 streams. Small, fast and good
// enough for Monte Carlo; it is the reference pcg32_random_r, so its outputs can be checked
// against the reference library.
class Rng {
  public:
    // The increment of the reference library's PCG32_INITIALIZER, used here as a stream id (the
    // initseq of pcg32_srandom_r), so the increment in use is this shifted left, plus one. Any
    // value selects a valid stream; this one is fixed because the pinned test outputs depend on it.
    static constexpr std::uint64_t default_stream = 0xda3e39cb94b95bdb;

    // As pcg32_srandom_r(seed, stream). Always pass hash_seed(...) as the seed: raw seeds that
    // differ only in their high bits give correlated sequences. The stream is for tests against
    // the reference.
    constexpr explicit Rng(std::uint64_t seed, std::uint64_t stream = default_stream)
        : increment_(stream << 1 | 1) {
        next_u32();
        state_ += seed;
        next_u32();
    }

    constexpr std::uint32_t next_u32() {
        const std::uint64_t old = state_;
        state_ = old * multiplier + increment_;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18) ^ old) >> 27);
        const auto rotation = static_cast<int>(old >> 59);
        return std::rotr(xorshifted, rotation);
    }

    // Uniform in [0, 1), one output each; see unit_float and unit_double.
    constexpr float next_float() { return unit_float(next_u32()); }
    constexpr double next_double() { return unit_double(next_u32()); }

  private:
    static constexpr std::uint64_t multiplier = 6364136223846793005;

    std::uint64_t state_ = 0;
    std::uint64_t increment_;
};

} // namespace cs
