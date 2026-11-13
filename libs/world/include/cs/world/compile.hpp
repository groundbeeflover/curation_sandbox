// SPDX-License-Identifier: Apache-2.0

#pragma once

// Compiling the document into the render world (world.hpp), all at once. Incremental updates
// from change lists arrive with M2-24.
//
// What each entity becomes, in milestone 1:
// - A Room: one instance of its inside surfaces, Lambert albedo default_wall_albedo. When its
//   outline is an axis-aligned rectangle these are geom::room_mesh's floor, ceiling and four
//   walls; otherwise only the walls, one quad per side of the outline from floor to ceiling,
//   facing in, and a problem, until rooms of any shape get floors and ceilings (M2-01, M2-31).
// - A Wall: nothing drawn yet; works and fixtures hang on its faces. Its visible surfaces are the
//   room's, whose sides lie on the walls' faces (tests/scenes/skeleton/README.md), until walls
//   and openings are built (M2-31).
// - An exhibit of a flat work hung on a wall: a painting instance (its face, at the work's true
//   size) and, when the entity has a Framing, a frame instance, both with the transform of
//   geom::on_wall_transform; plus an Exhibit with its visible area. Every frame style is a box
//   frame for now (default_frame below), with the framing's width and colour when it sets them;
//   styles, mats and glazing arrive with M2-08.
// - A luminaire on a track, on a wall or in a ceiling: a Light, a generic spot with the fixture
//   type's lumens times the dimming, aimed at a point or at an entity (the centre of an exhibit's
//   face; the position of a luminaire, viewpoint or lux sensor). One that is dimmed to 0 emits
//   nothing and is left out without a problem.
// - A viewpoint: a perspective camera.
//
// Compile never throws for a document that doc::load_project_folder accepts. Whatever geom or
// light cannot build is left out of the world, or shown in part (a frame that cannot be built
// leaves the work unframed), and listed in World::problems with its entity's id and the reason.

#include <cs/world/world.hpp>

namespace cs::doc {
class Document;
} // namespace cs::doc

namespace cs::world {

// The box frame that stands in for every frame style until M2-08, with the skeleton scene's
// numbers (M1-10): members 6 cm wide and 4 cm deep, overlapping the work by 5 mm, the low end of
// ADR 0003's 5 to 10 mm. A work deeper than that leaves the frame's front at least 5 mm in
// front of its face (front_lip_m), so that the two never lie in one plane and the tiers agree
// on which is seen. The colour is a dark stained wood, and a framing's colour is clamped to at
// most 1 in each channel, with a problem. [C]
struct DefaultFrame {
    double member_width_m = 0.06;
    double depth_m = 0.04;
    double rebate_m = 0.005;
    double front_lip_m = 0.005;
    double albedo = 0.1;
};
inline constexpr DefaultFrame default_frame{};

// The least distance from a wall's face to the face of a work hung on it. A sheet of paper hung
// flat on the wall (no stand-off, no depth) is shown this far out, about a thick paper's
// thickness, so that its face does not lie in the wall's plane, where the renderers would pick
// either surface. [C]
inline constexpr double min_face_offset_m = 0.001;

// Takes a document, which the caller keeps; the world copies what it needs.
World compile(const doc::Document& document);

} // namespace cs::world
