// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cs/beta/beta.hpp>

namespace cs::delta {

// A public header that needs beta: users of delta must see beta's headers.
struct Wrapped {
    int beta_value = cs::beta::value();
};

int value();

} // namespace cs::delta
