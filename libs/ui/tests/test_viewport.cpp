// SPDX-License-Identifier: Apache-2.0
//
// The viewport adapter in the application's window (issue #82), on the offscreen platform, where
// it draws headless: resizing, the exposure it uses, and walking's relative pointer mode.

#include "ui_test.hpp"
#include "viewport/view_renderer.hpp"
#include "viewport/viewport.hpp"

#include <cs/doc/project_io.hpp>
#include <cs/ui/main_window.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QSignalSpy>
#include <QSize>
#include <QTest>
#include <QWindow>

#include <filesystem>
#include <memory>
#include <string>

namespace {

using cs::ui::MainWindow;
using cs::ui::Viewport;

// How long a frame may take to come, beyond the viewport's own settling: a tier builds its
// pipeline and uploads the lights, and lavapipe under the sanitizers of the debug build is slow.
// qWaitFor returns as soon as the frame is there, so the bound costs only when the test fails.
constexpr int frame_margin_ms = 15000;

// The application's window on the skeleton, shown at 1024 x 700 with a first frame drawn.
std::unique_ptr<MainWindow> shown_shell() {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    window->open_project(cs::ui::test::skeleton_folder());
    window->resize(1024, 700);
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window.get()));
    Viewport* viewport = window->viewport();
    REQUIRE(QTest::qWaitFor(
        [&] { return viewport->frames() > 0 && !viewport->render_size().isEmpty(); },
        Viewport::settle_ms + frame_margin_ms));
    return window;
}

} // namespace

// Acceptance criterion: without a window surface (offscreen) the viewport renders headless, at
// its size in device pixels.
TEST_CASE("Viewport: headless on the offscreen platform, at its size in device pixels",
          "[ui][gpu]") {
    std::unique_ptr<MainWindow> window = shown_shell();
    Viewport* viewport = window->viewport();
    CHECK(viewport->headless());
    CHECK(viewport->render_size() == cs::ui::test::device_size(*viewport));
    const QImage frame = viewport->grab_frame();
    REQUIRE_FALSE(frame.isNull());
    CHECK(frame.size() == viewport->render_size());
    cs::ui::test::require_no_validation_messages(*window);
}

// Acceptance criterion: resizing recreates what the viewport draws into, without validation
// messages; the tier is made again once the size settles (M1-22's addition), not on every event.
TEST_CASE("Viewport: after a resize the render size follows once it settles, without validation "
          "messages",
          "[ui][gpu]") {
    std::unique_ptr<MainWindow> window = shown_shell();
    Viewport* viewport = window->viewport();
    // A frame drawn since `frames_from`, at the viewport's present size in device pixels.
    const auto drawn_at_new_size = [&](int frames_from) {
        const QSize size = cs::ui::test::device_size(*viewport);
        return viewport->render_size() == size && viewport->frames() > frames_from &&
               viewport->grab_frame().size() == size;
    };
    const auto describe = [&] {
        return "viewport " + cs::ui::test::to_string(cs::ui::test::device_size(*viewport)) +
               ", render size " + cs::ui::test::to_string(viewport->render_size()) +
               ", last frame " + cs::ui::test::to_string(viewport->grab_frame().size()) +
               ", frames " + std::to_string(viewport->frames());
    };
    const QSize first = viewport->render_size();
    const int frames_before = viewport->frames();

    QElapsedTimer since_resize;
    since_resize.start();
    window->resize(800, 500);
    QCoreApplication::processEvents();
    const QSize expected = cs::ui::test::device_size(*viewport);
    REQUIRE(expected != first);
    // Within settle_ms of the change the tier has not been made again yet (the timer cannot have
    // fired), so the render size is still the old one. Checked only when the events above were
    // handled in time to say so.
    if (since_resize.elapsed() < Viewport::settle_ms) {
        CHECK(viewport->render_size() == first);
    }

    // Once settled, a frame is drawn at the new size.
    const bool settled = QTest::qWaitFor([&] { return drawn_at_new_size(frames_before); },
                                         Viewport::settle_ms + frame_margin_ms);
    INFO(describe());
    REQUIRE(settled);
    CHECK(viewport->render_size() == expected);

    // Several changes in a row: the last size is the one that settles.
    const int frames_mid = viewport->frames();
    for (const QSize size : {QSize(700, 450), QSize(900, 600), QSize(640, 480)}) {
        window->resize(size);
        QCoreApplication::processEvents();
    }
    const bool settled_again = QTest::qWaitFor([&] { return drawn_at_new_size(frames_mid); },
                                               Viewport::settle_ms + frame_margin_ms);
    INFO(describe());
    REQUIRE(settled_again);

    cs::ui::test::require_no_validation_messages(*window);
}

// Acceptance criterion: the viewport uses the active viewpoint's EV100; with no viewpoint active
// it keeps the last one's (EV100 8 for a new project), and the status bar shows the EV100.
TEST_CASE("Viewport: the active viewpoint's EV100, kept when walking away, 8 for a new project; "
          "shown in the status bar",
          "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    Viewport* viewport = window->viewport();
    CHECK(viewport->ev100() == 8.0);
    CHECK(window->exposure_status() == QStringLiteral("EV100 8.0"));

    // Opening a project makes its first viewpoint active: the skeleton's, at EV100 8.
    const cs::world::World world =
        cs::world::compile(cs::doc::load_project_folder(cs::ui::test::skeleton_folder()));
    REQUIRE_FALSE(world.cameras.empty());
    window->open_project(cs::ui::test::skeleton_folder());
    CHECK(viewport->camera() == world.cameras.front());
    CHECK(viewport->ev100() == 8.0);

    // A viewpoint with EV100 6.5 becomes active.
    QSignalSpy ev100_changed(viewport, &Viewport::ev100_changed);
    cs::world::Camera darker = world.cameras.front();
    darker.ev100 = 6.5;
    viewport->show_viewpoint(darker);
    CHECK(viewport->camera() == darker);
    CHECK(viewport->ev100() == 6.5);
    REQUIRE_FALSE(ev100_changed.isEmpty());
    CHECK(ev100_changed.last()[0].toDouble() == 6.5);
    CHECK(window->exposure_status() == QStringLiteral("EV100 6.5"));
    CHECK(cs::ui::test::status_bar_shows(*window, QStringLiteral("EV100 6.5")));

    // Walking away: the camera moves, no viewpoint active, the EV100 stays.
    cs::world::Camera walked = darker;
    walked.entity.reset();
    walked.ev100.reset();
    walked.position_m.x += 0.5;
    viewport->set_camera(walked);
    CHECK(viewport->camera() == walked);
    CHECK(viewport->ev100() == 6.5);
    CHECK(window->exposure_status() == QStringLiteral("EV100 6.5"));

    // A viewpoint asking for automatic exposure (none in M1) keeps it too.
    cs::world::Camera automatic = world.cameras.front();
    automatic.ev100.reset();
    viewport->show_viewpoint(automatic);
    CHECK(viewport->ev100() == 6.5);

    // Opening another project starts it at EV100 8 too, also when it has no viewpoint (review of
    // #82): the last EV100 is kept only within one project.
    const std::filesystem::path no_viewpoint = cs::ui::test::fresh_out_folder("no-viewpoint");
    {
        MainWindow empty;
        empty.save_project(no_viewpoint);
    }
    window->open_project(no_viewpoint);
    CHECK(viewport->ev100() == 8.0);
    viewport->show_viewpoint(darker);
    REQUIRE(viewport->ev100() == 6.5);

    // A new project: EV100 8, at the default camera (an empty project has no viewpoint).
    window->new_project();
    CHECK(viewport->ev100() == 8.0);
    CHECK(viewport->camera() == cs::ui::default_camera());
    CHECK(window->exposure_status() == QStringLiteral("EV100 8.0"));
    CHECK(cs::ui::test::status_bar_shows(*window, QStringLiteral("EV100 8.0")));
    cs::ui::test::require_no_validation_messages(*window);
}

// Acceptance criterion: the viewport offers walking's relative pointer mode through PointerLock:
// lock, unlock, relative motion in logical pixels, and locked and unlocked notifications.
TEST_CASE("Viewport: relative pointer mode locks, reports motion in logical pixels, and unlocks",
          "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    Viewport* viewport = window->viewport();
    QSignalSpy locked_changed(viewport, &Viewport::pointer_locked_changed);
    QSignalSpy motion(viewport, &Viewport::pointer_motion);

    // Not shown: lock fails and changes nothing.
    CHECK_FALSE(viewport->lock_pointer());
    CHECK_FALSE(viewport->pointer_locked());
    CHECK(locked_changed.isEmpty());

    window->open_project(cs::ui::test::skeleton_folder());
    window->resize(1024, 700);
    window->show();
    window->activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(window.get()));
    // Headless, the viewport has no window of its own: the lock is on the top-level window.
    QWindow* locked_window = window->windowHandle();
    REQUIRE(locked_window != nullptr);

    REQUIRE(viewport->lock_pointer());
    CHECK(viewport->pointer_locked());
    REQUIRE(locked_changed.count() == 1);
    CHECK(locked_changed[0][0].toBool());

    // lock() warped the cursor into the window; that move is not motion.
    QCoreApplication::processEvents();
    CHECK(motion.isEmpty());
    const QPoint centre = QCursor::pos(locked_window->screen());
    CHECK(locked_window->geometry().contains(centre));

    // A move of (12, -9) logical pixels from where the cursor is held. QCursor::setPos rather
    // than QTest::mouseMove, which leaves the offscreen cursor where it was (test_pointer_lock).
    QCursor::setPos(locked_window->screen(), centre + QPoint(12, -9));
    QCoreApplication::processEvents();
    REQUIRE(motion.count() == 1);
    CHECK(motion[0][0].toDouble() == 12.0);
    CHECK(motion[0][1].toDouble() == -9.0);

    QCursor::setPos(locked_window->screen(), centre + QPoint(-30, 25));
    QCoreApplication::processEvents();
    REQUIRE(motion.count() == 2);
    CHECK(motion[1][0].toDouble() == -30.0);
    CHECK(motion[1][1].toDouble() == 25.0);

    SECTION("unlock_pointer()") {
        viewport->unlock_pointer();
    }
    SECTION("Esc") {
        QTest::keyClick(locked_window, Qt::Key_Escape);
    }
    CHECK_FALSE(viewport->pointer_locked());
    REQUIRE(locked_changed.count() == 2);
    CHECK_FALSE(locked_changed[1][0].toBool());

    // Unlocked, moves are not motion, and unlocking again tells nothing.
    QCursor::setPos(locked_window->screen(), centre + QPoint(5, 5));
    QCoreApplication::processEvents();
    viewport->unlock_pointer();
    CHECK(motion.count() == 2);
    CHECK(locked_changed.count() == 2);
    cs::ui::test::require_no_validation_messages(*window);
}
