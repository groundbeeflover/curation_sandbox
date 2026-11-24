// SPDX-License-Identifier: Apache-2.0

// The first editor commands (#66, M1-25): new_room, add_work and add_spot_for, each one
// transaction that undoes in one step, from the issue's acceptance criteria and the headers of
// libs/editor/include/cs/editor/commands/. At the end, add_work_in_row and hung_width_m (#88,
// M1-27): several paintings side by side on wall A. The numbers are the issue's and ADR 0006's:
// four walls lettered A to D, a work at the middle of wall A on the project's centre line, and a
// spot on a ceiling track (ceiling height - 1.6 m) x tan 30 degrees = 1.097 m from the face of
// a 3.5 m room, slid along it until its aim at the centre of the work's face is 30 degrees from
// vertical. The skeleton scene (tests/scenes/skeleton/README.md) is the worked example: there the
// slide is 0.465 m.
//
// Coordinates as in the document: metres, z up, the floor at z = 0. new_room(6, 4, 3.5) builds
// wall A along y = 0, so face A of wall A faces +y, its left end (seen facing it from the room) is
// at x = 6 and u runs towards x = 0 (geom/wall_face.hpp): u = 3 is x = 3.

#include <cs/editor/commands/hanging.hpp>
#include <cs/editor/commands/lighting.hpp>
#include <cs/editor/commands/room.hpp>

#include <cs/core/random.hpp>
#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/history.hpp>
#include <cs/doc/library.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>
#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/work_lux.hpp>
#include <cs/geom/box_frame.hpp>
#include <cs/geom/wall_face.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cs::editor;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using glm::dvec2;
using glm::dvec3;

namespace doc = cs::doc;
namespace geom = cs::geom;
namespace world = cs::world;

namespace Catch {

// Ids in failure messages, as text.
template <> struct StringMaker<cs::doc::Uuid> {
    static std::string convert(const cs::doc::Uuid& id) { return id.to_string(); }
};

} // namespace Catch

namespace {

constexpr double pi = std::numbers::pi;
constexpr double deg = pi / 180.0;
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

// ---- Tolerances ------------------------------------------------------------------------------

// The issue's: the track's distance from the face and the spot's slide within 1 mm, the aim within
// 0.1 degree.
constexpr double issue_m = 0.001;
constexpr double issue_aim_rad = 0.1 * deg;

// Lengths the commands take from the sizes given, by sums and halves (a wall's centre line is half
// of 0.1 m outside the outline, its face half of 0.1 m back in): exact but for rounding, about
// 1e-15 m. A nanometre allows any order of operations and still catches any real mistake.
constexpr double exact_m = 1e-9;
// The same for unit vectors.
constexpr double exact_unit = 1e-9;
// Degrees to radians, one multiplication and one division: rounding of about 1e-16 rad.
constexpr double exact_rad = 1e-12;

// ---- The session -----------------------------------------------------------------------------

// New ids, the same in every run: a fixed seed and a clock stopped at one millisecond,
// 1 800 000 000 000 ms, after the skeleton's ids (1 794 441 600 000 ms, 12 November 2026), so
// ids made for it sort after the scene's, as in a real session.
doc::IdGenerator fixed_ids() {
    return doc::IdGenerator(cs::hash_seed(66, 0, 0, 0, 0),
                            [] { return std::int64_t{1'800'000'000'000}; });
}

// The skeleton's test chart image, as an asset of the project (add_work does not look for it).
const doc::AssetRef chart_image{
    .sha256 = "26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a",
    .extension = "png"};

struct RoomIds {
    doc::Uuid room;
    std::array<doc::Uuid, 4> walls;
};

struct WorkIds {
    doc::Uuid work;
    doc::Uuid exhibit;
};

struct SpotIds {
    doc::Uuid spot;
    doc::Uuid track;
};

RoomIds commit_new_room(doc::History& history, doc::IdGenerator& ids, double width_m,
                        double depth_m, double height_m) {
    NewRoom made = new_room(history.document(), ids, width_m, depth_m, height_m);
    const RoomIds room{made.room, made.walls};
    history.commit(std::move(made.transaction));
    return room;
}

WorkIds commit_add_work(doc::History& history, doc::IdGenerator& ids, double width_m,
                        double height_m, std::string title = {}) {
    AddedWork made = add_work(history.document(), ids, chart_image, width_m, height_m, title);
    const WorkIds work{made.work, made.exhibit};
    history.commit(std::move(made.transaction));
    return work;
}

SpotIds commit_add_spot(doc::History& history, doc::IdGenerator& ids, const doc::Uuid& exhibit) {
    AddedSpot made = add_spot_for(history.document(), ids, exhibit);
    const SpotIds spot{made.spot, made.track};
    history.commit(std::move(made.transaction));
    return spot;
}

// The issue's room, new_room(6, 4, 3.5) (the skeleton's sizes), with the skeleton's chart,
// 0.73 m x 0.92 m, hung by add_work.
struct Gallery {
    RoomIds room;
    WorkIds work;
};

Gallery commit_gallery(doc::History& history, doc::IdGenerator& ids) {
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds work = commit_add_work(history, ids, 0.73, 0.92, "Test chart");
    return {room, work};
}

doc::Document load_skeleton() {
    return doc::load_project_folder(std::filesystem::path(CS_SOURCE_DIR "/tests/scenes/skeleton"));
}

doc::Uuid entity_named(const doc::Document& document, std::string_view name) {
    const auto& entities = document.entities();
    const auto it = std::find_if(entities.begin(), entities.end(),
                                 [&](const auto& entry) { return entry.second.name == name; });
    REQUIRE(it != entities.end());
    return it->first;
}

const doc::Entity& entity_of(const doc::Document& document, const doc::Uuid& id) {
    const doc::Entity* entity = document.find_entity(id);
    REQUIRE(entity != nullptr);
    return *entity;
}

// Changes one entity's components and commits it, as a curator's edit between commands would.
template <class Component>
void commit_set(doc::History& history, const doc::Uuid& entity, Component component) {
    doc::Transaction edit(history.document(), "Edit");
    edit.set(entity, std::move(component));
    history.commit(std::move(edit));
}

// ---- Geometry --------------------------------------------------------------------------------

dvec2 plan(const doc::Vec2& p) {
    return {p.x, p.y};
}

const doc::OnWall& on_wall_of(const doc::Entity& entity) {
    REQUIRE(entity.placement.has_value());
    const auto* at = std::get_if<doc::OnWall>(&*entity.placement);
    REQUIRE(at != nullptr);
    return *at;
}

const doc::OnTrack& on_track_of(const doc::Entity& entity) {
    REQUIRE(entity.placement.has_value());
    const auto* at = std::get_if<doc::OnTrack>(&*entity.placement);
    REQUIRE(at != nullptr);
    return *at;
}

geom::FaceFrame face_frame(const doc::Wall& wall, doc::WallFace face) {
    return geom::wall_face_frame(plan(wall.start_m), plan(wall.end_m), wall.thickness_m,
                                 face == doc::WallFace::a ? geom::WallFace::a : geom::WallFace::b);
}

// The face an exhibit hangs on.
geom::FaceFrame face_of(const doc::Document& document, const doc::Uuid& exhibit) {
    const doc::OnWall& at = on_wall_of(entity_of(document, exhibit));
    const doc::Entity& wall = entity_of(document, at.wall);
    REQUIRE(wall.wall.has_value());
    return face_frame(*wall.wall, at.face);
}

// How far out from the wall's face the work's face is, as lighting.hpp and world::compile put it:
// the stand-off plus the work's depth, at least world::min_face_offset_m.
double face_offset_m(const doc::Document& document, const doc::Uuid& exhibit) {
    const doc::Entity& entity = entity_of(document, exhibit);
    REQUIRE(entity.exhibit.has_value());
    const doc::Work* work = document.find_work(entity.exhibit->work);
    REQUIRE(work != nullptr);
    return std::max(on_wall_of(entity).stand_off_m + work->form.depth_m.value_or(0.0),
                    world::min_face_offset_m);
}

// The centre of the work's face, where the spot aims.
dvec3 work_face_centre(const doc::Document& document, const doc::Uuid& exhibit) {
    const doc::OnWall& at = on_wall_of(entity_of(document, exhibit));
    return face_of(document, exhibit)
        .point(at.distance_m, at.centre_height_m, face_offset_m(document, exhibit));
}

// The point distance_m along a polyline from its first point, at height_m.
dvec3 along_polyline(const std::vector<doc::Vec2>& points_m, double distance_m, double height_m) {
    REQUIRE(points_m.size() >= 2);
    double left_m = distance_m;
    for (std::size_t i = 0; i + 1 < points_m.size(); ++i) {
        const dvec2 a = plan(points_m[i]);
        const dvec2 b = plan(points_m[i + 1]);
        const double length_m = glm::length(b - a);
        if (left_m <= length_m || i + 2 == points_m.size()) {
            const dvec2 p = a + (b - a) * (left_m / length_m);
            return {p.x, p.y, height_m};
        }
        left_m -= length_m;
    }
    return {0.0, 0.0, height_m};
}

// Where a luminaire on a track is, from the document's numbers.
dvec3 spot_position(const doc::Document& document, const doc::Uuid& spot) {
    const doc::OnTrack& at = on_track_of(entity_of(document, spot));
    const doc::Entity& track = entity_of(document, at.track);
    REQUIRE(track.track.has_value());
    return along_polyline(track.track->points_m, at.distance_m, track.track->height_m);
}

// The angle between the downward vertical and the direction from `from` to `to`.
double from_vertical_rad(dvec3 from, dvec3 to) {
    const dvec3 d = to - from;
    return std::atan2(std::hypot(d.x, d.y), -d.z);
}

// The CCI's slide, analytically: from the point of the track opposite the work's face centre, the
// spot moves s along the track so that the horizontal distance to that centre, sqrt(s^2 + out^2)
// where `out` is the track's distance from the work's face, is drop x tan 30 degrees, drop being
// how far the centre is below the track.
double cci_slide_m(double drop_m, double out_m) {
    const double horizontal_m = drop_m * std::tan(30.0 * deg);
    return std::sqrt(horizontal_m * horizontal_m - out_m * out_m);
}

// (ceiling - 1.6 m) x tan 30 degrees, ADR 0006's distance, computed here rather than by the
// function under test.
double cci_distance_m(double ceiling_m) {
    return (ceiling_m - 1.6) * std::tan(30.0 * deg);
}

void check_near(dvec3 actual, dvec3 expected, double tolerance) {
    CHECK_THAT(actual.x, WithinAbs(expected.x, tolerance));
    CHECK_THAT(actual.y, WithinAbs(expected.y, tolerance));
    CHECK_THAT(actual.z, WithinAbs(expected.z, tolerance));
}

const world::Light& light_of(const world::World& compiled, const doc::Uuid& entity) {
    const world::Id id(entity.bytes());
    const auto it = std::find_if(compiled.lights.begin(), compiled.lights.end(),
                                 [&](const world::Light& light) { return light.entity == id; });
    REQUIRE(it != compiled.lights.end());
    return *it;
}

const world::Exhibit& exhibit_of(const world::World& compiled, const doc::Uuid& entity) {
    const world::Id id(entity.bytes());
    const auto it =
        std::find_if(compiled.exhibits.begin(), compiled.exhibits.end(),
                     [&](const world::Exhibit& exhibit) { return exhibit.entity == id; });
    REQUIRE(it != compiled.exhibits.end());
    return *it;
}

// Shorter first, then alphabetical: the order in which a spreadsheet letters its columns.
bool shortlex_less(const std::string& a, const std::string& b) {
    return a.size() != b.size() ? a.size() < b.size() : a < b;
}

bool only_capitals(const std::string& label) {
    return !label.empty() &&
           std::all_of(label.begin(), label.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

// ---- Undo ------------------------------------------------------------------------------------

// The issue's criterion for one command: committed, it is one more undo step; undone, the document
// serialises identical to before; redone, identical to after.
void check_undoes_in_one_step(doc::History& history, doc::Transaction transaction) {
    const std::string before = doc::to_project_json(history.document());
    const std::size_t steps = history.undo_count();

    const doc::ChangeList changes = history.commit(std::move(transaction));
    CHECK_FALSE(changes.empty());
    REQUIRE(history.undo_count() == steps + 1);
    const std::string after = doc::to_project_json(history.document());
    CHECK(after != before);

    history.undo();
    CHECK(history.undo_count() == steps);
    CHECK(doc::to_project_json(history.document()) == before);

    history.redo();
    CHECK(history.undo_count() == steps + 1);
    CHECK_FALSE(history.can_redo());
    CHECK(doc::to_project_json(history.document()) == after);
}

} // namespace

// ==== new_room ================================================================================

TEST_CASE("new_room: one entity, named Room, whose Room has the outline (0, 0), (w, 0), (w, d), "
          "(0, d), its floor at 0 and its ceiling at the height",
          "[editor][commands]") {
    const auto [width_m, depth_m, height_m] =
        GENERATE(std::tuple{6.0, 4.0, 3.5}, std::tuple{5.2, 7.5, 4.1});
    CAPTURE(width_m, depth_m, height_m);
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds made = commit_new_room(history, ids, width_m, depth_m, height_m);

    const doc::Entity& room = entity_of(history.document(), made.room);
    CHECK(room.name == "Room");
    REQUIRE(room.room.has_value());
    // The outline is the sizes themselves, so it is compared exactly.
    const std::vector<doc::Vec2> outline{
        {0.0, 0.0}, {width_m, 0.0}, {width_m, depth_m}, {0.0, depth_m}};
    CHECK(room.room->outline_m == outline);
    CHECK(room.room->floor_height_m == 0.0);
    CHECK(room.room->ceiling_height_m == height_m);
    CHECK_FALSE(room.wall.has_value());
    CHECK_FALSE(room.placement.has_value());

    // The room and its four walls, and nothing else.
    CHECK(history.document().entities().size() == 5);
    CHECK(history.document().works().empty());
}

TEST_CASE("new_room: four walls lettered A to D and named Wall A to Wall D, each 0.1 m "
          "thick and as high as the room",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds made = commit_new_room(history, ids, 6.0, 4.0, 3.5);

    CHECK(default_wall_thickness_m == 0.1);
    const std::array<std::string, 4> letters{"A", "B", "C", "D"};
    for (std::size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        const doc::Entity& entity = entity_of(history.document(), made.walls[i]);
        CHECK(entity.name == "Wall " + letters[i]);
        REQUIRE(entity.wall.has_value());
        CHECK(entity.wall->label == letters[i]);
        CHECK(entity.wall->height_m == 3.5);
        CHECK(entity.wall->thickness_m == default_wall_thickness_m);
        CHECK_FALSE(entity.room.has_value());
        CHECK_FALSE(entity.placement.has_value());
    }
}

TEST_CASE("new_room: each wall runs along its side of the outline, in order from y = 0, with "
          "face A on the side and facing into the room",
          "[editor][commands]") {
    const auto [width_m, depth_m] = GENERATE(std::pair{6.0, 4.0}, std::pair{5.2, 7.5});
    CAPTURE(width_m, depth_m);
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds made = commit_new_room(history, ids, width_m, depth_m, 3.5);

    // The outline runs anticlockwise, so the room is on the left of each side from p to q. Facing
    // a side's face A from inside the room, q is on the left: face A's left end is q, its u runs
    // from q to p, and its normal is the side's direction turned 90 degrees anticlockwise.
    const std::array<dvec2, 4> corners{dvec2{0.0, 0.0}, dvec2{width_m, 0.0},
                                       dvec2{width_m, depth_m}, dvec2{0.0, depth_m}};
    for (std::size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        const dvec2 p = corners[i];
        const dvec2 q = corners[(i + 1) % 4];
        const dvec2 side = glm::normalize(q - p);
        const doc::Entity& entity = entity_of(history.document(), made.walls[i]);
        REQUIRE(entity.wall.has_value());

        const geom::FaceFrame face = face_frame(*entity.wall, doc::WallFace::a);
        check_near(face.origin, {q.x, q.y, 0.0}, exact_m);
        check_near(face.u, {-side.x, -side.y, 0.0}, exact_unit);
        check_near(face.normal, {-side.y, side.x, 0.0}, exact_unit);
        CHECK_THAT(face.length_m, WithinAbs(glm::length(q - p), exact_m));

        // The room's middle is in front of face A, and behind face B.
        const dvec3 middle(width_m / 2.0, depth_m / 2.0, 1.0);
        CHECK(glm::dot(middle - face.origin, face.normal) > 0.0);
        const geom::FaceFrame back = face_frame(*entity.wall, doc::WallFace::b);
        CHECK(glm::dot(middle - back.origin, back.normal) < 0.0);
    }
}

TEST_CASE("new_room: the room and its walls are one transaction, New room, with ids "
          "taken for the room, then the walls in order",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    NewRoom made = new_room(history.document(), ids, 6.0, 4.0, 3.5);

    CHECK(made.transaction.name() == "New room");
    CHECK(&made.transaction.document() == &history.document());
    CHECK(made.room < made.walls[0]);
    CHECK(made.walls[0] < made.walls[1]);
    CHECK(made.walls[1] < made.walls[2]);
    CHECK(made.walls[2] < made.walls[3]);

    const doc::ChangeList changes = made.transaction.changes();
    CHECK_FALSE(changes.settings);
    CHECK(changes.fixture_types.empty());
    CHECK(changes.works.empty());
    REQUIRE(changes.entities.size() == 5);
    for (const doc::EntityChange& change : changes.entities) {
        CHECK(change.kind == doc::ChangeKind::added);
    }
    // The command made the transaction; it did not commit it.
    CHECK(history.document().entities().empty());
    CHECK(history.undo_count() == 0);

    history.commit(std::move(made.transaction));
    CHECK(history.undo_count() == 1);
    CHECK(history.undo_name().value_or("") == "New room");
    CHECK(history.document().entities().size() == 5);
}

TEST_CASE("new_room: a second room's walls continue the lettering, E to H", "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const RoomIds second = commit_new_room(history, ids, 3.0, 2.0, 2.8);

    CHECK(entity_of(history.document(), second.room).name == "Room");
    const std::array<std::string, 4> letters{"E", "F", "G", "H"};
    for (std::size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        const doc::Entity& entity = entity_of(history.document(), second.walls[i]);
        CHECK(entity.name == "Wall " + letters[i]);
        REQUIRE(entity.wall.has_value());
        CHECK(entity.wall->label == letters[i]);
        CHECK(entity.wall->height_m == 2.8);
    }
    CHECK(history.document().entities().size() == 10);
    CHECK(history.undo_count() == 2);
}

TEST_CASE("new_room: walls take the first unused labels, so a freed letter is used again",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds first = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    doc::Transaction remove_b(history.document(), "Delete wall B");
    remove_b.remove_entity(first.walls[1]);
    history.commit(std::move(remove_b));

    const RoomIds second = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const std::array<std::string, 4> letters{"B", "E", "F", "G"};
    for (std::size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        const doc::Entity& entity = entity_of(history.document(), second.walls[i]);
        REQUIRE(entity.wall.has_value());
        CHECK(entity.wall->label == letters[i]);
        CHECK(entity.name == "Wall " + letters[i]);
    }
}

TEST_CASE("new_room: sizes that are not finite and greater than 0 throw std::invalid_argument "
          "and make nothing",
          "[editor][commands]") {
    const double bad = GENERATE(0.0, -0.0, -1.0, -1e-9, not_a_number, inf, -inf);
    CAPTURE(bad);
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const std::string before = doc::to_project_json(history.document());

    CHECK_THROWS_AS(new_room(history.document(), ids, bad, 4.0, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(new_room(history.document(), ids, 6.0, bad, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(new_room(history.document(), ids, 6.0, 4.0, bad), std::invalid_argument);
    CHECK_THROWS_AS(new_room(history.document(), ids, bad, bad, bad), std::invalid_argument);

    CHECK(history.undo_count() == 0);
    CHECK(doc::to_project_json(history.document()) == before);
}

// From the code review: sizes near the ends of a double's range are finite and greater than 0, so
// they are taken, and each wall still stands exactly half a thickness outside its side. Exact:
// adding 0.05 m to a coordinate of 0 or of 1e-200 m, or to one of 1e160 m, is one rounding.
TEST_CASE("new_room: a tiny or a huge size gives walls half a thickness outside the outline",
          "[editor][commands]") {
    const double size_m = GENERATE(1e-200, 1e160);
    CAPTURE(size_m);
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, size_m, size_m, 3.5);
    const double half = default_wall_thickness_m / 2.0;
    const std::array<std::pair<doc::Vec2, doc::Vec2>, 4> expected{{
        {{0.0, -half}, {size_m, -half}},
        {{size_m + half, 0.0}, {size_m + half, size_m}},
        {{size_m, size_m + half}, {0.0, size_m + half}},
        {{-half, size_m}, {-half, 0.0}},
    }};
    for (std::size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        const doc::Wall& wall = *entity_of(history.document(), room.walls[i]).wall;
        CHECK(wall.start_m == expected[i].first);
        CHECK(wall.end_m == expected[i].second);
    }
}

// ==== Wall labels =============================================================================

TEST_CASE("wall_label: A to Z, then AA, AB, ..., AZ, BA, ..., ZZ, AAA, as a spreadsheet letters "
          "columns",
          "[editor][commands]") {
    CHECK(wall_label(0) == "A");
    CHECK(wall_label(1) == "B");
    CHECK(wall_label(3) == "D");
    CHECK(wall_label(4) == "E");
    CHECK(wall_label(25) == "Z");
    CHECK(wall_label(26) == "AA");
    CHECK(wall_label(27) == "AB");
    CHECK(wall_label(51) == "AZ");
    CHECK(wall_label(52) == "BA");
    CHECK(wall_label(701) == "ZZ");
    CHECK(wall_label(702) == "AAA");
    // 26 labels of one letter, 26^2 = 676 of two and 26^3 = 17 576 of three: the last of three
    // letters is 26 + 676 + 17 576 - 1 = 18 277, and the next has four.
    CHECK(wall_label(18'277) == "ZZZ");
    CHECK(wall_label(18'278) == "AAAA");
}

TEST_CASE("wall_label: every label to ZZZ is the next in order, none skipped or repeated",
          "[editor][commands]") {
    // From "A" (0) to "ZZZ" (18 277) there are exactly 18 278 strings of one to three capitals.
    // 18 278 labels of capitals that strictly increase, shorter first and then alphabetically, from
    // the one to the other, are therefore all of them, in order.
    bool capitals = true;
    bool increasing = true;
    std::string previous = wall_label(0);
    for (std::size_t i = 1; i <= 18'277; ++i) {
        std::string label = wall_label(i);
        capitals = capitals && only_capitals(label);
        increasing = increasing && shortlex_less(previous, label);
        previous = std::move(label);
    }
    CHECK(capitals);
    CHECK(increasing);
    CHECK(previous == "ZZZ");
}

TEST_CASE("wall_label: the largest index has a label of capitals", "[editor][commands]") {
    constexpr std::size_t largest = std::numeric_limits<std::size_t>::max();
    const std::string label = wall_label(largest);
    CHECK(only_capitals(label));
    CHECK(shortlex_less(wall_label(largest - 1), label));
    if constexpr (sizeof(std::size_t) == 8) {
        // Labels of at most n letters number 26 + 26^2 + ... + 26^n = (26^(n+1) - 26) / 25: for
        // n = 13 that is 2.6e18, below 2^64 - 1 = 1.8e19, and for n = 14 it is 6.7e19, above it.
        // So index 2^64 - 1 has 14 letters.
        CHECK(label.size() == 14);
    }
}

TEST_CASE("unused_wall_labels: the first labels in wall_label's order that no wall has",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    using Labels = std::vector<std::string>;

    CHECK(unused_wall_labels(history.document(), 4) == Labels{"A", "B", "C", "D"});
    CHECK(unused_wall_labels(history.document(), 0).empty());

    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    CHECK(unused_wall_labels(history.document(), 3) == Labels{"E", "F", "G"});
    CHECK(unused_wall_labels(history.document(), 0).empty());

    const Labels many = unused_wall_labels(history.document(), 1000);
    REQUIRE(many.size() == 1000);
    CHECK(many.front() == "E");
    CHECK(many.back() == wall_label(1003));

    // Wall B relabelled "Lobby": B is free again, and "Lobby" is no letter.
    doc::Wall b = *entity_of(history.document(), room.walls[1]).wall;
    b.label = "Lobby";
    commit_set(history, room.walls[1], b);
    CHECK(unused_wall_labels(history.document(), 3) == Labels{"B", "E", "F"});
}

TEST_CASE("find_wall_labelled: the wall with the label exactly, the oldest of several",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    CHECK_FALSE(find_wall_labelled(history.document(), "A").has_value());

    const RoomIds first = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    CHECK(find_wall_labelled(history.document(), "A") == first.walls[0]);
    CHECK(find_wall_labelled(history.document(), "D") == first.walls[3]);
    CHECK_FALSE(find_wall_labelled(history.document(), "E").has_value());
    CHECK_FALSE(find_wall_labelled(history.document(), "a").has_value());
    CHECK_FALSE(find_wall_labelled(history.document(), "A ").has_value());
    CHECK_FALSE(find_wall_labelled(history.document(), "Wall A").has_value());
    CHECK_FALSE(find_wall_labelled(history.document(), "").has_value());
    // Rooms are not walls.
    CHECK_FALSE(find_wall_labelled(history.document(), "Room").has_value());

    // A newer wall relabelled "A": the older is found.
    const RoomIds second = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    doc::Wall e = *entity_of(history.document(), second.walls[0]).wall;
    e.label = "A";
    commit_set(history, second.walls[0], e);
    REQUIRE(first.walls[0] < second.walls[0]);
    CHECK(find_wall_labelled(history.document(), "A") == first.walls[0]);
}

// ==== add_work ================================================================================

TEST_CASE("add_work: a Work of the size and image, preset oil-canvas-gloss, its other fields at "
          "their defaults",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds made = commit_add_work(history, ids, 0.73, 0.92, "Water Lilies");

    CHECK(default_material_preset == "oil-canvas-gloss");
    const doc::Work* work = history.document().find_work(made.work);
    REQUIRE(work != nullptr);
    doc::Work expected;
    expected.title = "Water Lilies";
    expected.form = doc::FlatForm{.width_m = 0.73, .height_m = 0.92, .depth_m = std::nullopt};
    expected.image = chart_image;
    expected.material.preset = std::string(default_material_preset);
    CHECK(*work == expected);
    // As the header spells the defaults out.
    CHECK(work->artist.empty());
    CHECK(work->number.empty());
    CHECK(work->sensitivity.responsivity == doc::Responsivity::low);
    CHECK_FALSE(work->source.has_value());
    CHECK(history.document().works().size() == 1);
}

TEST_CASE("add_work: an entity named after the title, with an Exhibit of the work, a box Framing "
          "and an OnWall placement, and nothing else",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    commit_new_room(history, ids, 6.0, 4.0, 3.5);

    std::optional<WorkIds> made;
    SECTION("titled") {
        made = commit_add_work(history, ids, 0.73, 0.92, "Water Lilies");
        CHECK(entity_of(history.document(), made->exhibit).name == "Water Lilies");
    }
    SECTION("untitled: named Work") {
        made = commit_add_work(history, ids, 0.73, 0.92);
        CHECK(entity_of(history.document(), made->exhibit).name == "Work");
        CHECK(history.document().find_work(made->work)->title.empty());
    }
    REQUIRE(made.has_value());

    const doc::Entity& entity = entity_of(history.document(), made->exhibit);
    REQUIRE(entity.exhibit.has_value());
    CHECK(entity.exhibit->work == made->work);
    CHECK(default_frame_style == "box");
    REQUIRE(entity.framing.has_value());
    // The style and no override: no width, colour, mat or glazing.
    doc::Framing box;
    box.style = std::string(default_frame_style);
    CHECK(*entity.framing == box);
    REQUIRE(entity.placement.has_value());
    CHECK(std::holds_alternative<doc::OnWall>(*entity.placement));
    CHECK_FALSE(entity.wall.has_value());
    CHECK_FALSE(entity.opening.has_value());
    CHECK_FALSE(entity.room.has_value());
    CHECK_FALSE(entity.track.has_value());
    CHECK_FALSE(entity.luminaire.has_value());
    CHECK_FALSE(entity.lux_sensor.has_value());
    CHECK_FALSE(entity.viewpoint.has_value());
    CHECK_FALSE(entity.dimension.has_value());

    // One exhibit, and one more entity than the room and its walls.
    const auto exhibits =
        std::count_if(history.document().entities().begin(), history.document().entities().end(),
                      [](const auto& entry) { return entry.second.exhibit.has_value(); });
    CHECK(exhibits == 1);
    CHECK(history.document().entities().size() == 6);
}

TEST_CASE("add_work: hangs on face A of wall A at the middle of the face, its centre on the "
          "project's centre line, 0.04 m out",
          "[editor][commands]") {
    const auto [width_m, centre_line_m, middle_u_m] =
        GENERATE(std::tuple{6.0, 1.50, 3.0}, std::tuple{5.0, 1.45, 2.5});
    CAPTURE(width_m, centre_line_m);
    doc::DocumentData data;
    data.settings.centre_line_m = centre_line_m;
    doc::History history{doc::Document(data)};
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, width_m, 4.0, 3.5);
    const WorkIds made = commit_add_work(history, ids, 0.73, 0.92, "Test chart");

    CHECK(default_wall_label == "A");
    CHECK(default_stand_off_m == 0.04);
    const doc::OnWall& at = on_wall_of(entity_of(history.document(), made.exhibit));
    CHECK(at.wall == room.walls[0]);
    CHECK(at.face == doc::WallFace::a);
    // The middle of a face as long as the room is wide; sizes halved, so exact but for rounding.
    CHECK_THAT(at.distance_m, WithinAbs(middle_u_m, exact_m));
    CHECK(at.centre_height_m == centre_line_m);
    CHECK(at.stand_off_m == default_stand_off_m);

    // In the world: the middle of wall A, x = width / 2 at y = 0, and 1.5 m (or 1.45 m) high.
    const dvec3 centre = work_face_centre(history.document(), made.exhibit);
    check_near(centre, {width_m / 2.0, 0.04, centre_line_m}, exact_m);
}

TEST_CASE("add_work: the work and its entity are one transaction, Add <title>, with ids "
          "taken for the work, then the entity",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);

    AddedWork titled = add_work(history.document(), ids, chart_image, 0.73, 0.92, "Water Lilies");
    CHECK(titled.transaction.name() == "Add Water Lilies");
    CHECK(&titled.transaction.document() == &history.document());
    CHECK(room.walls[3] < titled.work);
    CHECK(titled.work < titled.exhibit);
    const doc::ChangeList changes = titled.transaction.changes();
    CHECK_FALSE(changes.settings);
    CHECK(changes.fixture_types.empty());
    REQUIRE(changes.works.size() == 1);
    CHECK(changes.works[0] == doc::WorkChange{titled.work, doc::ChangeKind::added});
    REQUIRE(changes.entities.size() == 1);
    CHECK(changes.entities[0] == doc::EntityChange{titled.exhibit, doc::ChangeKind::added});
    CHECK(history.document().works().empty());

    history.commit(std::move(titled.transaction));
    CHECK(history.undo_count() == 2);
    CHECK(history.undo_name().value_or("") == "Add Water Lilies");

    AddedWork untitled = add_work(history.document(), ids, chart_image, 0.5, 0.4);
    CHECK(untitled.transaction.name() == "Add work");
}

TEST_CASE("add_work: throws std::invalid_argument with no wall labelled A, or sizes that are not "
          "finite and greater than 0",
          "[editor][commands]") {
    doc::IdGenerator ids = fixed_ids();

    SECTION("no walls") {
        const doc::Document empty;
        CHECK_THROWS_AS(add_work(empty, ids, chart_image, 0.73, 0.92), std::invalid_argument);
    }
    SECTION("wall A relabelled in lower case: labels are matched exactly") {
        doc::History history;
        const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
        doc::Wall a = *entity_of(history.document(), room.walls[0]).wall;
        a.label = "a";
        commit_set(history, room.walls[0], a);
        CHECK_THROWS_AS(add_work(history.document(), ids, chart_image, 0.73, 0.92),
                        std::invalid_argument);
    }
    SECTION("sizes") {
        const double bad = GENERATE(0.0, -0.0, -0.73, not_a_number, inf, -inf);
        CAPTURE(bad);
        doc::History history;
        commit_new_room(history, ids, 6.0, 4.0, 3.5);
        const std::string before = doc::to_project_json(history.document());
        CHECK_THROWS_AS(add_work(history.document(), ids, chart_image, bad, 0.92),
                        std::invalid_argument);
        CHECK_THROWS_AS(add_work(history.document(), ids, chart_image, 0.73, bad),
                        std::invalid_argument);
        CHECK(history.undo_count() == 1);
        CHECK(doc::to_project_json(history.document()) == before);
    }
}

// From the code review: what add_work makes must be a document the project file can hold, so a
// title or an image name the loader would refuse is refused here, before anything is committed.
TEST_CASE("add_work: a title longer than doc::max_string_bytes, or an image that is not an asset "
          "name, throws std::invalid_argument",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    commit_new_room(history, ids, 6.0, 4.0, 3.5);

    SECTION("the longest title is taken, one byte more is refused") {
        CHECK_NOTHROW(add_work(history.document(), ids, chart_image, 0.73, 0.92,
                               std::string(doc::max_string_bytes, 'x')));
        CHECK_THROWS_AS(add_work(history.document(), ids, chart_image, 0.73, 0.92,
                                 std::string(doc::max_string_bytes + 1, 'x')),
                        std::invalid_argument);
    }
    SECTION("asset names") {
        const std::string hash = chart_image.sha256;
        const doc::AssetRef bad = GENERATE_COPY(
            doc::AssetRef{"", "png"}, doc::AssetRef{hash.substr(1), "png"},
            doc::AssetRef{hash + "0", "png"}, doc::AssetRef{"G" + hash.substr(1), "png"},
            doc::AssetRef{"A" + hash.substr(1), "png"}, doc::AssetRef{hash, ""},
            doc::AssetRef{hash, "PNG"}, doc::AssetRef{hash, "p.ng"}, doc::AssetRef{hash, "../x"},
            doc::AssetRef{hash, "abcdefghi"});
        CAPTURE(bad.sha256, bad.extension);
        CHECK_THROWS_AS(add_work(history.document(), ids, bad, 0.73, 0.92), std::invalid_argument);
    }
}

// ==== Lighting: the CCI's numbers =============================================================

TEST_CASE("cci_track_distance_m: (ceiling - 1.6 m) x tan 30 degrees, 1.097 m for a 3.5 m ceiling",
          "[editor][commands][accuracy]") {
    CHECK(cci_aim_from_vertical_rad == pi / 6.0);
    CHECK(cci_reference_height_m == 1.6);

    // The issue's: 1.9 x 0.57735 = 1.09697 m, 1.097 m within 1 mm.
    CHECK_THAT(cci_track_distance_m(3.5), WithinAbs(1.097, issue_m));
    // The formula itself, to rounding: a subtraction, a tangent and a product, a few ulps.
    CHECK_THAT(cci_track_distance_m(3.5), WithinAbs(1.9 * std::tan(pi / 6.0), 1e-12));
    // 2.6 m: 1.0 x tan 30 = 1 / sqrt(3). 4.6 m: 3 x tan 30 = sqrt(3).
    CHECK_THAT(cci_track_distance_m(2.6), WithinAbs(1.0 / std::sqrt(3.0), 1e-12));
    CHECK_THAT(cci_track_distance_m(4.6), WithinAbs(std::sqrt(3.0), 1e-12));
    // Just above the reference height, just out from the wall.
    CHECK(cci_track_distance_m(1.6001) > 0.0);
    CHECK_THAT(cci_track_distance_m(1.6001), WithinAbs(0.0001 * std::tan(pi / 6.0), 1e-12));
}

TEST_CASE("cci_track_distance_m: throws std::invalid_argument for a ceiling at or below 1.6 m, "
          "or not finite",
          "[editor][commands]") {
    const double bad = GENERATE(1.6, 1.5999, 1.0, 0.0, -3.5, not_a_number, inf, -inf);
    CAPTURE(bad);
    CHECK_THROWS_AS(cci_track_distance_m(bad), std::invalid_argument);
}

TEST_CASE("generic_spot: Generic spot 800 lm, 800 lm, beam 15 and field 25 degrees as full "
          "angles in radians, 3000 K, as in the skeleton scene",
          "[editor][commands][accuracy]") {
    CHECK(generic_spot_id == "generic-spot-800");
    const doc::FixtureType spot = generic_spot();
    CHECK(spot.name == "Generic spot 800 lm");
    CHECK(spot.flux_lm == 800.0);
    CHECK_THAT(spot.beam_angle_rad, WithinAbs(15.0 * deg, exact_rad));
    CHECK_THAT(spot.field_angle_rad, WithinAbs(25.0 * deg, exact_rad));
    CHECK(spot.colour_temperature_k == 3000.0);

    const doc::Document skeleton = load_skeleton();
    const doc::FixtureType* scene = skeleton.library().find_fixture_type("generic-spot-800");
    REQUIRE(scene != nullptr);
    CHECK(spot.name == scene->name);
    CHECK(spot.flux_lm == scene->flux_lm);
    CHECK_THAT(spot.beam_angle_rad, WithinAbs(scene->beam_angle_rad, exact_rad));
    CHECK_THAT(spot.field_angle_rad, WithinAbs(scene->field_angle_rad, exact_rad));
    CHECK(spot.colour_temperature_k == scene->colour_temperature_k);
}

// ==== add_spot_for ============================================================================

TEST_CASE("add_spot_for: a luminaire, Spot 1, of generic-spot-800 at full output, aimed at the "
          "exhibit, on a track, Track A, in one transaction, Add spot",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);

    AddedSpot made = add_spot_for(history.document(), ids, gallery.work.exhibit);
    CHECK(made.transaction.name() == "Add spot");
    CHECK(&made.transaction.document() == &history.document());
    // New ids for the track, then the spot.
    CHECK(gallery.work.exhibit < made.track);
    CHECK(made.track < made.spot);
    const doc::ChangeList changes = made.transaction.changes();
    REQUIRE(changes.entities.size() == 2);
    CHECK(changes.entities[0] == doc::EntityChange{made.track, doc::ChangeKind::added});
    CHECK(changes.entities[1] == doc::EntityChange{made.spot, doc::ChangeKind::added});
    CHECK(changes.works.empty());
    CHECK_FALSE(changes.settings);
    CHECK(history.document().entities().size() == 6);

    const doc::Uuid spot_id = made.spot;
    const doc::Uuid track_id = made.track;
    history.commit(std::move(made.transaction));
    CHECK(history.undo_count() == 3);

    const doc::Entity& spot = entity_of(history.document(), spot_id);
    CHECK(spot.name == "Spot 1");
    REQUIRE(spot.luminaire.has_value());
    CHECK(spot.luminaire->fixture_type == generic_spot_id);
    CHECK(spot.luminaire->dimming == 1.0);
    CHECK(spot.luminaire->aim == doc::Aim{doc::AimAtEntity{gallery.work.exhibit}});
    CHECK(on_track_of(spot).track == track_id);

    const doc::Entity& track = entity_of(history.document(), track_id);
    CHECK(track.name == "Track A");
    CHECK(track.track.has_value());
    CHECK_FALSE(track.luminaire.has_value());
}

// From the code review: a count of luminaires repeated a name once a spot was deleted.
TEST_CASE("add_spot_for: spots take the lowest unused number, so no two share a name",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    const SpotIds first = commit_add_spot(history, ids, gallery.work.exhibit);
    const SpotIds second = commit_add_spot(history, ids, gallery.work.exhibit);
    CHECK(entity_of(history.document(), first.spot).name == "Spot 1");
    CHECK(entity_of(history.document(), second.spot).name == "Spot 2");

    // Spot 1 deleted: its number is used again, then the next free one is 3.
    doc::Transaction remove(history.document(), "Delete Spot 1");
    remove.remove_entity(first.spot);
    history.commit(std::move(remove));
    const SpotIds again = commit_add_spot(history, ids, gallery.work.exhibit);
    CHECK(entity_of(history.document(), again.spot).name == "Spot 1");

    // Any entity's name counts, a renamed spot's or a viewpoint's; names that are not
    // "Spot <n>" do not.
    doc::Entity viewpoint;
    viewpoint.name = "Spot 3";
    viewpoint.viewpoint = doc::Viewpoint{};
    doc::Transaction rename(history.document(), "Rename");
    rename.rename_entity(second.spot, "Window wash");
    rename.add_entity(ids.next(), std::move(viewpoint));
    history.commit(std::move(rename));
    const SpotIds after = commit_add_spot(history, ids, gallery.work.exhibit);
    CHECK(entity_of(history.document(), after.spot).name == "Spot 2");
    const SpotIds last = commit_add_spot(history, ids, gallery.work.exhibit);
    CHECK(entity_of(history.document(), last.spot).name == "Spot 4");
}

TEST_CASE("add_spot_for: the library gains generic-spot-800, unless it holds a fixture type of "
          "that id, which the spot uses as it is",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);

    SECTION("an empty library gains generic_spot()") {
        AddedSpot made = add_spot_for(history.document(), ids, gallery.work.exhibit);
        const doc::ChangeList changes = made.transaction.changes();
        REQUIRE(changes.fixture_types.size() == 1);
        CHECK(changes.fixture_types[0] ==
              doc::FixtureTypeChange{std::string(generic_spot_id), doc::ChangeKind::added});
        history.commit(std::move(made.transaction));
        const doc::FixtureType* type =
            history.document().library().find_fixture_type(std::string(generic_spot_id));
        REQUIRE(type != nullptr);
        CHECK(*type == generic_spot());
        CHECK(history.document().library().fixture_types.size() == 1);
    }
    SECTION("a fixture type of that id already there is kept") {
        doc::FixtureType ours = generic_spot();
        ours.name = "Our spot";
        ours.flux_lm = 1200.0;
        doc::Transaction add_type(history.document(), "Add fixture type");
        add_type.set_fixture_type(std::string(generic_spot_id), ours);
        history.commit(std::move(add_type));

        AddedSpot made = add_spot_for(history.document(), ids, gallery.work.exhibit);
        CHECK(made.transaction.changes().fixture_types.empty());
        const doc::Uuid spot_id = made.spot;
        history.commit(std::move(made.transaction));
        const doc::FixtureType* type =
            history.document().library().find_fixture_type(std::string(generic_spot_id));
        REQUIRE(type != nullptr);
        CHECK(*type == ours);
        CHECK(entity_of(history.document(), spot_id).luminaire->fixture_type == generic_spot_id);
    }
}

TEST_CASE("add_spot_for: the track is on the ceiling, 3.5 m, 1.097 m from wall A's face within "
          "1 mm, parallel to the face and as long as it",
          "[editor][commands][accuracy]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);

    const doc::Entity& entity = entity_of(history.document(), spot.track);
    REQUIRE(entity.track.has_value());
    const doc::Track& track = *entity.track;
    CHECK(track.height_m == 3.5);
    REQUIRE(track.points_m.size() >= 2);

    const geom::FaceFrame face = face_of(history.document(), gallery.work.exhibit);
    for (const doc::Vec2& point : track.points_m) {
        const dvec3 p(point.x, point.y, 0.0);
        const double out_m = glm::dot(p - face.origin, face.normal);
        // The issue's 1.097 m within 1 mm; and, being parallel, every point the same distance out,
        // to rounding.
        CHECK_THAT(out_m, WithinAbs(1.097, issue_m));
        CHECK_THAT(out_m, WithinAbs(cci_distance_m(3.5), exact_m));
    }
    // Its first point is opposite the face's left end and its last opposite the right end, so a
    // distance along it is a u along the face.
    const dvec3 first(track.points_m.front().x, track.points_m.front().y, 0.0);
    const dvec3 last(track.points_m.back().x, track.points_m.back().y, 0.0);
    CHECK_THAT(glm::dot(first - face.origin, face.u), WithinAbs(0.0, exact_m));
    CHECK_THAT(glm::dot(last - face.origin, face.u), WithinAbs(face.length_m, exact_m));
    // For wall A along y = 0: from (6, 1.097) to (0, 1.097).
    check_near(first, {6.0, cci_distance_m(3.5), 0.0}, exact_m);
    check_near(last, {0.0, cci_distance_m(3.5), 0.0}, exact_m);
}

TEST_CASE("add_spot_for: the spot's aim at the centre of the work's face is 30 degrees from "
          "vertical within 0.1 degree, 0.465 m along the track from the point opposite the work",
          "[editor][commands][accuracy]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);
    const doc::Document& document = history.document();

    // The centre of the chart's face: u = 3 on face A of wall A (x = 3), on the 1.5 m centre line,
    // the 0.04 m stand-off out from the face at y = 0 (the work has no depth).
    const dvec3 centre = work_face_centre(document, gallery.work.exhibit);
    check_near(centre, {3.0, 0.04, 1.5}, exact_m);

    // The aim, from the document's numbers.
    const dvec3 position = spot_position(document, spot.spot);
    CHECK_THAT(from_vertical_rad(position, centre), WithinAbs(30.0 * deg, issue_aim_rad));

    // The point of the track opposite the work is at the work's u, 3.0 m along it. The spot is
    // 0.465 m from there within 1 mm: README's 0.4648 m, or exactly
    // sqrt((2.0 x tan 30)^2 - (1.09697 - 0.04)^2) = 0.46493 m.
    const doc::OnWall& work_at = on_wall_of(entity_of(document, gallery.work.exhibit));
    const doc::OnTrack& spot_at = on_track_of(entity_of(document, spot.spot));
    CHECK_THAT(std::abs(spot_at.distance_m - work_at.distance_m), WithinAbs(0.465, issue_m));
    CHECK_THAT(std::abs(spot_at.distance_m - work_at.distance_m),
               WithinAbs(cci_slide_m(2.0, cci_distance_m(3.5) - 0.04), issue_m));
    // Slid towards the track's first point, so that, seen facing the work, the light comes from
    // the upper left: towards x = 6, to x = 3.465 as the skeleton's spot.
    CHECK(spot_at.distance_m < work_at.distance_m);
    check_near(position, {3.465, 1.097, 3.5}, issue_m);
}

TEST_CASE("add_spot_for: world::compile puts the light where the document says, aimed at the "
          "centre of the work's face 30 degrees from vertical",
          "[editor][commands][accuracy]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);

    const world::World compiled = world::compile(history.document());
    REQUIRE(compiled.lights.size() == 1);
    const world::Light& light = light_of(compiled, spot.spot);
    const world::Exhibit& exhibit = exhibit_of(compiled, gallery.work.exhibit);

    // The issue's numbers, through the world the engine measures.
    check_near(exhibit.face_centre_m, {3.0, 0.04, 1.5}, exact_m);
    check_near(light.luminaire.position_m(), {3.465, 1.097, 3.5}, issue_m);
    check_near(light.luminaire.position_m(), spot_position(history.document(), spot.spot), exact_m);
    const dvec3 axis = light.luminaire.axis();
    CHECK_THAT(std::atan2(std::hypot(axis.x, axis.y), -axis.z),
               WithinAbs(30.0 * deg, issue_aim_rad));
    check_near(axis, glm::normalize(exhibit.face_centre_m - light.luminaire.position_m()),
               exact_unit);

    // The generic spot's light: 800 lm at full output, beam 15 and field 25 degrees.
    REQUIRE(light.spot != nullptr);
    CHECK_THAT(light.spot->total_lumens(), WithinRel(800.0, 1e-12));
    CHECK_THAT(light.spot->beam_angle_rad(), WithinAbs(15.0 * deg, exact_rad));
    CHECK_THAT(light.spot->field_angle_rad(), WithinAbs(25.0 * deg, exact_rad));
    CHECK(light.colour_temperature_k == 3000.0);
}

TEST_CASE("add_spot_for: on the skeleton scene's test chart, 0.465 m along the track from the "
          "point opposite within 1 mm, and 30 degrees from vertical within 0.1 degree",
          "[editor][commands][accuracy]") {
    doc::History history{load_skeleton()};
    doc::IdGenerator ids = fixed_ids();
    const doc::Uuid chart = entity_named(history.document(), "Test chart");
    const doc::Uuid track_a = entity_named(history.document(), "Track A");
    const doc::Uuid spot_1 = entity_named(history.document(), "Spot 1");

    AddedSpot made = add_spot_for(history.document(), ids, chart);
    // The library holds generic-spot-800 already.
    CHECK(made.transaction.changes().fixture_types.empty());
    // The scene's Track A runs from x = 1 to x = 5, not the face's length (x = 6 to 0), so it is
    // not within 1 mm of the CCI's track and a new one is made.
    CHECK(made.track != track_a);
    const SpotIds spot{made.spot, made.track};
    history.commit(std::move(made.transaction));
    const doc::Document& document = history.document();

    // One luminaire in the scene already: this is Spot 2.
    CHECK(entity_of(document, spot.spot).name == "Spot 2");

    const dvec3 centre = work_face_centre(document, chart);
    check_near(centre, {3.0, 0.04, 1.5}, exact_m);
    const dvec3 position = spot_position(document, spot.spot);
    CHECK_THAT(from_vertical_rad(position, centre), WithinAbs(30.0 * deg, issue_aim_rad));

    const doc::OnWall& chart_at = on_wall_of(entity_of(document, chart));
    const doc::OnTrack& spot_at = on_track_of(entity_of(document, spot.spot));
    CHECK_THAT(std::abs(spot_at.distance_m - chart_at.distance_m), WithinAbs(0.465, issue_m));
    CHECK(spot_at.distance_m < chart_at.distance_m);

    // Where the scene's own Spot 1 is, within 1 mm (README: x = 3.465 m).
    const world::World compiled = world::compile(document);
    const world::Light& added = light_of(compiled, spot.spot);
    const world::Light& scene_spot = light_of(compiled, spot_1);
    check_near(added.luminaire.position_m(), scene_spot.luminaire.position_m(), issue_m);
    check_near(added.luminaire.position_m(), {3.465, 1.097, 3.5}, issue_m);
    const dvec3 axis = added.luminaire.axis();
    CHECK_THAT(std::atan2(std::hypot(axis.x, axis.y), -axis.z),
               WithinAbs(30.0 * deg, issue_aim_rad));
}

TEST_CASE("add_spot_for: a second spot for the same exhibit reuses the track",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    const SpotIds first = commit_add_spot(history, ids, gallery.work.exhibit);

    AddedSpot second = add_spot_for(history.document(), ids, gallery.work.exhibit);
    CHECK(second.track == first.track);
    CHECK(first.spot < second.spot);
    const doc::ChangeList changes = second.transaction.changes();
    REQUIRE(changes.entities.size() == 1);
    CHECK(changes.entities[0] == doc::EntityChange{second.spot, doc::ChangeKind::added});
    CHECK(changes.fixture_types.empty());

    const doc::Uuid spot_id = second.spot;
    history.commit(std::move(second.transaction));
    const doc::Entity& spot = entity_of(history.document(), spot_id);
    CHECK(spot.name == "Spot 2");
    CHECK(on_track_of(spot).track == first.track);
    // One track in the document still.
    const auto tracks =
        std::count_if(history.document().entities().begin(), history.document().entities().end(),
                      [](const auto& entry) { return entry.second.track.has_value(); });
    CHECK(tracks == 1);
}

TEST_CASE("add_spot_for: a track in the document within 1 mm of the CCI's is used, one 2 mm off "
          "is not",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);

    // The CCI's track for face A of wall A: from (6, d) to (0, d) at 3.5 m, d = 1.9 x tan 30.
    const double d = cci_distance_m(3.5);
    const auto [dx, dy, dz, reused] =
        GENERATE(std::tuple{0.0005, -0.0005, 0.0005, true}, std::tuple{0.0, 0.0, 0.0, true},
                 std::tuple{0.002, 0.0, 0.0, false}, std::tuple{0.0, 0.002, 0.0, false},
                 std::tuple{0.0, 0.0, 0.002, false});
    CAPTURE(dx, dy, dz, reused);
    const doc::Uuid existing = ids.next();
    doc::Transaction add_track(history.document(), "Add track");
    doc::Entity track;
    track.name = "Our track";
    track.track =
        doc::Track{.points_m = {{6.0 + dx, d + dy}, {0.0 - dx, d - dy}}, .height_m = 3.5 + dz};
    add_track.add_entity(existing, track);
    history.commit(std::move(add_track));

    AddedSpot made = add_spot_for(history.document(), ids, gallery.work.exhibit);
    CHECK((made.track == existing) == reused);
    const auto added = made.transaction.changes().entities.size();
    CHECK(added == (reused ? 1u : 2u));
}

TEST_CASE("add_spot_for: the work's depth and stand-off move the aim point out from the face, at "
          "least world::min_face_offset_m",
          "[editor][commands][accuracy]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    const auto [stand_off_m, depth_m, offset_m] =
        GENERATE(std::tuple{0.04, 0.03, 0.07}, std::tuple{0.0, 0.0, world::min_face_offset_m},
                 std::tuple{0.1, 0.0, 0.1});
    CAPTURE(stand_off_m, depth_m, offset_m);

    doc::Work work = *history.document().find_work(gallery.work.work);
    work.form.depth_m = depth_m > 0.0 ? std::optional<double>(depth_m) : std::nullopt;
    doc::OnWall at = on_wall_of(entity_of(history.document(), gallery.work.exhibit));
    at.stand_off_m = stand_off_m;
    doc::Transaction edit(history.document(), "Edit");
    edit.set_work(gallery.work.work, work);
    edit.set(gallery.work.exhibit, doc::Placement{at});
    history.commit(std::move(edit));

    const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);
    const doc::Document& document = history.document();
    const dvec3 centre = work_face_centre(document, gallery.work.exhibit);
    check_near(centre, {3.0, offset_m, 1.5}, exact_m);
    CHECK_THAT(from_vertical_rad(spot_position(document, spot.spot), centre),
               WithinAbs(30.0 * deg, issue_aim_rad));
    const double slide_m = std::abs(on_track_of(entity_of(document, spot.spot)).distance_m - 3.0);
    CHECK_THAT(slide_m, WithinAbs(cci_slide_m(2.0, cci_distance_m(3.5) - offset_m), issue_m));

    // add_spot_for repeats world::compile's rule for where the work's face is; the World must
    // agree, so that the light aims at the point the slide was worked out for.
    const world::World compiled = world::compile(document);
    check_near(exhibit_of(compiled, gallery.work.exhibit).face_centre_m, centre, exact_m);
    const dvec3 axis = light_of(compiled, spot.spot).luminaire.axis();
    CHECK_THAT(std::atan2(std::hypot(axis.x, axis.y), -axis.z),
               WithinAbs(30.0 * deg, issue_aim_rad));
}

TEST_CASE("add_spot_for: with the project's centre line at 1.45 m the aim is still 30 degrees",
          "[editor][commands][accuracy]") {
    doc::DocumentData data;
    data.settings.centre_line_m = 1.45;
    doc::History history{doc::Document(data)};
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);

    const dvec3 centre = work_face_centre(history.document(), gallery.work.exhibit);
    check_near(centre, {3.0, 0.04, 1.45}, exact_m);
    CHECK_THAT(from_vertical_rad(spot_position(history.document(), spot.spot), centre),
               WithinAbs(30.0 * deg, issue_aim_rad));
    // A drop of 2.05 m: sqrt((2.05 x tan 30)^2 - 1.05697^2) = 0.5326 m.
    const double slide_m =
        std::abs(on_track_of(entity_of(history.document(), spot.spot)).distance_m - 3.0);
    CHECK_THAT(slide_m, WithinAbs(cci_slide_m(2.05, cci_distance_m(3.5) - 0.04), issue_m));
}

TEST_CASE("add_spot_for: with no room for a slide towards the track's first point, towards its "
          "last; with no room either way, to its first point",
          "[editor][commands][accuracy]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();

    SECTION("a work 0.2 m from the face's left end: slid 0.465 m towards the last point") {
        const Gallery gallery = commit_gallery(history, ids);
        doc::OnWall at = on_wall_of(entity_of(history.document(), gallery.work.exhibit));
        at.distance_m = 0.2;
        commit_set(history, gallery.work.exhibit, doc::Placement{at});

        const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);
        const double distance_m = on_track_of(entity_of(history.document(), spot.spot)).distance_m;
        CHECK_THAT(distance_m, WithinAbs(0.2 + 0.465, issue_m));
        CHECK_THAT(from_vertical_rad(spot_position(history.document(), spot.spot),
                                     work_face_centre(history.document(), gallery.work.exhibit)),
                   WithinAbs(30.0 * deg, issue_aim_rad));
    }
    SECTION("a face 0.8 m long, the work in its middle: at the first point") {
        // 0.4 - 0.465 < 0 and 0.4 + 0.465 > 0.8: the slide fits neither way.
        commit_new_room(history, ids, 0.8, 4.0, 3.5);
        const WorkIds work = commit_add_work(history, ids, 0.3, 0.4);
        CHECK_THAT(on_wall_of(entity_of(history.document(), work.exhibit)).distance_m,
                   WithinAbs(0.4, exact_m));

        const SpotIds spot = commit_add_spot(history, ids, work.exhibit);
        CHECK_THAT(on_track_of(entity_of(history.document(), spot.spot)).distance_m,
                   WithinAbs(0.0, exact_m));
    }
}

TEST_CASE("add_spot_for: a work hung so high that the aim from the point opposite is already "
          "beyond 30 degrees keeps the spot opposite",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    // Centred at 3.0 m: from the point opposite, 1.057 m out and 0.5 m down, the aim is
    // atan(1.057 / 0.5) = 64.7 degrees from vertical.
    doc::OnWall at = on_wall_of(entity_of(history.document(), gallery.work.exhibit));
    at.centre_height_m = 3.0;
    commit_set(history, gallery.work.exhibit, doc::Placement{at});

    const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);
    CHECK_THAT(on_track_of(entity_of(history.document(), spot.spot)).distance_m,
               WithinAbs(3.0, exact_m));
}

TEST_CASE("add_spot_for: the ceiling is that of the first room, in order of id, whose outline "
          "holds the work's face centre; with none, the top of the wall",
          "[editor][commands][accuracy]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();

    SECTION("two rooms hold it: the first's ceiling, 3.5 m") {
        commit_new_room(history, ids, 6.0, 4.0, 3.5);
        commit_new_room(history, ids, 6.0, 4.0, 4.0);
        const WorkIds work = commit_add_work(history, ids, 0.73, 0.92);
        const SpotIds spot = commit_add_spot(history, ids, work.exhibit);
        CHECK(entity_of(history.document(), spot.track).track->height_m == 3.5);
    }
    SECTION("only the second holds it: its ceiling, 4.0 m, and the track (4.0 - 1.6) x tan 30 "
            "= 1.386 m out") {
        const RoomIds first = commit_new_room(history, ids, 6.0, 4.0, 3.5);
        commit_new_room(history, ids, 6.0, 4.0, 4.0);
        doc::Room moved = *entity_of(history.document(), first.room).room;
        for (doc::Vec2& point : moved.outline_m) {
            point.x += 100.0;
        }
        commit_set(history, first.room, moved);
        const WorkIds work = commit_add_work(history, ids, 0.73, 0.92);
        const SpotIds spot = commit_add_spot(history, ids, work.exhibit);

        const doc::Track& track = *entity_of(history.document(), spot.track).track;
        CHECK(track.height_m == 4.0);
        const geom::FaceFrame face = face_of(history.document(), work.exhibit);
        const dvec3 first_point(track.points_m.front().x, track.points_m.front().y, 0.0);
        CHECK_THAT(glm::dot(first_point - face.origin, face.normal),
                   WithinAbs(cci_distance_m(4.0), issue_m));
        CHECK_THAT(from_vertical_rad(spot_position(history.document(), spot.spot),
                                     work_face_centre(history.document(), work.exhibit)),
                   WithinAbs(30.0 * deg, issue_aim_rad));
    }
    // From the code review: rooms one above the other share an outline in plan, so the room is
    // found by the work's height too. The upper room, z 3.5 to 7 m, is first by id; the work's
    // centre, at 1.5 m, is in the lower one.
    SECTION("rooms one above the other: the one the work is in, by height, 3.5 m") {
        const RoomIds upper = commit_new_room(history, ids, 6.0, 4.0, 7.0);
        doc::Room raised = *entity_of(history.document(), upper.room).room;
        raised.floor_height_m = 3.5;
        commit_set(history, upper.room, raised);
        commit_new_room(history, ids, 6.0, 4.0, 3.5);
        const WorkIds work = commit_add_work(history, ids, 0.73, 0.92);
        const SpotIds spot = commit_add_spot(history, ids, work.exhibit);

        const doc::Track& track = *entity_of(history.document(), spot.track).track;
        CHECK(track.height_m == 3.5);
        const geom::FaceFrame face = face_of(history.document(), work.exhibit);
        const dvec3 first_point(track.points_m.front().x, track.points_m.front().y, 0.0);
        CHECK_THAT(glm::dot(first_point - face.origin, face.normal),
                   WithinAbs(cci_distance_m(3.5), issue_m));
    }
    SECTION("no room: the floor at 0 and the ceiling at the top of the wall, 3.0 m") {
        const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
        doc::Transaction edit(history.document(), "Edit");
        edit.remove_entity(room.room);
        doc::Wall a = *entity_of(history.document(), room.walls[0]).wall;
        a.height_m = 3.0;
        edit.set(room.walls[0], a);
        history.commit(std::move(edit));
        const WorkIds work = commit_add_work(history, ids, 0.73, 0.92);
        const SpotIds spot = commit_add_spot(history, ids, work.exhibit);

        const doc::Track& track = *entity_of(history.document(), spot.track).track;
        CHECK(track.height_m == 3.0);
        const geom::FaceFrame face = face_of(history.document(), work.exhibit);
        const dvec3 first_point(track.points_m.front().x, track.points_m.front().y, 0.0);
        // (3.0 - 1.6) x tan 30 = 0.808 m.
        CHECK_THAT(glm::dot(first_point - face.origin, face.normal),
                   WithinAbs(cci_distance_m(3.0), issue_m));
        CHECK_THAT(from_vertical_rad(spot_position(history.document(), spot.spot),
                                     work_face_centre(history.document(), work.exhibit)),
                   WithinAbs(30.0 * deg, issue_aim_rad));
    }
}

TEST_CASE("add_spot_for: throws std::invalid_argument unless the entity is an exhibit hung on a "
          "wall it can be lit on",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();

    SECTION("no such entity, or one that is not an exhibit") {
        const Gallery gallery = commit_gallery(history, ids);
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, ids.next()), std::invalid_argument);
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, doc::Uuid{}), std::invalid_argument);
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, gallery.room.room),
                        std::invalid_argument);
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, gallery.room.walls[0]),
                        std::invalid_argument);
        // Nor a work's id, which names no entity.
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, gallery.work.work),
                        std::invalid_argument);
    }
    SECTION("an exhibit with no placement, or one that is not on a wall") {
        const Gallery gallery = commit_gallery(history, ids);
        SECTION("none") {
            doc::Transaction edit(history.document(), "Edit");
            edit.remove(gallery.work.exhibit, doc::ComponentKind::placement);
            history.commit(std::move(edit));
        }
        SECTION("on the floor") {
            commit_set(history, gallery.work.exhibit,
                       doc::Placement{doc::OnFloor{.room = gallery.room.room,
                                                   .position_m = {3.0, 2.0},
                                                   .rotation_rad = 0.0}});
        }
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, gallery.work.exhibit),
                        std::invalid_argument);
    }
    SECTION("a ceiling no more than 1.6 m above the floor") {
        const double ceiling_m = GENERATE(1.6, 1.2);
        CAPTURE(ceiling_m);
        commit_new_room(history, ids, 6.0, 4.0, ceiling_m);
        const WorkIds work = commit_add_work(history, ids, 0.3, 0.2);
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, work.exhibit), std::invalid_argument);
    }
    SECTION("a work's face further out from the wall than the track") {
        // The track is 1.097 m out; a stand-off of 1.2 m puts the work's face beyond it.
        const Gallery gallery = commit_gallery(history, ids);
        doc::OnWall at = on_wall_of(entity_of(history.document(), gallery.work.exhibit));
        at.stand_off_m = 1.2;
        commit_set(history, gallery.work.exhibit, doc::Placement{at});
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, gallery.work.exhibit),
                        std::invalid_argument);
    }
    SECTION("a work's face centre at or above the ceiling") {
        const double centre_m = GENERATE(3.5, 3.6);
        CAPTURE(centre_m);
        const Gallery gallery = commit_gallery(history, ids);
        doc::OnWall at = on_wall_of(entity_of(history.document(), gallery.work.exhibit));
        at.centre_height_m = centre_m;
        commit_set(history, gallery.work.exhibit, doc::Placement{at});
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, gallery.work.exhibit),
                        std::invalid_argument);
    }
}

TEST_CASE("add_spot_for: a document that dangles or holds a degenerate wall is refused cleanly",
          "[editor][commands]") {
    // Documents made from data directly are not checked (document.hpp), so the command must not
    // trust what it reads.
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    doc::DocumentData data = history.document().data();

    SECTION("the exhibit's work is missing") {
        data.works.clear();
    }
    SECTION("the placement's wall is missing") {
        data.entities.erase(gallery.room.walls[0]);
    }
    SECTION("the placement names an entity that is not a wall") {
        std::get<doc::OnWall>(*data.entities.at(gallery.work.exhibit).placement).wall =
            gallery.room.room;
    }
    SECTION("the wall's ends are equal, so it has no face") {
        doc::Wall& wall = *data.entities.at(gallery.room.walls[0]).wall;
        wall.end_m = wall.start_m;
    }
    SECTION("the wall's ends are not finite") {
        data.entities.at(gallery.room.walls[0]).wall->end_m.x = not_a_number;
    }
    const doc::Document broken(std::move(data));
    CHECK_THROWS_AS(add_spot_for(broken, ids, gallery.work.exhibit), std::invalid_argument);
}

// From the security review: what add_spot_for makes must be a document the project file can
// hold, which takes only finite numbers and names of at most doc::max_string_bytes.
TEST_CASE("add_spot_for: a track beyond the largest double is refused, and a wall label too long "
          "to name the track after gives the name Track",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    doc::Wall a = *entity_of(history.document(), gallery.room.walls[0]).wall;

    SECTION("a wall at the edge of a double's range, its face towards +x and 1e306 m high with no "
            "room round the work: the track, about 5.8e305 m out, would be beyond 1.798e308") {
        a.start_m = {1.7976e308, 4.0};
        a.end_m = {1.7976e308, 0.0};
        a.height_m = 1e306;
        commit_set(history, gallery.room.walls[0], a);
        CHECK_THROWS_AS(add_spot_for(history.document(), ids, gallery.work.exhibit),
                        std::invalid_argument);
    }
    SECTION("labels whose track name is just within the limit, and one byte over it") {
        const std::size_t fits = doc::max_string_bytes - std::string_view("Track ").size();
        const std::size_t length = GENERATE_COPY(fits, fits + 1);
        CAPTURE(length);
        a.label = std::string(length, 'A');
        commit_set(history, gallery.room.walls[0], a);
        const SpotIds spot = commit_add_spot(history, ids, gallery.work.exhibit);
        const std::string& name = entity_of(history.document(), spot.track).name;
        CHECK(name == (length == fits ? "Track " + a.label : std::string("Track")));
        CHECK(name.size() <= doc::max_string_bytes);
    }
}

// ==== Undo ====================================================================================

TEST_CASE("commands: each undoes in one step, the document after undo serialising identical to "
          "before, and redoes",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const std::string empty = doc::to_project_json(history.document());

    NewRoom room = new_room(history.document(), ids, 6.0, 4.0, 3.5);
    check_undoes_in_one_step(history, std::move(room.transaction));

    AddedWork work = add_work(history.document(), ids, chart_image, 0.73, 0.92, "Test chart");
    const doc::Uuid exhibit = work.exhibit;
    check_undoes_in_one_step(history, std::move(work.transaction));

    AddedSpot spot = add_spot_for(history.document(), ids, exhibit);
    check_undoes_in_one_step(history, std::move(spot.transaction));

    // A second spot, which reuses the track.
    AddedSpot second = add_spot_for(history.document(), ids, exhibit);
    check_undoes_in_one_step(history, std::move(second.transaction));

    // Three commands and four steps; undone, the empty project again.
    CHECK(history.undo_count() == 4);
    for (int i = 0; i < 4; ++i) {
        history.undo();
    }
    CHECK_FALSE(history.can_undo());
    CHECK(doc::to_project_json(history.document()) == empty);
}

TEST_CASE("commands: add_spot_for on the skeleton scene undoes in one step to the scene as loaded",
          "[editor][commands]") {
    doc::History history{load_skeleton()};
    doc::IdGenerator ids = fixed_ids();
    const doc::Uuid chart = entity_named(history.document(), "Test chart");

    AddedSpot spot = add_spot_for(history.document(), ids, chart);
    check_undoes_in_one_step(history, std::move(spot.transaction));
    CHECK(history.undo_count() == 1);
}

TEST_CASE("commands: what they make reads back from its project.json", "[editor][commands]") {
    // The loader is strict (project_io.hpp): every value in range, every reference resolved. A
    // document the commands make must load, or the curator could not save it.
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const Gallery gallery = commit_gallery(history, ids);
    commit_add_spot(history, ids, gallery.work.exhibit);
    commit_add_spot(history, ids, gallery.work.exhibit);
    commit_new_room(history, ids, 3.0, 2.0, 2.8);

    const std::string text = doc::to_project_json(history.document());
    const doc::Document read = doc::parse_project_json(text);
    CHECK(read == history.document());
    CHECK(doc::to_project_json(read) == text);
}

// ==== add_work_in_row (#88, M1-27) ============================================================
//
// The issue's criterion: "editor::add_work_in_row (hanging.hpp), with add_work's arguments and
// result, hangs the new work on face A of wall A on the centre line and moves the works already
// there so that the row is centred on the face with equal 0.4 m gaps between the frames' outer
// edges, all in one transaction; when either end would be closer than 0.3 m to the face's ends it
// throws std::invalid_argument ('Wall A is full') and changes nothing. [editor] tests: three works
// of different widths do not overlap, have equal gaps and are centred; one too wide is refused;
// undo puts the others back."
//
// In new_room(6, 4, 3.5), face A of wall A is 6 m long and a distance along it, u, runs from its
// left end (x = 6) seen from the room; "left to right" is increasing u. A box-framed work takes
// its width - 2 x 0.005 m (the frame's rebate) + 2 x 0.06 m (its members) on the wall: 0.11 m
// more than the work (world::default_frame).

namespace {

// A work's width on the wall in a box frame with members member_width_m wide, from
// world::default_frame's numbers: the sight opening, the work less the rebate on each side, plus
// a member on each side (geom::outer_size).
double framed_width_m(double work_width_m,
                      double member_width_m = world::default_frame.member_width_m) {
    return work_width_m - 2.0 * world::default_frame.rebate_m + 2.0 * member_width_m;
}

doc::Work flat_work(double width_m, double height_m) {
    doc::Work work;
    work.form = doc::FlatForm{.width_m = width_m, .height_m = height_m, .depth_m = std::nullopt};
    work.image = chart_image;
    return work;
}

doc::Framing framing_of_style(std::string style) {
    doc::Framing framing;
    framing.style = std::move(style);
    return framing;
}

WorkIds commit_add_work_in_row(doc::History& history, doc::IdGenerator& ids, double width_m,
                               double height_m, std::string title = {}) {
    AddedWork made =
        add_work_in_row(history.document(), ids, chart_image, width_m, height_m, std::move(title));
    const WorkIds work{made.work, made.exhibit};
    history.commit(std::move(made.transaction));
    return work;
}

// One exhibit of the row on face A of wall A, with its width on the wall worked out here (not by
// hung_width_m, which has a test of its own).
struct RowWork {
    doc::Uuid exhibit;
    double distance_m = 0.0;
    double width_m = 0.0;
    double left_m() const { return distance_m - width_m / 2.0; }
    double right_m() const { return distance_m + width_m / 2.0; }
};

// Every exhibit on face A of `wall`, left to right (by distance, then id, as the header orders
// them).
std::vector<RowWork> row_on(const doc::Document& document, const doc::Uuid& wall) {
    std::vector<RowWork> row;
    for (const auto& [id, entity] : document.entities()) {
        if (!entity.exhibit.has_value() || !entity.placement.has_value()) {
            continue;
        }
        const auto* at = std::get_if<doc::OnWall>(&*entity.placement);
        if (at == nullptr || at->wall != wall || at->face != doc::WallFace::a) {
            continue;
        }
        const doc::Work* work = document.find_work(entity.exhibit->work);
        REQUIRE(work != nullptr);
        const double width_m =
            entity.framing.has_value()
                ? framed_width_m(work->form.width_m, entity.framing->width_m.value_or(
                                                         world::default_frame.member_width_m))
                : work->form.width_m;
        row.push_back({id, at->distance_m, width_m});
    }
    std::sort(row.begin(), row.end(), [](const RowWork& a, const RowWork& b) {
        return a.distance_m != b.distance_m ? a.distance_m < b.distance_m : a.exhibit < b.exhibit;
    });
    return row;
}

// The issue's rule for a row on a face face_length_m long: no overlap, row_gap_m between each
// work's outer edge and the next's, the same clearance at both ends, at least row_end_clearance_m.
void check_centred_row(const std::vector<RowWork>& row, double face_length_m) {
    REQUIRE_FALSE(row.empty());
    for (std::size_t i = 0; i + 1 < row.size(); ++i) {
        CAPTURE(i);
        CHECK(row[i].right_m() < row[i + 1].left_m());
        CHECK_THAT(row[i + 1].left_m() - row[i].right_m(), WithinAbs(row_gap_m, exact_m));
    }
    const double left_clearance_m = row.front().left_m();
    const double right_clearance_m = face_length_m - row.back().right_m();
    CAPTURE(left_clearance_m, right_clearance_m);
    CHECK_THAT(left_clearance_m, WithinAbs(right_clearance_m, exact_m));
    CHECK(left_clearance_m >= row_end_clearance_m - exact_m);
}

std::vector<doc::Uuid> exhibits_of(const std::vector<RowWork>& row) {
    std::vector<doc::Uuid> exhibits;
    for (const RowWork& work : row) {
        exhibits.push_back(work.exhibit);
    }
    return exhibits;
}

} // namespace

TEST_CASE("hung_width_m: a framed work's outer width, its width - 2 x 0.005 + 2 x 0.06 m, "
          "whatever the style",
          "[editor][commands]") {
    // world::default_frame's numbers, which the issue's widths assume.
    CHECK(world::default_frame.member_width_m == 0.06);
    CHECK(world::default_frame.rebate_m == 0.005);

    const auto [width_m, height_m] =
        GENERATE(std::pair{0.73, 0.92}, std::pair{0.5, 0.4}, std::pair{1.2, 0.8});
    CAPTURE(width_m, height_m);
    const doc::Work work = flat_work(width_m, height_m);
    const double expected_m = width_m - 2.0 * 0.005 + 2.0 * 0.06;

    // Sums of a few numbers near 1: exact but for rounding (exact_m above).
    CHECK_THAT(hung_width_m(work, framing_of_style("box")), WithinAbs(expected_m, exact_m));
    // Every style is a box frame until M2-08.
    CHECK_THAT(hung_width_m(work, framing_of_style("gilded-swept")),
               WithinAbs(expected_m, exact_m));
    // geom::outer_size of the same frame, as world::compile builds it.
    const geom::BoxFrame frame{.work_width_m = width_m,
                               .work_height_m = height_m,
                               .member_width_m = world::default_frame.member_width_m,
                               .depth_m = world::default_frame.depth_m,
                               .rebate_m = world::default_frame.rebate_m};
    CHECK_THAT(hung_width_m(work, framing_of_style("box")),
               WithinAbs(geom::outer_size(frame).x, exact_m));
}

TEST_CASE("hung_width_m: the framing's member width when it sets one", "[editor][commands]") {
    const doc::Work work = flat_work(0.73, 0.92);
    doc::Framing narrow = framing_of_style("box");
    narrow.width_m = 0.03;
    // 0.73 - 0.01 + 2 x 0.03 = 0.78.
    CHECK_THAT(hung_width_m(work, narrow), WithinAbs(0.78, exact_m));
    doc::Framing wide = framing_of_style("box");
    wide.width_m = 0.1;
    // 0.73 - 0.01 + 2 x 0.1 = 0.92.
    CHECK_THAT(hung_width_m(work, wide), WithinAbs(0.92, exact_m));
}

TEST_CASE("hung_width_m: an unframed work, or one whose frame cannot be built, takes its own "
          "width",
          "[editor][commands]") {
    CHECK(hung_width_m(flat_work(0.73, 0.92), std::nullopt) == 0.73);
    CHECK(hung_width_m(flat_work(1.2, 0.8), std::nullopt) == 1.2);
    // Narrower or lower than twice the rebate, 0.01 m: the sight opening is empty, so no frame
    // (geom::box_frame refuses it) and the work is shown unframed.
    CHECK(hung_width_m(flat_work(0.008, 0.5), framing_of_style("box")) == 0.008);
    CHECK(hung_width_m(flat_work(0.5, 0.008), framing_of_style("box")) == 0.5);
}

// hung_width_m repeats world::compile's recipe for a frame (world/src/compile.cpp, add_frame),
// which is private to world. Should a frame's style or numbers change there (M2-08), this test
// fails rather than the row's gaps going wrong unnoticed.
TEST_CASE("hung_width_m: the width of the frame world::compile builds round the work",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const auto [width_m, height_m] =
        GENERATE(std::pair{0.73, 0.92}, std::pair{0.5, 0.4}, std::pair{1.2, 0.8});
    // The framing's own member width, and a work with a depth of its own (a panel, a canvas on
    // stretchers), which compile builds into the frame's depth.
    const auto [member_m, depth_m] =
        GENERATE(std::pair{std::optional<double>{}, std::optional<double>{}},
                 std::pair{std::optional<double>{0.03}, std::optional<double>{}},
                 std::pair{std::optional<double>{}, std::optional<double>{0.05}});
    CAPTURE(width_m, height_m, member_m.value_or(-1.0), depth_m.value_or(-1.0));
    const WorkIds added = commit_add_work_in_row(history, ids, width_m, height_m, "Framed");
    {
        doc::Transaction change(history.document(), "Change the frame");
        doc::Framing framing = *entity_of(history.document(), added.exhibit).framing;
        framing.width_m = member_m;
        change.set(added.exhibit, std::move(framing));
        doc::Work work = *history.document().find_work(added.work);
        work.form.depth_m = depth_m;
        change.set_work(added.work, std::move(work));
        history.commit(std::move(change));
    }
    const doc::Document& document = history.document();
    const world::World compiled = world::compile(document);
    const world::Exhibit& exhibit = exhibit_of(compiled, added.exhibit);
    REQUIRE(exhibit.frame_instance.has_value());
    const world::Instance& frame = compiled.instances.at(*exhibit.frame_instance);
    REQUIRE(frame.part == world::Part::frame);

    // The frame's extent along the face, from its vertices in world coordinates.
    double left_m = inf;
    double right_m = -inf;
    for (const dvec3& position : compiled.meshes.at(frame.mesh).positions) {
        const dvec3 at_m = dvec3(frame.transform * glm::dvec4(position, 1.0));
        const double u_m = glm::dot(at_m - exhibit.face_centre_m, exhibit.face_u);
        left_m = std::min(left_m, u_m);
        right_m = std::max(right_m, u_m);
    }
    const doc::Entity& entity = entity_of(document, added.exhibit);
    // A rotation and a translation of numbers near 1 to 10 m: rounding of about 1e-15 m.
    CHECK_THAT(hung_width_m(*document.find_work(added.work), entity.framing),
               WithinAbs(right_m - left_m, exact_m));
}

TEST_CASE("add_work_in_row: three works of different widths hang left to right in the order "
          "added, 0.4 m apart, without overlapping, the row centred on face A of wall A",
          "[editor][commands]") {
    CHECK(row_gap_m == 0.4);
    CHECK(row_end_clearance_m == 0.3);
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const doc::Uuid wall_a = room.walls[0];

    // Widths on the wall: 0.61, 0.84 and 1.31 m.
    const WorkIds one = commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
    {
        INFO("one work: in the middle of the face");
        const std::vector<RowWork> row = row_on(history.document(), wall_a);
        REQUIRE(row.size() == 1);
        CHECK(row[0].exhibit == one.exhibit);
        CHECK_THAT(row[0].distance_m, WithinAbs(3.0, exact_m));
        check_centred_row(row, 6.0);
    }

    const WorkIds two = commit_add_work_in_row(history, ids, 0.73, 0.92, "Two");
    {
        // The row is 0.61 + 0.4 + 0.84 = 1.85 m, from u = (6 - 1.85) / 2 = 2.075: centres at
        // 2.075 + 0.305 = 2.38 and 2.075 + 0.61 + 0.4 + 0.42 = 3.505.
        INFO("two works");
        const std::vector<RowWork> row = row_on(history.document(), wall_a);
        REQUIRE(row.size() == 2);
        CHECK(exhibits_of(row) == std::vector<doc::Uuid>{one.exhibit, two.exhibit});
        CHECK_THAT(row[0].distance_m, WithinAbs(2.38, exact_m));
        CHECK_THAT(row[1].distance_m, WithinAbs(3.505, exact_m));
        check_centred_row(row, 6.0);
    }

    const WorkIds three = commit_add_work_in_row(history, ids, 1.2, 0.8, "Three");
    const doc::Document& document = history.document();
    // The row is 0.61 + 0.4 + 0.84 + 0.4 + 1.31 = 3.56 m, from u = (6 - 3.56) / 2 = 1.22 to
    // 4.78: centres at 1.22 + 0.305 = 1.525, 1.22 + 0.61 + 0.4 + 0.42 = 2.65 and
    // 4.78 - 0.655 = 4.125; 1.22 m clear at each end.
    const std::vector<RowWork> row = row_on(document, wall_a);
    REQUIRE(row.size() == 3);
    CHECK(exhibits_of(row) == std::vector<doc::Uuid>{one.exhibit, two.exhibit, three.exhibit});
    CHECK_THAT(row[0].distance_m, WithinAbs(1.525, exact_m));
    CHECK_THAT(row[1].distance_m, WithinAbs(2.65, exact_m));
    CHECK_THAT(row[2].distance_m, WithinAbs(4.125, exact_m));
    CHECK_THAT(row[0].left_m(), WithinAbs(1.22, exact_m));
    CHECK_THAT(6.0 - row[2].right_m(), WithinAbs(1.22, exact_m));
    check_centred_row(row, 6.0);

    // Every one on face A of wall A, on the centre line, 0.04 m out; the face is 6 m long.
    for (const doc::Uuid& exhibit : {one.exhibit, two.exhibit, three.exhibit}) {
        CAPTURE(exhibit);
        const doc::OnWall& at = on_wall_of(entity_of(document, exhibit));
        CHECK(at.wall == wall_a);
        CHECK(at.face == doc::WallFace::a);
        CHECK(at.centre_height_m == document.settings().centre_line_m);
        CHECK(at.stand_off_m == default_stand_off_m);
        CHECK_THAT(face_of(document, exhibit).length_m, WithinAbs(6.0, exact_m));
    }
    // Each work as add_work makes it: a box frame, the size, the image, the title.
    const doc::Entity& third = entity_of(document, three.exhibit);
    CHECK(third.name == "Three");
    REQUIRE(third.exhibit.has_value());
    CHECK(third.exhibit->work == three.work);
    REQUIRE(third.framing.has_value());
    CHECK(*third.framing == framing_of_style(std::string(default_frame_style)));
    const doc::Work* work = document.find_work(three.work);
    REQUIRE(work != nullptr);
    CHECK(work->title == "Three");
    CHECK(work->form.width_m == 1.2);
    CHECK(work->form.height_m == 0.8);
    CHECK(work->image == chart_image);
    CHECK(work->material.preset == std::string(default_material_preset));
    CHECK_FALSE(work->source.has_value());
    CHECK(document.works().size() == 3);
}

TEST_CASE("add_work_in_row: a work hung by add_work in the middle is moved into the row with the "
          "new one",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds middle = commit_add_work(history, ids, 0.73, 0.92, "Middle");
    REQUIRE_THAT(on_wall_of(entity_of(history.document(), middle.exhibit)).distance_m,
                 WithinAbs(3.0, exact_m));

    const WorkIds added = commit_add_work_in_row(history, ids, 0.5, 0.4, "Added");
    // The row is 0.84 + 0.4 + 0.61 = 1.85 m from u = 2.075: centres at 2.075 + 0.42 = 2.495 and
    // 2.075 + 0.84 + 0.4 + 0.305 = 3.62.
    const std::vector<RowWork> row = row_on(history.document(), room.walls[0]);
    REQUIRE(row.size() == 2);
    CHECK(exhibits_of(row) == std::vector<doc::Uuid>{middle.exhibit, added.exhibit});
    CHECK_THAT(row[0].distance_m, WithinAbs(2.495, exact_m));
    CHECK_THAT(row[1].distance_m, WithinAbs(3.62, exact_m));
    check_centred_row(row, 6.0);
}

TEST_CASE("add_work_in_row: the works already there keep their order along the face, equal "
          "distances by id, and the new one goes at the right end",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    // Both hung by add_work at the middle of the face, u = 3: the same distance.
    const WorkIds first = commit_add_work(history, ids, 0.73, 0.92, "First");
    const WorkIds second = commit_add_work(history, ids, 0.5, 0.4, "Second");
    REQUIRE(first.exhibit < second.exhibit);

    std::vector<doc::Uuid> expected;
    SECTION("at the same distance: in order of id") {
        expected = {first.exhibit, second.exhibit};
    }
    SECTION("the second moved to u = 1 m, left of the first: in order of distance") {
        doc::OnWall at = on_wall_of(entity_of(history.document(), second.exhibit));
        at.distance_m = 1.0;
        commit_set(history, second.exhibit, doc::Placement{at});
        expected = {second.exhibit, first.exhibit};
    }
    const WorkIds added = commit_add_work_in_row(history, ids, 1.2, 0.8, "Added");
    expected.push_back(added.exhibit);

    const std::vector<RowWork> row = row_on(history.document(), room.walls[0]);
    REQUIRE(row.size() == 3);
    CHECK(exhibits_of(row) == expected);
    // Strictly left to right now: no two at one distance.
    CHECK(row[0].distance_m < row[1].distance_m);
    CHECK(row[1].distance_m < row[2].distance_m);
    check_centred_row(row, 6.0);
}

TEST_CASE("add_work_in_row: the others keep their heights and stand-offs; the new work is on the "
          "project's centre line, 0.04 m out",
          "[editor][commands]") {
    const double centre_line_m = GENERATE(1.5, 1.45);
    CAPTURE(centre_line_m);
    doc::DocumentData data;
    data.settings.centre_line_m = centre_line_m;
    doc::History history{doc::Document(data)};
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds first = commit_add_work_in_row(history, ids, 0.73, 0.92, "First");

    doc::OnWall raised = on_wall_of(entity_of(history.document(), first.exhibit));
    raised.centre_height_m = 1.7;
    raised.stand_off_m = 0.02;
    commit_set(history, first.exhibit, doc::Placement{raised});

    const WorkIds second = commit_add_work_in_row(history, ids, 0.5, 0.4, "Second");
    const doc::OnWall& first_at = on_wall_of(entity_of(history.document(), first.exhibit));
    CHECK(first_at.centre_height_m == 1.7);
    CHECK(first_at.stand_off_m == 0.02);
    CHECK(first_at.wall == room.walls[0]);
    CHECK(first_at.face == doc::WallFace::a);
    // Moved along the face, from the middle to the row's left.
    CHECK(first_at.distance_m < 3.0);
    const doc::OnWall& second_at = on_wall_of(entity_of(history.document(), second.exhibit));
    CHECK(second_at.centre_height_m == centre_line_m);
    CHECK(second_at.stand_off_m == default_stand_off_m);
    check_centred_row(row_on(history.document(), room.walls[0]), 6.0);
}

TEST_CASE("add_work_in_row: an unframed work in the row takes its own width",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds bare = commit_add_work_in_row(history, ids, 0.5, 0.4, "Bare");
    doc::Transaction unframe(history.document(), "Remove frame");
    unframe.remove(bare.exhibit, doc::ComponentKind::framing);
    history.commit(std::move(unframe));

    const WorkIds framed = commit_add_work_in_row(history, ids, 0.73, 0.92, "Framed");
    // The row is 0.5 + 0.4 + 0.84 = 1.74 m from u = (6 - 1.74) / 2 = 2.13: centres at
    // 2.13 + 0.25 = 2.38 and 2.13 + 0.5 + 0.4 + 0.42 = 3.45.
    const std::vector<RowWork> row = row_on(history.document(), room.walls[0]);
    REQUIRE(row.size() == 2);
    CHECK(exhibits_of(row) == std::vector<doc::Uuid>{bare.exhibit, framed.exhibit});
    CHECK_THAT(row[0].distance_m, WithinAbs(2.38, exact_m));
    CHECK_THAT(row[1].distance_m, WithinAbs(3.45, exact_m));
    check_centred_row(row, 6.0);
}

TEST_CASE("add_work_in_row: works on another wall, or on face B of wall A, are not in the row and "
          "stay where they are",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds on_b = commit_add_work(history, ids, 0.73, 0.92, "On wall B");
    commit_set(history, on_b.exhibit,
               doc::Placement{doc::OnWall{.wall = room.walls[1],
                                          .face = doc::WallFace::a,
                                          .distance_m = 2.0,
                                          .centre_height_m = 1.5,
                                          .stand_off_m = 0.04}});
    const WorkIds behind = commit_add_work(history, ids, 0.5, 0.4, "Behind wall A");
    commit_set(history, behind.exhibit,
               doc::Placement{doc::OnWall{.wall = room.walls[0],
                                          .face = doc::WallFace::b,
                                          .distance_m = 1.0,
                                          .centre_height_m = 1.5,
                                          .stand_off_m = 0.04}});
    const doc::Placement on_b_before = *entity_of(history.document(), on_b.exhibit).placement;
    const doc::Placement behind_before = *entity_of(history.document(), behind.exhibit).placement;

    const WorkIds added = commit_add_work_in_row(history, ids, 0.73, 0.92, "Added");
    CHECK(*entity_of(history.document(), on_b.exhibit).placement == on_b_before);
    CHECK(*entity_of(history.document(), behind.exhibit).placement == behind_before);
    // Alone in the row: in the middle of the face.
    const std::vector<RowWork> row = row_on(history.document(), room.walls[0]);
    REQUIRE(row.size() == 1);
    CHECK(row[0].exhibit == added.exhibit);
    CHECK_THAT(row[0].distance_m, WithinAbs(3.0, exact_m));
}

TEST_CASE("add_work_in_row: one transaction named as add_work's, adding the work and its exhibit "
          "and moving the others, with ids taken for the work, then the entity",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds one = commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
    const WorkIds two = commit_add_work_in_row(history, ids, 0.73, 0.92, "Two");
    const std::size_t steps = history.undo_count();

    AddedWork made = add_work_in_row(history.document(), ids, chart_image, 1.2, 0.8, "Three");
    CHECK(made.transaction.name() == "Add Three");
    CHECK(&made.transaction.document() == &history.document());
    CHECK(room.walls[3] < made.work);
    CHECK(two.exhibit < made.work);
    CHECK(made.work < made.exhibit);

    const doc::ChangeList changes = made.transaction.changes();
    CHECK_FALSE(changes.settings);
    CHECK(changes.fixture_types.empty());
    REQUIRE(changes.works.size() == 1);
    CHECK(changes.works[0] == doc::WorkChange{made.work, doc::ChangeKind::added});
    REQUIRE(changes.entities.size() == 1);
    CHECK(changes.entities[0] == doc::EntityChange{made.exhibit, doc::ChangeKind::added});
    // The others: their placements changed, and nothing else of theirs.
    std::vector<doc::Uuid> moved;
    for (const doc::ComponentChange& change : changes.components) {
        if (change.entity == made.exhibit) {
            CHECK(change.kind == doc::ChangeKind::added);
            continue;
        }
        CAPTURE(change.entity);
        CHECK(change.component == doc::ComponentKind::placement);
        CHECK(change.kind == doc::ChangeKind::changed);
        moved.push_back(change.entity);
    }
    CHECK(moved == std::vector<doc::Uuid>{one.exhibit, two.exhibit});
    // Made, not committed.
    CHECK(history.document().works().size() == 2);
    CHECK(history.undo_count() == steps);

    history.commit(std::move(made.transaction));
    CHECK(history.undo_count() == steps + 1);
    CHECK(history.undo_name().value_or("") == "Add Three");

    AddedWork untitled = add_work_in_row(history.document(), ids, chart_image, 0.3, 0.2);
    CHECK(untitled.transaction.name() == "Add work");
}

TEST_CASE("add_work_in_row: one undo takes the new work away and puts the others back",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
    const WorkIds one = commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
    const WorkIds two = commit_add_work_in_row(history, ids, 0.73, 0.92, "Two");
    const doc::OnWall one_before = on_wall_of(entity_of(history.document(), one.exhibit));
    const doc::OnWall two_before = on_wall_of(entity_of(history.document(), two.exhibit));
    const std::string before = doc::to_project_json(history.document());

    const WorkIds three = commit_add_work_in_row(history, ids, 1.2, 0.8, "Three");
    // Moved left by the third.
    CHECK(on_wall_of(entity_of(history.document(), one.exhibit)).distance_m <
          one_before.distance_m);
    CHECK(on_wall_of(entity_of(history.document(), two.exhibit)).distance_m <
          two_before.distance_m);

    history.undo();
    CHECK(history.document().find_work(three.work) == nullptr);
    CHECK(history.document().find_entity(three.exhibit) == nullptr);
    // Back where they were, exactly: undo restores the values it replaced.
    CHECK(on_wall_of(entity_of(history.document(), one.exhibit)) == one_before);
    CHECK(on_wall_of(entity_of(history.document(), two.exhibit)) == two_before);
    CHECK(doc::to_project_json(history.document()) == before);
    check_centred_row(row_on(history.document(), room.walls[0]), 6.0);

    // And the whole of it undoes and redoes in one step, as every command does.
    AddedWork again = add_work_in_row(history.document(), ids, chart_image, 1.2, 0.8, "Three");
    check_undoes_in_one_step(history, std::move(again.transaction));
}

TEST_CASE("add_work_in_row: a row that would leave less than 0.3 m at an end of the face throws "
          "std::invalid_argument, 'Wall A is full', and changes nothing",
          "[editor][commands]") {
    using Catch::Matchers::ContainsSubstring;
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    commit_new_room(history, ids, 6.0, 4.0, 3.5);

    double width_m = 0.0;
    SECTION("one work too wide for an empty wall: 5.291 m, 5.401 m framed, 0.2995 m at each end") {
        width_m = 5.291;
    }
    SECTION("a fourth after three: 3.56 m of row + 0.4 + 1.61 = 5.57 m, beyond 6 - 2 x 0.3") {
        commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
        commit_add_work_in_row(history, ids, 0.73, 0.92, "Two");
        commit_add_work_in_row(history, ids, 1.2, 0.8, "Three");
        width_m = 1.5;
    }
    SECTION("1 mm more than fits after two: 6 - 0.6 - (0.61 + 0.4 + 0.84 + 0.4) = 3.15 m framed, "
            "so 3.041 m is 3.151 m") {
        commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
        commit_add_work_in_row(history, ids, 0.73, 0.92, "Two");
        width_m = 3.041;
    }
    CAPTURE(width_m);
    const std::string before = doc::to_project_json(history.document());
    const std::size_t steps = history.undo_count();

    CHECK_THROWS_AS(add_work_in_row(history.document(), ids, chart_image, width_m, 0.5, "Big"),
                    std::invalid_argument);
    CHECK_THROWS_WITH(add_work_in_row(history.document(), ids, chart_image, width_m, 0.5, "Big"),
                      ContainsSubstring("Wall A is full"));
    // Nothing changed: no work, no entity, every placement as it was. Ids may have been taken.
    CHECK(doc::to_project_json(history.document()) == before);
    CHECK(history.undo_count() == steps);
}

// An exact fit is not closer than 0.3 m, so it is taken. The sums are exact only analytically: in
// doubles they may come out a few ulps either side, so this asks that a rounding error far below
// a millimetre does not refuse a row that fits.
TEST_CASE("add_work_in_row: a row that leaves exactly 0.3 m at each end is taken",
          "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);

    SECTION("one work, 5.29 m: 5.4 m framed") {
        CHECK_NOTHROW(commit_add_work_in_row(history, ids, 5.29, 0.5, "Wide"));
    }
    SECTION("a third after two: 3.04 m, 3.15 m framed, makes the row 5.4 m") {
        commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
        commit_add_work_in_row(history, ids, 0.73, 0.92, "Two");
        CHECK_NOTHROW(commit_add_work_in_row(history, ids, 3.04, 0.5, "Wide"));
    }
    const std::vector<RowWork> row = row_on(history.document(), room.walls[0]);
    REQUIRE_FALSE(row.empty());
    // 0.3 m at each end; exact_m allows the rounding of the sums.
    CHECK_THAT(row.front().left_m(), WithinAbs(0.3, exact_m));
    CHECK_THAT(6.0 - row.back().right_m(), WithinAbs(0.3, exact_m));
    check_centred_row(row, 6.0);
}

TEST_CASE("add_work_in_row: refuses what add_work refuses, with std::invalid_argument",
          "[editor][commands]") {
    doc::IdGenerator ids = fixed_ids();

    SECTION("no walls") {
        const doc::Document empty;
        CHECK_THROWS_AS(add_work_in_row(empty, ids, chart_image, 0.73, 0.92),
                        std::invalid_argument);
    }
    SECTION("wall A relabelled in lower case") {
        doc::History history;
        const RoomIds room = commit_new_room(history, ids, 6.0, 4.0, 3.5);
        doc::Wall a = *entity_of(history.document(), room.walls[0]).wall;
        a.label = "a";
        commit_set(history, room.walls[0], a);
        CHECK_THROWS_AS(add_work_in_row(history.document(), ids, chart_image, 0.73, 0.92),
                        std::invalid_argument);
    }
    SECTION("sizes, an image that is not an asset name, a title too long") {
        doc::History history;
        commit_new_room(history, ids, 6.0, 4.0, 3.5);
        commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
        const std::string before = doc::to_project_json(history.document());
        const doc::Document& document = history.document();

        const double bad = GENERATE(0.0, -0.0, -0.73, not_a_number, inf, -inf);
        CAPTURE(bad);
        CHECK_THROWS_AS(add_work_in_row(document, ids, chart_image, bad, 0.92),
                        std::invalid_argument);
        CHECK_THROWS_AS(add_work_in_row(document, ids, chart_image, 0.73, bad),
                        std::invalid_argument);
        CHECK_THROWS_AS(
            add_work_in_row(document, ids, doc::AssetRef{chart_image.sha256, "PNG"}, 0.73, 0.92),
            std::invalid_argument);
        CHECK_THROWS_AS(add_work_in_row(document, ids, chart_image, 0.73, 0.92,
                                        std::string(doc::max_string_bytes + 1, 'x')),
                        std::invalid_argument);
        CHECK(doc::to_project_json(history.document()) == before);
    }
}

TEST_CASE("add_work_in_row: what it makes reads back from its project.json", "[editor][commands]") {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    commit_new_room(history, ids, 6.0, 4.0, 3.5);
    commit_add_work_in_row(history, ids, 0.5, 0.4, "One");
    commit_add_work(history, ids, 0.73, 0.92, "Two");
    commit_add_work_in_row(history, ids, 1.2, 0.8, "Three");

    const std::string text = doc::to_project_json(history.document());
    const doc::Document read = doc::parse_project_json(text);
    CHECK(read == history.document());
    CHECK(doc::to_project_json(read) == text);
}

// ---- add_work_in_row and the spots that light the row ----------------------------------------

// The owner's choice in the M1-27 session (23 November 2026): a spot aimed at a work the row moves
// moves with it, along its track, so that it keeps the angle add_spot_for chose for it.

// A room of 6 x 4 x 3.5 m with One (0.73 x 0.92 m, 0.84 m framed) hung by add_work_in_row at
// the middle of face A, 3 m, and a spot add_spot_for aimed at it.
struct LitRow {
    doc::History history;
    doc::IdGenerator ids = fixed_ids();
    WorkIds one;
    SpotIds spot;

    LitRow() {
        commit_new_room(history, ids, 6.0, 4.0, 3.5);
        one = commit_add_work_in_row(history, ids, 0.73, 0.92, "One");
        spot = commit_add_spot(history, ids, one.exhibit);
    }

    // Adds Two (0.5 x 0.4 m, 0.61 m framed): the row is 0.84 + 0.4 + 0.61 = 1.85 m from
    // u = 2.075 m, so One's centre moves from 3 m to 2.075 + 0.42 = 2.495 m, 0.505 m to the left.
    void add_two() { commit_add_work_in_row(history, ids, 0.5, 0.4, "Two"); }

    const doc::OnTrack& spot_at() const {
        return on_track_of(entity_of(history.document(), spot.spot));
    }

    void commit(doc::Transaction transaction) { history.commit(std::move(transaction)); }
};

constexpr double one_moves_m = -0.505;

TEST_CASE("add_work_in_row: a spot aimed at a work it moves slides along its track by as much, "
          "keeping its place relative to the work, and one undo puts it back",
          "[editor][commands]") {
    LitRow lit;
    const doc::OnTrack before = lit.spot_at();
    const world::World first = world::compile(lit.history.document());
    const dvec3 offset_before_m = light_of(first, lit.spot.spot).luminaire.position_m() -
                                  exhibit_of(first, lit.one.exhibit).face_centre_m;

    lit.add_two();
    const doc::Document& document = lit.history.document();
    CHECK_THAT(on_wall_of(entity_of(document, lit.one.exhibit)).distance_m,
               WithinAbs(3.0 + one_moves_m, exact_m));
    const doc::OnTrack after = lit.spot_at();
    CHECK(after.track == before.track);
    // add_spot_for's track begins opposite the face's left end: a distance along it is a u along
    // the face.
    CHECK_THAT(after.distance_m, WithinAbs(before.distance_m + one_moves_m, exact_m));
    // Where the light is, seen from the work, is as it was: so is the angle of its aim.
    const world::World moved = world::compile(document);
    check_near(light_of(moved, lit.spot.spot).luminaire.position_m() -
                   exhibit_of(moved, lit.one.exhibit).face_centre_m,
               offset_before_m, exact_m);
    CHECK(entity_of(document, lit.spot.spot).luminaire->aim ==
          doc::Aim{doc::AimAtEntity{lit.one.exhibit}});

    lit.history.undo();
    CHECK(lit.spot_at() == before);
}

TEST_CASE("add_work_in_row: a spot aimed at a point, or on a track at an angle to the face, stays "
          "where it is",
          "[editor][commands]") {
    LitRow lit;
    doc::Transaction change(lit.history.document(), "Change the spot");
    SECTION("aimed at a point") {
        doc::Luminaire luminaire = *entity_of(lit.history.document(), lit.spot.spot).luminaire;
        luminaire.aim = doc::AimAtPoint{doc::Vec3{3.0, 0.1, 1.5}};
        change.set(lit.spot.spot, std::move(luminaire));
    }
    SECTION("on a track at an angle to the face") {
        doc::Track track = *entity_of(lit.history.document(), lit.spot.track).track;
        REQUIRE(track.points_m.size() == 2);
        track.points_m[1].y += 1.0;
        change.set(lit.spot.track, std::move(track));
    }
    lit.commit(std::move(change));
    const doc::OnTrack before = lit.spot_at();

    lit.add_two();
    CHECK(lit.spot_at() == before);
}

// A track drawn from the face's right end to its left: a distance along it runs against u, so the
// spot slides the other way along it, and to the same place.
TEST_CASE("add_work_in_row: a spot on a track drawn against the face slides with the work too",
          "[editor][commands]") {
    LitRow lit;
    const world::World first = world::compile(lit.history.document());
    const dvec3 light_before_m = light_of(first, lit.spot.spot).luminaire.position_m();
    const dvec3 offset_before_m = light_before_m - exhibit_of(first, lit.one.exhibit).face_centre_m;

    // The same track with its points swapped, and the spot at the same place on it.
    doc::Track track = *entity_of(lit.history.document(), lit.spot.track).track;
    REQUIRE(track.points_m.size() == 2);
    std::swap(track.points_m[0], track.points_m[1]);
    const double length_m = glm::length(dvec2(track.points_m[1].x - track.points_m[0].x,
                                              track.points_m[1].y - track.points_m[0].y));
    doc::OnTrack reversed = lit.spot_at();
    reversed.distance_m = length_m - reversed.distance_m;
    doc::Transaction change(lit.history.document(), "Reverse the track");
    change.set(lit.spot.track, std::move(track));
    change.set(lit.spot.spot, doc::Placement{reversed});
    lit.commit(std::move(change));
    const world::World swapped = world::compile(lit.history.document());
    check_near(light_of(swapped, lit.spot.spot).luminaire.position_m(), light_before_m, exact_m);

    lit.add_two();
    CHECK_THAT(lit.spot_at().distance_m, WithinAbs(reversed.distance_m - one_moves_m, exact_m));
    const world::World moved = world::compile(lit.history.document());
    check_near(light_of(moved, lit.spot.spot).luminaire.position_m() -
                   exhibit_of(moved, lit.one.exhibit).face_centre_m,
               offset_before_m, exact_m);
}

// The light and the work move along the wall together, and nothing lies between them, so the
// direct light on the work is the engine's same reading but for rounding. The total is not
// compared: the room stays where it is, so the light it reflects onto the work changes.
// Tolerance: a translation of the same geometry by 0.505 m changes only rounding, about 1e-15
// relative; 1e-9 allows any order of operations and still catches a spot that moved by 0.1 mm.
TEST_CASE("add_work_in_row: the direct lux on a work it moves, lit by its spot, stays the same",
          "[editor][accuracy]") {
    LitRow lit;
    const auto direct_on_one = [&lit] {
        const world::World compiled = world::compile(lit.history.document());
        const cs::engine::Scene scene(compiled);
        cs::engine::LuxOptions options;
        options.samples = 1; // direct light does not depend on the indirect paths
        return cs::engine::work_lux_grid(scene, exhibit_of(compiled, lit.one.exhibit), options)
            .direct_lux.mean;
    };
    const double before_lx = direct_on_one();
    // Lit as the CCI advises: hundreds of lux from the generic 800 lm spot.
    REQUIRE(before_lx > 10.0);

    lit.add_two();
    CHECK_THAT(direct_on_one(), WithinRel(before_lx, 1e-9));
}

// Where the clamp acts, the spot is nearer the work than add_spot_for put it, so its angle from
// vertical is less than 30 degrees and the work reads more lux (said in the header and the pull
// request): add_spot_for, placing it afresh, would put it on the work's other side.
TEST_CASE("add_work_in_row: a spot that would slide past its track's end stops at it",
          "[editor][commands]") {
    LitRow lit;
    // 0.2 m from the track's first point: sliding 0.505 m towards it would pass it.
    doc::OnTrack near_start = lit.spot_at();
    near_start.distance_m = 0.2;
    doc::Transaction change(lit.history.document(), "Move the spot");
    change.set(lit.spot.spot, doc::Placement{near_start});
    lit.commit(std::move(change));

    lit.add_two();
    CHECK(lit.spot_at().distance_m == 0.0);
}

// A spot placed by hand beyond its track's end (compile marks it beyond_track) is not one
// add_spot_for makes: moving it onto the track would change its light on the work for any move of
// the work, however small.
TEST_CASE("add_work_in_row: a spot already beyond its track's end stays where it is",
          "[editor][commands]") {
    LitRow lit;
    const doc::Entity* holder = lit.history.document().find_entity(lit.spot_at().track);
    REQUIRE(holder != nullptr);
    REQUIRE(holder->track);
    REQUIRE(holder->track->points_m.size() == 2);
    const doc::Vec2 a = holder->track->points_m[0];
    const doc::Vec2 b = holder->track->points_m[1];
    doc::OnTrack beyond = lit.spot_at();
    beyond.distance_m = glm::length(dvec2(b.x - a.x, b.y - a.y)) + 0.3;
    doc::Transaction change(lit.history.document(), "Move the spot");
    change.set(lit.spot.spot, doc::Placement{beyond});
    lit.commit(std::move(change));

    lit.add_two();
    CHECK(lit.spot_at() == beyond);
}
