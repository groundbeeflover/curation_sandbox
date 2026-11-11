// SPDX-License-Identifier: Apache-2.0

#include <cs/doc/uuid.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace cs::doc {

namespace {

constexpr std::size_t text_length = 36;

bool is_hyphen_position(std::size_t i) {
    return i == 8 || i == 13 || i == 18 || i == 23;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// The largest Unix time in milliseconds that fits the 48 bits of a version 7 UUID.
constexpr std::int64_t max_ms = (std::int64_t{1} << 48) - 1;

// The 74-bit counter is rand_a (12 bits, in the first 8 bytes) followed by rand_b (62 bits, in the
// last 8 bytes, after the 2 variant bits).
constexpr std::uint64_t rand_a_mask = 0x0fff;
constexpr std::uint64_t rand_b_mask = (std::uint64_t{1} << 62) - 1;
constexpr std::uint64_t version_bits = std::uint64_t{7} << 12;
constexpr std::uint64_t variant_bits = std::uint64_t{0b10} << 62;

} // namespace

std::optional<Uuid> Uuid::parse(std::string_view text) {
    if (text.size() != text_length) {
        return std::nullopt;
    }
    Bytes bytes{};
    std::size_t nibble = 0;
    for (std::size_t i = 0; i < text_length; ++i) {
        if (is_hyphen_position(i)) {
            if (text[i] != '-') {
                return std::nullopt;
            }
            continue;
        }
        const int value = hex_value(text[i]);
        if (value < 0) {
            return std::nullopt;
        }
        bytes[nibble / 2] =
            static_cast<std::uint8_t>(bytes[nibble / 2] | value << (nibble % 2 == 0 ? 4 : 0));
        ++nibble;
    }
    return Uuid(bytes);
}

std::string Uuid::to_string() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(text_length);
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text.push_back('-');
        }
        text.push_back(digits[bytes_[i] >> 4]);
        text.push_back(digits[bytes_[i] & 0x0f]);
    }
    return text;
}

std::int64_t unix_time_ms() {
    // Since C++20 the system clock's epoch is the Unix epoch.
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

IdGenerator::IdGenerator(std::uint64_t seed, Clock clock)
    : clock_(std::move(clock)), rng_state_(seed) {}

// SplitMix64 (Steele, Lea and Flood, 2014): a small, well-mixed generator for the random bits.
// The project's cs::Rng (PCG32, milestone 1 task M1-05) is for sampling; ids need no more than
// this.
std::uint64_t IdGenerator::random() {
    std::uint64_t z = (rng_state_ += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

// A new millisecond: the counter starts at a random value whose top bit is 0, which leaves at
// least 2^73 increments before it can overflow (RFC 9562, section 6.2).
void IdGenerator::start_millisecond(std::int64_t ms) {
    last_ms_ = ms;
    const std::uint64_t rand_a = random() & (rand_a_mask >> 1);
    const std::uint64_t rand_b = random() & rand_b_mask;
    high_ = static_cast<std::uint64_t>(ms) << 16 | version_bits | rand_a;
    low_ = variant_bits | rand_b;
}

Uuid IdGenerator::next() {
    const std::int64_t now = std::clamp<std::int64_t>(clock_(), 0, max_ms);
    if (now > last_ms_) {
        start_millisecond(now);
    } else {
        // The same millisecond, or the clock stepped back: count up from the last id.
        std::uint64_t rand_b = (low_ & rand_b_mask) + 1;
        std::uint64_t rand_a = high_ & rand_a_mask;
        if (rand_b > rand_b_mask) {
            rand_b = 0;
            ++rand_a;
        }
        if (rand_a > rand_a_mask) {
            // 2^74 ids in one millisecond: borrow the next millisecond (RFC 9562, section 6.2).
            start_millisecond(std::min(last_ms_ + 1, max_ms));
        } else {
            high_ = (high_ & ~rand_a_mask) | rand_a;
            low_ = variant_bits | rand_b;
        }
    }

    Uuid::Bytes bytes{};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(high_ >> (56 - 8 * i));
        bytes[8 + i] = static_cast<std::uint8_t>(low_ >> (56 - 8 * i));
    }
    return Uuid(bytes);
}

} // namespace cs::doc

std::size_t std::hash<cs::doc::Uuid>::operator()(const cs::doc::Uuid& id) const noexcept {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        high = high << 8 | id.bytes()[i];
        low = low << 8 | id.bytes()[8 + i];
    }
    return static_cast<std::size_t>(high ^ (low * 0x9e3779b97f4a7c15));
}
