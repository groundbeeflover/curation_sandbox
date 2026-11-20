// SPDX-License-Identifier: Apache-2.0
//
// The File menu (file_menu.cpp, issue #82) and the project API it drives: New, Open folder, Save
// and Quit, on a bare MainWindow, which has no viewport and needs no GPU. The actions open dialogs,
// so the tests call the window's project API that they call.

#include "ui_test.hpp"

#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QChar>
#include <QMenu>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <fstream>
#include <optional>

namespace cs::ui {
void register_file_menu(MainWindow& window);
} // namespace cs::ui

namespace {

using cs::ui::MainWindow;
namespace fs = std::filesystem;

bool same_folder(const std::optional<fs::path>& folder, const fs::path& expected) {
    return folder.has_value() && fs::equivalent(*folder, expected);
}

} // namespace

// Acceptance criterion: the main window has a File menu with New, Open folder, Save and Quit.
TEST_CASE("File menu: New, Open folder, Save and Quit, in that order", "[ui]") {
    MainWindow window;
    cs::ui::register_file_menu(window);

    QMenu* file = cs::ui::test::find_menu(window, QStringLiteral("File"));
    REQUIRE(file != nullptr);
    // The items, without separators (where they sit is the feature's choice).
    QStringList items = cs::ui::test::menu_items(*file);
    items.removeAll(QStringLiteral("---"));
    const QString open_folder = QStringLiteral("Open folder") + QChar(0x2026);
    CHECK(items == QStringList{QStringLiteral("New"), open_folder, QStringLiteral("Save"),
                               QStringLiteral("Quit")});
    // The File menu is the first.
    const auto layout = cs::ui::test::menu_bar_layout(window);
    REQUIRE_FALSE(layout.empty());
    CHECK(layout.front().title == QStringLiteral("File"));
}

// Acceptance criterion: File > Open folder opens a project folder; the title shows it.
TEST_CASE("MainWindow: open_project on the skeleton sets the folder and the title; new_project "
          "clears them",
          "[ui]") {
    MainWindow window;
    QSignalSpy changed(&window, &MainWindow::project_changed);
    // A new window holds an empty project.
    CHECK_FALSE(window.project_folder().has_value());
    CHECK(window.document() == cs::doc::Document());

    const fs::path skeleton = cs::ui::test::skeleton_folder();
    window.open_project(skeleton);

    CHECK(changed.count() == 1);
    CHECK(same_folder(window.project_folder(), skeleton));
    CHECK(window.windowTitle().contains(QStringLiteral("skeleton")));
    CHECK(window.document() == cs::doc::load_project_folder(skeleton));

    window.new_project();
    CHECK(changed.count() == 2);
    CHECK_FALSE(window.project_folder().has_value());
    CHECK(window.document() == cs::doc::Document());
    CHECK_FALSE(window.windowTitle().contains(QStringLiteral("skeleton")));
}

// Acceptance criterion (Open folder, untrusted input fails cleanly): a folder that holds no
// project throws, as doc::load_project_folder does, and leaves the open project as it was.
TEST_CASE("MainWindow: open_project on a folder without a project throws and changes nothing",
          "[ui]") {
    MainWindow window;
    const fs::path skeleton = cs::ui::test::skeleton_folder();
    window.open_project(skeleton);
    const cs::doc::Document before = window.document();
    const QString title = window.windowTitle();
    QSignalSpy changed(&window, &MainWindow::project_changed);

    SECTION("an empty folder") {
        const fs::path empty = cs::ui::test::fresh_out_folder("test-open-empty");
        REQUIRE_FALSE(fs::exists(empty / "project.json"));
        CHECK_THROWS(window.open_project(empty));
    }
    SECTION("a folder that does not exist") {
        const fs::path missing = cs::ui::test::ui_out_dir() / "test-open-missing";
        fs::remove_all(missing);
        CHECK_THROWS(window.open_project(missing));
    }
    SECTION("a project.json that is not a project") {
        const fs::path broken = cs::ui::test::fresh_out_folder("test-open-broken");
        {
            std::ofstream file(broken / "project.json");
            file << "{ \"schemaVersion\": 99 }\n";
        }
        CHECK_THROWS(window.open_project(broken));
    }

    CHECK(window.document() == before);
    CHECK(same_folder(window.project_folder(), skeleton));
    CHECK(window.windowTitle() == title);
    CHECK(changed.isEmpty());
}

// Acceptance criterion: File > Save writes the project folder, which loads back the same document.
TEST_CASE("MainWindow: save_project writes a project.json that loads back equal", "[ui]") {
    MainWindow window;
    window.open_project(cs::ui::test::skeleton_folder());
    QSignalSpy changed(&window, &MainWindow::project_changed);

    const fs::path folder = cs::ui::test::fresh_out_folder("test-save-project");
    window.save_project(folder);

    REQUIRE(fs::exists(folder / "project.json"));
    CHECK(cs::doc::load_project_folder(folder) == window.document());
    CHECK(window.document() == cs::doc::load_project_folder(cs::ui::test::skeleton_folder()));
    // The folder saved to becomes the project's.
    CHECK(same_folder(window.project_folder(), folder));
    CHECK(changed.count() == 1);
}

// Acceptance criterion: Save of a new project writes an empty project that loads back equal.
TEST_CASE("MainWindow: save_project of a new project loads back as an empty project", "[ui]") {
    MainWindow window;
    const fs::path folder = cs::ui::test::fresh_out_folder("test-save-new-project");
    window.save_project(folder);

    REQUIRE(fs::exists(folder / "project.json"));
    CHECK(cs::doc::load_project_folder(folder) == cs::doc::Document());
    CHECK(same_folder(window.project_folder(), folder));
}

// Review of #82: a folder given with a trailing separator, as shell completion writes it, still
// names the project in the title.
TEST_CASE("MainWindow: the title names a folder given with a trailing separator", "[ui]") {
    MainWindow window;
    window.open_project(cs::ui::test::skeleton_folder() / "");
    CHECK(window.windowTitle().contains(QStringLiteral("skeleton")));
}
