// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the ui tests of the shell (issue #82) share: the platform check, the folders, reading menus
// as text, finding inspector pages in the dock, and making the application's window or GPU
// context, which skip without a Vulkan device and fail without one under CS_REQUIRE_GPU.
//
// Only one cs::gpu::Device (so one GpuContext, one window from make_main_window) may be alive at a
// time: every test scopes its window or context so that they never overlap.

#include "viewport/gpu_context.hpp"
#include "viewport/viewport.hpp"

#include <cs/gpu/device.hpp>
#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QSize>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cs::ui::test {

// ---- Platform and environment ------------------------------------------------------------------

inline void require_offscreen() {
    const QString platform = QGuiApplication::platformName();
    if (platform != QLatin1String("offscreen")) {
        SKIP("drives the shell headless, which needs QT_QPA_PLATFORM=offscreen; the platform is "
             << platform.toStdString());
    }
}

// The value of an environment variable, or nothing when it is unset. MSVC deprecates getenv, so
// Windows reads through _dupenv_s.
inline std::optional<std::string> read_env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

// Sets (or, with nothing, unsets) an environment variable for one scope, and restores it.
class ScopedEnv {
  public:
    ScopedEnv(std::string name, const std::optional<std::string>& value)
        : name_(std::move(name)), previous_(read_env(name_.c_str())) {
        write(value);
    }
    ~ScopedEnv() { write(previous_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    void write(const std::optional<std::string>& value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
        if (value) {
            setenv(name_.c_str(), value->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

// CS_REQUIRE_GPU set, not empty and not "0": gpu's one reading of it (cs/gpu/device.hpp), rather
// than ui's code under test.
inline bool require_gpu_env() {
    return gpu::gpu_required();
}

// ---- Folders -----------------------------------------------------------------------------------

inline std::filesystem::path skeleton_folder() {
    return std::filesystem::path(CS_SOURCE_DIR) / "tests" / "scenes" / "skeleton";
}

// build/<preset>/ui-out/, created.
inline std::filesystem::path ui_out_dir() {
    const std::filesystem::path dir(CS_UI_OUT_DIR);
    std::filesystem::create_directories(dir);
    return dir;
}

// An empty folder of ui-out/ for one test.
inline std::filesystem::path fresh_out_folder(const std::string& name) {
    const std::filesystem::path dir = ui_out_dir() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// ---- Menus as text -----------------------------------------------------------------------------

// An action's text as a reader sees it: without the '&' of a mnemonic ("&&" is a literal '&'),
// without a shortcut after a tab, and with "..." written as the ellipsis character.
inline QString plain_text(const QString& text) {
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c == QLatin1Char('\t')) {
            break;
        }
        if (c == QLatin1Char('&')) {
            if (i + 1 < text.size() && text[i + 1] == QLatin1Char('&')) {
                out += QLatin1Char('&');
                ++i;
            }
            continue;
        }
        out += c;
    }
    out.replace(QStringLiteral("..."), QString(QChar(0x2026)));
    return out;
}

// A menu's items in order, separators as "---".
inline QStringList menu_items(const QMenu& menu) {
    QStringList items;
    for (const QAction* action : menu.actions()) {
        items << (action->isSeparator() ? QStringLiteral("---") : plain_text(action->text()));
    }
    return items;
}

struct MenuLayout {
    QString title;
    QStringList items;
    bool operator==(const MenuLayout&) const = default;
};

// The menu bar as text, left to right.
inline std::vector<MenuLayout> menu_bar_layout(QMainWindow& window) {
    std::vector<MenuLayout> layout;
    for (const QAction* action : window.menuBar()->actions()) {
        MenuLayout menu{plain_text(action->text()), {}};
        if (const QMenu* m = action->menu()) {
            menu.items = menu_items(*m);
        }
        layout.push_back(std::move(menu));
    }
    return layout;
}

inline std::string to_string(const std::vector<MenuLayout>& layout) {
    std::string text;
    for (const MenuLayout& menu : layout) {
        text += menu.title.toStdString() + ": " +
                menu.items.join(QStringLiteral(", ")).toStdString() + "\n";
    }
    return text;
}

// The menu bar's menu with this title (mnemonics ignored), or null.
inline QMenu* find_menu(QMainWindow& window, const QString& title) {
    for (QAction* action : window.menuBar()->actions()) {
        if (plain_text(action->text()) == title && action->menu() != nullptr) {
            return action->menu();
        }
    }
    return nullptr;
}

// ---- The inspector dock ------------------------------------------------------------------------

inline QDockWidget* inspector_dock(QMainWindow& window) {
    return window.findChild<QDockWidget*>(QStringLiteral("inspector"));
}

// The order in which the dock shows these pages, read from the widget tree without assuming how
// the dock lays them out (a tool box, tabs, a column of group boxes): the pages' lowest common
// ancestor holds each page, or a wrapper of it, at some index of its layout (or of its children
// when it has no layout); the indices, in that order, must increase. Returns the index of each page
// below that ancestor, or fails when a page is not in the dock.
inline std::vector<int> positions_in_dock(QDockWidget& dock, const std::vector<QWidget*>& pages) {
    for (QWidget* page : pages) {
        REQUIRE(page != nullptr);
        INFO("every inspector page is a descendant of the dock");
        REQUIRE(dock.isAncestorOf(page));
    }
    if (pages.size() < 2) {
        return std::vector<int>(pages.size(), 0);
    }
    // The lowest ancestor of the first page (the page's parent upwards) that holds all the others.
    QWidget* common = pages.front()->parentWidget();
    const auto holds_all = [&](QWidget* candidate) {
        for (QWidget* page : pages) {
            if (!candidate->isAncestorOf(page)) {
                return false;
            }
        }
        return true;
    };
    while (common != nullptr && !holds_all(common)) {
        common = common->parentWidget();
    }
    REQUIRE(common != nullptr);
    std::vector<int> positions;
    for (QWidget* page : pages) {
        QWidget* child = page;
        while (child->parentWidget() != common) {
            child = child->parentWidget();
        }
        int index = common->layout() != nullptr ? common->layout()->indexOf(child) : -1;
        if (index < 0) {
            index = static_cast<int>(common->children().indexOf(child));
        }
        positions.push_back(index);
    }
    return positions;
}

// ---- The status bar ----------------------------------------------------------------------------

// A label of the status bar shows exactly this text.
inline bool status_bar_shows(QMainWindow& window, const QString& text) {
    for (const QLabel* label : window.statusBar()->findChildren<QLabel*>()) {
        if (label->text() == text) {
            return true;
        }
    }
    return false;
}

// ---- The application's window and GPU ----------------------------------------------------------

// make_main_window(), with a viewport: the test skips without one (no Vulkan device), or fails
// when CS_REQUIRE_GPU asks for a device.
inline std::unique_ptr<MainWindow> make_shell() {
    std::unique_ptr<MainWindow> window;
    try {
        window = make_main_window();
    } catch (const std::exception& e) {
        FAIL("make_main_window() threw: " << e.what());
    }
    REQUIRE(window != nullptr);
    if (window->viewport() == nullptr) {
        if (require_gpu_env()) {
            FAIL("the window has no viewport, and CS_REQUIRE_GPU requires one: "
                 << window->viewport_problem().toStdString());
        } else {
            SKIP("no viewport (no Vulkan device): " << window->viewport_problem().toStdString());
        }
    }
    REQUIRE(window->gpu() != nullptr);
    return window;
}

// A headless GpuContext of the application, or skip (no device) or fail (CS_REQUIRE_GPU).
inline std::unique_ptr<GpuContext> make_headless_gpu() {
    std::unique_ptr<GpuContext> gpu;
    QString problem;
    try {
        gpu = make_app_gpu_context(true, problem);
    } catch (const std::exception& e) {
        FAIL("make_app_gpu_context(headless) threw: " << e.what());
    }
    if (gpu == nullptr) {
        if (require_gpu_env()) {
            FAIL("no Vulkan device, and CS_REQUIRE_GPU requires one: " << problem.toStdString());
        } else {
            SKIP("no Vulkan device: " << problem.toStdString());
        }
    }
    return gpu;
}

// Zero validation messages (ADR 0011, point 8). In a debug build the claim needs the layer: with
// it off, this fails rather than passing on a run that could not have seen a message. Release
// builds run without the layer, so there the status need only say what the context knows.
inline void require_no_validation_messages(const GpuContext& gpu, const QString& status) {
#ifndef NDEBUG
    INFO("a debug build claims zero validation messages only with the validation layer on");
    REQUIRE(gpu.validation_active());
    REQUIRE(gpu.validation_messages() == 0);
    REQUIRE(status == QStringLiteral("validation: 0"));
#else
    CHECK(status == validation_status_text(gpu.validation_active(), gpu.validation_messages()));
    if (gpu.validation_active()) {
        CHECK(gpu.validation_messages() == 0);
    }
#endif
}

inline void require_no_validation_messages(MainWindow& window) {
    REQUIRE(window.gpu() != nullptr);
    require_no_validation_messages(*window.gpu(), window.validation_status());
}

inline std::string to_string(const QSize& size) {
    return std::to_string(size.width()) + " x " + std::to_string(size.height());
}

// The viewport's size in device pixels: its logical size times its device pixel ratio.
inline QSize device_size(const Viewport& viewport) {
    return viewport.size() * viewport.devicePixelRatioF();
}

} // namespace cs::ui::test
