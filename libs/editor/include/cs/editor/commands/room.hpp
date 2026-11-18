// SPDX-License-Identifier: Apache-2.0

#pragma once

// Building a room (ADR 0006, Building): a box room from its width, depth and height, with its
// four walls lettered.
//
// Every command of libs/editor/commands builds one doc::Transaction, the whole of one user
// action, and returns it with the ids of what it adds; the caller commits it with
// doc::History::commit, so the action undoes in one step. A command reads the document the
// transaction is made for, and takes its new ids from the caller's doc::IdGenerator. One whose
// arguments break its rule throws std::invalid_argument and makes nothing.

#include <cs/doc/document.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cs::editor {

// The thickness of the walls new_room builds: the skeleton scene's. [C]
inline constexpr double default_wall_thickness_m = 0.1;

// The label of the wall lettered `index`, counting from 0, as a spreadsheet letters columns:
// "A" to "Z", then "AA", "AB", ..., "AZ", "BA", and so on.
std::string wall_label(std::size_t index);

// The first `count` labels in wall_label's order that no wall of the document has, in that
// order: in a document whose walls are lettered A to D, "E", "F", ...
std::vector<std::string> unused_wall_labels(const doc::Document& document, std::size_t count);

// The wall labelled `label` (exactly, case and all); of several, the first in order of id, the
// oldest. nullopt when no wall has the label.
std::optional<doc::Uuid> find_wall_labelled(const doc::Document& document, std::string_view label);

struct NewRoom {
    doc::Transaction transaction; // named "New room"
    doc::Uuid room;
    std::array<doc::Uuid, 4> walls; // in the order they are lettered
};

// A room width_m along x, depth_m along y and height_m high, as the skeleton scene's
// (tests/scenes/skeleton/README.md): an entity named "Room" whose outline is (0, 0),
// (width_m, 0), (width_m, depth_m), (0, depth_m), anticlockwise, with its floor at z = 0 and its
// ceiling at height_m; and one wall along each side of the outline in that order, starting along
// y = 0, each as high as the room and default_wall_thickness_m thick. Each wall stands outside the
// outline, with its face A on the side and facing into the room, and runs as long as the side.
// The walls take the first four unused_wall_labels, A to D in a document without walls, and are
// named after them: "Wall A". New ids are taken for the room, then the walls in order.
//
// Throws std::invalid_argument unless each size is finite and greater than 0.
NewRoom new_room(const doc::Document& document, doc::IdGenerator& ids, double width_m,
                 double depth_m, double height_m);

} // namespace cs::editor
