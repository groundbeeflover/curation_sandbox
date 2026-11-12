// SPDX-License-Identifier: Apache-2.0

#include <cs/core/random.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <vector>

TEST_CASE("Rng is PCG32 and matches the reference implementation", "[core][random]") {
    // The first outputs of pcg32-demo from the PCG reference C library (pcg-random.org),
    // seeded with pcg32_srandom_r(&rng, 42, 54): initial state 42 on stream 54.
    cs::Rng rng(42, 54);
    const std::array<std::uint32_t, 6> expected = {0xa15c02b7, 0x7b47f409, 0xba1d3330,
                                                   0x83d2f293, 0xbfa4784b, 0xcbed606e};
    for (const std::uint32_t value : expected) {
        CHECK(rng.next_u32() == value);
    }
}

TEST_CASE("an Rng seeded by hash_seed gives fixed outputs for scene seed 42", "[core][random]") {
    // Computed by an independent Python model of hash_seed (three SplitMix64 finaliser steps)
    // and PCG32. If these change, every render and golden image changes with them.
    SECTION("pixel (0, 0), sample 0, dimension 0") {
        cs::Rng rng(cs::hash_seed(42, 0, 0, 0, 0));
        const std::array<std::uint32_t, 8> expected = {0x321f7c14, 0x01fb88e2, 0xf86c8caf,
                                                       0xd96d5886, 0x59d3b3b9, 0x16b86810,
                                                       0xd8c97130, 0x033ae589};
        for (const std::uint32_t value : expected) {
            CHECK(rng.next_u32() == value);
        }
    }
    SECTION("pixel (3, 7), sample 5, dimension 1") {
        cs::Rng rng(cs::hash_seed(42, 3, 7, 5, 1));
        const std::array<std::uint32_t, 8> expected = {0x0ba5a966, 0xca24d20f, 0xa7ce691b,
                                                       0xea033005, 0xbd31001c, 0x15d46196,
                                                       0x131eea1a, 0xf8e7bbeb};
        for (const std::uint32_t value : expected) {
            CHECK(rng.next_u32() == value);
        }
    }
}

TEST_CASE("hash_seed depends on every argument and on their order", "[core][random]") {
    const std::uint64_t base = cs::hash_seed(42, 3, 7, 5, 1);
    CHECK(cs::hash_seed(43, 3, 7, 5, 1) != base);
    CHECK(cs::hash_seed(42, 4, 7, 5, 1) != base);
    CHECK(cs::hash_seed(42, 3, 8, 5, 1) != base);
    CHECK(cs::hash_seed(42, 3, 7, 6, 1) != base);
    CHECK(cs::hash_seed(42, 3, 7, 5, 2) != base);

    // Swapping x and y, or sample and dimension, is a different stream of numbers.
    CHECK(cs::hash_seed(42, 7, 3, 5, 1) != base);
    CHECK(cs::hash_seed(42, 3, 7, 1, 5) != base);

    // All zeros still gives a well-mixed seed, not zero.
    CHECK(cs::hash_seed(0, 0, 0, 0, 0) != 0);
}

TEST_CASE("no two samples or dimensions of one pixel share a seed", "[core][random]") {
    std::set<std::uint64_t> seeds;
    for (std::uint32_t sample = 0; sample < 1024; ++sample) {
        for (std::uint32_t dimension = 0; dimension < 16; ++dimension) {
            seeds.insert(cs::hash_seed(42, 17, 99, sample, dimension));
        }
    }
    CHECK(seeds.size() == 1024 * 16);

    // Nor do the pixels of an image at the same sample.
    seeds.clear();
    for (std::uint32_t y = 0; y < 128; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            seeds.insert(cs::hash_seed(42, x, y, 0, 0));
        }
    }
    CHECK(seeds.size() == 128 * 128);
}

TEST_CASE("random numbers do not depend on the order pixels are drawn in", "[core][random]") {
    // Threads finish pixels in any order. Each pixel sample seeds its own Rng, so drawing the
    // image forwards, backwards, or with several generators alive at once gives the same values.
    constexpr std::uint32_t size = 8;
    constexpr std::uint32_t samples = 4;
    constexpr std::uint32_t draws = 5;
    const auto index = [](std::uint32_t x, std::uint32_t y, std::uint32_t s, std::uint32_t d) {
        return ((y * size + x) * samples + s) * draws + d;
    };

    std::vector<std::uint32_t> forwards(size * size * samples * draws);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            for (std::uint32_t s = 0; s < samples; ++s) {
                cs::Rng rng(cs::hash_seed(7, x, y, s, 0));
                for (std::uint32_t d = 0; d < draws; ++d) {
                    forwards[index(x, y, s, d)] = rng.next_u32();
                }
            }
        }
    }

    std::vector<std::uint32_t> backwards(forwards.size());
    for (std::uint32_t y = size; y-- > 0;) {
        for (std::uint32_t x = size; x-- > 0;) {
            for (std::uint32_t s = samples; s-- > 0;) {
                cs::Rng rng(cs::hash_seed(7, x, y, s, 0));
                for (std::uint32_t d = 0; d < draws; ++d) {
                    backwards[index(x, y, s, d)] = rng.next_u32();
                }
            }
        }
    }
    CHECK(backwards == forwards);

    // Two generators drawn alternately, as two threads might, do not disturb each other.
    cs::Rng a(cs::hash_seed(7, 0, 0, 0, 0));
    cs::Rng b(cs::hash_seed(7, 5, 3, 2, 0));
    for (std::uint32_t d = 0; d < draws; ++d) {
        CHECK(a.next_u32() == forwards[index(0, 0, 0, d)]);
        CHECK(b.next_u32() == forwards[index(5, 3, 2, d)]);
    }
}

TEST_CASE("unit floats lie in [0, 1)", "[core][random]") {
    // The conversion from 32 random bits: 0 maps to 0, and all ones stays below 1.
    CHECK(cs::unit_float(0) == 0.0f);
    CHECK(cs::unit_float(0xffffffff) < 1.0f);
    CHECK(cs::unit_double(0) == 0.0);
    CHECK(cs::unit_double(0xffffffff) < 1.0);
    CHECK(cs::unit_float(0x80000000) == 0.5f);
    CHECK(cs::unit_double(0x80000000) == 0.5);

    cs::Rng rng(cs::hash_seed(1, 0, 0, 0, 0));
    float min_f = 1.0f;
    float max_f = 0.0f;
    double min_d = 1.0;
    double max_d = 0.0;
    for (int i = 0; i < 1'000'000; ++i) {
        const float f = rng.next_float();
        const double d = rng.next_double();
        min_f = f < min_f ? f : min_f;
        max_f = f > max_f ? f : max_f;
        min_d = d < min_d ? d : min_d;
        max_d = d > max_d ? d : max_d;
    }
    CHECK(min_f >= 0.0f);
    CHECK(max_f < 1.0f);
    CHECK(min_d >= 0.0);
    CHECK(max_d < 1.0);
}

namespace {

// Pearson's chi-square statistic for draws sorted into equal bins of [0, 1).
template <typename Draw> double chi_square(Draw draw, int bins, int draws) {
    std::vector<int> counts(static_cast<std::size_t>(bins), 0);
    for (int i = 0; i < draws; ++i) {
        const auto bin = static_cast<std::size_t>(draw() * bins);
        ++counts.at(bin); // at(): a value of 1.0 would land outside the bins and throw
    }
    const double expected = static_cast<double>(draws) / bins;
    double statistic = 0.0;
    for (const int count : counts) {
        const double difference = count - expected;
        statistic += difference * difference / expected;
    }
    return statistic;
}

} // namespace

TEST_CASE("uniform floats pass a chi-square test", "[core][random]") {
    // 64 bins have 63 degrees of freedom; the 99.9% critical value of the chi-square
    // distribution with 63 degrees of freedom is 103.442. A uniform generator exceeds it one
    // time in a thousand; with a fixed seed the outcome is fixed, so this is not flaky.
    constexpr double critical_999 = 103.442;
    constexpr int bins = 64;
    constexpr int draws = 1'000'000;

    cs::Rng rng_f(cs::hash_seed(2, 0, 0, 0, 0));
    const double chi_f = chi_square([&] { return rng_f.next_float(); }, bins, draws);
    INFO("chi-square of next_float: " << chi_f);
    CHECK(chi_f < critical_999);

    cs::Rng rng_d(cs::hash_seed(2, 0, 0, 0, 1));
    const double chi_d = chi_square([&] { return rng_d.next_double(); }, bins, draws);
    INFO("chi-square of next_double: " << chi_d);
    CHECK(chi_d < critical_999);
}
