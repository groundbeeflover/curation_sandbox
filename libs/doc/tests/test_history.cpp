// SPDX-License-Identifier: Apache-2.0

// The history (history.hpp, M1-09): committing, undoing and redoing transactions, the names the
// interface shows for them, and a property test of 1000 random transactions on the skeleton
// scene that are all undone and redone. How a single transaction is built and checked is in
// test_transaction.cpp.
//
// The skeleton's ids are fixed: the work ...01 (the test chart), the room ...02, walls A to D
// ...03 to ...06, the chart's entity ...07 (hung OnWall on wall A), track A ...08, the spot ...09
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

#include "temp_folder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cs::doc;
using cs::doc::testing::TempFolder;

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

namespace Catch {

// Readable failure messages for change lists.
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

} // namespace Catch

namespace {

// ---- The skeleton scene ----------------------------------------------------------------------

// The skeleton's ids are 01a1cd0d-dc00-7000-8000-0000000000<n>.
constexpr Uuid skeleton_id(std::uint8_t n) {
    return Uuid(Uuid::Bytes{0x01, 0xa1, 0xcd, 0x0d, 0xdc, 0x00, 0x70, 0x00, 0x80, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, n});
}

constexpr Uuid test_chart_work = skeleton_id(0x01);
constexpr Uuid wall_a = skeleton_id(0x03);
constexpr Uuid wall_b = skeleton_id(0x04);
constexpr Uuid wall_c = skeleton_id(0x05);
constexpr Uuid test_chart = skeleton_id(0x07);
constexpr Uuid viewpoint = skeleton_id(0x0a);

const LibraryId spot_type = "generic-spot-800";

std::filesystem::path scene_folder() {
    return std::filesystem::path(CS_SOURCE_DIR "/tests/scenes/skeleton");
}

Document load_skeleton() {
    return load_project_folder(scene_folder());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Empty when the texts are equal; otherwise where they first differ, for the failure message.
std::string first_difference(std::string_view actual, std::string_view expected) {
    const auto [a, e] =
        std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
    if (a == actual.end() && e == expected.end()) {
        return {};
    }
    const auto at = static_cast<std::size_t>(a - actual.begin());
    const auto line = std::count(actual.begin(), a, '\n') + 1;
    return "first difference at byte " + std::to_string(at) + ", line " + std::to_string(line) +
           ": actual \"" + std::string(actual.substr(at, 60)) + "\", expected \"" +
           std::string(expected.substr(at, 60)) + "\"";
}

// New ids, the same in every run: a fixed seed and a clock stopped at one millisecond.
IdGenerator fixed_ids(std::uint64_t seed) {
    return IdGenerator(cs::hash_seed(seed, 0, 0, 0, 1),
                       [] { return std::int64_t{1'800'000'000'000}; });
}

std::string name_or_none(std::optional<std::string_view> name) {
    return name ? std::string(*name) : std::string("(none)");
}

ChangeKind inverse(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::added:
        return ChangeKind::removed;
    case ChangeKind::removed:
        return ChangeKind::added;
    case ChangeKind::changed:
        return ChangeKind::changed;
    }
    return kind;
}

// The change list of applying a change list's changes backwards: what was added is removed and
// what was removed is added. The ids, and so the order, are the same.
ChangeList inverse(ChangeList changes) {
    for (auto& change : changes.fixture_types) {
        change.kind = inverse(change.kind);
    }
    for (auto& change : changes.works) {
        change.kind = inverse(change.kind);
    }
    for (auto& change : changes.entities) {
        change.kind = inverse(change.kind);
    }
    for (auto& change : changes.components) {
        change.kind = inverse(change.kind);
    }
    return changes;
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

// An entity with a name and no components.
Entity named(std::string name) {
    Entity entity;
    entity.name = std::move(name);
    return entity;
}

ChangeList commit_rename(History& history, const Uuid& entity, std::string name,
                         std::string transaction_name) {
    Transaction rename(history.document(), std::move(transaction_name));
    rename.rename_entity(entity, std::move(name));
    return history.commit(std::move(rename));
}

// ---- Random transactions ---------------------------------------------------------------------

// Builds random transactions from a fixed seed. Each call it makes keeps the transaction's rules,
// so none throws invalid_argument, and gives values within project_io's ranges, so the document
// still saves. Some calls leave a dangling reference: on purpose (a placement on an entity that
// does not exist, an exhibit of a missing work, a host that may close a cycle), or as removals
// do (a wall that holds a placement, a work an exhibit shows). Their commits are refused.
class RandomEditor {
  public:
    explicit RandomEditor(std::uint64_t seed)
        : rng_(cs::hash_seed(seed, 0, 0, 0, 0)), ids_(fixed_ids(seed)) {}

    // Adds one to three random calls to the transaction.
    void edit(Transaction& transaction) {
        added_entities_.clear();
        added_works_.clear();
        added_types_.clear();
        const std::uint32_t calls = 1 + below(3);
        for (std::uint32_t call = 0; call < calls; ++call) {
            one_call(transaction);
        }
    }

  private:
    cs::Rng rng_;
    IdGenerator ids_;
    int counter_ = 0;
    // Ids the current transaction gave things the document does not hold.
    std::vector<Uuid> added_entities_;
    std::vector<Uuid> added_works_;
    std::vector<LibraryId> added_types_;

    // ---- Random numbers ----

    std::uint32_t below(std::uint32_t n) { return rng_.next_u32() % n; }
    bool chance(double p) { return rng_.next_double() < p; }
    // Uniform in [low, high], rounded to a thousandth (a millimetre for lengths), so that the
    // project.json is readable when a test fails.
    double between(double low, double high) {
        return std::round((low + (high - low) * rng_.next_double()) * 1000.0) / 1000.0;
    }
    template <class T> const T& pick(const std::vector<T>& items) {
        return items[below(static_cast<std::uint32_t>(items.size()))];
    }
    std::string next_name(std::string_view prefix) {
        return std::string(prefix) + " " + std::to_string(++counter_);
    }

    // ---- What the transaction holds now ----

    // Entities as the transaction leaves them, with a component of the kind when one is given.
    std::vector<Uuid> entities(const Transaction& transaction,
                               std::optional<ComponentKind> with = std::nullopt) const {
        std::vector<Uuid> result;
        const auto consider = [&](const Uuid& id) {
            const Entity* entity = transaction.find_entity(id);
            if (entity != nullptr && (!with || entity->has(*with))) {
                result.push_back(id);
            }
        };
        for (const auto& entry : transaction.document().entities()) {
            consider(entry.first);
        }
        for (const Uuid& id : added_entities_) {
            consider(id);
        }
        return result;
    }

    std::vector<Uuid> works(const Transaction& transaction) const {
        std::vector<Uuid> result;
        for (const auto& entry : transaction.document().works()) {
            if (transaction.find_work(entry.first) != nullptr) {
                result.push_back(entry.first);
            }
        }
        for (const Uuid& id : added_works_) {
            if (transaction.find_work(id) != nullptr) {
                result.push_back(id);
            }
        }
        return result;
    }

    std::vector<LibraryId> fixture_types(const Transaction& transaction) const {
        std::vector<LibraryId> result;
        for (const auto& entry : transaction.document().library().fixture_types) {
            if (transaction.find_fixture_type(entry.first) != nullptr) {
                result.push_back(entry.first);
            }
        }
        for (const LibraryId& id : added_types_) {
            if (transaction.find_fixture_type(id) != nullptr) {
                result.push_back(id);
            }
        }
        return result;
    }

    // ---- Random values, within project_io's ranges ----

    Vec2 plan_point() { return {between(-2.0, 8.0), between(-2.0, 6.0)}; }
    Vec3 point() { return {between(-2.0, 8.0), between(-2.0, 6.0), between(0.0, 4.0)}; }

    Wall wall() {
        // At least 2 m long, so its ends never round to the same point.
        const Vec2 start = plan_point();
        const double angle = between(0.0, 2.0 * std::numbers::pi);
        const double length = between(2.0, 5.0);
        return {.start_m = start,
                .end_m = {start.x + std::round(length * std::cos(angle) * 1000.0) / 1000.0,
                          start.y + std::round(length * std::sin(angle) * 1000.0) / 1000.0},
                .height_m = between(2.2, 4.0),
                .thickness_m = between(0.05, 0.3),
                .finish_a = chance(0.3) ? std::optional<LibraryId>("wall-white") : std::nullopt,
                .finish_b = std::nullopt,
                .label = next_name("W")};
    }

    Room room() {
        const Vec2 corner = plan_point();
        const double width = between(1.0, 8.0);
        const double depth = between(1.0, 8.0);
        return {.outline_m = {corner,
                              {corner.x + width, corner.y},
                              {corner.x + width, corner.y + depth},
                              {corner.x, corner.y + depth}},
                .floor_height_m = 0.0,
                .ceiling_height_m = between(2.5, 5.0),
                .floor_finish = std::nullopt,
                .ceiling_finish = std::nullopt};
    }

    Track track() {
        Track result;
        result.height_m = between(2.5, 4.0);
        Vec2 at = plan_point();
        const std::uint32_t points = 2 + below(3);
        for (std::uint32_t i = 0; i < points; ++i) {
            result.points_m.push_back(at);
            at.x += between(0.5, 2.0); // consecutive points never coincide
            at.y = between(-2.0, 6.0);
        }
        return result;
    }

    Opening opening(const Uuid& wall) {
        return {.wall = wall,
                .offset_m = between(0.0, 0.5),
                .width_m = between(0.6, 1.0),
                .height_m = between(1.8, 2.1),
                .sill_m = 0.0,
                .kind = chance(0.5) ? OpeningKind::door : OpeningKind::passage};
    }

    Framing framing() {
        static constexpr Glazing glazings[] = {Glazing::none,
                                               Glazing::float_glass,
                                               Glazing::low_iron_glass,
                                               Glazing::acrylic,
                                               Glazing::anti_reflective_glass,
                                               Glazing::anti_reflective_acrylic,
                                               Glazing::non_glare_glass};
        return {.style = "box",
                .width_m = chance(0.5) ? std::optional<double>(between(0.02, 0.1)) : std::nullopt,
                .colour = std::nullopt,
                .mat = std::nullopt,
                .glazing = glazings[below(7)]};
    }

    OnWall on_wall(const Uuid& wall) {
        return {.wall = wall,
                .face = chance(0.5) ? WallFace::a : WallFace::b,
                .distance_m = between(0.5, 4.0),
                .centre_height_m = between(1.0, 2.0),
                .stand_off_m = between(0.0, 0.1)};
    }

    LuxSensor lux_sensor() {
        // z of the normal is 1, so it is never zero.
        return {.position_m = point(), .normal = {between(-1.0, 1.0), between(-1.0, 1.0), 1.0}};
    }

    Viewpoint viewpoint() {
        return {.position_m = point(),
                .yaw_rad = between(-3.0, 3.0),
                .pitch_rad = between(-1.2, 1.2),
                .field_of_view_rad = between(0.3, 2.0),
                .ev100 = chance(0.5) ? std::optional<double>(between(4.0, 14.0)) : std::nullopt};
    }

    Work work() {
        static constexpr Responsivity responsivities[] = {Responsivity::irresponsive,
                                                          Responsivity::low, Responsivity::medium,
                                                          Responsivity::high};
        Work result;
        result.number = std::to_string(++counter_);
        result.title = next_name("Work");
        result.artist = "Anonymous";
        result.medium = "Oil on canvas";
        result.form.width_m = between(0.1, 2.0);
        result.form.height_m = between(0.1, 2.0);
        result.material.preset = "oil-canvas-gloss";
        result.sensitivity.responsivity = responsivities[below(4)];
        if (chance(0.5)) {
            result.form.depth_m = between(0.01, 0.05);
        }
        if (chance(0.3)) {
            result.lender = "A lender";
        }
        if (chance(0.3)) {
            result.material.base_roughness = between(0.0, 1.0);
        }
        return result;
    }

    FixtureType fixture_type() {
        const double beam = between(0.1, 0.6);
        return {.name = next_name("Spot"),
                .flux_lm = between(100.0, 3000.0),
                .beam_angle_rad = beam,
                .field_angle_rad = beam + between(0.0, 0.4),
                .colour_temperature_k = between(2700.0, 4000.0)};
    }

    Aim aim(const Transaction& transaction) {
        const std::vector<Uuid> targets = entities(transaction);
        if (!targets.empty() && chance(0.5)) {
            return AimAtEntity{pick(targets)};
        }
        return AimAtPoint{point()};
    }

    // ---- Calls ----

    void one_call(Transaction& transaction) {
        const std::uint32_t roll = below(100);
        if (roll < 18) {
            add_entity(transaction);
        } else if (roll < 26) {
            remove_entity(transaction);
        } else if (roll < 34) {
            rename_entity(transaction);
        } else if (roll < 56) {
            change_component(transaction);
        } else if (roll < 62) {
            remove_component(transaction);
        } else if (roll < 68) {
            set_settings(transaction);
        } else if (roll < 76) {
            set_work(transaction);
        } else if (roll < 80) {
            remove_work(transaction);
        } else if (roll < 85) {
            set_fixture_type(transaction);
        } else if (roll < 88) {
            remove_fixture_type(transaction);
        } else if (roll < 94) {
            dangle(transaction);
        } else {
            replace(transaction);
        }
    }

    void set_settings(Transaction& transaction) {
        transaction.set_settings(
            {.centre_line_m = between(1.2, 1.7),
             .eye_height_m = between(1.4, 1.8),
             .opening_hours_per_year = std::round(between(0.0, 8784.0)),
             .display_units = chance(0.5) ? DisplayUnits::centimetres : DisplayUnits::inches});
    }

    Uuid new_work(Transaction& transaction) {
        const Uuid id = ids_.next();
        transaction.set_work(id, work());
        added_works_.push_back(id);
        return id;
    }

    LibraryId new_fixture_type(Transaction& transaction) {
        LibraryId id = "spot-" + std::to_string(++counter_);
        transaction.set_fixture_type(id, fixture_type());
        added_types_.push_back(id);
        return id;
    }

    void add_entity(Transaction& transaction) {
        const Uuid id = ids_.next();
        Entity entity = named(next_name("Thing"));
        const std::vector<Uuid> walls = entities(transaction, ComponentKind::wall);
        const std::vector<Uuid> rooms = entities(transaction, ComponentKind::room);
        const std::vector<Uuid> tracks = entities(transaction, ComponentKind::track);
        switch (below(10)) {
        case 0:
            entity.wall = wall();
            break;
        case 1:
            entity.room = room();
            break;
        case 2:
            if (walls.empty()) {
                entity.wall = wall();
            } else {
                entity.opening = opening(pick(walls));
            }
            break;
        case 3: {
            std::vector<Uuid> available = works(transaction);
            const Uuid shown = available.empty() ? new_work(transaction) : pick(available);
            entity.exhibit = Exhibit{shown};
            entity.framing = framing();
            if (!walls.empty()) {
                entity.placement = on_wall(pick(walls));
            }
            break;
        }
        case 4:
            entity.track = track();
            break;
        case 5: {
            std::vector<LibraryId> types = fixture_types(transaction);
            const LibraryId type = types.empty() ? new_fixture_type(transaction) : pick(types);
            entity.luminaire = Luminaire{
                .fixture_type = type,
                .dimming = between(0.0, 1.0),
                .aim = aim(transaction),
                .colour_temperature_k =
                    chance(0.3) ? std::optional<double>(between(2700.0, 4000.0)) : std::nullopt};
            if (!tracks.empty()) {
                entity.placement = OnTrack{.track = pick(tracks), .distance_m = between(0.0, 0.5)};
            } else if (!rooms.empty()) {
                entity.placement = OnCeiling{.room = pick(rooms), .position_m = plan_point()};
            }
            break;
        }
        case 6:
            entity.lux_sensor = lux_sensor();
            break;
        case 7:
            entity.viewpoint = viewpoint();
            break;
        case 8: {
            const std::vector<Uuid> anchors = entities(transaction);
            Dimension dimension;
            dimension.from.point_m = point();
            dimension.to.point_m = point();
            if (!anchors.empty() && chance(0.7)) {
                dimension.from.entity = pick(anchors);
            }
            entity.dimension = dimension;
            break;
        }
        default: {
            // A plinth: on a room's floor, or on another entity (a new entity closes no cycle).
            const std::vector<Uuid> hosts = entities(transaction);
            if (!rooms.empty() && chance(0.5)) {
                entity.placement = OnFloor{.room = pick(rooms),
                                           .position_m = plan_point(),
                                           .rotation_rad = between(-3.0, 3.0)};
            } else if (!hosts.empty()) {
                entity.placement =
                    OnHost{.host = pick(hosts),
                           .offset_m = {between(-0.5, 0.5), between(-0.5, 0.5), between(0.0, 1.0)},
                           .rotation_rad = between(-3.0, 3.0)};
            }
            break;
        }
        }
        transaction.add_entity(id, std::move(entity));
        added_entities_.push_back(id);
    }

    void remove_entity(Transaction& transaction) {
        const std::vector<Uuid> candidates = entities(transaction);
        if (candidates.empty()) {
            add_entity(transaction);
            return;
        }
        transaction.remove_entity(pick(candidates));
    }

    void rename_entity(Transaction& transaction) {
        const std::vector<Uuid> candidates = entities(transaction);
        if (candidates.empty()) {
            add_entity(transaction);
            return;
        }
        transaction.rename_entity(pick(candidates), next_name("Renamed"));
    }

    // A random entity with a component, and one of its components; nullopt when none has one.
    std::optional<std::pair<Uuid, ComponentKind>>
    component_of_some_entity(const Transaction& transaction) {
        std::vector<Uuid> candidates;
        for (const Uuid& id : entities(transaction)) {
            const Entity& entity = *transaction.find_entity(id);
            if (std::any_of(all_component_kinds.begin(), all_component_kinds.end(),
                            [&](ComponentKind kind) { return entity.has(kind); })) {
                candidates.push_back(id);
            }
        }
        if (candidates.empty()) {
            return std::nullopt;
        }
        const Uuid id = pick(candidates);
        std::vector<ComponentKind> kinds;
        for (const ComponentKind kind : all_component_kinds) {
            if (transaction.find_entity(id)->has(kind)) {
                kinds.push_back(kind);
            }
        }
        return std::pair{id, pick(kinds)};
    }

    void change_component(Transaction& transaction) {
        const auto chosen = component_of_some_entity(transaction);
        if (!chosen) {
            add_entity(transaction);
            return;
        }
        const auto [id, kind] = *chosen;
        const Entity entity = *transaction.find_entity(id);
        switch (kind) {
        case ComponentKind::wall: {
            Wall value = *entity.wall;
            value.height_m = between(2.2, 4.0);
            transaction.set(id, value);
            break;
        }
        case ComponentKind::opening: {
            Opening value = *entity.opening;
            value.offset_m = between(0.0, 0.5);
            transaction.set(id, value);
            break;
        }
        case ComponentKind::room: {
            Room value = *entity.room;
            value.ceiling_height_m = value.floor_height_m + between(2.5, 5.0);
            transaction.set(id, value);
            break;
        }
        case ComponentKind::exhibit: {
            const std::vector<Uuid> available = works(transaction);
            if (available.empty()) {
                set_settings(transaction);
                return;
            }
            transaction.set(id, Exhibit{pick(available)});
            break;
        }
        case ComponentKind::framing:
            transaction.set(id, framing());
            break;
        case ComponentKind::placement: {
            Placement value = *entity.placement;
            if (auto* on = std::get_if<OnWall>(&value)) {
                on->distance_m = between(0.5, 4.0);
                on->centre_height_m = between(1.0, 2.0);
            } else if (auto* on_floor = std::get_if<OnFloor>(&value)) {
                on_floor->position_m = plan_point();
            } else if (auto* on_host = std::get_if<OnHost>(&value)) {
                on_host->rotation_rad = between(-3.0, 3.0);
            } else if (auto* on_track = std::get_if<OnTrack>(&value)) {
                on_track->distance_m = between(0.0, 0.5);
            } else if (auto* on_ceiling = std::get_if<OnCeiling>(&value)) {
                on_ceiling->position_m = plan_point();
            }
            transaction.set(id, value);
            break;
        }
        case ComponentKind::track: {
            Track value = *entity.track;
            value.height_m = between(2.5, 4.0);
            transaction.set(id, value);
            break;
        }
        case ComponentKind::luminaire: {
            Luminaire value = *entity.luminaire;
            value.dimming = between(0.0, 1.0);
            transaction.set(id, value);
            break;
        }
        case ComponentKind::lux_sensor:
            transaction.set(id, lux_sensor());
            break;
        case ComponentKind::viewpoint:
            transaction.set(id, viewpoint());
            break;
        case ComponentKind::dimension: {
            Dimension value = *entity.dimension;
            value.to.point_m = point();
            transaction.set(id, value);
            break;
        }
        }
    }

    void remove_component(Transaction& transaction) {
        const auto chosen = component_of_some_entity(transaction);
        if (!chosen) {
            add_entity(transaction);
            return;
        }
        transaction.remove(chosen->first, chosen->second);
    }

    void set_work(Transaction& transaction) {
        const std::vector<Uuid> available = works(transaction);
        if (available.empty() || chance(0.5)) {
            new_work(transaction);
            return;
        }
        const Uuid id = pick(available);
        Work value = *transaction.find_work(id);
        value.title = next_name("Retitled");
        value.sensitivity.max_lux =
            chance(0.5) ? std::optional<double>(between(0.0, 200.0)) : std::nullopt;
        transaction.set_work(id, value);
    }

    void remove_work(Transaction& transaction) {
        const std::vector<Uuid> available = works(transaction);
        if (available.empty()) {
            new_work(transaction);
            return;
        }
        transaction.remove_work(pick(available));
    }

    void set_fixture_type(Transaction& transaction) {
        const std::vector<LibraryId> available = fixture_types(transaction);
        if (available.empty() || chance(0.5)) {
            new_fixture_type(transaction);
            return;
        }
        const LibraryId& id = pick(available);
        FixtureType value = *transaction.find_fixture_type(id);
        value.flux_lm = between(100.0, 3000.0);
        transaction.set_fixture_type(id, value);
    }

    void remove_fixture_type(Transaction& transaction) {
        const std::vector<LibraryId> available = fixture_types(transaction);
        if (available.empty()) {
            new_fixture_type(transaction);
            return;
        }
        transaction.remove_fixture_type(pick(available));
    }

    // A reference that is dangling (to an id never used) or may be (a wall that may not be one,
    // a host that may close a cycle).
    void dangle(Transaction& transaction) {
        const std::vector<Uuid> candidates = entities(transaction);
        if (candidates.empty()) {
            add_entity(transaction);
            return;
        }
        const Uuid id = pick(candidates);
        switch (below(4)) {
        case 0:
            transaction.set(id, on_wall(ids_.next()));
            break;
        case 1:
            transaction.set(id, Exhibit{ids_.next()});
            break;
        case 2:
            transaction.set(id, OnHost{.host = pick(candidates),
                                       .offset_m = {0.0, 0.0, between(0.0, 1.0)},
                                       .rotation_rad = 0.0});
            break;
        default:
            transaction.set(id, on_wall(pick(candidates)));
            break;
        }
    }

    // A new placement, by a valid reference.
    void replace(Transaction& transaction) {
        const std::vector<Uuid> candidates = entities(transaction);
        const std::vector<Uuid> walls = entities(transaction, ComponentKind::wall);
        const std::vector<Uuid> rooms = entities(transaction, ComponentKind::room);
        const std::vector<Uuid> tracks = entities(transaction, ComponentKind::track);
        if (candidates.empty()) {
            add_entity(transaction);
            return;
        }
        const Uuid id = pick(candidates);
        switch (below(4)) {
        case 0:
            if (!walls.empty()) {
                transaction.set(id, on_wall(pick(walls)));
                return;
            }
            break;
        case 1:
            if (!rooms.empty()) {
                transaction.set(id, OnFloor{.room = pick(rooms),
                                            .position_m = plan_point(),
                                            .rotation_rad = between(-3.0, 3.0)});
                return;
            }
            break;
        case 2:
            if (!tracks.empty()) {
                transaction.set(id,
                                OnTrack{.track = pick(tracks), .distance_m = between(0.0, 0.5)});
                return;
            }
            break;
        default:
            if (!rooms.empty()) {
                transaction.set(id, OnCeiling{.room = pick(rooms), .position_m = plan_point()});
                return;
            }
            break;
        }
        transaction.rename_entity(id, next_name("Unmoved"));
    }
};

} // namespace

// ---- The stacks --------------------------------------------------------------------------------

TEST_CASE("a new History holds an empty project with nothing to undo or redo", "[doc][history]") {
    const History history{};
    CHECK(history.document() == Document{});
    CHECK(history.document().data() == DocumentData{});
    CHECK_FALSE(history.can_undo());
    CHECK_FALSE(history.can_redo());
    CHECK(history.undo_count() == 0);
    CHECK(history.redo_count() == 0);
    CHECK_FALSE(history.undo_name().has_value());
    CHECK_FALSE(history.redo_name().has_value());
}

TEST_CASE("a History made from a document holds it, with nothing to undo or redo",
          "[doc][history]") {
    const DocumentData skeleton = load_skeleton().data();
    const History history(load_skeleton());
    CHECK(history.document().data() == skeleton);
    CHECK_FALSE(history.can_undo());
    CHECK_FALSE(history.can_redo());
    CHECK_FALSE(history.undo_name().has_value());
    CHECK_FALSE(history.redo_name().has_value());
}

TEST_CASE("undo and redo throw logic_error when there is nothing to undo or redo",
          "[doc][history]") {
    History history(load_skeleton());
    const DocumentData before = history.document().data();
    CHECK_THROWS_AS(history.undo(), std::logic_error);
    CHECK_THROWS_AS(history.redo(), std::logic_error);
    CHECK(history.document().data() == before);

    (void)commit_rename(history, wall_b, "North wall", "Rename Wall B");
    CHECK_THROWS_AS(history.redo(), std::logic_error);
    (void)history.undo();
    CHECK_THROWS_AS(history.undo(), std::logic_error);
    CHECK(history.document().data() == before);
    CHECK(history.redo_count() == 1);
}

TEST_CASE("undo applies the before values and redo the after values, last in first out",
          "[doc][history]") {
    History history(load_skeleton());
    const DocumentData start = history.document().data();

    (void)commit_rename(history, wall_b, "North wall", "Rename Wall B");
    const DocumentData after_first = history.document().data();
    Transaction move(history.document(), "Move Test chart");
    move.set(test_chart, OnWall{.wall = wall_a,
                                .face = WallFace::a,
                                .distance_m = 2.4,
                                .centre_height_m = 1.5,
                                .stand_off_m = 0.04});
    (void)history.commit(std::move(move));
    const DocumentData after_second = history.document().data();
    REQUIRE(after_first != start);
    REQUIRE(after_second != after_first);
    CHECK(history.undo_count() == 2);
    CHECK(history.can_undo());
    CHECK_FALSE(history.can_redo());

    (void)history.undo();
    CHECK(history.document().data() == after_first);
    CHECK(history.undo_count() == 1);
    CHECK(history.redo_count() == 1);
    (void)history.undo();
    CHECK(history.document().data() == start);
    CHECK(history.undo_count() == 0);
    CHECK(history.redo_count() == 2);
    CHECK_FALSE(history.can_undo());
    CHECK(history.can_redo());

    (void)history.redo();
    CHECK(history.document().data() == after_first);
    (void)history.redo();
    CHECK(history.document().data() == after_second);
    CHECK(history.undo_count() == 2);
    CHECK(history.redo_count() == 0);
}

TEST_CASE("AC2: undo reports the inverse of the commit's change list, and redo the commit's own",
          "[doc][history]") {
    History history(load_skeleton());
    IdGenerator ids = fixed_ids(60);
    const Uuid sensor = ids.next();
    const Uuid work = ids.next();
    // The ids from the stopped clock are later than the skeleton's, so sort after them.
    REQUIRE(viewpoint < sensor);

    Transaction transaction(history.document(), "Several things");
    Entity sensor_entity = named("Sensor");
    sensor_entity.lux_sensor = LuxSensor{.position_m = {3.0, 0.01, 1.5}, .normal = {0.0, 1.0, 0.0}};
    transaction.add_entity(sensor, sensor_entity);
    transaction.remove_entity(viewpoint);
    transaction.rename_entity(wall_b, "North wall");
    Wall wall = *history.document().find_entity(wall_b)->wall;
    wall.height_m = 3.0;
    transaction.set(wall_b, wall);
    transaction.set_work(work, sample_work("Water Lilies"));
    FixtureType type = *history.document().library().find_fixture_type(spot_type);
    type.flux_lm = 900.0;
    transaction.set_fixture_type(spot_type, type);
    ProjectSettings settings = history.document().settings();
    settings.eye_height_m = 1.5;
    transaction.set_settings(settings);

    ChangeList committed;
    committed.settings = true;
    committed.fixture_types = {{spot_type, ChangeKind::changed}};
    committed.works = {{work, ChangeKind::added}};
    committed.entities = {{wall_b, ChangeKind::changed},
                          {viewpoint, ChangeKind::removed},
                          {sensor, ChangeKind::added}};
    committed.components = {{wall_b, ComponentKind::wall, ChangeKind::changed},
                            {viewpoint, ComponentKind::viewpoint, ChangeKind::removed},
                            {sensor, ComponentKind::lux_sensor, ChangeKind::added}};

    ChangeList undone;
    undone.settings = true;
    undone.fixture_types = {{spot_type, ChangeKind::changed}};
    undone.works = {{work, ChangeKind::removed}};
    undone.entities = {{wall_b, ChangeKind::changed},
                       {viewpoint, ChangeKind::added},
                       {sensor, ChangeKind::removed}};
    undone.components = {{wall_b, ComponentKind::wall, ChangeKind::changed},
                         {viewpoint, ComponentKind::viewpoint, ChangeKind::added},
                         {sensor, ComponentKind::lux_sensor, ChangeKind::removed}};
    REQUIRE(inverse(committed) == undone);

    CHECK(history.commit(std::move(transaction)) == committed);
    CHECK(history.undo() == undone);
    CHECK(history.redo() == committed);
    CHECK(history.undo() == undone);
}

TEST_CASE("AC6: each transaction carries a name; the history names the next undo and redo",
          "[doc][history]") {
    History history(load_skeleton());
    CHECK_FALSE(history.undo_name().has_value());
    CHECK_FALSE(history.redo_name().has_value());

    Transaction move(history.document(), "Move Water Lilies");
    CHECK(move.name() == "Move Water Lilies");
    move.set(test_chart, OnWall{.wall = wall_a,
                                .face = WallFace::a,
                                .distance_m = 2.4,
                                .centre_height_m = 1.5,
                                .stand_off_m = 0.04});
    (void)history.commit(std::move(move));
    CHECK(name_or_none(history.undo_name()) == "Move Water Lilies");
    CHECK_FALSE(history.redo_name().has_value());

    // Any UTF-8 text, kept byte for byte.
    (void)commit_rename(history, wall_b, "Mur nord", "Renommer le mur B \xc2\xab nord \xc2\xbb");
    CHECK(name_or_none(history.undo_name()) == "Renommer le mur B \xc2\xab nord \xc2\xbb");
    CHECK_FALSE(history.redo_name().has_value());

    (void)history.undo();
    CHECK(name_or_none(history.undo_name()) == "Move Water Lilies");
    CHECK(name_or_none(history.redo_name()) == "Renommer le mur B \xc2\xab nord \xc2\xbb");

    (void)history.undo();
    CHECK_FALSE(history.undo_name().has_value());
    CHECK(name_or_none(history.redo_name()) == "Move Water Lilies");

    (void)history.redo();
    CHECK(name_or_none(history.undo_name()) == "Move Water Lilies");
    CHECK(name_or_none(history.redo_name()) == "Renommer le mur B \xc2\xab nord \xc2\xbb");

    (void)history.redo();
    CHECK(name_or_none(history.undo_name()) == "Renommer le mur B \xc2\xab nord \xc2\xbb");
    CHECK_FALSE(history.redo_name().has_value());
}

TEST_CASE("AC5: committing a new transaction after an undo clears the redo stack",
          "[doc][history]") {
    History history(load_skeleton());
    const DocumentData start = history.document().data();
    (void)commit_rename(history, wall_a, "South wall", "Rename Wall A");
    (void)commit_rename(history, wall_b, "North wall", "Rename Wall B");
    (void)history.undo();
    REQUIRE(history.redo_count() == 1);

    (void)commit_rename(history, wall_c, "East wall", "Rename Wall C");
    CHECK(history.redo_count() == 0);
    CHECK_FALSE(history.can_redo());
    CHECK_FALSE(history.redo_name().has_value());
    CHECK_THROWS_AS(history.redo(), std::logic_error);
    CHECK(history.undo_count() == 2);
    CHECK(name_or_none(history.undo_name()) == "Rename Wall C");
    CHECK(history.document().find_entity(wall_b)->name == "Wall B");

    // What is left to undo: wall C's rename, then wall A's.
    (void)history.undo();
    CHECK(history.document().find_entity(wall_c)->name == "Wall C");
    CHECK(name_or_none(history.undo_name()) == "Rename Wall A");
    (void)history.undo();
    CHECK(history.document().data() == start);
}

TEST_CASE("AC5: a commit that changes nothing, or is refused, leaves both stacks as they were",
          "[doc][history]") {
    History history(load_skeleton());
    (void)commit_rename(history, wall_a, "South wall", "Rename Wall A");
    (void)commit_rename(history, wall_b, "North wall", "Rename Wall B");
    (void)history.undo();
    const DocumentData before = history.document().data();

    SECTION("a transaction that changes nothing") {
        Transaction nothing(history.document(), "Nothing");
        nothing.rename_entity(wall_c, "Wall C"); // its name already
        CHECK(history.commit(std::move(nothing)) == ChangeList{});
    }
    SECTION("a transaction refused for a dangling reference") {
        Transaction remove(history.document(), "Delete Wall A");
        remove.remove_entity(wall_a);
        CHECK_THROWS_AS(history.commit(std::move(remove)), ReferenceError);
    }
    SECTION("a transaction refused for an old revision") {
        Transaction stale(history.document(), "Rename Wall C");
        stale.rename_entity(wall_c, "East wall");
        (void)history.redo();
        (void)history.undo();
        CHECK_THROWS_AS(history.commit(std::move(stale)), std::invalid_argument);
    }

    CHECK(history.document().data() == before);
    CHECK(history.undo_count() == 1);
    CHECK(history.redo_count() == 1);
    CHECK(name_or_none(history.undo_name()) == "Rename Wall A");
    CHECK(name_or_none(history.redo_name()) == "Rename Wall B");
    (void)history.redo();
    CHECK(history.document().find_entity(wall_b)->name == "North wall");
}

TEST_CASE("undoing every commit gives back the skeleton's project.json byte for byte",
          "[doc][history]") {
    History history(load_skeleton());
    const std::string file = read_file(scene_folder() / "project.json");
    IdGenerator ids = fixed_ids(61);

    (void)commit_rename(history, wall_b, "North wall", "Rename Wall B");
    Transaction remove(history.document(), "Delete Wall A");
    // Wall A goes with the chart that hangs on it, its work, and the spot aimed at the chart.
    remove.remove_entity(test_chart);
    remove.remove_entity(skeleton_id(0x09));
    remove.remove_work(test_chart_work);
    remove.remove_entity(wall_a);
    (void)history.commit(std::move(remove));
    Transaction add(history.document(), "Add a work");
    add.set_work(ids.next(), sample_work("Water Lilies"));
    (void)history.commit(std::move(add));
    REQUIRE(history.undo_count() == 3);
    const std::string after = to_project_json(history.document());

    while (history.can_undo()) {
        (void)history.undo();
    }
    const std::string undone = to_project_json(history.document());
    CAPTURE(first_difference(undone, file));
    CHECK(undone == file);

    while (history.can_redo()) {
        (void)history.redo();
    }
    const std::string redone = to_project_json(history.document());
    CAPTURE(first_difference(redone, after));
    CHECK(redone == after);
}

// ---- The property test -------------------------------------------------------------------------

TEST_CASE("AC3 and AC5: 1000 random transactions on the skeleton, all undone, serialise as the "
          "original; all redone, as after the 1000; refusals are counted and the result saves",
          "[doc][history]") {
    History history(load_skeleton());
    const std::string original = to_project_json(history.document());
    RandomEditor editor(60);

    constexpr int transaction_count = 1000;
    int refused = 0;
    std::size_t recorded = 0;
    for (int i = 0; i < transaction_count; ++i) {
        CAPTURE(i);
        Transaction transaction(history.document(), "Random edit " + std::to_string(i));
        editor.edit(transaction);
        const ChangeList predicted = transaction.changes();
        const DocumentData before = history.document().data();
        try {
            const ChangeList changes = history.commit(std::move(transaction));
            // Before reference checks, changes() says what the commit then reports.
            REQUIRE(changes == predicted);
            if (!changes.empty()) {
                ++recorded;
            }
        } catch (const ReferenceError& error) {
            CAPTURE(error.what());
            ++refused;
            REQUIRE(history.document().data() == before);
        }
        REQUIRE(history.undo_count() == recorded);
        REQUIRE(history.redo_count() == 0);
    }

    // Some of the random transactions are refused, but not all. (The seed is fixed, so these
    // counts are the same in every run; they are printed for whoever changes the generator.)
    INFO(refused << " of " << transaction_count << " transactions refused, " << recorded
                 << " recorded");
    CHECK(refused > 0);
    CHECK(refused < transaction_count);
    REQUIRE(recorded > 0);
    const std::string after = to_project_json(history.document());
    CHECK(after != original);

    std::vector<ChangeList> undone;
    while (history.can_undo()) {
        undone.push_back(history.undo());
    }
    CHECK(undone.size() == recorded);
    CHECK(history.redo_count() == recorded);
    const std::string all_undone = to_project_json(history.document());
    CAPTURE(first_difference(all_undone, original));
    CHECK(all_undone == original);

    // Redo applies the transactions again in the order they were committed, each reporting the
    // inverse of what its undo reported.
    std::size_t step = undone.size();
    std::size_t mismatched = 0;
    while (history.can_redo()) {
        REQUIRE(step > 0);
        --step;
        if (history.redo() != inverse(undone[step])) {
            ++mismatched;
        }
    }
    CHECK(step == 0);
    CHECK(mismatched == 0);
    const std::string all_redone = to_project_json(history.document());
    CAPTURE(first_difference(all_redone, after));
    CHECK(all_redone == after);

    // AC5: the document after the 1000 saves, and reads back as itself.
    const TempFolder folder("history_random_transactions");
    REQUIRE_NOTHROW(save_project_folder(history.document(), folder.path()));
    const Document loaded = load_project_folder(folder.path());
    CHECK(loaded == history.document());
    CHECK(read_file(folder.path() / "project.json") == after);
}
