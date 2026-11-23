// SPDX-License-Identifier: Apache-2.0
//
// UI features: each libs/ui/src/features/<name>.cpp defines
//
//     void cs::ui::register_<name>(MainWindow& window);
//
// which adds its menus, items, docks and inspector pages to the window. cs_generate_registry
// (libs/ui/CMakeLists.txt) generates register_features(), which calls every one of them in
// file-name order; the window sorts what they add by the orders below, so that order never
// matters. A new feature is a new file, never an edit to main_window.cpp or to a list.
#pragma once

#include <cs/ui/main_window.hpp>

namespace cs::ui {

// Calls every feature's register function (generated).
void register_features(MainWindow& window);

// The menu bar's menus, left to right. Gaps leave room for menus to come.
namespace menu_order {
inline constexpr int file = 0;
inline constexpr int edit = 10;
inline constexpr int view = 20;
inline constexpr int help = 100;
} // namespace menu_order

} // namespace cs::ui
