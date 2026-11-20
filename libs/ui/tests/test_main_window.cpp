// SPDX-License-Identifier: Apache-2.0
//
// The main window's own rules (issue #82): menus, menu items and inspector pages are sorted by an
// explicit order, never by the order in which features register; the File menu and the inspector
// dock are features, registered through cs_generate_registry. These need no GPU: a bare
// MainWindow has no viewport.

#include "features/feature.hpp"
#include "ui_test.hpp"

#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSignalSpy>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <vector>

// Only the generated registry declares the features' register functions.
namespace cs::ui {
void register_file_menu(MainWindow& window);
void register_inspector_host(MainWindow& window);
} // namespace cs::ui

namespace {

using cs::ui::MainWindow;
using cs::ui::test::menu_bar_layout;
using cs::ui::test::menu_items;
using cs::ui::test::MenuLayout;

// A feature of the test's own, as M1-34's edit_menu.cpp would be: an Edit menu whose items are
// added out of their order (Redo before Undo).
void register_test_edit_menu(MainWindow& window) {
    QMenu* edit = window.add_menu(QStringLiteral("&Edit"), cs::ui::menu_order::edit);
    window.add_menu_item(edit, 20, QStringLiteral("&Redo"));
    window.add_menu_item(edit, 10, QStringLiteral("&Undo"));
}

} // namespace

// Acceptance criterion: menus and items carry an explicit order and the window sorts by it, so
// nothing depends on registration order; two features registered in both orders give the same
// menu bar, with File first.
TEST_CASE("MainWindow: two features registered in either order give the same menu bar, File "
          "first",
          "[ui]") {
    MainWindow file_first;
    cs::ui::register_file_menu(file_first);
    register_test_edit_menu(file_first);

    MainWindow edit_first;
    register_test_edit_menu(edit_first);
    cs::ui::register_file_menu(edit_first);

    const std::vector<MenuLayout> a = menu_bar_layout(file_first);
    const std::vector<MenuLayout> b = menu_bar_layout(edit_first);
    INFO("file menu registered first:\n" << cs::ui::test::to_string(a));
    INFO("edit menu registered first:\n" << cs::ui::test::to_string(b));
    CHECK(a == b);
    REQUIRE(a.size() == 2);
    CHECK(a[0].title == QStringLiteral("File"));
    CHECK(a[1].title == QStringLiteral("Edit"));
    // Undo (order 10) before Redo (order 20), although Redo was added first.
    CHECK(a[1].items == QStringList{QStringLiteral("Undo"), QStringLiteral("Redo")});
}

// Acceptance criterion: items within a menu are placed by their order (equal orders by text), not
// by when they were added; a separator is placed by its order like an item.
TEST_CASE("MainWindow: items of a menu sort by order, then text, not by insertion", "[ui]") {
    MainWindow window;
    QMenu* menu = window.add_menu(QStringLiteral("Test"), 50);
    REQUIRE(menu != nullptr);

    QAction* c = window.add_menu_item(menu, 30, QStringLiteral("C"));
    QAction* a = window.add_menu_item(menu, 10, QStringLiteral("A"));
    window.add_menu_separator(menu, 25);
    QAction* b = window.add_menu_item(menu, 20, QStringLiteral("B"));
    // Equal orders: by text, whichever came first.
    window.add_menu_item(menu, 40, QStringLiteral("Z"));
    window.add_menu_item(menu, 40, QStringLiteral("Y"));

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a->text() == QStringLiteral("A"));
    CHECK(menu_items(*menu) == QStringList{QStringLiteral("A"), QStringLiteral("B"),
                                           QStringLiteral("---"), QStringLiteral("C"),
                                           QStringLiteral("Y"), QStringLiteral("Z")});
}

// Acceptance criterion: menus sort by their order (equal orders by title); add_menu with a title
// that exists returns that menu and ignores the new order.
TEST_CASE("MainWindow: menus sort by order then title; add_menu of an existing title returns the "
          "same menu",
          "[ui]") {
    MainWindow window;
    QMenu* help = window.add_menu(QStringLiteral("Help"), cs::ui::menu_order::help);
    QMenu* beta = window.add_menu(QStringLiteral("Beta"), 50);
    QMenu* alpha = window.add_menu(QStringLiteral("Alpha"), 50);
    QMenu* view = window.add_menu(QStringLiteral("View"), cs::ui::menu_order::view);

    QMenu* view_again = window.add_menu(QStringLiteral("View"), 1000);
    CHECK(view_again == view);
    QMenu* help_again = window.add_menu(QStringLiteral("Help"), -5);
    CHECK(help_again == help);
    REQUIRE(alpha != nullptr);
    REQUIRE(beta != nullptr);

    QStringList titles;
    for (const MenuLayout& menu : menu_bar_layout(window)) {
        titles << menu.title;
    }
    // View (20), Alpha and Beta (50, by title), Help (100): one each.
    CHECK(titles == QStringList{QStringLiteral("View"), QStringLiteral("Alpha"),
                                QStringLiteral("Beta"), QStringLiteral("Help")});
}

TEST_CASE("menu_order: File, Edit, View, Help from left to right", "[ui]") {
    STATIC_CHECK(cs::ui::menu_order::file < cs::ui::menu_order::edit);
    STATIC_CHECK(cs::ui::menu_order::edit < cs::ui::menu_order::view);
    STATIC_CHECK(cs::ui::menu_order::view < cs::ui::menu_order::help);
}

// Acceptance criterion: inspector pages carry an order and the dock (inspector_host.cpp) shows
// them sorted by it (equal orders by title), whether they were added before or after the dock
// was made.
TEST_CASE("MainWindow: inspector pages added before and after the inspector dock show sorted by "
          "order",
          "[ui]") {
    MainWindow window;
    auto* a = new QLabel(QStringLiteral("a"));
    auto* ab = new QLabel(QStringLiteral("ab"));
    auto* b = new QLabel(QStringLiteral("b"));
    auto* c = new QLabel(QStringLiteral("c"));

    SECTION("the dock first") {
        cs::ui::register_inspector_host(window);
        window.add_inspector_page(30, QStringLiteral("C"), c);
        window.add_inspector_page(20, QStringLiteral("B"), b);
        window.add_inspector_page(10, QStringLiteral("A"), a);
        window.add_inspector_page(20, QStringLiteral("Ab"), ab);
    }
    SECTION("the dock last") {
        window.add_inspector_page(20, QStringLiteral("B"), b);
        window.add_inspector_page(30, QStringLiteral("C"), c);
        window.add_inspector_page(20, QStringLiteral("Ab"), ab);
        window.add_inspector_page(10, QStringLiteral("A"), a);
        cs::ui::register_inspector_host(window);
    }
    SECTION("the dock between") {
        window.add_inspector_page(20, QStringLiteral("B"), b);
        window.add_inspector_page(30, QStringLiteral("C"), c);
        cs::ui::register_inspector_host(window);
        window.add_inspector_page(20, QStringLiteral("Ab"), ab);
        window.add_inspector_page(10, QStringLiteral("A"), a);
    }

    // inspector_pages(): A (10), Ab and B (20, by title), C (30).
    const auto& pages = window.inspector_pages();
    REQUIRE(pages.size() == 4);
    CHECK(pages[0].order == 10);
    CHECK(pages[0].title == QStringLiteral("A"));
    CHECK(pages[0].widget == a);
    CHECK(pages[1].order == 20);
    CHECK(pages[1].title == QStringLiteral("Ab"));
    CHECK(pages[1].widget == ab);
    CHECK(pages[2].order == 20);
    CHECK(pages[2].title == QStringLiteral("B"));
    CHECK(pages[2].widget == b);
    CHECK(pages[3].order == 30);
    CHECK(pages[3].title == QStringLiteral("C"));
    CHECK(pages[3].widget == c);

    // The dock shows them in that order.
    QDockWidget* dock = cs::ui::test::inspector_dock(window);
    REQUIRE(dock != nullptr);
    const std::vector<int> positions = cs::ui::test::positions_in_dock(*dock, {a, ab, b, c});
    INFO("positions of A, Ab, B, C in the dock: " << positions[0] << ", " << positions[1] << ", "
                                                  << positions[2] << ", " << positions[3]);
    CHECK(std::is_sorted(positions.begin(), positions.end()));
    CHECK(std::adjacent_find(positions.begin(), positions.end()) == positions.end());
}

// Acceptance criterion (inspector order): inspector_page_added gives the new page's index in the
// sorted inspector_pages().
TEST_CASE("MainWindow: inspector_page_added gives the page's sorted index", "[ui]") {
    MainWindow window;
    cs::ui::register_inspector_host(window);
    QSignalSpy added(&window, &MainWindow::inspector_page_added);

    window.add_inspector_page(20, QStringLiteral("B"), new QLabel);
    window.add_inspector_page(30, QStringLiteral("C"), new QLabel);
    window.add_inspector_page(10, QStringLiteral("A"), new QLabel);

    REQUIRE(added.count() == 3);
    CHECK(added[0][0].toInt() == 0); // B, alone
    CHECK(added[1][0].toInt() == 1); // C, after B
    CHECK(added[2][0].toInt() == 0); // A, before both
}

// Acceptance criterion: every features/*.cpp is registered through cs_generate_registry; the
// File menu and the inspector dock are themselves features.
TEST_CASE("register_features: registers the File menu first and the inspector dock", "[ui]") {
    MainWindow window;
    CHECK(cs::ui::test::inspector_dock(window) == nullptr);
    CHECK(window.menuBar()->actions().isEmpty());

    cs::ui::register_features(window);

    const std::vector<MenuLayout> layout = menu_bar_layout(window);
    INFO(cs::ui::test::to_string(layout));
    REQUIRE_FALSE(layout.empty());
    CHECK(layout.front().title == QStringLiteral("File"));
    CHECK(cs::ui::test::inspector_dock(window) != nullptr);
}

// Review of #82: a menu's title with and without its mnemonic names one menu, so that two
// features that spell it differently do not make two File menus.
TEST_CASE("MainWindow: add_menu ignores the mnemonic when it finds a menu by title", "[ui]") {
    MainWindow window;
    QMenu* file = window.add_menu(QStringLiteral("&File"), cs::ui::menu_order::file);
    CHECK(window.add_menu(QStringLiteral("File"), cs::ui::menu_order::file) == file);
    // A literal ampersand ("&&") still tells titles apart.
    QMenu* both = window.add_menu(QStringLiteral("A&&B"), cs::ui::menu_order::view);
    CHECK(window.add_menu(QStringLiteral("AB"), cs::ui::menu_order::view) != both);
    CHECK(window.add_menu(QStringLiteral("A&&B"), cs::ui::menu_order::view) == both);
}
