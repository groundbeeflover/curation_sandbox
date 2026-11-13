// SPDX-License-Identifier: Apache-2.0

#include <cs/world/ids.hpp>

#include <cs/doc/uuid.hpp>

namespace cs::world {

// The document's own formatter, so that an id reads the same in the world as in project.json.
std::string Id::to_string() const {
    return doc::Uuid(bytes_).to_string();
}

} // namespace cs::world
