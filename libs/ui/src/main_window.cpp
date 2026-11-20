// SPDX-License-Identifier: Apache-2.0
#include <cs/ui/main_window.hpp>

#include "features/feature.hpp"
#include "project_assets_copy.hpp"
#include "viewport/exposure.hpp"
#include "viewport/gpu_context.hpp"
#include "viewport/viewport.hpp"

#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QStatusBar>
#include <QTemporaryDir>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cs::ui {

namespace {

// The property that holds a menu's or an item's order, on its QAction.
constexpr const char* order_property = "cs_order";

// A menu's title without its mnemonic markers, so that "&File" and "File" name one menu.
QString plain_title(const QString& title) {
    QString plain = title;
    plain.replace(QLatin1String("&&"), QLatin1String("\x01"));
    plain.remove(QLatin1Char('&'));
    plain.replace(QLatin1Char('\x01'), QLatin1Char('&'));
    return plain;
}

int order_of(const QAction* action) {
    return action->property(order_property).toInt();
}

// The text an item sorts by: a separator's is empty, so it comes before equal-order items.
QString sort_text(const QAction* action) {
    return action->menu() != nullptr ? action->menu()->title() : action->text();
}

// The first of `actions` that a new action of this order and text goes before: the first of
// higher order, or of equal order and later text. Null to append.
QAction* insertion_point(const QList<QAction*>& actions, int order, const QString& text) {
    for (QAction* action : actions) {
        const int other = order_of(action);
        if (other > order || (other == order && sort_text(action) > text)) {
            return action;
        }
    }
    return nullptr;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), history_(std::make_unique<doc::History>()) {
    setObjectName(QStringLiteral("main_window"));
    resize(1280, 800);
    exposure_label_ = new QLabel(this);
    exposure_label_->setObjectName(QStringLiteral("exposure_status"));
    validation_label_ = new QLabel(this);
    validation_label_->setObjectName(QStringLiteral("validation_status"));
    statusBar()->addPermanentWidget(exposure_label_);
    statusBar()->addPermanentWidget(validation_label_);
    update_title();
    update_status();
}

MainWindow::~MainWindow() {
    // The viewport uses the device: it goes before the GPU context, which QWidget's own
    // destructor, deleting the children, would leave too late.
    delete viewport_;
    viewport_ = nullptr;
    gpu_.reset();
}

// ---- Menus ------------------------------------------------------------------------------------

QMenu* MainWindow::add_menu(const QString& title, int order) {
    for (QAction* action : menuBar()->actions()) {
        if (action->menu() != nullptr &&
            plain_title(action->menu()->title()) == plain_title(title)) {
            return action->menu();
        }
    }
    auto* menu = new QMenu(title, this);
    menu->menuAction()->setProperty(order_property, order);
    QAction* before = insertion_point(menuBar()->actions(), order, title);
    menuBar()->insertMenu(before, menu);
    return menu;
}

QAction* MainWindow::add_menu_item(QMenu* menu, int order, const QString& text) {
    auto* action = new QAction(text, menu);
    action->setProperty(order_property, order);
    menu->insertAction(insertion_point(menu->actions(), order, text), action);
    return action;
}

QAction* MainWindow::add_menu_separator(QMenu* menu, int order) {
    auto* action = new QAction(menu);
    action->setSeparator(true);
    action->setProperty(order_property, order);
    menu->insertAction(insertion_point(menu->actions(), order, QString()), action);
    return action;
}

// ---- The inspector ----------------------------------------------------------------------------

void MainWindow::add_inspector_page(int order, const QString& title, QWidget* page) {
    page->setParent(this);
    page->hide();
    const auto at = std::find_if(pages_.begin(), pages_.end(), [&](const InspectorPage& other) {
        return other.order > order || (other.order == order && other.title > title);
    });
    const auto index = static_cast<int>(at - pages_.begin());
    pages_.insert(at, InspectorPage{order, title, page});
    emit inspector_page_added(index);
}

// ---- The project ------------------------------------------------------------------------------

void MainWindow::new_project() {
    history_ = std::make_unique<doc::History>();
    folder_.reset();
    untitled_assets_.reset();
    show_world(true);
    emit project_changed();
}

void MainWindow::open_project(const std::filesystem::path& folder) {
    auto history = std::make_unique<doc::History>(doc::load_project_folder(folder));
    history_ = std::move(history);
    folder_ = folder;
    untitled_assets_.reset();
    show_world(true);
    emit project_changed();
}

void MainWindow::save_project(const std::filesystem::path& folder) {
    copy_project_assets(document(), current_assets_folder(), folder / "assets");
    doc::save_project_folder(document(), folder);
    folder_ = folder;
    // The images are in the project's folder now.
    untitled_assets_.reset();
    update_title();
    emit project_changed();
}

std::filesystem::path MainWindow::assets_folder() {
    if (!folder_ && !untitled_assets_) {
        auto folder = std::make_unique<QTemporaryDir>();
        if (!folder->isValid()) {
            throw std::runtime_error("cannot make a temporary folder for the project's images: " +
                                     folder->errorString().toStdString());
        }
        untitled_assets_ = std::move(folder);
    }
    return current_assets_folder();
}

std::filesystem::path MainWindow::current_assets_folder() const {
    if (folder_) {
        return *folder_ / "assets";
    }
    if (untitled_assets_) {
        return std::filesystem::path(untitled_assets_->path().toStdU16String());
    }
    // An untitled project that has stored no image yet.
    return {};
}

// ---- Changing the document --------------------------------------------------------------------

doc::ChangeList MainWindow::commit(doc::Transaction transaction) {
    return document_edited(history_->commit(std::move(transaction)));
}

doc::ChangeList MainWindow::undo() {
    return document_edited(history_->undo());
}

doc::ChangeList MainWindow::redo() {
    return document_edited(history_->redo());
}

doc::ChangeList MainWindow::document_edited(doc::ChangeList changes) {
    if (!changes.empty()) {
        // The change is in the history now: whatever showing it does, features hear of it.
        try {
            // A full compile until the world updates from the change list (M2-24).
            show_world(false);
        } catch (const std::exception& error) {
            problems_ = {std::string("the project cannot be shown: ") + error.what()};
            std::cerr << "curation: warning: " << problems_.front() << '\n';
        }
        emit document_changed(changes);
    }
    return changes;
}

void MainWindow::show_world(bool new_project) {
    update_title();
    problems_.clear();
    const world::World world = world::compile(document());
    for (const world::Problem& problem : world.problems) {
        problems_.push_back("entity " + problem.entity.to_string() + ": " + problem.reason);
    }
    QString message;
    if (!world.problems.empty()) {
        message = tr("%n part(s) of the project cannot be shown; see the log", nullptr,
                     static_cast<int>(world.problems.size()));
    }
    if (viewport_ != nullptr) {
        if (new_project) {
            // Each project starts at EV100 8; its first viewpoint, if any, sets its own.
            viewport_->reset_exposure();
        }
        try {
            const std::filesystem::path assets = current_assets_folder();
            const std::vector<std::string> problems = new_project
                                                          ? viewport_->set_world(world, assets)
                                                          : viewport_->update_world(world, assets);
            problems_.insert(problems_.end(), problems.begin(), problems.end());
        } catch (const std::exception& error) {
            problems_.push_back(std::string("the viewport cannot show the project: ") +
                                error.what());
            // Why the viewport is empty matters more than the parts left out.
            message = tr("The viewport cannot show this project: %1")
                          .arg(QString::fromStdString(error.what()));
            // An empty world, so that the previous picture does not stay on screen.
            try {
                static_cast<void>(viewport_->update_world(world::World{}, {}));
            } catch (const std::exception& empty_error) {
                std::cerr << "curation: the viewport cannot be cleared: " << empty_error.what()
                          << '\n';
            }
        }
    }
    for (const std::string& problem : problems_) {
        std::cerr << "curation: warning: " << problem << '\n';
    }
    // Only the window's own message is cleared, never one a feature has shown since.
    if (!message.isEmpty()) {
        statusBar()->showMessage(message);
    } else if (!problem_message_.isEmpty() && statusBar()->currentMessage() == problem_message_) {
        statusBar()->clearMessage();
    }
    problem_message_ = message;
    update_status();
}

void MainWindow::update_title() {
    QString name = tr("Untitled");
    if (folder_) {
        // "skeleton/" names the folder skeleton too; u16string keeps any name whole on Windows.
        std::filesystem::path folder = folder_->lexically_normal();
        if (folder.filename().empty()) {
            folder = folder.parent_path();
        }
        name = QString::fromStdU16String(folder.filename().u16string());
    }
    setWindowTitle(tr("%1 — Curation Sandbox").arg(name));
}

// ---- The status bar ---------------------------------------------------------------------------

QString MainWindow::exposure_status() const {
    const double ev100 = viewport_ != nullptr ? viewport_->ev100() : Exposure::default_ev100;
    return QStringLiteral("EV100 %1").arg(ev100, 0, 'f', 1);
}

QString MainWindow::validation_status() const {
    return gpu_ ? gpu_->validation_status() : validation_status_text(false, 0);
}

void MainWindow::update_status() {
    exposure_label_->setText(exposure_status());
    validation_label_->setText(validation_status());
}

void MainWindow::set_gpu(std::unique_ptr<GpuContext> gpu, bool headless, QString problem) {
    gpu_ = std::move(gpu);
    viewport_problem_ = std::move(problem);
    if (gpu_) {
        connect(gpu_.get(), &GpuContext::validation_message, this, &MainWindow::update_status);
        try {
            viewport_ = new Viewport(*gpu_, headless, this);
        } catch (const std::exception& error) {
            // A platform whose windows Qt cannot give a Vulkan surface: the viewport still draws,
            // into an image painted by a widget, as on the offscreen platform.
            std::cerr << "curation: the viewport cannot use a Vulkan window, so it draws "
                         "headless: "
                      << error.what() << '\n';
            viewport_ = new Viewport(*gpu_, true, this);
        }
        connect(viewport_, &Viewport::ev100_changed, this, &MainWindow::update_status);
        connect(viewport_, &Viewport::frame_drawn, this, &MainWindow::viewport_frame_drawn);
        setCentralWidget(viewport_);
    } else {
        auto* label = new QLabel(tr("No viewport: %1").arg(viewport_problem_), this);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        setCentralWidget(label);
    }
    update_status();
}

QImage MainWindow::screenshot() {
    QImage image = grab().toImage();
    if (viewport_ == nullptr || viewport_->headless()) {
        // Headless, the viewport is a widget, and grab() shows it.
        return image;
    }
    const QImage frame = viewport_->grab_frame();
    if (!frame.isNull()) {
        QPainter painter(&image);
        const QRect where(viewport_->mapTo(this, QPoint(0, 0)), viewport_->size());
        painter.drawImage(where, frame);
    }
    return image;
}

std::unique_ptr<MainWindow> make_main_window(const std::optional<std::filesystem::path>& project) {
    auto window = std::make_unique<MainWindow>();
    const bool headless = !platform_has_vulkan_surfaces();
    QString problem;
    std::unique_ptr<GpuContext> gpu = make_app_gpu_context(headless, problem);
    window->set_gpu(std::move(gpu), headless, problem);
    register_features(*window);
    if (project) {
        window->open_project(*project);
    } else {
        window->new_project();
    }
    return window;
}

} // namespace cs::ui
