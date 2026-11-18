// SPDX-License-Identifier: Apache-2.0

// The first editor commands (#66, M1-25): new_room, add_work and add_spot_for, each one
// transaction that undoes in one step, from the issue's acceptance criteria and the headers of
// libs/editor/include/cs/editor/commands/. The numbers are the issue's and ADR 0006's: four walls
// lettered A to D, a work at the middle of wall A on the project's centre line, and a spot on a
// ceiling track (ceiling height - 1.6 m) x tan 30 degrees = 1.097 m from the face of a 3.5 m room,
// slid along it until its aim at the centre of the work's face is 30 degrees from vertical. The
// skeleton scene (tests/scenes/skeleton/README.md) is the worked example: there the slide is
// 0.465 m.
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
#include <cs/geom/wall_face.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

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
