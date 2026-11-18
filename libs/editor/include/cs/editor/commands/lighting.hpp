// SPDX-License-Identifier: Apache-2.0

#pragma once

// Lighting a work the way the Canadian Conservation Institute advises (ADR 0006, Lighting): a
// spot on a ceiling track (ceiling height - 1.6 m) x tan 30 degrees from the wall, slid along the
// track until its aim at the work is 30 degrees from vertical. Conventions of commands as in
// room.hpp.

#include <cs/doc/document.hpp>
#include <cs/doc/library.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>

#include <numbers>
#include <string_view>

namespace cs::editor {

// The CCI's numbers, as ADR 0006 gives them: the angle from vertical at which a spot aims at a
// work, and the height above the floor that the track's distance from the wall is reckoned from.
inline constexpr double cci_aim_from_vertical_rad = std::numbers::pi / 6.0;
inline constexpr double cci_reference_height_m = 1.6;

// How far from a wall's face the CCI puts a ceiling track: (ceiling_height_m - 1.6 m) x tan 30
// degrees, where ceiling_height_m is the ceiling's height above the floor. 1.097 m for a 3.5 m
// ceiling. Throws std::invalid_argument unless ceiling_height_m is finite and greater than
// cci_reference_height_m.
double cci_track_distance_m(double ceiling_height_m);

// The fixture type add_spot_for uses, until fixture types come from data files (M3): a generic
// spot of 800 lm, beam 15 degrees and field 25 degrees (full angles), 3000 K, named
// "Generic spot 800 lm", as in the skeleton scene.
inline constexpr std::string_view generic_spot_id = "generic-spot-800";
doc::FixtureType generic_spot();

struct AddedSpot {
    doc::Transaction transaction; // named "Add spot"
    doc::Uuid spot;
    doc::Uuid track; // the track it is on: a new one, or one it reused
};

// A spot that lights `exhibit`, an entity with an Exhibit of a work in the document and an OnWall
// placement on a wall, as the CCI advises:
// - The ceiling is that of the room the centre of the work's face is in: whose outline holds it
//   in plan, and whose floor is at or below it and ceiling above it (the first such room in order
//   of id). Where no room is, the floor is at z = 0 and the ceiling at the top of the wall.
// - The track is straight and parallel to the face, cci_track_distance_m of the ceiling's height
//   above the floor out from it, at the ceiling's height, and as long as the face: its first
//   point is opposite the face's left end (seen facing it), so that a distance along it is a u
//   along the face. It is named "Track <the wall's label>". A track already in the document whose
//   two points and height are each within 1 mm of these is used instead of a new one.
// - The spot is a luminaire of type generic_spot_id at full output, aimed at the exhibit (an
//   AimAtEntity), on the track (OnTrack), named "Spot <n>" for the lowest n from 1 that no
//   entity of the document is named with, so that, as with wall letters, no two share a name and
//   a deleted spot's number is used again. It sits where its aim at the centre of the work's face
//   is cci_aim_from_vertical_rad from vertical. That centre is out from the wall's face by the
//   stand-off plus the work's depth, but at least world::min_face_offset_m, as world::compile
//   puts it. From the point of the track opposite that centre, the spot is slid towards the
//   track's first point (so that, seen facing the work, the light comes from the upper left),
//   or towards its last where the track ends too soon for that; where it ends too soon both
//   ways, the spot is at the track's point nearest the first of the two. Where
//   the aim from the point opposite is already further than 30 degrees from vertical (a work
//   hung high), the spot stays opposite.
// - The project's library gains generic_spot() as generic_spot_id, unless it holds a fixture
//   type of that id already, which the spot then uses as it is.
// New ids are taken for the track, when it is new, then the spot.
//
// Throws std::invalid_argument unless `exhibit` is such an entity with a placement on a wall
// whose face geom::wall_face_frame accepts; the ceiling is more than cci_reference_height_m above
// the floor; the track is further out from the wall's face than the work's face; and the centre of
// the work's face is below the ceiling.
AddedSpot add_spot_for(const doc::Document& document, doc::IdGenerator& ids,
                       const doc::Uuid& exhibit);

} // namespace cs::editor
