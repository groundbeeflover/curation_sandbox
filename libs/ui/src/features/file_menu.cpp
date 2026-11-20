// SPDX-License-Identifier: Apache-2.0
//
// The File menu: New, Open folder, Save and Quit. A project is a folder for now (ADR 0007): the
// .curation container arrives later.

#include "features/feature.hpp"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>

#include <exception>
#include <filesystem>

namespace cs::ui {

namespace {

// The items' orders within the menu.
constexpr int new_order = 0;
constexpr int open_order = 10;
constexpr int save_order = 20;
constexpr int quit_separator_order = 90;
constexpr int quit_order = 100;

std::filesystem::path to_path(const QString& folder) {
    return std::filesystem::path(folder.toStdU16String());
}

void open_folder(MainWindow& window) {
    const QString folder =
        QFileDialog::getExistingDirectory(&window, MainWindow::tr("Open project folder"));
    if (folder.isEmpty()) {
        return;
    }
    try {
        window.open_project(to_path(folder));
    } catch (const std::exception& error) {
        QMessageBox::warning(&window, MainWindow::tr("Cannot open the project"),
                             QString::fromStdString(error.what()));
    }
}

void save(MainWindow& window) {
    std::filesystem::path folder;
    if (window.project_folder()) {
        folder = *window.project_folder();
    } else {
        const QString chosen =
            QFileDialog::getExistingDirectory(&window, MainWindow::tr("Save project to a folder"));
        if (chosen.isEmpty()) {
            return;
        }
        folder = to_path(chosen);
    }
    try {
        window.save_project(folder);
    } catch (const std::exception& error) {
        QMessageBox::warning(&window, MainWindow::tr("Cannot save the project"),
                             QString::fromStdString(error.what()));
    }
}

} // namespace

void register_file_menu(MainWindow& window) {
    QMenu* menu = window.add_menu(MainWindow::tr("&File"), menu_order::file);

    QAction* new_project = window.add_menu_item(menu, new_order, MainWindow::tr("&New"));
    new_project->setShortcut(QKeySequence::New);
    QObject::connect(new_project, &QAction::triggered, &window,
                     [&window] { window.new_project(); });

    QAction* open = window.add_menu_item(menu, open_order, MainWindow::tr("&Open folder…"));
    open->setShortcut(QKeySequence::Open);
    QObject::connect(open, &QAction::triggered, &window, [&window] { open_folder(window); });

    QAction* save_item = window.add_menu_item(menu, save_order, MainWindow::tr("&Save"));
    save_item->setShortcut(QKeySequence::Save);
    QObject::connect(save_item, &QAction::triggered, &window, [&window] { save(window); });

    window.add_menu_separator(menu, quit_separator_order);
    QAction* quit = window.add_menu_item(menu, quit_order, MainWindow::tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    QObject::connect(quit, &QAction::triggered, &window, [&window] { window.close(); });
}

} // namespace cs::ui
