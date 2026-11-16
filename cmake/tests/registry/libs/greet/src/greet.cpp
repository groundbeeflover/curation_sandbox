// SPDX-License-Identifier: Apache-2.0

// A source outside the registry's directory, so the module stays a compiled library (not
// header-only) when the registry has no files.

#include <cs/greet/greet.hpp>

namespace cs::greet {

std::string default_prefix() {
    return "> ";
}

} // namespace cs::greet
