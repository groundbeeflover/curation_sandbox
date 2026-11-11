// SPDX-License-Identifier: Apache-2.0

#pragma once

// Identifiers of works and entities: UUIDs (RFC 9562). New ones are version 7, which begin with
// the time they were made in milliseconds, so sorting ids sorts things in the order they were
// created, and a project file that lists entities by id lists them in that order too.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace cs::doc {

// A 128-bit UUID. The default value is the nil UUID (all zero), which never names anything.
class Uuid {
  public:
    using Bytes = std::array<std::uint8_t, 16>;

    constexpr Uuid() = default;
    constexpr explicit Uuid(const Bytes& bytes) : bytes_(bytes) {}

    // Reads the canonical text form, 8-4-4-4-12 hexadecimal digits with hyphens
    // ("0190f2a4-5b6c-7d8e-9f01-23456789abcd"), in either case. Anything else, including braces,
    // missing hyphens or surrounding spaces, gives nullopt.
    static std::optional<Uuid> parse(std::string_view text);

    // The canonical text form, lowercase, 36 characters.
    std::string to_string() const;

    const Bytes& bytes() const { return bytes_; }

    // The version field (the high nibble of byte 6): 7 for ids made by IdGenerator.
    int version() const { return bytes_[6] >> 4; }

    bool is_nil() const { return *this == Uuid{}; }

    // Byte by byte, which for version 7 ids is the order of creation. The text forms sort the
    // same way.
    auto operator<=>(const Uuid&) const = default;

  private:
    Bytes bytes_{};
};

// Milliseconds since the Unix epoch, from the system clock.
std::int64_t unix_time_ms();

// Makes version 7 UUIDs (RFC 9562, section 5.7). Each id holds the 48-bit Unix time in
// milliseconds, then a 74-bit counter that starts at a random value in each new millisecond and
// counts up within it (RFC 9562, section 6.2, methods 1 and 2). Every id is therefore greater
// than the one before, even when many are made in the same millisecond or the clock steps
// backwards: then the generator keeps the last time it saw and goes on counting.
//
// The random bits come from a generator seeded with `seed`, so a test with a fixed seed and a
// fixed clock gets the same ids every time. Two generators with different seeds make different
// ids even in the same millisecond, with overwhelming probability.
class IdGenerator {
  public:
    // Returns the current time in milliseconds since the Unix epoch.
    using Clock = std::function<std::int64_t()>;

    explicit IdGenerator(std::uint64_t seed, Clock clock = unix_time_ms);

    // A new id, greater than every id this generator has made before. Throws std::overflow_error
    // only if 2^74 ids are asked for in the last millisecond that 48 bits can hold (year 10889).
    Uuid next();

  private:
    Clock clock_;
    std::uint64_t rng_state_;
    std::int64_t last_ms_ = -1;
    std::uint64_t high_ = 0; // the first 8 bytes of the last id
    std::uint64_t low_ = 0;  // the last 8 bytes of the last id

    std::uint64_t random();
    void start_millisecond(std::int64_t ms);
};

} // namespace cs::doc

template <> struct std::hash<cs::doc::Uuid> {
    std::size_t operator()(const cs::doc::Uuid& id) const noexcept;
};
