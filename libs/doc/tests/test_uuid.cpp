// SPDX-License-Identifier: Apache-2.0

// Uuid and IdGenerator (uuid.hpp): the canonical text form, and version 7 ids (RFC 9562,
// sections 5.7 and 6.2) that sort in the order they were made.

#include <cs/doc/uuid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using cs::doc::IdGenerator;
using cs::doc::Uuid;

namespace {

// The 48-bit big-endian Unix time in milliseconds that begins a version 7 UUID (RFC 9562, 5.7).
std::int64_t unix_ms_of(const Uuid& id) {
    std::int64_t ms = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        ms = (ms << 8) | id.bytes()[i];
    }
    return ms;
}

// The variant field of RFC 9562 UUIDs: the top two bits of byte 8 are 10.
bool has_rfc9562_variant(const Uuid& id) {
    return (id.bytes()[8] & 0xC0) == 0x80;
}

// The 74 bits that are neither time, version nor variant (rand_a and rand_b), with the others
// cleared: in IdGenerator's ids, the counter of the millisecond.
Uuid counter_bits_of(const Uuid& id) {
    Uuid::Bytes bytes = id.bytes();
    for (std::size_t i = 0; i < 6; ++i) {
        bytes[i] = 0;
    }
    bytes[6] = static_cast<std::uint8_t>(bytes[6] & 0x0F);
    bytes[8] = static_cast<std::uint8_t>(bytes[8] & 0x3F);
    return Uuid(bytes);
}

// An id from text the test knows to be valid.
Uuid uuid(std::string_view text) {
    const std::optional<Uuid> id = Uuid::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

// 2024-01-01T00:00:00Z and 2100-01-01T00:00:00Z in milliseconds since the Unix epoch.
constexpr std::int64_t ms_2024 = 1'704'067'200'000;
constexpr std::int64_t ms_2100 = 4'102'444'800'000;

// A time that fits the 48 bits (0x0190f2a45b6c, in July 2024).
constexpr std::int64_t fixed_ms = 0x0190f2a45b6c;

// Each id is greater than the one before, both as a Uuid and as text. One check over the whole
// list, which stays fast for 10^5 ids under the sanitizers.
void check_strictly_increasing(const std::vector<Uuid>& ids) {
    std::vector<std::string> texts;
    texts.reserve(ids.size());
    for (const Uuid& id : ids) {
        texts.push_back(id.to_string());
    }
    const auto id_step = std::adjacent_find(ids.begin(), ids.end(), std::greater_equal<>{});
    CHECK(id_step == ids.end());
    const auto text_step = std::adjacent_find(texts.begin(), texts.end(), std::greater_equal<>{});
    if (text_step != texts.end()) {
        FAIL_CHECK("not increasing: " << *text_step << " then " << *(text_step + 1));
    }
}

} // namespace

TEST_CASE("UUID text reads to its bytes and writes back the same", "[doc][uuid]") {
    const std::string text = "0190f2a4-5b6c-7d8e-9f01-23456789abcd";
    const Uuid id = uuid(text);

    const Uuid::Bytes expected = {0x01, 0x90, 0xf2, 0xa4, 0x5b, 0x6c, 0x7d, 0x8e,
                                  0x9f, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd};
    CHECK(id.bytes() == expected);
    CHECK(id.to_string() == text);
    CHECK(id.version() == 7);
    CHECK_FALSE(id.is_nil());

    // From bytes to text and back.
    const Uuid from_bytes(expected);
    CHECK(from_bytes == id);
    CHECK(Uuid::parse(from_bytes.to_string()) == from_bytes);

    // Every byte value appears in some position, so every hex digit is read and written.
    const Uuid all_digits(Uuid::Bytes{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                                      0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff});
    CHECK(all_digits.to_string() == "00112233-4455-6677-8899-aabbccddeeff");
    CHECK(Uuid::parse("00112233-4455-6677-8899-aabbccddeeff") == all_digits);
}

TEST_CASE("UUID text reads in upper or mixed case and is written in lowercase", "[doc][uuid]") {
    const Uuid lower = uuid("0190f2a4-5b6c-7d8e-9f01-23456789abcd");
    CHECK(uuid("0190F2A4-5B6C-7D8E-9F01-23456789ABCD") == lower);
    CHECK(uuid("0190f2A4-5b6C-7D8e-9F01-23456789aBcD") == lower);
    CHECK(uuid("0190F2A4-5B6C-7D8E-9F01-23456789ABCD").to_string() ==
          "0190f2a4-5b6c-7d8e-9f01-23456789abcd");
}

TEST_CASE("the default UUID is nil, all zeros", "[doc][uuid]") {
    const Uuid nil;
    CHECK(nil.is_nil());
    CHECK(nil.version() == 0);
    CHECK(nil.to_string() == "00000000-0000-0000-0000-000000000000");
    CHECK(nil.to_string().size() == 36);
    // The nil text form is well formed, so it parses; it just names nothing.
    CHECK(Uuid::parse("00000000-0000-0000-0000-000000000000") == nil);
}

TEST_CASE("malformed UUID text is rejected", "[doc][uuid]") {
    // Untrusted input: anything but exactly 8-4-4-4-12 hex digits with hyphens is refused.
    const std::vector<std::string> bad = {
        "",
        "-",
        "0190f2a4-5b6c-7d8e-9f01-23456789abc",                    // 35 characters
        "0190f2a4-5b6c-7d8e-9f01-23456789abcde",                  // 37 characters
        "0190f2a45b6c7d8e9f0123456789abcd",                       // no hyphens
        "{0190f2a4-5b6c-7d8e-9f01-23456789abcd}",                 // braces
        "urn:uuid:0190f2a4-5b6c-7d8e-9f01-23456789abcd",          // URN prefix
        " 0190f2a4-5b6c-7d8e-9f01-23456789abcd",                  // leading space
        "0190f2a4-5b6c-7d8e-9f01-23456789abcd ",                  // trailing space
        "0190f2a4-5b6c-7d8e-9f01-23456789abcd\n",                 // trailing newline
        "0190f2a-45b6c-7d8e-9f01-23456789abcd",                   // a hyphen one place early
        "0190f2a45-b6c-7d8e-9f01-23456789abcd",                   // a hyphen one place late
        "0190f2a4_5b6c_7d8e_9f01_23456789abcd",                   // wrong separators
        "0190f2a4-5b6c-7d8e-9f01-23456789abcg",                   // 'g' is not hexadecimal
        "0190f2a4-5b6c-7d8e-9f01-2345678 abcd",                   // a space inside a group
        "0x90f2a4-5b6c-7d8e-9f01-23456789abcd",                   // a 0x prefix, as strtoul accepts
        "+190f2a4-5b6c-7d8e-9f01-23456789abcd",                   // a sign, as strtoul accepts
        "0190f2a4-+b6c-7d8e-9f01-23456789abcd",                   // a sign inside a group
        "0190f2a4-5b6c-7d8e-9f01--3456789abcd",                   // a hyphen in place of a digit
        std::string(36, '-'),                                     // 36 hyphens
        "0190f2a4-5b6c-7d8e-9f01-23456789ab\xc3\xa9",             // non-ASCII in place of 2 digits
        std::string("0190f2a4-5b6c-7d8e-9f01-23456789abc\0", 36), // an embedded NUL
    };
    for (const std::string& text : bad) {
        CAPTURE(text);
        CHECK_FALSE(Uuid::parse(text).has_value());
    }
}

TEST_CASE("UUIDs compare byte by byte, and their text sorts the same way", "[doc][uuid]") {
    // Ordered by their bytes, read as unsigned: 0x7f... before 0x80..., and the last byte
    // decides only when all the others are equal.
    const std::vector<Uuid> ascending = {
        Uuid(),
        uuid("00000000-0000-0000-0000-000000000001"),
        uuid("00000000-0000-0000-0000-0000000000ff"),
        uuid("00000000-0000-0000-0000-000000000100"),
        uuid("0190f2a4-5b6c-7d8e-9f01-23456789abcd"),
        uuid("0190f2a4-5b6c-7d8e-9f01-23456789abce"),
        uuid("0190f2a4-5b6d-7000-8000-000000000000"),
        uuid("7fffffff-ffff-ffff-ffff-ffffffffffff"),
        uuid("80000000-0000-0000-0000-000000000000"),
        uuid("ffffffff-ffff-ffff-ffff-ffffffffffff"),
    };
    for (std::size_t i = 1; i < ascending.size(); ++i) {
        CAPTURE(i);
        CHECK(ascending[i - 1] < ascending[i]);
        CHECK(ascending[i - 1] != ascending[i]);
        CHECK(ascending[i - 1].to_string() < ascending[i].to_string());
    }
}

TEST_CASE("equal UUIDs have equal hashes and work in unordered containers", "[doc][uuid]") {
    const Uuid a = uuid("0190f2a4-5b6c-7d8e-9f01-23456789abcd");
    const Uuid a_again = uuid("0190F2A4-5B6C-7D8E-9F01-23456789ABCD");
    const Uuid b = uuid("0190f2a4-5b6c-7d8e-9f01-23456789abce");
    CHECK(std::hash<Uuid>{}(a) == std::hash<Uuid>{}(a_again));

    std::unordered_set<Uuid> set = {a, b};
    CHECK(set.size() == 2);
    CHECK(set.contains(a_again));
    set.insert(a_again);
    CHECK(set.size() == 2);
}

TEST_CASE("unix_time_ms is milliseconds since the Unix epoch", "[doc][uuid]") {
    // Not seconds (about 1.7e9) or a steady clock (time since boot): between 2024 and 2100.
    const std::int64_t now = cs::doc::unix_time_ms();
    CHECK(now > ms_2024);
    CHECK(now < ms_2100);
}

TEST_CASE("generated ids are version 7 with the RFC 9562 variant", "[doc][uuid]") {
    SECTION("with the system clock") {
        IdGenerator generator(1);
        for (int i = 0; i < 1000; ++i) {
            const Uuid id = generator.next();
            CAPTURE(i, id.to_string());
            CHECK(id.version() == 7);
            CHECK(has_rfc9562_variant(id));
            CHECK_FALSE(id.is_nil());
        }
    }
    SECTION("with a clock that stays in one millisecond, so the counter carries a long way") {
        IdGenerator generator(2, [] { return fixed_ms; });
        for (int i = 0; i < 10'000; ++i) {
            const Uuid id = generator.next();
            CAPTURE(i, id.to_string());
            REQUIRE(id.version() == 7);
            REQUIRE(has_rfc9562_variant(id));
        }
    }
}

TEST_CASE("a generated id begins with the clock's time in milliseconds", "[doc][uuid]") {
    std::int64_t now = 0;
    IdGenerator generator(3, [&now] { return now; });

    // Increasing times, from the epoch to the largest the 48 bits hold.
    const std::vector<std::int64_t> times = {
        0, 1, 255, 256, ms_2024, fixed_ms, fixed_ms + 1, (std::int64_t{1} << 48) - 1,
    };
    for (const std::int64_t t : times) {
        now = t;
        const Uuid id = generator.next();
        CAPTURE(t, id.to_string());
        CHECK(unix_ms_of(id) == t);
        CHECK(id.version() == 7);
        CHECK(has_rfc9562_variant(id));
        CHECK_FALSE(id.is_nil());
    }

    // With the system clock, the time is that of the call.
    IdGenerator system(4);
    const std::int64_t before = cs::doc::unix_time_ms();
    const Uuid id = system.next();
    const std::int64_t after = cs::doc::unix_time_ms();
    CHECK(unix_ms_of(id) >= before);
    CHECK(unix_ms_of(id) <= after);
}

TEST_CASE("10^5 generated ids are unique and sort in creation order", "[doc][uuid]") {
    // Acceptance criterion: "10^5 generated ids are unique and sort in creation order", with the
    // real clock, so many ids share a millisecond and the counter must order them.
    constexpr std::size_t count = 100'000;
    IdGenerator generator(5);
    std::vector<Uuid> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(generator.next());
    }

    std::unordered_set<Uuid> unique(ids.begin(), ids.end());
    CHECK(unique.size() == count);
    check_strictly_increasing(ids);
}

TEST_CASE("ids made within one millisecond still increase", "[doc][uuid]") {
    constexpr std::size_t count = 100'000;
    IdGenerator generator(6, [] { return fixed_ms; });
    std::vector<Uuid> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(generator.next());
    }
    check_strictly_increasing(ids);
    // The counter has 74 bits and starts at a random value: 10^5 steps do not overflow it
    // (except with probability about 10^5 / 2^74), so the time stays the clock's.
    CHECK(std::all_of(ids.begin(), ids.end(),
                      [](const Uuid& id) { return unix_ms_of(id) == fixed_ms; }));
}

TEST_CASE("ids keep increasing when the clock steps backwards", "[doc][uuid]") {
    std::int64_t now = fixed_ms;
    IdGenerator generator(7, [&now] { return now; });

    std::vector<Uuid> ids;
    for (int i = 0; i < 100; ++i) {
        ids.push_back(generator.next());
    }

    // The clock goes back a second: the generator keeps the last time it saw and counts on.
    now = fixed_ms - 1000;
    for (int i = 0; i < 100; ++i) {
        const Uuid id = generator.next();
        CHECK(unix_ms_of(id) == fixed_ms);
        ids.push_back(id);
    }

    // Back to the epoch: still no going back.
    now = 0;
    ids.push_back(generator.next());
    CHECK(unix_ms_of(ids.back()) == fixed_ms);

    // When the clock passes the last time again, ids carry the new time.
    now = fixed_ms + 1;
    const Uuid later = generator.next();
    CHECK(unix_ms_of(later) == fixed_ms + 1);
    ids.push_back(later);

    for (std::size_t i = 1; i < ids.size(); ++i) {
        CAPTURE(i, ids[i - 1].to_string(), ids[i].to_string());
        CHECK(ids[i - 1] < ids[i]);
        CHECK(ids[i - 1].to_string() < ids[i].to_string());
        CHECK(ids[i].version() == 7);
        CHECK(has_rfc9562_variant(ids[i]));
    }
}

TEST_CASE("each new millisecond starts the counter at a random value", "[doc][uuid]") {
    // RFC 9562, 6.2: the counter is seeded with random bits in each new millisecond, not reset
    // to zero. 64 starting values of 74 random bits coincide with probability about 2^-63.
    std::int64_t now = fixed_ms;
    IdGenerator generator(8, [&now] { return now; });
    std::unordered_set<Uuid> starts;
    for (int i = 0; i < 64; ++i) {
        now = fixed_ms + i;
        starts.insert(counter_bits_of(generator.next()));
    }
    CHECK(starts.size() == 64);
}

TEST_CASE("the same seed and clock give the same ids", "[doc][uuid]") {
    // Two generators fed the same scripted clock: 500 ids in one millisecond, then 500 in the
    // next, then a step back.
    std::int64_t now = fixed_ms;
    IdGenerator a(42, [&now] { return now; });
    IdGenerator b(42, [&now] { return now; });
    for (int i = 0; i < 1500; ++i) {
        now = i < 500 ? fixed_ms : i < 1000 ? fixed_ms + 1 : fixed_ms - 5;
        const Uuid id_a = a.next();
        const Uuid id_b = b.next();
        CAPTURE(i);
        REQUIRE(id_a == id_b);
    }
}

TEST_CASE("different seeds give different ids in the same millisecond", "[doc][uuid]") {
    IdGenerator a(1, [] { return fixed_ms; });
    IdGenerator b(2, [] { return fixed_ms; });
    std::vector<Uuid> from_a;
    std::unordered_set<Uuid> from_b;
    Uuid first_b;
    for (int i = 0; i < 1000; ++i) {
        from_a.push_back(a.next());
        const Uuid id_b = b.next();
        if (i == 0) {
            first_b = id_b;
        }
        from_b.insert(id_b);
    }
    // Same time, so only the random counter can tell the first ids apart.
    CHECK(unix_ms_of(from_a.front()) == unix_ms_of(first_b));
    CHECK(counter_bits_of(from_a.front()) != counter_bits_of(first_b));
    // Two counters of 1000 steps from independent random starts in 2^74 overlap with
    // probability about 2000 / 2^74.
    for (const Uuid& id : from_a) {
        CAPTURE(id.to_string());
        CHECK_FALSE(from_b.contains(id));
    }
}
