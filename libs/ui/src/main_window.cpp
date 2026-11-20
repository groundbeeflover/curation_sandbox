// SPDX-License-Identifier: Apache-2.0
#include <cs/ui/main_window.hpp>

#include "features/feature.hpp"
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

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace cs::ui {

namespace {

// The property that holds a menu's or an item's order, on its QAction.
constexpr const char* order_property = "cs_order";

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
    : QMainWindow(parent), document_(std::make_unique<doc::Document>()) {
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
        if (action->menu() != nullptr && action->menu()->title() == title) {
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
    document_ = std::make_unique<doc::Document>();
    folder_.reset();
    if (viewport_ != nullptr) {
        viewport_->reset_exposure();
    }
    show_project();
}

void MainWindow::open_project(const std::filesystem::path& folder) {
    document_ = std::make_unique<doc::Document>(doc::load_project_folder(folder));
    folder_ = folder;
    show_project();
}

void MainWindow::save_project(const std::filesystem::path& folder) {
    doc::save_project_folder(*document_, folder);
    folder_ = folder;
    update_title();
    emit project_changed();
}

void MainWindow::show_project() {
    update_title();
    if (viewport_ != nullptr) {
        const world::World world = world::compile(*document_);
        for (const world::Problem& problem : world.problems) {
            std::cerr << "curation: warning: entity " << problem.entity.to_string() << ": "
                      << problem.reason << '\n';
        }
        try {
            const std::filesystem::path assets =
                folder_ ? *folder_ / "assets" : std::filesystem::path();
            for (const std::string& problem : viewport_->set_world(world, assets)) {
                std::cerr << "curation: warning: " << problem << '\n';
            }
        } catch (const std::exception& error) {
            std::cerr << "curation: the viewport cannot show the project: " << error.what() << '\n';
            statusBar()->showMessage(tr("The viewport cannot show this project: %1")
                                         .arg(QString::fromStdString(error.what())));
        }
        const std::size_t problems = world.problems.size();
        if (problems > 0) {
            statusBar()->showMessage(tr("%n part(s) of the project cannot be shown; see the log",
                                        nullptr, static_cast<int>(problems)));
        }
    }
    update_status();
    emit project_changed();
}

void MainWindow::update_title() {
    const QString name =
        folder_ ? QString::fromStdString(folder_->filename().string()) : tr("Untitled");
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
        viewport_ = new Viewport(*gpu_, headless, this);
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
