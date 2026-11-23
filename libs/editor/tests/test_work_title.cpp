// SPDX-License-Identifier: Apache-2.0

// A work's title and source from the file it was imported from (#88, M1-27), from the issue's
// acceptance criterion and libs/editor/include/cs/editor/work_title.hpp: "editor::
// title_from_file_name takes the file name's stem, turns underscores into spaces, collapses and
// trims white space, replaces bytes that are not UTF-8 with U+FFFD and cuts at a character
// boundary to doc::max_string_bytes ('Monet_Haystacks  final.JPG' becomes 'Monet Haystacks final';
// '.png' becomes an empty, untitled work); its source is FromImport with the file name, made valid
// UTF-8 and bounded the same way". `ctest --preset debug -L editor` runs a test of each case.
//
// Invalid bytes are replaced as the Unicode Standard's "maximal subpart" practice (section 3.9,
// U+FFFD substitution) and WHATWG's UTF-8 decoder count them: each maximal run of bytes that
// begins a well-formed sequence but does not finish it is one U+FFFD; any other byte that cannot
// begin a sequence (0x80 to 0xC1, 0xF5 to 0xFF) is one U+FFFD of its own. The well-formed
// sequences are those of the Standard's table 3-7, written out in is_valid_utf8 below.
//
// File names come from outside and are hostile (CLAUDE.md): every result is checked to be valid
// UTF-8 of at most doc::max_string_bytes, whatever the bytes given.

#include <cs/editor/work_title.hpp>

#include <cs/core/random.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/work.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

using cs::editor::source_from_file_name;
using cs::editor::title_from_file_name;

namespace {

constexpr std::size_t max_bytes = cs::doc::max_string_bytes;

// U+FFFD REPLACEMENT CHARACTER in UTF-8: three bytes.
const std::string fffd = "\xEF\xBF\xBD";
// U+00E9 (e acute), two bytes; U+00A0 (no-break space), two bytes; U+1F3A8 (artist palette), four.
const std::string e_acute = "\xC3\xA9";
const std::string no_break_space = "\xC2\xA0";
const std::string palette = "\xF0\x9F\x8E\xA8";

std::string repeat(const std::string& part, std::size_t times) {
    std::string out;
    out.reserve(part.size() * times);
    for (std::size_t i = 0; i < times; ++i) {
        out += part;
    }
    return out;
}

// Whether `text` is well-formed UTF-8: every sequence one of the Unicode Standard's table 3-7,
// so no overlong form, no surrogate (U+D800 to U+DFFF), nothing above U+10FFFF, nothing cut short.
bool is_valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead <= 0x7F) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char second_low = 0x80;
        unsigned char second_high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            second_low = 0xA0;
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            second_high = 0x9F;
        } else if (lead == 0xF0) {
            length = 4;
            second_low = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            second_high = 0x8F;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if (second < second_low || second > second_high) {
            return false;
        }
        for (std::size_t k = 2; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if (next < 0x80 || next > 0xBF) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

// The bytes as a failure message shows them: printable ASCII as it is, any other byte as \xHH.
// Long strings are shortened to their first and last 40 bytes.
std::string escaped(std::string_view bytes) {
    const auto escape = [](std::string_view part) {
        std::string out;
        for (const char c : part) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x20 && byte < 0x7F && byte != '\\') {
                out += c;
            } else {
                std::array<char, 8> hex{};
                std::snprintf(hex.data(), hex.size(), "\\x%02X", static_cast<unsigned>(byte));
                out += hex.data();
            }
        }
        return out;
    };
    if (bytes.size() <= 100) {
        return "\"" + escape(bytes) + "\"";
    }
    return "\"" + escape(bytes.substr(0, 40)) + "\" ... (" + std::to_string(bytes.size()) +
           " bytes) ... \"" + escape(bytes.substr(bytes.size() - 40)) + "\"";
}

// Every result of either function, whatever the input: valid UTF-8, within the project file's
// limit.
void check_well_formed(const std::string& result) {
    INFO("result " << escaped(result));
    CHECK(is_valid_utf8(result));
    CHECK(result.size() <= max_bytes);
}

void check_title(std::string_view file_name, const std::string& expected) {
    INFO("file name " << escaped(file_name));
    const std::string title = title_from_file_name(file_name);
    INFO("title " << escaped(title) << ", expected " << escaped(expected));
    CHECK(title == expected);
    check_well_formed(title);
}

void check_source(std::string_view file_name, const std::string& expected) {
    INFO("file name " << escaped(file_name));
    const cs::doc::FromImport source = source_from_file_name(file_name);
    INFO("source " << escaped(source.original_name) << ", expected " << escaped(expected));
    CHECK(source.original_name == expected);
    CHECK(source == cs::doc::FromImport{expected});
    check_well_formed(source.original_name);
}

// ---- A reference, written from the specification --------------------------------------------

// WHATWG's UTF-8 decoder (Encoding Standard, section 9.1.1), keeping the bytes of each well-formed
// sequence and writing U+FFFD for each error: one per maximal subpart.
std::string reference_valid_utf8(std::string_view bytes) {
    std::string out;
    std::size_t needed = 0;
    std::size_t start = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        if (needed == 0) {
            if (byte <= 0x7F) {
                out += static_cast<char>(byte);
            } else if (byte >= 0xC2 && byte <= 0xDF) {
                needed = 1;
                start = i;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                low = byte == 0xE0 ? 0xA0 : 0x80;
                high = byte == 0xED ? 0x9F : 0xBF;
                needed = 2;
                start = i;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                low = byte == 0xF0 ? 0x90 : 0x80;
                high = byte == 0xF4 ? 0x8F : 0xBF;
                needed = 3;
                start = i;
            } else {
                out += fffd;
            }
            ++i;
            continue;
        }
        if (byte < low || byte > high) {
            // The sequence so far is one maximal subpart; the byte is read again on its own.
            out += fffd;
            needed = 0;
            low = 0x80;
            high = 0xBF;
            continue;
        }
        low = 0x80;
        high = 0xBF;
        ++i;
        if (i - start == needed + 1) {
            out.append(bytes.substr(start, needed + 1));
            needed = 0;
        }
    }
    if (needed != 0) {
        out += fffd;
    }
    return out;
}

// Valid UTF-8 cut to at most max_bytes at a code point boundary: back over continuation bytes.
std::string reference_cut(std::string text) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    text.resize(cut);
    return text;
}

bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Runs of ASCII white space made one space, none at either end.
std::string reference_collapse_and_trim(const std::string& text) {
    std::string out;
    bool pending_space = false;
    for (const char c : text) {
        if (is_ascii_space(c)) {
            pending_space = true;
            continue;
        }
        if (pending_space && !out.empty()) {
            out += ' ';
        }
        pending_space = false;
        out += c;
    }
    return out;
}

// The header's steps, in its order.
std::string reference_title(std::string_view file_name) {
    const std::size_t dot = file_name.rfind('.');
    std::string stem(dot == std::string_view::npos ? file_name : file_name.substr(0, dot));
    for (char& c : stem) {
        if (c == '_') {
            c = ' ';
        }
    }
    return reference_collapse_and_trim(
        reference_cut(reference_collapse_and_trim(reference_valid_utf8(stem))));
}

std::string reference_source(std::string_view file_name) {
    return reference_cut(reference_valid_utf8(file_name));
}

} // namespace

// ==== The test's own helpers ==================================================================

// The checks below lean on is_valid_utf8 and the reference decoder, so they are checked first
// against table 3-7 and against the examples of the Unicode Standard (section 3.9, U+FFFD
// substitution of maximal subparts).
TEST_CASE("work title: the test's UTF-8 check and reference decoder follow table 3-7", "[editor]") {
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("Monet"));
    CHECK(is_valid_utf8("Caf" + e_acute));
    CHECK(is_valid_utf8(palette));
    CHECK(is_valid_utf8(fffd));
    CHECK(is_valid_utf8("\xF4\x8F\xBF\xBF")); // U+10FFFF, the last code point
    CHECK(is_valid_utf8("\xED\x9F\xBF"));     // U+D7FF, just below the surrogates
    CHECK_FALSE(is_valid_utf8("\xFF"));
    CHECK_FALSE(is_valid_utf8("caf\xE9"));
    CHECK_FALSE(is_valid_utf8("\xE2\x82"));         // cut short
    CHECK_FALSE(is_valid_utf8("\xC0\x80"));         // overlong U+0000
    CHECK_FALSE(is_valid_utf8("\xE0\x80\x80"));     // overlong
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));     // U+D800, a surrogate
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80")); // U+110000
    CHECK_FALSE(is_valid_utf8("\x80"));             // a continuation on its own

    // The Standard's example (table 3-8): 61 F1 80 80 E1 80 C2 62 80 63 80 BF 64 gives
    // a, FFFD, FFFD, FFFD, b, FFFD, c, FFFD, FFFD, d.
    CHECK(reference_valid_utf8("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64") ==
          "a" + fffd + fffd + fffd + "b" + fffd + "c" + fffd + fffd + "d");
}

// ==== title_from_file_name ====================================================================

TEST_CASE("title_from_file_name: the issue's examples, 'Monet_Haystacks  final.JPG' and '.png'",
          "[editor]") {
    check_title("Monet_Haystacks  final.JPG", "Monet Haystacks final");
    // An empty title: an untitled work.
    check_title(".png", "");
}

TEST_CASE("title_from_file_name: the stem is the part before the last '.', all of it when there "
          "is none",
          "[editor]") {
    check_title("Haystacks.png", "Haystacks");
    check_title("Haystacks", "Haystacks");
    check_title("Monet Haystacks final", "Monet Haystacks final");
    // Only the last extension goes.
    check_title("archive.tar.gz", "archive.tar");
    check_title("Mr. Smith.jpeg", "Mr. Smith");
    // A '.' at the end: an empty extension.
    check_title("Haystacks.", "Haystacks");
    // Nothing at all, and nothing but extensions.
    check_title("", "");
    check_title(".", "");
    check_title("..png", ".");
    // Bytes after the last '.' are the extension, valid or not, and are dropped.
    check_title("Haystacks.\xFF\xFE", "Haystacks");
}

TEST_CASE("title_from_file_name: each underscore becomes a space", "[editor]") {
    check_title("Water_Lilies.png", "Water Lilies");
    check_title("The_Water_Lily_Pond.jpg", "The Water Lily Pond");
    // Two underscores make two spaces, which collapse to one.
    check_title("Water__Lilies.png", "Water Lilies");
    check_title("Water_ _Lilies.png", "Water Lilies");
}

TEST_CASE("title_from_file_name: runs of ASCII white space (space, tab, line feed, vertical tab, "
          "form feed, carriage return) collapse to one space",
          "[editor]") {
    check_title("a  b.png", "a b");
    check_title("a\tb.png", "a b");
    check_title("a\nb.png", "a b");
    check_title("a\rb.png", "a b");
    check_title("a\vb.png", "a b");
    check_title("a\fb.png", "a b");
    check_title("a \t\n\v\f\r b.png", "a b");
    check_title("a\r\nb\tc  _ d.png", "a b c d");
    // Only ASCII white space: a no-break space (U+00A0) is a character of the title, kept.
    check_title("a" + no_break_space + "b.png", "a" + no_break_space + "b");
    check_title("a " + no_break_space + " b.png", "a " + no_break_space + " b");
}

TEST_CASE("title_from_file_name: white space and underscores are trimmed at both ends",
          "[editor]") {
    check_title("  Haystacks  .png", "Haystacks");
    check_title("\t\n Haystacks \r\n.png", "Haystacks");
    check_title("__Haystacks__.png", "Haystacks");
    check_title("_ \t_Haystacks_\v _.png", "Haystacks");
    // Nothing but white space and underscores: untitled.
    check_title("   .png", "");
    check_title("___.png", "");
    check_title(" _\t\n_ ", "");
}

TEST_CASE("title_from_file_name: valid multibyte characters are kept", "[editor]") {
    check_title("Caf" + e_acute + "_Terrace.png", "Caf" + e_acute + " Terrace");
    check_title(palette + "_Palette.png", palette + " Palette");
    // Japanese, three bytes each: "Kanagawa" in kanji, U+795E U+5948 U+5DDD.
    check_title("\xE7\xA5\x9E\xE5\xA5\x88\xE5\xB7\x9D.jpg", "\xE7\xA5\x9E\xE5\xA5\x88\xE5\xB7\x9D");
    // U+FFFD written in the name is a character like any other.
    check_title(fffd + ".png", fffd);
}

TEST_CASE("title_from_file_name: each maximal subpart of bytes that are not UTF-8 becomes one "
          "U+FFFD",
          "[editor]") {
    // A lone 0xFF, which no sequence begins with.
    check_title("\xFF.png", fffd);
    check_title("a\xFF"
                "b.png",
                "a" + fffd + "b");
    // Latin-1 e acute, 0xE9, which begins a three-byte sequence: cut short by the end of the stem,
    // or ended early by an underscore.
    check_title("caf\xE9.png", "caf" + fffd);
    check_title("Caf\xE9_Terrace.png", "Caf" + fffd + " Terrace");
    // A three-byte sequence cut short (E2 82 begins the euro sign, E2 82 AC): one U+FFFD.
    check_title("a\xE2\x82.png", "a" + fffd);
    check_title("a\xE2\x82"
                "b.png",
                "a" + fffd + "b");
    // A four-byte sequence cut short: one.
    check_title("\xF0\x9F\x8E"
                "x.png",
                fffd + "x");
    // A continuation byte on its own: one.
    check_title("a\x80"
                "b.png",
                "a" + fffd + "b");
    // The overlong form of U+0000, C0 80: C0 begins no sequence, so two.
    check_title("a\xC0\x80"
                "b.png",
                "a" + fffd + fffd + "b");
    // An overlong three-byte form, E0 80 80: E0 needs A0 to BF next, so three.
    check_title("a\xE0\x80\x80"
                "b.png",
                "a" + fffd + fffd + fffd + "b");
    // A surrogate, U+D800 as ED A0 80: ED needs 80 to 9F next, so three.
    check_title("a\xED\xA0\x80"
                "b.png",
                "a" + fffd + fffd + fffd + "b");
    // Above U+10FFFF, F4 90 80 80: F4 needs 80 to 8F next, so four.
    check_title("a\xF4\x90\x80\x80"
                "b.png",
                "a" + fffd + fffd + fffd + fffd + "b");
    // F5 to FF begin nothing.
    check_title("\xF5\xF8\xFE.png", fffd + fffd + fffd);
    // An invalid byte next to an underscore: the underscore is still a space.
    check_title("a\xE2_b.png", "a" + fffd + " b");
}

TEST_CASE("title_from_file_name: cut to doc::max_string_bytes at a code point boundary, then "
          "trimmed again",
          "[editor]") {
    REQUIRE(max_bytes == 4096);

    SECTION("5000 ASCII bytes: the first 4096") {
        check_title(std::string(5000, 'a'), std::string(4096, 'a'));
    }
    SECTION("exactly 4096 bytes: all of them") {
        check_title(std::string(4096, 'a') + ".png", std::string(4096, 'a'));
    }
    SECTION("'a' and 3000 two-byte characters: 'a' and 2047 of them, 4095 bytes, no sequence "
            "split") {
        // 1 + 2k <= 4096 gives k = 2047, 4095 bytes; the 2048th would end at byte 4097.
        const std::string title = title_from_file_name("a" + repeat(e_acute, 3000) + ".png");
        CHECK(title.size() == 4095);
        CHECK(title == "a" + repeat(e_acute, 2047));
        check_well_formed(title);
    }
    SECTION("4094 ASCII bytes and a four-byte character: the character does not fit") {
        check_title(std::string(4094, 'a') + palette + ".png", std::string(4094, 'a'));
    }
    SECTION("4095 'a' then ' b': cut after the space, which is then trimmed") {
        // 4097 bytes; the first 4096 end with the space.
        const std::string title = title_from_file_name(std::string(4095, 'a') + " b");
        CHECK(title == std::string(4095, 'a'));
        CHECK(title.size() == 4095);
    }
    SECTION("white space collapses before the cut, so a long run of it costs one byte") {
        check_title("a" + std::string(5000, ' ') + "b.png", "a b");
        check_title(std::string(5000, '_') + "Haystacks" + std::string(5000, '_') + ".png",
                    "Haystacks");
        check_title(std::string(5000, '\t') + "x.png", "x");
    }
    SECTION("invalid bytes grow threefold as U+FFFD, and are still cut at a boundary") {
        // 4096 bytes of 0xFF: 4096 U+FFFD of 3 bytes; 3k <= 4096 gives 1365, 4095 bytes.
        const std::string title = title_from_file_name(std::string(4096, '\xFF'));
        CHECK(title == repeat(fffd, 1365));
        CHECK(title.size() == 4095);
        check_well_formed(title);
        // 4000 'a' and 100 x 0xFF: 4000 + 3k <= 4096 gives 32, exactly 4096 bytes.
        check_title(std::string(4000, 'a') + std::string(100, '\xFF') + ".png",
                    std::string(4000, 'a') + repeat(fffd, 32));
    }
    SECTION("a name of a megabyte: bounded") {
        const std::string title = title_from_file_name(std::string(1 << 20, 'x') + ".png");
        CHECK(title == std::string(4096, 'x'));
    }
}

// ==== source_from_file_name ===================================================================

TEST_CASE("source_from_file_name: a valid name is kept as it was, underscores, white space and "
          "extension included",
          "[editor]") {
    check_source("Monet_Haystacks  final.JPG", "Monet_Haystacks  final.JPG");
    check_source("Photo.PNG", "Photo.PNG");
    check_source(".png", ".png");
    check_source("", "");
    check_source("  spaced\t name .png ", "  spaced\t name .png ");
    check_source("Caf" + e_acute + "_Terrace.png", "Caf" + e_acute + "_Terrace.png");
    check_source(palette + ".jpeg", palette + ".jpeg");
    check_source("archive.tar.gz", "archive.tar.gz");
}

TEST_CASE("source_from_file_name: each maximal subpart of bytes that are not UTF-8 becomes one "
          "U+FFFD",
          "[editor]") {
    check_source("\xFF", fffd);
    check_source("caf\xE9.png", "caf" + fffd + ".png");
    // E2 82 then '.', which ends the sequence early: one.
    check_source("a\xE2\x82.png", "a" + fffd + ".png");
    check_source("a\xC0\x80.png", "a" + fffd + fffd + ".png");
    check_source("\xED\xA0\x80.png", fffd + fffd + fffd + ".png");
    check_source("\xF4\x90\x80\x80.png", fffd + fffd + fffd + fffd + ".png");
    // In the extension too: the source is the whole name.
    check_source("Photo.\xFF", "Photo." + fffd);
    // Cut short at the very end.
    check_source("Photo.pn\xE2\x82", "Photo.pn" + fffd);
}

TEST_CASE("source_from_file_name: cut to doc::max_string_bytes at a code point boundary, not "
          "trimmed",
          "[editor]") {
    SECTION("5000 ASCII bytes: the first 4096") {
        check_source(std::string(5000, 'a'), std::string(4096, 'a'));
    }
    SECTION("exactly 4096 bytes: all of them") {
        check_source(std::string(4092, 'a') + ".png", std::string(4092, 'a') + ".png");
    }
    SECTION("'a' and 3000 two-byte characters: 4095 bytes") {
        check_source("a" + repeat(e_acute, 3000), "a" + repeat(e_acute, 2047));
    }
    SECTION("4095 'a' then ' b': the space at the cut stays, as the name had it") {
        check_source(std::string(4095, 'a') + " b", std::string(4095, 'a') + " ");
    }
    SECTION("4096 bytes of 0xFF: 1365 U+FFFD, 4095 bytes") {
        check_source(std::string(4096, '\xFF'), repeat(fffd, 1365));
    }
    SECTION("a name of a megabyte: bounded") {
        check_source(std::string(1 << 20, 'x') + ".png", std::string(4096, 'x'));
    }
}

// ==== Any bytes ===============================================================================

// Hostile input: every byte value, in a name of its own and between letters, gives valid UTF-8
// within the limit, and what the reference makes of it.
TEST_CASE("title and source: every byte value gives valid UTF-8, as the reference decodes it",
          "[editor]") {
    for (int value = 0; value < 256; ++value) {
        const char byte = static_cast<char>(value);
        for (const std::string& name : {std::string(1, byte), "a" + std::string(1, byte) + "b.png",
                                        std::string(3, byte) + ".jpg"}) {
            INFO("byte " << value);
            check_title(name, reference_title(name));
            check_source(name, reference_source(name));
        }
    }
}

// Hostile input: names of random bytes, drawn mostly from those that begin, continue or break a
// sequence, against the reference of the header's steps. Seeded (CLAUDE.md), so every run tests
// the same names; short names test the decoding, names near 4096 bytes the cut.
TEST_CASE("title and source: random byte names match the reference of the header's steps",
          "[editor]") {
    static constexpr std::array<unsigned char, 30> alphabet{
        'a',  'B',  '_',  ' ',  '\t', '\n', '\r', '.',  0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xA9, 0xBF,
        0xC0, 0xC2, 0xC3, 0xDF, 0xE0, 0xE2, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF, 0x82, 0xAC, 'z'};
    const auto random_name = [&](cs::Rng& rng, std::size_t length) {
        std::string name;
        name.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            name += static_cast<char>(alphabet[rng.next_u32() % alphabet.size()]);
        }
        return name;
    };

    std::size_t mismatched_titles = 0;
    std::size_t mismatched_sources = 0;
    std::size_t malformed = 0;
    std::string first_mismatch;
    const auto check_name = [&](const std::string& name) {
        const std::string title = title_from_file_name(name);
        const std::string source = source_from_file_name(name).original_name;
        if (title != reference_title(name)) {
            ++mismatched_titles;
            if (first_mismatch.empty()) {
                first_mismatch = "title of " + escaped(name) + " is " + escaped(title) +
                                 ", expected " + escaped(reference_title(name));
            }
        }
        if (source != reference_source(name)) {
            ++mismatched_sources;
            if (first_mismatch.empty()) {
                first_mismatch = "source of " + escaped(name) + " is " + escaped(source) +
                                 ", expected " + escaped(reference_source(name));
            }
        }
        if (!is_valid_utf8(title) || !is_valid_utf8(source) || title.size() > max_bytes ||
            source.size() > max_bytes) {
            ++malformed;
        }
    };

    for (std::uint32_t i = 0; i < 4000; ++i) {
        cs::Rng rng(cs::hash_seed(88, i, 0, 0, 0));
        check_name(random_name(rng, rng.next_u32() % 41));
    }
    for (std::uint32_t i = 0; i < 200; ++i) {
        cs::Rng rng(cs::hash_seed(88, i, 1, 0, 0));
        check_name(random_name(rng, 4070 + rng.next_u32() % 60));
    }

    INFO("first mismatch: " << first_mismatch);
    CHECK(mismatched_titles == 0);
    CHECK(mismatched_sources == 0);
    CHECK(malformed == 0);
}
