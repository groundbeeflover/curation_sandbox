// SPDX-License-Identifier: Apache-2.0
//
// The application's main window (ADR 0006, ADR 0011): menus, docks and a status bar around the
// Vulkan viewport, holding the open project.
//
// The window itself knows no feature. Each file of libs/ui/src/features/ registers itself
// through cs_generate_registry (cmake/CsRegistry.cmake), which calls them in file-name order, so
// the window sorts what they add by an explicit order instead: menus by a number per menu, items
// by a number per item, inspector pages by a number per page. Nothing depends on the order in
// which features register, and adding a feature never edits this file.
//
// This header includes no Vulkan: the viewport adapter (src/viewport/) is the only part of ui
// that does (CLAUDE.md).
#pragma once

#include <QImage>
#include <QMainWindow>
#include <QString>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QLabel;
class QMenu;
class QWidget;

namespace cs::doc {
class Document;
}

namespace cs::ui {

class GpuContext;
class Viewport;

// A page of the inspector dock, which inspector_host.cpp shows in order.
struct InspectorPage {
    int order = 0;
    QString title;
    QWidget* widget = nullptr; // the inspector's from add_inspector_page on
};

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    // A window with an empty project, no feature and no viewport: what tests of the window's
    // own rules start from. make_main_window() makes the application's.
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    // ---- Menus, sorted by order -------------------------------------------------------------

    // The menu bar's menu with this title, made at this order if there is none yet; a later
    // call with the same title returns the same menu and ignores its order. Menus of lower order
    // come first; equal orders are sorted by title, so that registration order never decides.
    QMenu* add_menu(const QString& title, int order);
    // A new item of the menu, placed among its items by order; equal orders by text. The
    // caller connects its triggered() and sets its shortcut.
    QAction* add_menu_item(QMenu* menu, int order, const QString& text);
    // A separator, placed among the menu's items by order like an item.
    QAction* add_menu_separator(QMenu* menu, int order);

    // ---- The inspector ----------------------------------------------------------------------

    // Hands a page to the inspector dock, which inspector_host.cpp makes and which shows the
    // pages in order (equal orders by title). The window takes ownership of the widget. Pages
    // added before or after the inspector exists are shown alike.
    void add_inspector_page(int order, const QString& title, QWidget* page);
    // Every page so far, sorted.
    [[nodiscard]] const std::vector<InspectorPage>& inspector_pages() const { return pages_; }

    // ---- The project ------------------------------------------------------------------------

    [[nodiscard]] const doc::Document& document() const { return *document_; }
    // The folder the project was opened from or saved to; nothing for a new project.
    [[nodiscard]] const std::optional<std::filesystem::path>& project_folder() const {
        return folder_;
    }

    // An empty project, at the default exposure (EV100 8).
    void new_project();
    // Opens a project folder (doc::load_project_folder). Throws as it does, leaving the open
    // project as it was.
    void open_project(const std::filesystem::path& folder);
    // Saves to a project folder (doc::save_project_folder), which becomes the project's folder.
    // Throws as it does.
    void save_project(const std::filesystem::path& folder);

    // ---- The viewport and the GPU -----------------------------------------------------------

    // Null in a window without a viewport, and when no Vulkan device was found.
    [[nodiscard]] Viewport* viewport() const { return viewport_; }
    [[nodiscard]] GpuContext* gpu() const { return gpu_.get(); }
    // Why there is no viewport, when the window was made with a GPU but has none: no device.
    [[nodiscard]] const QString& viewport_problem() const { return viewport_problem_; }

    // The status bar's texts: "EV100 8.0", and "validation: 0" or "validation: off".
    [[nodiscard]] QString exposure_status() const;
    [[nodiscard]] QString validation_status() const;

    // The window as it is shown: QWidget::grab of the widgets, with the viewport's last frame
    // painted where its native window sits, which grab() alone shows empty (ADR 0011, point 5).
    [[nodiscard]] QImage screenshot();

  signals:
    // After new_project(), open_project() and save_project().
    void project_changed();
    // After a page is added, with its index in inspector_pages().
    void inspector_page_added(int index);
    // The viewport drew a frame.
    void viewport_frame_drawn();

  private:
    friend std::unique_ptr<MainWindow> make_main_window();

    void set_gpu(std::unique_ptr<GpuContext> gpu, QString problem);
    void show_project();
    void update_title();
    void update_status();

    std::unique_ptr<doc::Document> document_;
    std::optional<std::filesystem::path> folder_;
    std::vector<InspectorPage> pages_;
    std::unique_ptr<GpuContext> gpu_; // outlives the viewport: ~MainWindow deletes it first
    Viewport* viewport_ = nullptr;    // the central widget's, when there is one
    QString viewport_problem_;
    QLabel* exposure_label_ = nullptr;
    QLabel* validation_label_ = nullptr;
};

// The application's window: a Vulkan device (headless on the offscreen platform, where the
// viewport draws into an image instead of a window surface), the viewport, every feature of
// libs/ui/src/features/ and an empty project. Without a Vulkan device the window still opens,
// with viewport() null and viewport_problem() saying why; it throws only when CS_REQUIRE_GPU is
// set (as the GPU tests do), and as cs::gpu::Device does for any other failure.
std::unique_ptr<MainWindow> make_main_window();

} // namespace cs::ui
