// SPDX-License-Identifier: Apache-2.0
#include <cs/alpha/alpha.hpp>
#include <cs/beta/beta.hpp>

namespace cs::beta {

int value() {
    return cs::alpha::value() + 1;
}

} // namespace cs::beta
