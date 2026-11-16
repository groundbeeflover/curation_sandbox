// SPDX-License-Identifier: Apache-2.0

// The mutation API (transaction.hpp, M1-09): every write to a document goes through a
// Transaction, which a History commits all at once or, when the result would hold a dangling
// reference, not at all; each commit reports what it changed. Undo, redo and the random
// transactions of the property test are in test_history.cpp.
//
// The tests run on the skeleton scene, tests/scenes/skeleton/, whose ids are fixed: the work
// ...01 (the test chart), the room ...02, walls A to D ...03 to ...06, the chart's entity ...07
// (hung OnWall on wall A), track A ...08, the spot ...09 (OnTrack on track A, aimed at the chart)
// and the viewpoint ...0a.

#include <cs/core/random.hpp>
#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/history.hpp>
#include <cs/doc/library.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cs::doc;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

std::string kind_text(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::added:
        return "added";
    case ChangeKind::changed:
        return "changed";
    case ChangeKind::removed:
        return "removed";
    }
    return "?";
}

} // namespace

// Readable failure messages for change lists and ids.
namespace Catch {

template <> struct StringMaker<cs::doc::ChangeList> {
    static std::string convert(const cs::doc::ChangeList& changes) {
        std::ostringstream out;
        out << "{ settings " << (changes.settings ? "changed" : "unchanged");
        for (const auto& change : changes.fixture_types) {
            out << "; fixture type " << change.fixture_type << " " << kind_text(change.kind);
        }
        for (const auto& change : changes.works) {
            out << "; work " << change.work.to_string() << " " << kind_text(change.kind);
        }
        for (const auto& change : changes.entities) {
            out << "; entity " << change.entity.to_string() << " " << kind_text(change.kind);
        }
        for (const auto& change : changes.components) {
            out << "; " << change.entity.to_string() << "." << component_key(change.component)
                << " " << kind_text(change.kind);
        }
        out << " }";
        return out.str();
    }
};

template <> struct StringMaker<cs::doc::Uuid> {
    static std::string convert(const cs::doc::Uuid& id) { return id.to_string(); }
};

} // namespace Catch

namespace {

// ---- The skeleton scene ----------------------------------------------------------------------

// The skeleton's ids are 01a1cd0d-dc00-7000-8000-0000000000<n>.
constexpr Uuid skeleton_id(std::uint8_t n) {
    return Uuid(Uuid::Bytes{0x01, 0xa1, 0xcd, 0x0d, 0xdc, 0x00, 0x70, 0x00, 0x80, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, n});
}

constexpr Uuid test_chart_work = skeleton_id(0x01);
constexpr Uuid gallery = skeleton_id(0x02);
constexpr Uuid wall_a = skeleton_id(0x03);
constexpr Uuid wall_b = skeleton_id(0x04);
constexpr Uuid wall_c = skeleton_id(0x05);
constexpr Uuid wall_d = skeleton_id(0x06);
constexpr Uuid test_chart = skeleton_id(0x07);
constexpr Uuid track_a = skeleton_id(0x08);
constexpr Uuid spot = skeleton_id(0x09);
constexpr Uuid viewpoint = skeleton_id(0x0a);
// An id of the skeleton's form that names nothing in it.
constexpr Uuid missing = skeleton_id(0x7f);

const LibraryId spot_type = "generic-spot-800";

Document load_skeleton() {
    return load_project_folder(std::filesystem::path(CS_SOURCE_DIR "/tests/scenes/skeleton"));
}

// New ids, the same in every run: a fixed seed and a clock stopped at one millisecond.
IdGenerator fixed_ids() {
    return IdGenerator(cs::hash_seed(60, 0, 0, 0, 0),
                       [] { return std::int64_t{1'800'000'000'000}; });
}

// ---- Sample values, all within project_io's ranges --------------------------------------------

// An entity with a name and no components.
Entity named(std::string name) {
    Entity entity;
    entity.name = std::move(name);
    return entity;
}

Wall sample_wall() {
    return {.start_m = {1.0, 1.0},
            .end_m = {4.0, 1.0},
            .height_m = 2.5,
            .thickness_m = 0.1,
            .finish_a = std::nullopt,
            .finish_b = std::nullopt,
            .label = "E"};
}

Work sample_work(std::string title) {
    Work work;
    work.number = "2";
    work.title = std::move(title);
    work.medium = "Oil on canvas";
    work.form.width_m = 0.5;
    work.form.height_m = 0.4;
    work.material.preset = "oil-canvas-gloss";
    work.sensitivity.responsivity = Responsivity::medium;
    return work;
}

FixtureType sample_fixture_type() {
    return {.name = "Generic flood 2000 lm",
            .flux_lm = 2000.0,
            .beam_angle_rad = 0.6,
            .field_angle_rad = 0.9,
            .colour_temperature_k = 3500.0};
}

LuxSensor sample_lux_sensor() {
    return {.position_m = {3.0, 0.01, 1.5}, .normal = {0.0, 1.0, 0.0}};
}

// ---- Helpers -----------------------------------------------------------------------------------

// The ReferenceError a commit throws, or nullopt when it commits. Any other exception escapes.
std::optional<ReferenceError> refusal(History& history, Transaction transaction) {
    try {
        (void)history.commit(std::move(transaction));
    } catch (const ReferenceError& error) {
        return error;
    }
    return std::nullopt;
}

std::string name_or_none(std::optional<std::string_view> name) {
    return name ? std::string(*name) : std::string("(none)");
}

// Commits a transaction that renames wall B; undoing it afterwards leaves a redo entry.
void commit_rename_wall_b(History& history) {
    Transaction rename(history.document(), "Rename Wall B");
    rename.rename_entity(wall_b, "North wall");
    REQUIRE_FALSE(history.commit(std::move(rename)).empty());
}

// What a test can see of a transaction: what it would change, and the value it gives to each
// thing a test touches.
struct Seen {
    ChangeList changes;
    std::vector<std::optional<Entity>> entities;
    std::vector<std::optional<Work>> works;
    std::vector<std::optional<FixtureType>> fixture_types;
    ProjectSettings settings;
    bool operator==(const Seen&) const = default;
};

template <class T> std::optional<T> value_of(const T* pointer) {
    return pointer ? std::optional<T>(*pointer) : std::nullopt;
}

Seen seen(const Transaction& transaction, const std::vector<Uuid>& extra_ids) {
    Seen result;
    result.changes = transaction.changes();
    result.settings = transaction.settings();
    std::vector<Uuid> ids = {gallery,    wall_a,  wall_b, wall_c,    wall_d,
                             test_chart, track_a, spot,   viewpoint, missing};
    ids.insert(ids.end(), extra_ids.begin(), extra_ids.end());
    for (const Uuid& id : ids) {
        result.entities.push_back(value_of(transaction.find_entity(id)));
        result.works.push_back(value_of(transaction.find_work(id)));
    }
    result.works.push_back(value_of(transaction.find_work(test_chart_work)));
    for (const LibraryId& id : {spot_type, LibraryId("generic-flood-2000")}) {
        result.fixture_types.push_back(value_of(transaction.find_fixture_type(id)));
    }
    return result;
}

} // namespace

// ---- AC1: the document is read-only; every write goes through a Transaction --------------------

TEST_CASE("AC1: a Document has no writers; only a Transaction committed by a History changes it",
          "[doc][transaction]") {
    // Every accessor of a Document gives a const view, whether the Document is const or not.
    static_assert(std::is_same_v<decltype(std::declval<Document&>().data()), const DocumentData&>);
    static_assert(std::is_same_v<decltype(std::declval<Document&>().entities()),
                                 const std::map<Uuid, Entity>&>);
    static_assert(
        std::is_same_v<decltype(std::declval<Document&>().works()), const std::map<Uuid, Work>&>);
    static_assert(std::is_same_v<decltype(std::declval<Document&>().library()), const Library&>);
    static_assert(
        std::is_same_v<decltype(std::declval<Document&>().settings()), const ProjectSettings&>);
    static_assert(
        std::is_same_v<decltype(std::declval<Document&>().find_entity(Uuid{})), const Entity*>);
    // A transaction is made from a const document, and the history hands out only a const one.
    static_assert(std::is_constructible_v<Transaction, const Document&, std::string>);
    static_assert(std::is_same_v<decltype(std::declval<History&>().document()), const Document&>);
    // History owns its document and is neither copied nor moved, so transactions can point at it.
    static_assert(!std::is_copy_constructible_v<History> && !std::is_move_constructible_v<History>);

    History history(load_skeleton());
    const DocumentData before = history.document().data();
    Transaction transaction(history.document(), "Rename Wall B");
    transaction.rename_entity(wall_b, "North wall");
    // Building a transaction does not touch the document.
    CHECK(history.document().data() == before);
    (void)history.commit(std::move(transaction));
    CHECK(history.document().find_entity(wall_b)->name == "North wall");
}

TEST_CASE("AC1: each operation of a Transaction, committed, gives the document it describes",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    DocumentData expected = history.document().data();
    ChangeList expected_changes;
    Transaction transaction(history.document(), "An edit");

    SECTION("add_entity") {
        const Uuid id = ids.next();
        Entity sensor = named("Sensor 1");
        sensor.lux_sensor = sample_lux_sensor();
        transaction.add_entity(id, sensor);
        expected.entities[id] = sensor;
        expected_changes.entities = {{id, ChangeKind::added}};
        expected_changes.components = {{id, ComponentKind::lux_sensor, ChangeKind::added}};
    }
    SECTION("add_entity with no components") {
        const Uuid id = ids.next();
        transaction.add_entity(id, named("Note"));
        expected.entities[id] = named("Note");
        expected_changes.entities = {{id, ChangeKind::added}};
    }
    SECTION("remove_entity") {
        transaction.remove_entity(viewpoint);
        expected.entities.erase(viewpoint);
        expected_changes.entities = {{viewpoint, ChangeKind::removed}};
        expected_changes.components = {{viewpoint, ComponentKind::viewpoint, ChangeKind::removed}};
    }
    SECTION("rename_entity") {
        transaction.rename_entity(wall_b, "North wall");
        expected.entities[wall_b].name = "North wall";
        expected_changes.entities = {{wall_b, ChangeKind::changed}};
    }
    SECTION("set replaces a wall") {
        Wall wall = *expected.entities[wall_b].wall;
        wall.height_m = 3.0;
        wall.finish_a = "wall-white";
        transaction.set(wall_b, wall);
        expected.entities[wall_b].wall = wall;
        expected_changes.components = {{wall_b, ComponentKind::wall, ChangeKind::changed}};
    }
    SECTION("set replaces a room") {
        Room room = *expected.entities[gallery].room;
        room.ceiling_height_m = 4.0;
        transaction.set(gallery, room);
        expected.entities[gallery].room = room;
        expected_changes.components = {{gallery, ComponentKind::room, ChangeKind::changed}};
    }
    SECTION("set replaces a framing") {
        const Framing framing{.style = "box",
                              .width_m = 0.05,
                              .colour = std::nullopt,
                              .mat = std::nullopt,
                              .glazing = Glazing::acrylic};
        transaction.set(test_chart, framing);
        expected.entities[test_chart].framing = framing;
        expected_changes.components = {{test_chart, ComponentKind::framing, ChangeKind::changed}};
    }
    SECTION("set replaces a placement") {
        const OnWall moved{.wall = wall_a,
                           .face = WallFace::a,
                           .distance_m = 2.4,
                           .centre_height_m = 1.5,
                           .stand_off_m = 0.04};
        transaction.set(test_chart, moved);
        expected.entities[test_chart].placement = moved;
        expected_changes.components = {{test_chart, ComponentKind::placement, ChangeKind::changed}};
    }
    SECTION("set replaces a track") {
        Track track = *expected.entities[track_a].track;
        track.height_m = 3.2;
        transaction.set(track_a, track);
        expected.entities[track_a].track = track;
        expected_changes.components = {{track_a, ComponentKind::track, ChangeKind::changed}};
    }
    SECTION("set replaces a luminaire") {
        Luminaire luminaire = *expected.entities[spot].luminaire;
        luminaire.dimming = 0.5;
        transaction.set(spot, luminaire);
        expected.entities[spot].luminaire = luminaire;
        expected_changes.components = {{spot, ComponentKind::luminaire, ChangeKind::changed}};
    }
    SECTION("set replaces a viewpoint") {
        Viewpoint view = *expected.entities[viewpoint].viewpoint;
        view.ev100.reset();
        transaction.set(viewpoint, view);
        expected.entities[viewpoint].viewpoint = view;
        expected_changes.components = {{viewpoint, ComponentKind::viewpoint, ChangeKind::changed}};
    }
    SECTION("set adds every kind of component to an entity without it") {
        // Set in the reverse of ComponentKind's order; the change list is in that order.
        const Uuid id = ids.next();
        Entity entity = named("Everything");
        entity.dimension = Dimension{.from = {.entity = wall_a, .point_m = {0.0, 0.0, 0.0}},
                                     .to = {.entity = std::nullopt, .point_m = {1.0, 1.0, 0.0}}};
        entity.viewpoint = Viewpoint{.position_m = {1.0, 1.0, 1.6},
                                     .yaw_rad = 0.5,
                                     .pitch_rad = 0.0,
                                     .field_of_view_rad = 0.9,
                                     .ev100 = std::nullopt};
        entity.lux_sensor = sample_lux_sensor();
        entity.luminaire = Luminaire{.fixture_type = spot_type,
                                     .dimming = 0.75,
                                     .aim = AimAtPoint{{3.0, 0.0, 1.5}},
                                     .colour_temperature_k = std::nullopt};
        entity.track = Track{.points_m = {{1.0, 2.0}, {5.0, 2.0}}, .height_m = 3.4};
        entity.placement = OnWall{.wall = wall_b,
                                  .face = WallFace::a,
                                  .distance_m = 2.0,
                                  .centre_height_m = 1.5,
                                  .stand_off_m = 0.02};
        entity.framing = Framing{.style = "box",
                                 .width_m = std::nullopt,
                                 .colour = std::nullopt,
                                 .mat = std::nullopt,
                                 .glazing = Glazing::none};
        entity.exhibit = Exhibit{.work = test_chart_work};
        entity.room = Room{.outline_m = {{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}},
                           .floor_height_m = 0.0,
                           .ceiling_height_m = 3.0,
                           .floor_finish = std::nullopt,
                           .ceiling_finish = std::nullopt};
        entity.opening = Opening{.wall = wall_c,
                                 .offset_m = 1.0,
                                 .width_m = 0.9,
                                 .height_m = 2.1,
                                 .sill_m = 0.0,
                                 .kind = OpeningKind::door};
        entity.wall = sample_wall();

        transaction.add_entity(id, named("Everything"));
        transaction.set(id, *entity.dimension);
        transaction.set(id, *entity.viewpoint);
        transaction.set(id, *entity.lux_sensor);
        transaction.set(id, *entity.luminaire);
        transaction.set(id, *entity.track);
        transaction.set(id, *entity.placement);
        transaction.set(id, *entity.framing);
        transaction.set(id, *entity.exhibit);
        transaction.set(id, *entity.room);
        transaction.set(id, *entity.opening);
        transaction.set(id, *entity.wall);

        expected.entities[id] = entity;
        expected_changes.entities = {{id, ChangeKind::added}};
        for (const ComponentKind kind : all_component_kinds) {
            expected_changes.components.push_back({id, kind, ChangeKind::added});
        }
    }
    SECTION("remove a component") {
        transaction.remove(test_chart, ComponentKind::framing);
        expected.entities[test_chart].framing.reset();
        expected_changes.components = {{test_chart, ComponentKind::framing, ChangeKind::removed}};
    }
    SECTION("set_work adds a work") {
        const Uuid id = ids.next();
        transaction.set_work(id, sample_work("Water Lilies"));
        expected.works[id] = sample_work("Water Lilies");
        expected_changes.works = {{id, ChangeKind::added}};
    }
    SECTION("set_work replaces a work") {
        Work work = expected.works[test_chart_work];
        work.title = "Test chart, second printing";
        work.sensitivity.max_lux = 50.0;
        transaction.set_work(test_chart_work, work);
        expected.works[test_chart_work] = work;
        expected_changes.works = {{test_chart_work, ChangeKind::changed}};
    }
    SECTION("remove_work, with the exhibit that showed it") {
        transaction.remove(test_chart, ComponentKind::exhibit);
        transaction.remove_work(test_chart_work);
        expected.entities[test_chart].exhibit.reset();
        expected.works.erase(test_chart_work);
        expected_changes.works = {{test_chart_work, ChangeKind::removed}};
        expected_changes.components = {{test_chart, ComponentKind::exhibit, ChangeKind::removed}};
    }
    SECTION("set_fixture_type adds a fixture type") {
        transaction.set_fixture_type("generic-flood-2000", sample_fixture_type());
        expected.library.fixture_types["generic-flood-2000"] = sample_fixture_type();
        expected_changes.fixture_types = {{"generic-flood-2000", ChangeKind::added}};
    }
    SECTION("set_fixture_type replaces a fixture type") {
        FixtureType type = expected.library.fixture_types[spot_type];
        type.flux_lm = 900.0;
        transaction.set_fixture_type(spot_type, type);
        expected.library.fixture_types[spot_type] = type;
        expected_changes.fixture_types = {{spot_type, ChangeKind::changed}};
    }
    SECTION("remove_fixture_type, with the luminaire that used it") {
        transaction.remove(spot, ComponentKind::luminaire);
        transaction.remove_fixture_type(spot_type);
        expected.entities[spot].luminaire.reset();
        expected.library.fixture_types.erase(spot_type);
        expected_changes.fixture_types = {{spot_type, ChangeKind::removed}};
        expected_changes.components = {{spot, ComponentKind::luminaire, ChangeKind::removed}};
    }
    SECTION("set_settings") {
        const ProjectSettings settings{.centre_line_m = 1.45,
                                       .eye_height_m = 1.55,
                                       .opening_hours_per_year = 3000.0,
                                       .display_units = DisplayUnits::inches};
        transaction.set_settings(settings);
        expected.settings = settings;
        expected_changes.settings = true;
    }

    const std::size_t undo_before = history.undo_count();
    const ChangeList predicted = transaction.changes();
    CHECK_FALSE(transaction.empty());
    const ChangeList changes = history.commit(std::move(transaction));
    CHECK(changes == expected_changes);
    CHECK(predicted == expected_changes);
    CHECK(history.document().data() == expected);
    CHECK(history.undo_count() == undo_before + 1);
}

TEST_CASE("AC1: a transaction sees its own changes, and the document sees none until the commit",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    const Document& document = history.document();
    const DocumentData before = document.data();
    const Uuid sensor = ids.next();
    const Uuid work = ids.next();

    Transaction transaction(document, "Several things");
    CHECK(transaction.name() == "Several things");
    CHECK(&transaction.document() == &document);
    CHECK(transaction.empty());

    transaction.add_entity(sensor, named("Sensor"));
    transaction.set(sensor, sample_lux_sensor());
    transaction.rename_entity(wall_b, "North wall");
    transaction.remove_entity(viewpoint);
    transaction.set_work(work, sample_work("Water Lilies"));
    transaction.set_fixture_type("generic-flood-2000", sample_fixture_type());
    ProjectSettings settings = document.settings();
    settings.eye_height_m = 1.5;
    transaction.set_settings(settings);

    REQUIRE(transaction.find_entity(sensor) != nullptr);
    CHECK(transaction.find_entity(sensor)->lux_sensor == sample_lux_sensor());
    CHECK(transaction.find_entity(wall_b)->name == "North wall");
    CHECK(transaction.find_entity(viewpoint) == nullptr);
    CHECK(transaction.find_entity(wall_a) != nullptr); // untouched things are seen through
    REQUIRE(transaction.find_work(work) != nullptr);
    CHECK(transaction.find_work(work)->title == "Water Lilies");
    CHECK(transaction.find_work(test_chart_work) != nullptr);
    CHECK(transaction.find_fixture_type("generic-flood-2000") != nullptr);
    CHECK(transaction.find_fixture_type(spot_type) != nullptr);
    CHECK(transaction.settings().eye_height_m == 1.5);
    CHECK_FALSE(transaction.empty());

    CHECK(document.data() == before);
    CHECK(document.find_entity(sensor) == nullptr);
    CHECK(document.find_entity(viewpoint) != nullptr);

    (void)history.commit(std::move(transaction));
    CHECK(document.find_entity(sensor) != nullptr);
    CHECK(document.find_entity(viewpoint) == nullptr);
    CHECK(document.find_entity(wall_b)->name == "North wall");
    CHECK(document.find_work(work) != nullptr);
    CHECK(document.library().find_fixture_type("generic-flood-2000") != nullptr);
    CHECK(document.settings().eye_height_m == 1.5);
}

TEST_CASE("AC1: a committed transaction is applied all at once, and a refused one not at all",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    const Uuid work = ids.next();
    const DocumentData before = history.document().data();

    const auto build = [&](Transaction& transaction) {
        transaction.rename_entity(wall_b, "North wall");
        transaction.set_work(work, sample_work("Water Lilies"));
        ProjectSettings settings = history.document().settings();
        settings.centre_line_m = 1.45;
        transaction.set_settings(settings);
    };

    SECTION("one dangling reference refuses everything in the transaction") {
        Transaction transaction(history.document(), "Mixed");
        build(transaction);
        transaction.remove_entity(wall_a); // the test chart hangs on it
        CHECK(refusal(history, std::move(transaction)).has_value());
        CHECK(history.document().data() == before);
        CHECK(history.undo_count() == 0);
        CHECK(history.redo_count() == 0);
    }
    SECTION("without it, everything is applied, and one undo takes it all back") {
        Transaction transaction(history.document(), "Mixed");
        build(transaction);
        (void)history.commit(std::move(transaction));
        CHECK(history.document().find_entity(wall_b)->name == "North wall");
        CHECK(history.document().find_work(work) != nullptr);
        CHECK(history.document().settings().centre_line_m == 1.45);
        CHECK(history.undo_count() == 1);
        (void)history.undo();
        CHECK(history.document().data() == before);
    }
}

TEST_CASE("AC1: a call that breaks its rule throws invalid_argument and leaves the transaction "
          "as it was",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    const Uuid added = ids.next();
    const Uuid work = ids.next();
    Transaction transaction(history.document(), "Rules");
    // Something already in the transaction, which a refused call must keep.
    transaction.rename_entity(wall_b, "North wall");
    transaction.add_entity(added, named("Added"));
    transaction.remove_entity(viewpoint);
    transaction.set_work(work, sample_work("Water Lilies"));
    transaction.remove(test_chart, ComponentKind::framing);
    transaction.set_fixture_type("generic-flood-2000", sample_fixture_type());
    const Seen before = seen(transaction, {added, work});

    SECTION("add_entity with the nil id") {
        CHECK_THROWS_AS(transaction.add_entity(Uuid{}, named("Nil")), std::invalid_argument);
    }
    SECTION("add_entity with the id of an entity of the document") {
        CHECK_THROWS_AS(transaction.add_entity(wall_a, named("Replacement")),
                        std::invalid_argument);
    }
    SECTION("add_entity with the id of an entity the transaction added") {
        CHECK_THROWS_AS(transaction.add_entity(added, named("Again")), std::invalid_argument);
    }
    SECTION("remove_entity of an id that names nothing") {
        CHECK_THROWS_AS(transaction.remove_entity(missing), std::invalid_argument);
    }
    SECTION("remove_entity of the nil id") {
        CHECK_THROWS_AS(transaction.remove_entity(Uuid{}), std::invalid_argument);
    }
    SECTION("remove_entity of an entity the transaction removed") {
        CHECK_THROWS_AS(transaction.remove_entity(viewpoint), std::invalid_argument);
    }
    SECTION("rename_entity of an id that names nothing") {
        CHECK_THROWS_AS(transaction.rename_entity(missing, "Ghost"), std::invalid_argument);
    }
    SECTION("rename_entity of an entity the transaction removed") {
        CHECK_THROWS_AS(transaction.rename_entity(viewpoint, "Ghost"), std::invalid_argument);
    }
    SECTION("set on an id that names nothing, for several kinds of component") {
        CHECK_THROWS_AS(transaction.set(missing, sample_wall()), std::invalid_argument);
        CHECK_THROWS_AS(transaction.set(missing, sample_lux_sensor()), std::invalid_argument);
        CHECK_THROWS_AS(transaction.set(missing, Exhibit{.work = test_chart_work}),
                        std::invalid_argument);
        CHECK_THROWS_AS(transaction.set(missing, OnWall{.wall = wall_a}), std::invalid_argument);
    }
    SECTION("set on an entity the transaction removed") {
        CHECK_THROWS_AS(transaction.set(viewpoint, sample_lux_sensor()), std::invalid_argument);
    }
    SECTION("remove of a component the entity does not have") {
        CHECK_THROWS_AS(transaction.remove(wall_a, ComponentKind::placement),
                        std::invalid_argument);
    }
    SECTION("remove of a component the transaction removed") {
        CHECK_THROWS_AS(transaction.remove(test_chart, ComponentKind::framing),
                        std::invalid_argument);
    }
    SECTION("remove of a component of an id that names nothing") {
        CHECK_THROWS_AS(transaction.remove(missing, ComponentKind::wall), std::invalid_argument);
    }
    SECTION("remove of a component of an entity the transaction removed") {
        CHECK_THROWS_AS(transaction.remove(viewpoint, ComponentKind::viewpoint),
                        std::invalid_argument);
    }
    SECTION("set_work with the nil id") {
        CHECK_THROWS_AS(transaction.set_work(Uuid{}, sample_work("Nil")), std::invalid_argument);
    }
    SECTION("remove_work of an id that names no work") {
        CHECK_THROWS_AS(transaction.remove_work(missing), std::invalid_argument);
        // An entity's id is not a work's.
        CHECK_THROWS_AS(transaction.remove_work(wall_a), std::invalid_argument);
    }
    SECTION("remove_work of a work the transaction removed") {
        transaction.remove_work(work);
        const Seen after_removal = seen(transaction, {added, work});
        CHECK_THROWS_AS(transaction.remove_work(work), std::invalid_argument);
        CHECK(seen(transaction, {added, work}) == after_removal);
        return;
    }
    SECTION("remove_fixture_type of an id the library does not hold") {
        CHECK_THROWS_AS(transaction.remove_fixture_type("generic-spot-801"), std::invalid_argument);
    }
    SECTION("remove_fixture_type of a fixture type the transaction removed") {
        transaction.remove_fixture_type("generic-flood-2000");
        const Seen after_removal = seen(transaction, {added, work});
        CHECK_THROWS_AS(transaction.remove_fixture_type("generic-flood-2000"),
                        std::invalid_argument);
        CHECK(seen(transaction, {added, work}) == after_removal);
        return;
    }

    CHECK(seen(transaction, {added, work}) == before);
}

TEST_CASE("AC1: each call sees the document as the calls before it in the transaction left it",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    const Uuid id = ids.next();
    Transaction transaction(history.document(), "Calls in order");

    SECTION("an entity added earlier can be given components, renamed and removed") {
        transaction.add_entity(id, named("Sensor"));
        CHECK_NOTHROW(transaction.set(id, sample_lux_sensor()));
        CHECK_NOTHROW(transaction.rename_entity(id, "Sensor on wall A"));
        CHECK_NOTHROW(transaction.remove(id, ComponentKind::lux_sensor));
        CHECK_NOTHROW(transaction.remove_entity(id));
        CHECK(transaction.find_entity(id) == nullptr);
    }
    SECTION("the id of an entity removed earlier can be added again") {
        transaction.remove_entity(viewpoint);
        CHECK_NOTHROW(transaction.add_entity(viewpoint, named("Another view")));
        REQUIRE(transaction.find_entity(viewpoint) != nullptr);
        CHECK(transaction.find_entity(viewpoint)->name == "Another view");
        CHECK_FALSE(transaction.find_entity(viewpoint)->viewpoint.has_value());
    }
    SECTION("a component set earlier can be removed") {
        transaction.set(wall_a, sample_lux_sensor());
        CHECK_NOTHROW(transaction.remove(wall_a, ComponentKind::lux_sensor));
    }
    SECTION("a work or fixture type set earlier can be removed") {
        transaction.set_work(id, sample_work("Water Lilies"));
        CHECK_NOTHROW(transaction.remove_work(id));
        transaction.set_fixture_type("generic-flood-2000", sample_fixture_type());
        CHECK_NOTHROW(transaction.remove_fixture_type("generic-flood-2000"));
    }
}

// ---- AC2: change lists -------------------------------------------------------------------------

TEST_CASE("AC2: setting the same component twice in one transaction yields one change",
          "[doc][transaction]") {
    History history(load_skeleton());
    const OnWall first{.wall = wall_a, .distance_m = 2.0, .centre_height_m = 1.5};
    const OnWall second{.wall = wall_a, .distance_m = 2.4, .centre_height_m = 1.5};

    SECTION("a component the entity has: one change") {
        Transaction drag(history.document(), "Move Test chart");
        drag.set(test_chart, first);
        drag.set(test_chart, second);
        ChangeList expected;
        expected.components = {{test_chart, ComponentKind::placement, ChangeKind::changed}};
        CHECK(drag.changes() == expected);
        CHECK(history.commit(std::move(drag)) == expected);
        CHECK(history.document().find_entity(test_chart)->placement == Placement{second});
        CHECK(history.undo_count() == 1);
    }
    SECTION("a component the entity lacked: one addition") {
        Transaction drag(history.document(), "Hang a sensor");
        drag.set(wall_a, sample_lux_sensor());
        LuxSensor moved = sample_lux_sensor();
        moved.position_m.x = 4.0;
        drag.set(wall_a, moved);
        ChangeList expected;
        expected.components = {{wall_a, ComponentKind::lux_sensor, ChangeKind::added}};
        CHECK(history.commit(std::move(drag)) == expected);
        CHECK(history.document().find_entity(wall_a)->lux_sensor == moved);
    }
    SECTION("renaming twice: one change of the entity") {
        Transaction rename(history.document(), "Rename");
        rename.rename_entity(wall_b, "North");
        rename.rename_entity(wall_b, "North wall");
        ChangeList expected;
        expected.entities = {{wall_b, ChangeKind::changed}};
        CHECK(history.commit(std::move(rename)) == expected);
    }
    SECTION("a work, a fixture type and the settings set twice: one change each") {
        Transaction edit(history.document(), "Edit twice");
        Work work = *history.document().find_work(test_chart_work);
        work.title = "First";
        edit.set_work(test_chart_work, work);
        work.title = "Second";
        edit.set_work(test_chart_work, work);
        FixtureType type = *history.document().library().find_fixture_type(spot_type);
        type.flux_lm = 900.0;
        edit.set_fixture_type(spot_type, type);
        type.flux_lm = 1000.0;
        edit.set_fixture_type(spot_type, type);
        ProjectSettings settings = history.document().settings();
        settings.centre_line_m = 1.4;
        edit.set_settings(settings);
        settings.centre_line_m = 1.45;
        edit.set_settings(settings);
        ChangeList expected;
        expected.settings = true;
        expected.fixture_types = {{spot_type, ChangeKind::changed}};
        expected.works = {{test_chart_work, ChangeKind::changed}};
        CHECK(history.commit(std::move(edit)) == expected);
        CHECK(history.document().find_work(test_chart_work)->title == "Second");
        CHECK(history.document().library().find_fixture_type(spot_type)->flux_lm == 1000.0);
        CHECK(history.document().settings().centre_line_m == 1.45);
    }
}

TEST_CASE("AC2: a value set and set back in one transaction is no change, and committing it "
          "records nothing",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    // A redo entry, which an empty commit must leave alone.
    commit_rename_wall_b(history);
    (void)history.undo();
    const DocumentData before = history.document().data();
    Transaction transaction(history.document(), "Nothing in the end");

    SECTION("a component") {
        const Placement original = *before.entities.at(test_chart).placement;
        transaction.set(test_chart, OnWall{.wall = wall_a, .distance_m = 1.0});
        transaction.set(test_chart, original);
    }
    SECTION("a name") {
        transaction.rename_entity(wall_b, "North wall");
        transaction.rename_entity(wall_b, "Wall B");
    }
    SECTION("a work") {
        Work work = before.works.at(test_chart_work);
        work.title = "Changed";
        transaction.set_work(test_chart_work, work);
        transaction.set_work(test_chart_work, before.works.at(test_chart_work));
    }
    SECTION("a fixture type") {
        FixtureType type = before.library.fixture_types.at(spot_type);
        type.flux_lm = 1.0;
        transaction.set_fixture_type(spot_type, type);
        transaction.set_fixture_type(spot_type, before.library.fixture_types.at(spot_type));
    }
    SECTION("the settings") {
        ProjectSettings settings = before.settings;
        settings.display_units = DisplayUnits::inches;
        transaction.set_settings(settings);
        transaction.set_settings(before.settings);
    }
    SECTION("an entity added and removed") {
        const Uuid id = ids.next();
        transaction.add_entity(id, named("Sensor"));
        transaction.set(id, sample_lux_sensor());
        transaction.remove_entity(id);
    }
    SECTION("a component added and removed") {
        transaction.set(wall_a, sample_lux_sensor());
        transaction.remove(wall_a, ComponentKind::lux_sensor);
    }
    SECTION("a component removed and set back") {
        transaction.remove(test_chart, ComponentKind::framing);
        transaction.set(test_chart, *before.entities.at(test_chart).framing);
    }
    SECTION("an entity removed and added back as it was") {
        transaction.remove_entity(viewpoint);
        transaction.add_entity(viewpoint, before.entities.at(viewpoint));
    }
    SECTION("a work added and removed") {
        const Uuid id = ids.next();
        transaction.set_work(id, sample_work("Water Lilies"));
        transaction.remove_work(id);
    }
    SECTION("a fixture type added and removed") {
        transaction.set_fixture_type("generic-flood-2000", sample_fixture_type());
        transaction.remove_fixture_type("generic-flood-2000");
    }

    CHECK(transaction.empty());
    CHECK(transaction.changes() == ChangeList{});
    CHECK(history.commit(std::move(transaction)) == ChangeList{});
    CHECK(history.document().data() == before);
    CHECK(history.undo_count() == 0);
    CHECK(history.redo_count() == 1);
    CHECK(name_or_none(history.redo_name()) == "Rename Wall B");
}

TEST_CASE("AC2: a component removed and set to a new value in one transaction is changed",
          "[doc][transaction]") {
    History history(load_skeleton());
    Transaction transaction(history.document(), "Reframe");
    transaction.remove(test_chart, ComponentKind::framing);
    transaction.set(test_chart, Framing{.style = "box",
                                        .width_m = std::nullopt,
                                        .colour = std::nullopt,
                                        .mat = std::nullopt,
                                        .glazing = Glazing::acrylic});
    ChangeList expected;
    expected.components = {{test_chart, ComponentKind::framing, ChangeKind::changed}};
    CHECK(transaction.changes() == expected);
    CHECK(history.commit(std::move(transaction)) == expected);
}

TEST_CASE("AC2: removing an entity reports the entity and each of its components removed",
          "[doc][transaction]") {
    History history(load_skeleton());
    Transaction transaction(history.document(), "Delete Test chart");
    transaction.remove_entity(test_chart);
    // changes() is before reference checks: the spot still aims at the chart, so this
    // transaction would be refused, but it says what it would change.
    ChangeList expected;
    expected.entities = {{test_chart, ChangeKind::removed}};
    expected.components = {{test_chart, ComponentKind::exhibit, ChangeKind::removed},
                           {test_chart, ComponentKind::framing, ChangeKind::removed},
                           {test_chart, ComponentKind::placement, ChangeKind::removed}};
    CHECK(transaction.changes() == expected);
}

TEST_CASE("AC2: change lists are sorted by id, and an entity's components in ComponentKind order",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    const Uuid first_work = ids.next();
    const Uuid second_work = ids.next();
    REQUIRE(first_work < second_work);

    // Everything is touched in the reverse of the order the list reports it in.
    Transaction transaction(history.document(), "Many things");
    transaction.set_fixture_type("zz-wash", sample_fixture_type());
    transaction.set_fixture_type("aa-flood", sample_fixture_type());
    transaction.set_work(second_work, sample_work("Second"));
    transaction.set_work(first_work, sample_work("First"));
    transaction.rename_entity(viewpoint, "View");
    transaction.rename_entity(wall_a, "South wall");
    Luminaire luminaire = *history.document().find_entity(spot)->luminaire;
    luminaire.dimming = 0.25;
    transaction.set(spot, luminaire);
    transaction.set(test_chart, OnWall{.wall = wall_a, .distance_m = 1.0, .centre_height_m = 1.5});
    transaction.set(test_chart, Framing{.style = "box",
                                        .width_m = std::nullopt,
                                        .colour = std::nullopt,
                                        .mat = std::nullopt,
                                        .glazing = Glazing::acrylic});
    transaction.set(test_chart, Exhibit{.work = first_work});
    Room room = *history.document().find_entity(gallery)->room;
    room.ceiling_height_m = 3.0;
    transaction.set(gallery, room);

    ChangeList expected;
    expected.fixture_types = {{"aa-flood", ChangeKind::added}, {"zz-wash", ChangeKind::added}};
    expected.works = {{first_work, ChangeKind::added}, {second_work, ChangeKind::added}};
    expected.entities = {{wall_a, ChangeKind::changed}, {viewpoint, ChangeKind::changed}};
    expected.components = {{gallery, ComponentKind::room, ChangeKind::changed},
                           {test_chart, ComponentKind::exhibit, ChangeKind::changed},
                           {test_chart, ComponentKind::framing, ChangeKind::changed},
                           {test_chart, ComponentKind::placement, ChangeKind::changed},
                           {spot, ComponentKind::luminaire, ChangeKind::changed}};
    CHECK(transaction.changes() == expected);
    CHECK(history.commit(std::move(transaction)) == expected);
}

// ---- AC4: dangling references
// --------------------------------------------------------------------

TEST_CASE("AC4: check_references accepts the skeleton scene and an empty document",
          "[doc][transaction]") {
    CHECK_NOTHROW(check_references(load_skeleton().data()));
    CHECK_NOTHROW(check_references(DocumentData{}));
}

TEST_CASE("AC4: check_references names the entity and the member of a dangling reference, "
          "taking entities in order of id",
          "[doc][transaction]") {
    DocumentData data = load_skeleton().data();

    SECTION("one problem") {
        data.entities.erase(wall_a);
        try {
            check_references(data);
            FAIL("check_references accepted a placement on a removed wall");
        } catch (const ReferenceError& error) {
            CHECK(error.entity() == test_chart);
            CHECK(error.member() == "placement.wall");
            CHECK_THAT(std::string(error.what()),
                       StartsWith("entity " + test_chart.to_string() + ": placement.wall: "));
            CHECK_THAT(std::string(error.what()), ContainsSubstring(wall_a.to_string()));
        }
    }
    SECTION("two problems: the entity with the smaller id is named") {
        // The chart (...07) hangs on wall A, the spot (...09) on track A.
        data.entities.erase(track_a);
        data.entities.erase(wall_a);
        try {
            check_references(data);
            FAIL("check_references accepted two dangling references");
        } catch (const ReferenceError& error) {
            CHECK(error.entity() == test_chart);
            CHECK(error.member() == "placement.wall");
        }
    }
}

TEST_CASE("AC4: removing wall A while the test chart hangs on it is refused, naming the chart and "
          "placement.wall, and changes nothing",
          "[doc][transaction]") {
    History history(load_skeleton());
    // A redo entry, so that the test also sees the refusal leave the redo stack alone.
    commit_rename_wall_b(history);
    (void)history.undo();
    const DocumentData before = history.document().data();
    const std::string before_json = to_project_json(history.document());

    Transaction transaction(history.document(), "Delete Wall A");
    SECTION("the whole entity") {
        transaction.remove_entity(wall_a);
    }
    SECTION("only its wall component") {
        transaction.remove(wall_a, ComponentKind::wall);
    }

    const std::optional<ReferenceError> error = refusal(history, std::move(transaction));
    REQUIRE(error.has_value());
    CHECK(error->entity() == test_chart);
    CHECK(error->member() == "placement.wall");
    const std::string what = error->what();
    CHECK_THAT(what, ContainsSubstring(test_chart.to_string()));
    CHECK_THAT(what, ContainsSubstring(wall_a.to_string()));
    CHECK_THAT(what, ContainsSubstring("placement.wall"));

    CHECK(history.document().data() == before);
    CHECK(to_project_json(history.document()) == before_json);
    CHECK(history.undo_count() == 0);
    CHECK(history.redo_count() == 1);
    CHECK(name_or_none(history.redo_name()) == "Rename Wall B");

    // The history still works: a new transaction commits, and clears the redo stack.
    Transaction rename(history.document(), "Rename Wall C");
    rename.rename_entity(wall_c, "East wall");
    CHECK_FALSE(history.commit(std::move(rename)).empty());
    CHECK(history.undo_count() == 1);
    CHECK(history.redo_count() == 0);
}

TEST_CASE("AC4: removing wall A with what hangs on it, in one transaction, commits",
          "[doc][transaction]") {
    // In the skeleton, wall A is referenced only by the chart's placement, and the chart only by
    // the spot's aim (see the project.json).
    History history(load_skeleton());
    const std::string before_json = to_project_json(history.document());
    DocumentData expected = history.document().data();
    Transaction transaction(history.document(), "Delete Wall A");
    ChangeList expected_changes;

    SECTION("the wall, then the chart's placement: the work comes off the wall") {
        transaction.remove_entity(wall_a);
        transaction.remove(test_chart, ComponentKind::placement);
        expected.entities.erase(wall_a);
        expected.entities[test_chart].placement.reset();
        expected_changes.entities = {{wall_a, ChangeKind::removed}};
        expected_changes.components = {{wall_a, ComponentKind::wall, ChangeKind::removed},
                                       {test_chart, ComponentKind::placement, ChangeKind::removed}};
    }
    SECTION("the chart, its work and the spot aimed at it, then the wall") {
        // References are checked at the commit, so the order within the transaction is free.
        transaction.remove_entity(test_chart);
        transaction.remove_work(test_chart_work);
        transaction.remove_entity(spot);
        transaction.remove_entity(wall_a);
        expected.entities.erase(test_chart);
        expected.entities.erase(spot);
        expected.entities.erase(wall_a);
        expected.works.erase(test_chart_work);
        expected_changes.works = {{test_chart_work, ChangeKind::removed}};
        expected_changes.entities = {{wall_a, ChangeKind::removed},
                                     {test_chart, ChangeKind::removed},
                                     {spot, ChangeKind::removed}};
        expected_changes.components = {{wall_a, ComponentKind::wall, ChangeKind::removed},
                                       {test_chart, ComponentKind::exhibit, ChangeKind::removed},
                                       {test_chart, ComponentKind::framing, ChangeKind::removed},
                                       {test_chart, ComponentKind::placement, ChangeKind::removed},
                                       {spot, ComponentKind::placement, ChangeKind::removed},
                                       {spot, ComponentKind::luminaire, ChangeKind::removed}};
    }
    SECTION("the wall and the chart, with the spot re-aimed at a point") {
        transaction.remove_entity(wall_a);
        transaction.remove_entity(test_chart);
        Luminaire luminaire = *expected.entities[spot].luminaire;
        luminaire.aim = AimAtPoint{{3.0, 0.0, 1.5}};
        transaction.set(spot, luminaire);
        expected.entities.erase(wall_a);
        expected.entities.erase(test_chart);
        expected.entities[spot].luminaire = luminaire;
        expected_changes.entities = {{wall_a, ChangeKind::removed},
                                     {test_chart, ChangeKind::removed}};
        expected_changes.components = {{wall_a, ComponentKind::wall, ChangeKind::removed},
                                       {test_chart, ComponentKind::exhibit, ChangeKind::removed},
                                       {test_chart, ComponentKind::framing, ChangeKind::removed},
                                       {test_chart, ComponentKind::placement, ChangeKind::removed},
                                       {spot, ComponentKind::luminaire, ChangeKind::changed}};
    }

    const ChangeList changes = history.commit(std::move(transaction));
    CHECK(changes == expected_changes);
    CHECK(history.document().data() == expected);
    CHECK_NOTHROW(check_references(history.document().data()));
    REQUIRE(history.undo_count() == 1);
    (void)history.undo();
    CHECK(to_project_json(history.document()) == before_json);
}

TEST_CASE("AC4: a commit that leaves any kind of dangling reference is refused, naming the entity "
          "and the member",
          "[doc][transaction]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids();
    const DocumentData before = history.document().data();
    Transaction transaction(history.document(), "Dangling");
    Uuid expected_entity;
    std::string expected_member;

    SECTION("the wall and the chart, while the spot still aims at the chart") {
        transaction.remove_entity(wall_a);
        transaction.remove_entity(test_chart);
        expected_entity = spot;
        expected_member = "luminaire.aim.target";
    }
    SECTION("a work an exhibit shows") {
        transaction.remove_work(test_chart_work);
        expected_entity = test_chart;
        expected_member = "exhibit.work";
    }
    SECTION("an exhibit of a work the document does not hold") {
        transaction.set(test_chart, Exhibit{.work = missing});
        expected_entity = test_chart;
        expected_member = "exhibit.work";
    }
    SECTION("a fixture type a luminaire uses") {
        transaction.remove_fixture_type(spot_type);
        expected_entity = spot;
        expected_member = "luminaire.fixtureType";
    }
    SECTION("the track a luminaire is on") {
        transaction.remove_entity(track_a);
        expected_entity = spot;
        expected_member = "placement.track";
    }
    SECTION("the track component of the entity a luminaire is on") {
        transaction.remove(track_a, ComponentKind::track);
        expected_entity = spot;
        expected_member = "placement.track";
    }
    SECTION("a placement on a wall that is a room") {
        transaction.set(test_chart, OnWall{.wall = gallery, .distance_m = 1.0});
        expected_entity = test_chart;
        expected_member = "placement.wall";
    }
    SECTION("a placement on the floor of a room that is a wall") {
        transaction.set(test_chart, OnFloor{.room = wall_b, .position_m = {1.0, 1.0}});
        expected_entity = test_chart;
        expected_member = "placement.room";
    }
    SECTION("a placement in the ceiling of a missing room") {
        transaction.set(spot, OnCeiling{.room = missing, .position_m = {1.0, 1.0}});
        expected_entity = spot;
        expected_member = "placement.room";
    }
    SECTION("an entity hosted on itself") {
        transaction.set(test_chart, OnHost{.host = test_chart, .offset_m = {0.0, 0.0, 0.1}});
        expected_entity = test_chart;
        expected_member = "placement.host";
    }
    SECTION("an opening in a missing wall") {
        transaction.set(viewpoint, Opening{.wall = missing,
                                           .offset_m = 1.0,
                                           .width_m = 0.9,
                                           .height_m = 2.1,
                                           .kind = OpeningKind::door});
        expected_entity = viewpoint;
        expected_member = "opening.wall";
    }
    SECTION("a dimension from a missing entity") {
        transaction.set(viewpoint,
                        Dimension{.from = {.entity = missing, .point_m = {0.0, 0.0, 0.0}},
                                  .to = {.entity = std::nullopt, .point_m = {1.0, 0.0, 0.0}}});
        expected_entity = viewpoint;
        expected_member = "dimension.from.entity";
    }
    SECTION("a new entity placed on a missing wall") {
        const Uuid id = ids.next();
        Entity label = named("Label");
        label.placement = OnWall{.wall = missing};
        transaction.add_entity(id, label);
        expected_entity = id;
        expected_member = "placement.wall";
    }

    const std::optional<ReferenceError> error = refusal(history, std::move(transaction));
    REQUIRE(error.has_value());
    CHECK(error->entity() == expected_entity);
    CHECK(error->member() == expected_member);
    CHECK_THAT(std::string(error->what()),
               StartsWith("entity " + expected_entity.to_string() + ": " + expected_member + ": "));
    CHECK(history.document().data() == before);
    CHECK(history.undo_count() == 0);
}

// ---- AC7: a transaction is made for one revision of one document --------------------------------

TEST_CASE("AC7: each commit, undo and redo adds one to the document's revision",
          "[doc][transaction]") {
    History history(load_skeleton());
    const std::uint64_t start = history.document().revision();
    commit_rename_wall_b(history);
    CHECK(history.document().revision() == start + 1);
    (void)history.undo();
    CHECK(history.document().revision() == start + 2);
    (void)history.redo();
    CHECK(history.document().revision() == start + 3);
    CHECK(Document{}.revision() == 0);
}

TEST_CASE("AC7: a transaction made for an older revision is refused with invalid_argument",
          "[doc][transaction]") {
    History history(load_skeleton());
    Transaction stale(history.document(), "Rename Wall C");
    stale.rename_entity(wall_c, "East wall");

    SECTION("after another commit") {
        commit_rename_wall_b(history);
    }
    SECTION("after an undo") {
        commit_rename_wall_b(history);
        (void)history.undo();
        // The data is back to what the transaction was made for, but the revision is not.
        REQUIRE(history.document().data() == load_skeleton().data());
    }
    SECTION("after an undo and a redo") {
        commit_rename_wall_b(history);
        (void)history.undo();
        (void)history.redo();
    }

    const DocumentData before = history.document().data();
    const std::size_t undo_count = history.undo_count();
    const std::size_t redo_count = history.redo_count();
    CHECK_THROWS_AS(history.commit(std::move(stale)), std::invalid_argument);
    CHECK(history.document().data() == before);
    CHECK(history.document().find_entity(wall_c)->name == "Wall C");
    CHECK(history.undo_count() == undo_count);
    CHECK(history.redo_count() == redo_count);
}

TEST_CASE("AC7: a transaction made for another document is refused with invalid_argument",
          "[doc][transaction]") {
    History history(load_skeleton());
    const DocumentData before = history.document().data();

    SECTION("another history's document, equal in data and revision") {
        History other(load_skeleton());
        REQUIRE(other.document() == history.document());
        REQUIRE(other.document().revision() == history.document().revision());
        Transaction transaction(other.document(), "Rename Wall C");
        transaction.rename_entity(wall_c, "East wall");
        CHECK_THROWS_AS(history.commit(std::move(transaction)), std::invalid_argument);
        CHECK(other.document().find_entity(wall_c)->name == "Wall C");
    }
    SECTION("a document no history holds") {
        const Document loose = load_skeleton();
        Transaction transaction(loose, "Rename Wall C");
        transaction.rename_entity(wall_c, "East wall");
        CHECK_THROWS_AS(history.commit(std::move(transaction)), std::invalid_argument);
    }

    CHECK(history.document().data() == before);
    CHECK(history.undo_count() == 0);
}
