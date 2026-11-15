// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

namespace cs::greet {

void register_bye(std::vector<std::string>& names, const std::string& prefix) {
    names.push_back(prefix + "bye");
}

} // namespace cs::greet
