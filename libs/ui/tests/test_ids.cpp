// SPDX-License-Identifier: Apache-2.0
//
// MainWindow::ids(): one id generator for the window's lifetime, shared by every feature (the
// planning review of 2026-11-21 on #82). Features are separate files that cannot share anything
// but the window, and two generators with one seed would make the same id in the same
// millisecond.

#include <cs/doc/uuid.hpp>
#include <cs/editor/commands/room.hpp>
#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

using cs::doc::IdGenerator;
using cs::doc::Uuid;
using cs::ui::MainWindow;

namespace {

// The 48-bit Unix time in milliseconds at the start of a version 7 id.
std::uint64_t millisecond_of(const Uuid& id) {
    std::uint64_t ms = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        ms = ms << 8 | id.bytes()[i];
    }
    return ms;
}

// Two features, each a separate file that only sees the window: each asks it for the ids.
IdGenerator& first_feature_ids(MainWindow& window) {
    return window.ids();
}
IdGenerator& second_feature_ids(MainWindow& window) {
    return window.ids();
}

} // namespace

TEST_CASE("MainWindow: every feature's ids() is the window's one generator", "[ui]") {
    MainWindow window;
    CHECK(&first_feature_ids(window) == &second_feature_ids(window));

    // New and Open keep it: the ids after New go on growing from the ids before.
    const Uuid before = window.ids().next();
    IdGenerator* const generator = &window.ids();
    window.new_project();
    CHECK(&window.ids() == generator);
    CHECK(window.ids().next() > before);
}

TEST_CASE("MainWindow: ids made in one millisecond by two features are all different", "[ui]") {
    MainWindow window;
    // ids() uses the system clock, so the test makes ids fast enough that many share a
    // millisecond, alternating between the two features, and checks that it did.
    constexpr std::size_t count = 20'000;
    std::vector<Uuid> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        IdGenerator& generator =
            i % 2 == 0 ? first_feature_ids(window) : second_feature_ids(window);
        ids.push_back(generator.next());
    }

    std::size_t sharing = 0;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        CHECK(ids[i] > ids[i - 1]); // strictly growing, so all different
        CHECK(ids[i].version() == 7);
        if (millisecond_of(ids[i]) == millisecond_of(ids[i - 1])) {
            ++sharing;
        }
    }
    const std::set<Uuid> distinct(ids.begin(), ids.end());
    CHECK(distinct.size() == count);
    INFO("ids sharing their predecessor's millisecond: " << sharing);
    // Any sharing shows the case; a slow machine (a sanitizer build) may share fewer.
    REQUIRE(sharing > 0);
}

TEST_CASE("MainWindow: a feature's command makes its ids with the window's generator", "[ui]") {
    MainWindow window;
    // A feature's command (the room tool, here) with the window's ids, committed through the
    // window: the next id the window makes is greater than every id in the room.
    cs::editor::NewRoom room = cs::editor::new_room(window.document(), window.ids(), 6.0, 4.0, 3.0);
    const Uuid room_id = room.room;
    const std::array<Uuid, 4> walls = room.walls;
    window.commit(std::move(room.transaction));
    const Uuid next = window.ids().next();
    CHECK(next > room_id);
    for (const Uuid& wall : walls) {
        CHECK(next > wall);
    }
}
