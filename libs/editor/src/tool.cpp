// SPDX-License-Identifier: Apache-2.0

#include <cs/editor/tool.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <string_view>
#include <system_error>

namespace cs::editor {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim_left(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    return text;
}

std::string_view trim(std::string_view text) {
    text = trim_left(text);
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

// A unit's length in metres as a fraction at most 1, divided first so that no finite number
// overflows: "160 cm" is the double nearest 1.6.
struct Unit {
    std::string_view name;
    double numerator;
    double denominator;
    double to_metres(double number) const { return number / denominator * numerator; }
};
constexpr std::array units{Unit{"", 1.0, 1.0}, Unit{"m", 1.0, 1.0}, Unit{"cm", 1.0, 100.0},
                           Unit{"mm", 1.0, 1000.0}, Unit{"in", 254.0, 10000.0}};

} // namespace

std::optional<TypedValue> parse_typed_value(std::string_view text) {
    if (text.size() > max_typed_text_bytes) {
        return std::nullopt;
    }
    std::string_view rest = trim(text);

    TypedValue typed;
    if (!rest.empty() && (lower(rest.front()) == 'u' || lower(rest.front()) == 'h')) {
        typed.field = lower(rest.front()) == 'u' ? TypedField::u : TypedField::h;
        rest = trim_left(rest.substr(1));
        if (rest.empty() || rest.front() != '=') {
            return std::nullopt;
        }
        rest = trim_left(rest.substr(1));
    }

    // std::from_chars takes no '+', and would take "inf" and "nan": the number must start with a
    // digit or the decimal point, after its sign.
    bool negative = false;
    if (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
        negative = rest.front() == '-';
        rest.remove_prefix(1);
    }
    if (rest.empty() || !((rest.front() >= '0' && rest.front() <= '9') || rest.front() == '.')) {
        return std::nullopt;
    }
    double number = 0.0;
    const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), number);
    if (error != std::errc{}) {
        return std::nullopt; // not a number, or out of a double's range
    }
    rest = trim_left(rest.substr(static_cast<std::size_t>(end - rest.data())));

    for (const Unit& unit : units) {
        if (equal_ignoring_case(rest, unit.name)) {
            typed.value_m = unit.to_metres(negative ? -number : number);
            if (!std::isfinite(typed.value_m)) {
                return std::nullopt;
            }
            return typed;
        }
    }
    return std::nullopt;
}

} // namespace cs::editor
