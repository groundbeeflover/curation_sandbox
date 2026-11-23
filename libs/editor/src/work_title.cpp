// SPDX-License-Identifier: Apache-2.0

#include <cs/editor/work_title.hpp>

#include <cs/doc/project_io.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cs::editor {

namespace {

constexpr std::string_view replacement = "\xEF\xBF\xBD"; // U+FFFD in UTF-8

// The length of the valid UTF-8 sequence at the start of `text`, or 0 when it does not start with
// one; `subpart` is then the length of its maximal subpart: the lead byte and the continuation
// bytes that could still have completed it (at least 1), which become one U+FFFD (Unicode
// Standard, section 3.9, "U+FFFD Substitution of Maximal Subparts").
std::size_t valid_sequence(std::string_view text, std::size_t& subpart) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(0);
    subpart = 1;
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    // The range of the second byte, narrower than 80..BF after E0, ED, F0 and F4: that excludes
    // overlong forms, surrogates and code points above U+10FFFF.
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        low = lead == 0xE0 ? 0xA0 : 0x80;
        high = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        low = lead == 0xF0 ? 0x90 : 0x80;
        high = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0; // 80..C1 and F5..FF never begin a sequence
    }
    for (std::size_t i = 1; i < length; ++i) {
        if (i >= text.size()) {
            return 0;
        }
        const unsigned char b = byte(i);
        const bool in_range = i == 1 ? (b >= low && b <= high) : (b >= 0x80 && b <= 0xBF);
        if (!in_range) {
            return 0;
        }
        subpart = i + 1;
    }
    return length;
}

// `text` with each maximal subpart that is not valid UTF-8 replaced by U+FFFD.
std::string valid_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    while (!text.empty()) {
        std::size_t subpart = 1;
        const std::size_t length = valid_sequence(text, subpart);
        if (length > 0) {
            out.append(text.substr(0, length));
            text.remove_prefix(length);
        } else {
            out.append(replacement);
            text.remove_prefix(subpart);
        }
    }
    return out;
}

// Valid UTF-8 cut to at most doc::max_string_bytes, before a code point's first byte.
void cut_to_bound(std::string& text) {
    if (text.size() <= doc::max_string_bytes) {
        return;
    }
    std::size_t end = doc::max_string_bytes;
    // A continuation byte (10xxxxxx) is inside a code point; step back to the byte that begins it.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    text.resize(end);
}

bool is_white_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Each run of ASCII white space made one space, none at either end.
std::string collapse_white_space(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool space_pending = false;
    for (const char c : text) {
        if (is_white_space(c)) {
            space_pending = !out.empty();
            continue;
        }
        if (space_pending) {
            out.push_back(' ');
            space_pending = false;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

std::string title_from_file_name(std::string_view file_name) {
    // '.' is ASCII, so it is never a byte of a longer UTF-8 sequence, valid or not.
    const std::size_t dot = file_name.rfind('.');
    std::string stem(dot == std::string_view::npos ? file_name : file_name.substr(0, dot));
    for (char& c : stem) {
        if (c == '_') {
            c = ' ';
        }
    }
    std::string title = collapse_white_space(valid_utf8(stem));
    cut_to_bound(title);
    // The cut may end the title just after a space.
    while (!title.empty() && title.back() == ' ') {
        title.pop_back();
    }
    return title;
}

doc::FromImport source_from_file_name(std::string_view file_name) {
    std::string name = valid_utf8(file_name);
    cut_to_bound(name);
    return doc::FromImport{std::move(name)};
}

} // namespace cs::editor
