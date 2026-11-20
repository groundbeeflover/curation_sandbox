// SPDX-License-Identifier: Apache-2.0
//
// The application's shell, end to end (issue #82): an offscreen QTest opens tests/scenes/skeleton
// in the window of make_main_window(), checks the title, the docks and the status bar, and saves a
// screenshot of the widgets to build/<preset>/ui-out/.

#include "ui_test.hpp"
#include "viewport/viewport.hpp"

#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QDockWidget>
#include <QImage>
#include <QPoint>
#include <QTest>

#include <exception>
#include <filesystem>
#include <memory>

namespace {

using cs::ui::MainWindow;
using cs::ui::Viewport;

bool not_black(const QColor& colour) {
    return colour.red() + colour.green() + colour.blue() > 0;
}

} // namespace

// Acceptance criteria: an offscreen QTest opens tests/scenes/skeleton, checks the title and
// docks, and saves a screenshot of the widgets to build/<preset>/ui-out/; the window shows the
// Standard tier (headless, through the tone-mapping pass) with File first in the menu bar; the
// status bar shows the EV100 and the validation count, and a claim of zero messages fails when
// the layer is off.
TEST_CASE("Shell: the skeleton opens offscreen with its title, docks, status and a screenshot",
          "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    Viewport* viewport = window->viewport();
    CHECK(viewport->headless());

    window->open_project(cs::ui::test::skeleton_folder());
    window->resize(1024, 700);
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window.get()));
    // Generous for lavapipe under the sanitizers; qWaitFor returns once the frame is there.
    REQUIRE(QTest::qWaitFor(
        [&] { return viewport->frames() > 0 && !viewport->render_size().isEmpty(); },
        Viewport::settle_ms + 15000));

    // Title and docks.
    CHECK(window->windowTitle().contains(QStringLiteral("skeleton")));
    QDockWidget* inspector = cs::ui::test::inspector_dock(*window);
    REQUIRE(inspector != nullptr);
    CHECK(inspector->isVisible());
    const auto menus = cs::ui::test::menu_bar_layout(*window);
    REQUIRE_FALSE(menus.empty());
    CHECK(menus.front().title == QStringLiteral("File"));

    // The skeleton's viewpoint has EV100 8.
    CHECK(viewport->ev100() == 8.0);
    CHECK(window->exposure_status() == QStringLiteral("EV100 8.0"));
    CHECK(cs::ui::test::status_bar_shows(*window, QStringLiteral("EV100 8.0")));
    CHECK(cs::ui::test::status_bar_shows(*window, window->validation_status()));

    // The viewport draws the lit chart at its centre (the viewpoint looks straight at it): not
    // black. The darkest patch at about 1000 lux is >= 2 nits, about 15 in sRGB at EV100 8.
    const QImage frame = viewport->grab_frame();
    REQUIRE_FALSE(frame.isNull());
    CHECK(frame.size() == viewport->render_size());
    CHECK(not_black(frame.pixelColor(frame.width() / 2, frame.height() / 2)));

    // The screenshot of the widgets shows the viewport's frame where the viewport sits.
    const QImage screenshot = window->screenshot();
    REQUIRE_FALSE(screenshot.isNull());
    const double ratio = screenshot.devicePixelRatio();
    const QPoint centre = viewport->mapTo(window.get(), viewport->rect().center()) * ratio;
    REQUIRE(screenshot.rect().contains(centre));
    CHECK(not_black(screenshot.pixelColor(centre)));
    const std::filesystem::path png = cs::ui::test::ui_out_dir() / "shell-skeleton.png";
    std::filesystem::remove(png);
    REQUIRE(screenshot.save(QString::fromStdString(png.string()), "PNG"));
    CHECK(std::filesystem::file_size(png) > 0);
    CHECK_FALSE(QImage(QString::fromStdString(png.string())).isNull());

    // No validation messages: in a debug build the layer must be on for this to mean anything.
    cs::ui::test::require_no_validation_messages(*window);
}

// Acceptance criterion: the window still opens without a viewport, saying why; then it has the
// features' menus and dock, and the status says what it can.
TEST_CASE("Shell: make_main_window has the features' File menu and inspector dock", "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window;
    try {
        window = cs::ui::make_main_window();
    } catch (const std::exception& e) {
        FAIL("make_main_window() threw: " << e.what());
    }
    REQUIRE(window != nullptr);
    if (window->viewport() == nullptr) {
        // Only without a device, and only when no device is required (it would have thrown).
        CHECK_FALSE(cs::ui::test::require_gpu_env());
        CHECK_FALSE(window->viewport_problem().isEmpty());
    } else {
        CHECK(window->viewport_problem().isEmpty());
    }
    CHECK(cs::ui::test::inspector_dock(*window) != nullptr);
    const auto menus = cs::ui::test::menu_bar_layout(*window);
    REQUIRE_FALSE(menus.empty());
    CHECK(menus.front().title == QStringLiteral("File"));
    CHECK(window->exposure_status() == QStringLiteral("EV100 8.0"));
    CHECK_FALSE(window->project_folder().has_value());
}
