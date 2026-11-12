// SPDX-License-Identifier: Apache-2.0
#include <cs/beta/beta.hpp>
#include <cs/epsilon/epsilon.hpp>
#include <cs/eta/eta.hpp>

namespace cs::eta {

int value() {
    return cs::epsilon::value() + cs::beta::value();
}

} // namespace cs::eta
