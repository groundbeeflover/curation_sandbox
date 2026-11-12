// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cs/beta/beta.hpp>

namespace cs::epsilon {

inline int value() {
    return cs::beta::value() + 2;
}

} // namespace cs::epsilon
