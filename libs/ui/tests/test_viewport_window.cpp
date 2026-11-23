// SPDX-License-Identifier: Apache-2.0
//
// The viewport's window backend on a platform with window surfaces (wayland, xcb, windows): it
// presents, and resizing recreates the swapchain without validation messages (issue #82, first
// acceptance criterion). It needs a display, so it skips on the offscreen platform, where the
// headless backend's tests run instead.

#include "viewport/gpu_context.hpp"
#include "viewport/viewport.hpp"

#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QGuiApplication>
#include <QTest>

#include <filesystem>
#include <memory>

namespace {

void require_window_surfaces() {
    if (!cs::ui::platform_has_vulkan_surfaces()) {
        SKIP("needs a platform with window surfaces (run without QT_QPA_PLATFORM on a desktop); "
             "the platform is "
             << QGuiApplication::platformName().toStdString());
    }
}

} // namespace

TEST_CASE("Viewport window: presents the skeleton and survives resizes without validation "
          "messages",
          "[ui]") {
    require_window_surfaces();
    std::unique_ptr<cs::ui::MainWindow> window =
        cs::ui::make_main_window(std::filesystem::path(CS_SOURCE_DIR) / "tests/scenes/skeleton");
    cs::ui::Viewport* viewport = window->viewport();
    if (viewport == nullptr) {
        SKIP("no Vulkan device: " << window->viewport_problem().toStdString());
    }
    REQUIRE_FALSE(viewport->headless());
    window->resize(900, 600);
    window->show();
    REQUIRE(QTest::qWaitForWindowExposed(window.get()));
    REQUIRE(QTest::qWaitFor([&] { return viewport->frames() > 0; }, 5000));
    CHECK(viewport->swapchains_created() >= 1);

    // Under a tiling compositor a resize is only a request (ADR 0011, point 7), so the test asks
    // for sizes and checks that frames keep coming, not which sizes it got.
    for (const QSize size : {QSize(700, 450), QSize(1200, 800), QSize(500, 380)}) {
        const int frames = viewport->frames();
        const QSize before = viewport->size();
        window->resize(size);
        QTest::qWait(cs::ui::Viewport::settle_ms * 2);
        if (viewport->size() != before) {
            REQUIRE(QTest::qWaitFor([&] { return viewport->frames() > frames; }, 5000));
        }
    }
    CHECK_FALSE(window->screenshot().isNull());

    // Zero validation messages is a claim only the layer can back (ADR 0011, point 8).
#ifndef NDEBUG
    REQUIRE(window->gpu()->validation_active());
#endif
    CHECK(window->gpu()->validation_messages() == 0);
}
