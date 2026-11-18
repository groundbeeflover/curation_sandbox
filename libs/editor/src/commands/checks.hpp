// SPDX-License-Identifier: Apache-2.0

#pragma once

// Checks the commands share.

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cs::editor::detail {

// Throws std::invalid_argument, naming the command and the value, unless value_m is finite and
// greater than 0.
inline void check_length(std::string_view command, std::string_view what, double value_m) {
    if (!std::isfinite(value_m) || !(value_m > 0.0)) {
        throw std::invalid_argument(std::string(command) + ": " + std::string(what) +
                                    " must be finite and greater than 0, not " +
                                    std::to_string(value_m));
    }
}

} // namespace cs::editor::detail
