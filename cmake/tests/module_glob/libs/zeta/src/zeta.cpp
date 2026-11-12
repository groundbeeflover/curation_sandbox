// SPDX-License-Identifier: Apache-2.0
#include <cs/beta/beta.hpp>
#include <cs/delta/delta.hpp>
#include <cs/zeta/zeta.hpp>

namespace cs::zeta {

int value() {
    return cs::delta::value() + cs::beta::value();
}

} // namespace cs::zeta
