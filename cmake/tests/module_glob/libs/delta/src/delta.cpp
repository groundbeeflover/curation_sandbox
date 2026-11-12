// SPDX-License-Identifier: Apache-2.0
#include <cs/delta/delta.hpp>

namespace cs::delta {

int value() {
    return Wrapped{}.beta_value + 1;
}

} // namespace cs::delta
