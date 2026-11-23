// SPDX-License-Identifier: Apache-2.0

#pragma once

// Hanging a work (ADR 0006, Hanging): a painting at its true size, in a frame, on the project's
// centre line. Conventions of commands as in room.hpp.

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace cs::editor {

// The wall add_work hangs on: wall A, which new_room builds along y = 0.
inline constexpr std::string_view default_wall_label = "A";

// What add_work gives a new work, until the import of M2-11 asks.
inline constexpr std::string_view default_frame_style = "box";                  // M1's only frame
inline constexpr std::string_view default_material_preset = "oil-canvas-gloss"; // ADR 0003's 1
// From the face to the back of the work and its frame: the skeleton scene's. [C]
inline constexpr double default_stand_off_m = 0.04;

struct AddedWork {
    doc::Transaction transaction; // named "Add <title>", or "Add work" for an untitled one
    doc::Uuid work;
    doc::Uuid exhibit; // the entity that shows the work, with its framing and placement
};

// A flat work width_m wide and height_m high (unframed), showing `image`, hung on face A of the
// wall labelled default_wall_label (find_wall_labelled in room.hpp) at the middle of the face,
// its centre at the project's centre line (ProjectSettings::centre_line_m), default_stand_off_m
// from the face. Made of:
// - a Work with the title, the size, the image, material preset default_material_preset and its
//   other fields at their defaults (empty text, low responsivity, no source);
// - an entity named after the title ("Work" when it is empty) with an Exhibit of the work, a
//   Framing of style default_frame_style and nothing else set, and an OnWall placement.
// New ids are taken for the work, then the entity. `image` is an asset of the project already
// (ADR 0007); it is not looked for on disk.
//
// Throws std::invalid_argument unless width_m and height_m are finite and greater than 0, the
// image names an asset as the project file does (a SHA-256 of 64 lowercase hexadecimal digits,
// an extension of 1 to 8 characters from a-z and 0-9), the title is at most doc::max_string_bytes
// long, and the document has a wall labelled default_wall_label.
AddedWork add_work(const doc::Document& document, doc::IdGenerator& ids, const doc::AssetRef& image,
                   double width_m, double height_m, std::string title = {});

// The gap add_work_in_row leaves between neighbouring works, from one's outer edge to the next's,
// and the least it leaves between the row's ends and the face's ends. [C]
inline constexpr double row_gap_m = 0.4;
inline constexpr double row_end_clearance_m = 0.3;

// The width a work takes on a wall, from outer edge to outer edge, as world::compile builds it:
// the outer width of its frame (geom::outer_size of world::default_frame with the framing's member
// width, whatever the style, until M2-08), or the work's own width when it is unframed or its
// frame cannot be built (then shown unframed).
double hung_width_m(const doc::Work& work, const std::optional<doc::Framing>& framing);

// As add_work, with the same arguments, result and refusals, but hanging the new work in a row with
// the works already on the wall: several paintings side by side, as a curator drafts a hang
// (blueprint J1), and the hang of M1-50's sample paintings.
//
// The row is every exhibit placed on face A of the wall labelled default_wall_label, left to right
// in order of their distance along the face (equal distances in order of id), then the new work at
// its right end. The new work's centre is at the project's centre line; the others keep their
// heights and stand-offs. Each is moved along the face so that the row, from the first work's
// outer left edge to the last one's outer right edge (hung_width_m), is centred on the face, with
// row_gap_m between each work and the next. A spot that lights a work it moves (a luminaire aimed
// at its exhibit by an AimAtEntity, OnTrack on a track of two points parallel to the face, as
// add_spot_for makes) slides along its track by as much, so that it lights the work from the same
// place relative to it, at the same angle; it stops at the track's ends. Other luminaires stay
// where they are. All of it is the one transaction add_work names, so one undo takes the new work
// away and puts the others, and their spots, back.
//
// Throws std::invalid_argument, changing nothing, as add_work does, and when either end of the row
// would be closer than row_end_clearance_m to the face's ends: "Wall A is full: ...", saying how
// long the row and the wall are and what to do, for the curator.
AddedWork add_work_in_row(const doc::Document& document, doc::IdGenerator& ids,
                          const doc::AssetRef& image, double width_m, double height_m,
                          std::string title = {});

} // namespace cs::editor
