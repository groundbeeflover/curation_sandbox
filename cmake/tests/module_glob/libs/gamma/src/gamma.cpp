// SPDX-License-Identifier: Apache-2.0
#include <cs/beta/beta.hpp>
#include <cs/gamma/gamma.hpp>

namespace cs::gamma {

int value() {
    return cs::beta::value() + 1;
}

} // namespace cs::gamma
