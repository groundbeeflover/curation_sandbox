// SPDX-License-Identifier: Apache-2.0
//
// Changing the document through the main window (issue #82, owner's addition of 2026-11-20): the
// window holds a doc::History; commit(), undo() and redo() return the doc::ChangeList, refresh
// the viewport keeping its camera and EV100, and emit document_changed(); a refused commit throws
// as History::commit does and changes nothing; New and Open start a fresh History.
//
// Most of these need no GPU: a bare MainWindow has no viewport. The last tests use the
// application's window and its viewport, and are tagged [gpu].

#include "ui_test.hpp"
#include "viewport/view_renderer.hpp"
#include "viewport/viewport.hpp"

#include <cs/core/random.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/history.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/editor/commands/hanging.hpp>
#include <cs/editor/commands/room.hpp>
#include <cs/ui/main_window.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QMetaObject>
#include <QObject>
#include <QSignalSpy>
#include <QTest>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cs::ui::MainWindow;
using cs::ui::Viewport;
using namespace std::string_view_literals;

// Ids from a fixed seed and a fixed clock, so that every run builds the same documents.
cs::doc::IdGenerator fixed_ids(std::uint32_t stream) {
    return cs::doc::IdGenerator(cs::hash_seed(82, stream, 0, 0, 0),
                                [] { return std::int64_t{1'800'000'000'000}; });
}

// The skeleton's test chart, as an asset of the project. add_work does not look for it on disk.
cs::doc::AssetRef chart_image() {
    return {"26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a", "png"};
}

// Every document_changed() of a window, with the document's revision when it came: the signal
// comes after the change, so the revision is already the new one.
class ChangeRecorder {
  public:
    explicit ChangeRecorder(MainWindow& window) {
        connection_ = QObject::connect(&window, &MainWindow::document_changed,
                                       [this, &window](const cs::doc::ChangeList& changes) {
                                           changes_.push_back(changes);
                                           revisions_.push_back(window.document().revision());
                                       });
    }
    ~ChangeRecorder() { QObject::disconnect(connection_); }
    ChangeRecorder(const ChangeRecorder&) = delete;
    ChangeRecorder& operator=(const ChangeRecorder&) = delete;

    [[nodiscard]] const std::vector<cs::doc::ChangeList>& changes() const { return changes_; }
    [[nodiscard]] const std::vector<std::uint64_t>& revisions() const { return revisions_; }

  private:
    QMetaObject::Connection connection_;
    std::vector<cs::doc::ChangeList> changes_;
    std::vector<std::uint64_t> revisions_;
};

// What a refused commit must leave as it was: the document, its revision, both stacks.
struct HistoryState {
    cs::doc::Document document;
    std::uint64_t revision = 0;
    std::size_t undo_count = 0;
    std::size_t redo_count = 0;
    std::optional<std::string> undo_name;
    std::optional<std::string> redo_name;
    std::vector<std::string> problems;
    bool operator==(const HistoryState&) const = default;
};

std::optional<std::string> to_string(std::optional<std::string_view> name) {
    if (!name) {
        return std::nullopt;
    }
    return std::string(*name);
}

HistoryState state_of(const MainWindow& window) {
    const cs::doc::History& history = window.history();
    return {
        window.document(),        window.document().revision(),   history.undo_count(),
        history.redo_count(),     to_string(history.undo_name()), to_string(history.redo_name()),
        window.project_problems()};
}

// What commit_room added.
struct RoomIds {
    cs::doc::Uuid room;
    std::array<cs::doc::Uuid, 4> walls; // A to D
};

// A 6 x 4 x 3 m room, committed through the window.
RoomIds commit_room(MainWindow& window, cs::doc::IdGenerator& ids) {
    cs::editor::NewRoom room = cs::editor::new_room(window.document(), ids, 6.0, 4.0, 3.0);
    const RoomIds added{room.room, room.walls};
    window.commit(std::move(room.transaction));
    return added;
}

} // namespace

// Acceptance criterion: commit() of a transaction that changes something returns its ChangeList,
// puts it on history(), changes document(), and then emits document_changed() once with it.
TEST_CASE("MainWindow: commit returns the ChangeList, records it in history() and emits "
          "document_changed once",
          "[ui]") {
    MainWindow window;
    ChangeRecorder recorder(window);
    cs::doc::IdGenerator ids = fixed_ids(0);
    REQUIRE_FALSE(window.history().can_undo());
    const std::uint64_t revision = window.document().revision();

    cs::editor::NewRoom room = cs::editor::new_room(window.document(), ids, 6.0, 4.0, 3.0);
    const cs::doc::ChangeList expected = room.transaction.changes();
    REQUIRE_FALSE(expected.empty());

    const cs::doc::ChangeList changes = window.commit(std::move(room.transaction));

    CHECK(changes == expected);
    // On the history: one step to undo, named after the transaction, nothing to redo.
    CHECK(window.history().can_undo());
    CHECK(window.history().undo_count() == 1);
    CHECK(window.history().undo_name() == "New room"sv);
    CHECK_FALSE(window.history().can_redo());
    // The window's document is the history's, changed: the room and its four walls are in it.
    CHECK(&window.document() == &window.history().document());
    CHECK(window.document().revision() == revision + 1);
    CHECK(window.document().find_entity(room.room) != nullptr);
    for (const cs::doc::Uuid& wall : room.walls) {
        CHECK(window.document().find_entity(wall) != nullptr);
    }
    // One signal, with the same ChangeList, once the document holds the change.
    REQUIRE(recorder.changes().size() == 1);
    CHECK(recorder.changes()[0] == changes);
    CHECK(recorder.revisions()[0] == revision + 1);

    // A second change: one more signal, with its own ChangeList.
    cs::editor::AddedWork work =
        cs::editor::add_work(window.document(), ids, chart_image(), 0.5, 0.4, "Chart");
    const cs::doc::ChangeList work_changes = window.commit(std::move(work.transaction));
    CHECK_FALSE(work_changes.empty());
    CHECK(window.document().find_work(work.work) != nullptr);
    CHECK(window.history().undo_count() == 2);
    CHECK(window.history().undo_name() == "Add Chart"sv);
    REQUIRE(recorder.changes().size() == 2);
    CHECK(recorder.changes()[1] == work_changes);
}

// Acceptance criterion: a transaction that changes nothing is not recorded (History::commit) and
// emits nothing, since document_changed() comes only for a change.
TEST_CASE("MainWindow: a commit that changes nothing returns an empty ChangeList and emits "
          "nothing",
          "[ui]") {
    MainWindow window;
    cs::doc::IdGenerator ids = fixed_ids(1);
    commit_room(window, ids);
    ChangeRecorder recorder(window);
    const HistoryState before = state_of(window);

    SECTION("an empty transaction") {
        const cs::doc::ChangeList changes =
            window.commit(cs::doc::Transaction(window.document(), "Nothing"));
        CHECK(changes.empty());
    }
    SECTION("a transaction that sets what is already there") {
        cs::doc::Transaction same(window.document(), "Same settings");
        same.set_settings(window.document().settings());
        REQUIRE(same.empty());
        CHECK(window.commit(std::move(same)).empty());
    }

    CHECK(recorder.changes().empty());
    CHECK(state_of(window) == before);
}

// Acceptance criterion: a refused commit throws as History::commit does and changes nothing.
// A transaction made for an older revision of the document is refused with
// std::invalid_argument. The redo stack is not empty, so that a refused commit clearing it (as an
// accepted one does) would show.
TEST_CASE("MainWindow: a commit made for an older revision throws std::invalid_argument and "
          "changes nothing",
          "[ui]") {
    MainWindow window;
    cs::doc::IdGenerator ids = fixed_ids(2);
    const RoomIds room = commit_room(window, ids);

    // Made at this revision, committed after two more changes.
    cs::doc::Transaction stale(window.document(), "Rename room");
    stale.rename_entity(room.room, "Gallery");

    cs::editor::AddedWork work =
        cs::editor::add_work(window.document(), ids, chart_image(), 0.5, 0.4, "Chart");
    window.commit(std::move(work.transaction));
    window.undo();
    REQUIRE(window.history().can_redo());

    ChangeRecorder recorder(window);
    const HistoryState before = state_of(window);
    CHECK_THROWS_AS(window.commit(std::move(stale)), std::invalid_argument);
    CHECK(state_of(window) == before);
    CHECK(recorder.changes().empty());
}

// Acceptance criterion: a refused commit throws as History::commit does and changes nothing. A
// transaction whose result holds a reference check_references refuses throws that
// doc::ReferenceError: here, removing the wall a work hangs on.
TEST_CASE("MainWindow: a commit leaving a dangling reference throws doc::ReferenceError and "
          "changes nothing",
          "[ui]") {
    MainWindow window;
    cs::doc::IdGenerator ids = fixed_ids(3);
    const RoomIds room = commit_room(window, ids);
    cs::editor::AddedWork work =
        cs::editor::add_work(window.document(), ids, chart_image(), 0.5, 0.4, "Chart");
    window.commit(std::move(work.transaction));
    // add_work hangs on wall A, the first of new_room's walls.
    const std::optional<cs::doc::Uuid> wall_a =
        cs::editor::find_wall_labelled(window.document(), cs::editor::default_wall_label);
    REQUIRE(wall_a.has_value());
    CHECK(*wall_a == room.walls[0]);

    ChangeRecorder recorder(window);
    const HistoryState before = state_of(window);
    cs::doc::Transaction remove(window.document(), "Remove wall A");
    remove.remove_entity(*wall_a);
    CHECK_THROWS_AS(window.commit(std::move(remove)), cs::doc::ReferenceError);
    CHECK(state_of(window) == before);
    CHECK(recorder.changes().empty());
}

// Acceptance criterion: undo() and redo() return the ChangeList and emit document_changed() with
// it, as commit() does; the document goes back and forth.
TEST_CASE("MainWindow: undo and redo round-trip the document with document_changed", "[ui]") {
    MainWindow window;
    cs::doc::IdGenerator ids = fixed_ids(4);
    const cs::doc::Document empty = window.document();

    cs::editor::NewRoom room = cs::editor::new_room(window.document(), ids, 6.0, 4.0, 3.0);
    const cs::doc::ChangeList room_changes = window.commit(std::move(room.transaction));
    const cs::doc::Document with_room = window.document();
    cs::editor::AddedWork work =
        cs::editor::add_work(window.document(), ids, chart_image(), 0.5, 0.4, "Chart");
    const cs::doc::ChangeList work_changes = window.commit(std::move(work.transaction));
    const cs::doc::Document with_work = window.document();

    ChangeRecorder recorder(window);
    std::uint64_t revision = window.document().revision();

    // Undo the work: it is removed again.
    const cs::doc::ChangeList undo_work = window.undo();
    CHECK(window.document() == with_room);
    CHECK(window.document().revision() == ++revision);
    REQUIRE(undo_work.works.size() == 1);
    CHECK(undo_work.works[0].work == work.work);
    CHECK(undo_work.works[0].kind == cs::doc::ChangeKind::removed);
    CHECK(window.history().can_redo());
    CHECK(window.history().redo_name() == "Add Chart"sv);
    CHECK(window.history().undo_name() == "New room"sv);
    REQUIRE(recorder.changes().size() == 1);
    CHECK(recorder.changes()[0] == undo_work);
    CHECK(recorder.revisions()[0] == revision);

    // Undo the room: the empty project, nothing more to undo.
    const cs::doc::ChangeList undo_room = window.undo();
    CHECK(window.document() == empty);
    CHECK_FALSE(undo_room.empty());
    CHECK_FALSE(window.history().can_undo());
    CHECK(window.history().redo_count() == 2);
    REQUIRE(recorder.changes().size() == 2);
    CHECK(recorder.changes()[1] == undo_room);

    // Redo both: the same changes as their commits, from the same documents.
    const cs::doc::ChangeList redo_room = window.redo();
    CHECK(redo_room == room_changes);
    CHECK(window.document() == with_room);
    const cs::doc::ChangeList redo_work = window.redo();
    CHECK(redo_work == work_changes);
    CHECK(window.document() == with_work);
    CHECK_FALSE(window.history().can_redo());
    CHECK(window.history().undo_count() == 2);
    REQUIRE(recorder.changes().size() == 4);
    CHECK(recorder.changes()[2] == redo_room);
    CHECK(recorder.changes()[3] == redo_work);
}

// Acceptance criterion (edge case): undo() and redo() with nothing to undo or redo throw
// std::logic_error, as History's do, and change nothing.
TEST_CASE("MainWindow: undo and redo with nothing to undo or redo throw std::logic_error", "[ui]") {
    MainWindow window;
    ChangeRecorder recorder(window);

    SECTION("a new window") {
        const HistoryState before = state_of(window);
        CHECK_THROWS_AS(window.undo(), std::logic_error);
        CHECK_THROWS_AS(window.redo(), std::logic_error);
        CHECK(state_of(window) == before);
        CHECK(recorder.changes().empty());
    }
    SECTION("after a commit, nothing to redo; after its undo, nothing more to undo") {
        cs::doc::IdGenerator ids = fixed_ids(5);
        commit_room(window, ids);
        const std::size_t signal_count = recorder.changes().size();
        HistoryState before = state_of(window);
        CHECK_THROWS_AS(window.redo(), std::logic_error);
        CHECK(state_of(window) == before);

        window.undo();
        before = state_of(window);
        CHECK_THROWS_AS(window.undo(), std::logic_error);
        CHECK(state_of(window) == before);
        // Only the undo that did something was signalled.
        CHECK(recorder.changes().size() == signal_count + 1);
    }
}

// Acceptance criterion: New and Open start a fresh History, with nothing to undo or redo.
TEST_CASE("MainWindow: new_project and open_project start a fresh history", "[ui]") {
    MainWindow window;
    cs::doc::IdGenerator ids = fixed_ids(6);
    // Something on both stacks.
    commit_room(window, ids);
    cs::editor::AddedWork work =
        cs::editor::add_work(window.document(), ids, chart_image(), 0.5, 0.4, "Chart");
    window.commit(std::move(work.transaction));
    window.undo();
    REQUIRE(window.history().can_undo());
    REQUIRE(window.history().can_redo());

    const auto require_fresh = [&] {
        CHECK_FALSE(window.history().can_undo());
        CHECK_FALSE(window.history().can_redo());
        CHECK(window.history().undo_count() == 0);
        CHECK(window.history().redo_count() == 0);
        CHECK_FALSE(window.history().undo_name().has_value());
        CHECK(&window.document() == &window.history().document());
        CHECK_THROWS_AS(window.undo(), std::logic_error);
        CHECK_THROWS_AS(window.redo(), std::logic_error);
    };

    SECTION("new_project") {
        window.new_project();
        CHECK(window.document() == cs::doc::Document());
        require_fresh();
    }
    SECTION("open_project") {
        const cs::doc::Document skeleton =
            cs::doc::load_project_folder(cs::ui::test::skeleton_folder());
        window.open_project(cs::ui::test::skeleton_folder());
        CHECK(window.document() == skeleton);
        require_fresh();

        // The fresh history holds the opened document: a change to it undoes back to it.
        ChangeRecorder recorder(window);
        cs::doc::IdGenerator more = fixed_ids(7);
        cs::editor::NewRoom room = cs::editor::new_room(window.document(), more, 2.0, 2.0, 2.5);
        const cs::doc::ChangeList changes = window.commit(std::move(room.transaction));
        CHECK_FALSE(changes.empty());
        CHECK(window.history().undo_count() == 1);
        window.undo();
        CHECK(window.document() == skeleton);
        CHECK(recorder.changes().size() == 2);
    }
}

// ---- With the viewport -------------------------------------------------------------------------

namespace {

// How long a frame may take beyond the viewport's settling (as test_viewport.cpp): lavapipe under
// the sanitizers of the debug build is slow; qWaitFor returns as soon as the frame is there.
constexpr int frame_margin_ms = 15000;

} // namespace

// Acceptance criterion: a change recompiles the world and refreshes the viewport, keeping the
// camera and the EV100, then emits document_changed(); undo and redo do the same. New and Open
// instead jump to the first viewpoint (EV100 8 for the skeleton's and for a new project).
TEST_CASE("MainWindow: commit, undo and redo keep the viewport's camera and EV100; New and Open "
          "jump to the first viewpoint",
          "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    Viewport* viewport = window->viewport();

    const cs::world::World skeleton_world =
        cs::world::compile(cs::doc::load_project_folder(cs::ui::test::skeleton_folder()));
    REQUIRE_FALSE(skeleton_world.cameras.empty());
    const cs::world::Camera first_viewpoint = skeleton_world.cameras.front();
    REQUIRE(first_viewpoint.ev100 == 8.0);

    window->open_project(cs::ui::test::skeleton_folder());
    window->resize(1024, 700);
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window.get()));
    REQUIRE(QTest::qWaitFor(
        [&] { return viewport->frames() > 0 && !viewport->render_size().isEmpty(); },
        Viewport::settle_ms + frame_margin_ms));
    CHECK(viewport->camera() == first_viewpoint);
    CHECK(viewport->ev100() == 8.0);

    cs::world::Camera expected_camera;
    double expected_ev100 = 0.0;
    SECTION("a camera moved with set_camera, no viewpoint active, at EV100 8") {
        cs::world::Camera walked = first_viewpoint;
        walked.entity.reset();
        walked.ev100.reset();
        walked.position_m.x += 0.5;
        walked.position_m.y -= 0.25;
        viewport->set_camera(walked);
        expected_camera = walked;
        expected_ev100 = 8.0;
    }
    SECTION("a viewpoint with an EV100 of its own, 6.5") {
        cs::world::Camera darker = first_viewpoint;
        darker.ev100 = 6.5;
        viewport->show_viewpoint(darker);
        expected_camera = darker;
        expected_ev100 = 6.5;
    }
    REQUIRE(viewport->camera() == expected_camera);
    REQUIRE(viewport->ev100() == expected_ev100);

    // The viewport has kept the camera and EV100 when document_changed() comes.
    std::optional<cs::world::Camera> camera_at_signal;
    double ev100_at_signal = 0.0;
    QObject::connect(window.get(), &MainWindow::document_changed, [&](const cs::doc::ChangeList&) {
        camera_at_signal = viewport->camera();
        ev100_at_signal = viewport->ev100();
    });

    const auto require_kept = [&](const char* what) {
        INFO(what);
        CHECK(viewport->camera() == expected_camera);
        CHECK(viewport->ev100() == expected_ev100);
        REQUIRE(camera_at_signal.has_value());
        CHECK(*camera_at_signal == expected_camera);
        CHECK(ev100_at_signal == expected_ev100);
        camera_at_signal.reset();
    };

    cs::doc::IdGenerator ids = fixed_ids(8);
    cs::editor::NewRoom room = cs::editor::new_room(window->document(), ids, 2.0, 2.0, 2.5);
    const int frames_before = viewport->frames();
    REQUIRE_FALSE(window->commit(std::move(room.transaction)).empty());
    require_kept("after commit");
    // The viewport draws the changed world: a frame after the commit.
    CHECK(QTest::qWaitFor([&] { return viewport->frames() > frames_before; },
                          Viewport::settle_ms + frame_margin_ms));
    CHECK(viewport->camera() == expected_camera);
    CHECK(viewport->ev100() == expected_ev100);

    window->undo();
    require_kept("after undo");
    window->redo();
    require_kept("after redo");

    // Open jumps to the project's first viewpoint and its EV100.
    window->open_project(cs::ui::test::skeleton_folder());
    CHECK(viewport->camera() == first_viewpoint);
    CHECK(viewport->ev100() == 8.0);

    // New: the default camera at EV100 8.
    viewport->show_viewpoint(expected_camera);
    window->new_project();
    CHECK(viewport->camera() == cs::ui::default_camera());
    CHECK(viewport->ev100() == 8.0);

    cs::ui::test::require_no_validation_messages(*window);
}

// Acceptance criterion: a refused commit changes nothing, the viewport included.
TEST_CASE("MainWindow: a refused commit leaves the viewport's camera and EV100", "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    Viewport* viewport = window->viewport();
    window->open_project(cs::ui::test::skeleton_folder());

    cs::doc::Transaction stale(window->document(), "Stale");
    stale.set_settings([&] {
        cs::doc::ProjectSettings settings = window->document().settings();
        settings.centre_line_m += 0.1;
        return settings;
    }());
    cs::doc::IdGenerator ids = fixed_ids(9);
    cs::editor::NewRoom room = cs::editor::new_room(window->document(), ids, 2.0, 2.0, 2.5);
    window->commit(std::move(room.transaction));

    cs::world::Camera darker = viewport->camera();
    darker.ev100 = 5.5;
    viewport->show_viewpoint(darker);
    QSignalSpy ev100_changed(viewport, &Viewport::ev100_changed);
    const std::vector<std::string> problems = window->project_problems();

    CHECK_THROWS_AS(window->commit(std::move(stale)), std::invalid_argument);
    CHECK(viewport->camera() == darker);
    CHECK(viewport->ev100() == 5.5);
    CHECK(ev100_changed.isEmpty());
    CHECK(window->project_problems() == problems);
    cs::ui::test::require_no_validation_messages(*window);
}
