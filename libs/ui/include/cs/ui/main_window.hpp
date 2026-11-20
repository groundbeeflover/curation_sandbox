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
// The document changes only through commit(), undo() and redo() (CLAUDE.md, ADR 0007), which the
// window's doc::History applies. Each change that changes something compiles the world again,
// shows it in the viewport from the same camera at the same EV100, and emits document_changed():
// features update from it rather than from the document's revision.
//
// This header includes no Vulkan: the viewport adapter (src/viewport/) is the only part of ui
// that does (CLAUDE.md).
#pragma once

#include <cs/doc/history.hpp>
#include <cs/doc/transaction.hpp>

#include <QImage>
#include <QMainWindow>
#include <QString>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QAction;
class QLabel;
class QMenu;
class QTemporaryDir;
class QWidget;

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

    [[nodiscard]] const doc::Document& document() const { return history_->document(); }
    // The open document with its undo and redo stacks: undo_name(), can_undo() and the counts.
    [[nodiscard]] const doc::History& history() const { return *history_; }
    // The folder the project was opened from or saved to; nothing for a new project.
    [[nodiscard]] const std::optional<std::filesystem::path>& project_folder() const {
        return folder_;
    }

    // An empty project, at the default exposure (EV100 8), with nothing to undo.
    void new_project();
    // Opens a project folder (doc::load_project_folder), with nothing to undo. Throws as it
    // does, leaving the open project as it was.
    void open_project(const std::filesystem::path& folder);
    // Saves to a project folder, which becomes the project's folder. First copies every asset
    // the document names (works' images) from assets_folder() into <folder>/assets when that is
    // another folder: each through a temporary file renamed once whole, skipping a file already
    // there. Then writes the document (doc::save_project_folder). Throws before writing the
    // document when an asset cannot be copied (std::invalid_argument for a name
    // assets::asset_file refuses, std::runtime_error for a failed copy), and as
    // save_project_folder does; the project's folder stays as it was. An asset the document names
    // but the folder lacks, holds as anything but a regular file, or holds as anything but the
    // image its name says (assets::store_image checks it), is not copied (the log says so): the
    // project already shows it as a problem.
    void save_project(const std::filesystem::path& folder);

    // Where the project's images are stored (assets::store_image) and loaded from:
    // <project folder>/assets, or for an untitled project a temporary folder the window owns,
    // made the first time it is asked for. New, Open and a save of an untitled project delete
    // the temporary folder. Throws std::runtime_error when the temporary folder cannot be made.
    [[nodiscard]] std::filesystem::path assets_folder();

    // ---- Changing the document --------------------------------------------------------------

    // Commits a transaction made for document() (doc::History::commit) and returns what it
    // changed. When something changed: the world is compiled again, the viewport shows it from
    // the same camera at the same EV100, and document_changed() is emitted. Throws as
    // History::commit does (std::invalid_argument for a transaction made for another revision,
    // doc::ReferenceError for a dangling reference), and then changes nothing. A transaction
    // refers to the document it was made for, which New and Open replace: commit it before
    // either, never after.
    doc::ChangeList commit(doc::Transaction transaction);
    // Undo and redo the last transaction (doc::History), with the same refresh and signal as
    // commit(). Throw std::logic_error when there is nothing to undo or redo.
    doc::ChangeList undo();
    doc::ChangeList redo();

    // What could not be shown of the project, one line each: the world's problems (an entity
    // world::compile leaves out) and the viewport's (an image shown as its base colour), or why
    // the viewport cannot show it at all. Updated by every New, Open and change.
    [[nodiscard]] const std::vector<std::string>& project_problems() const { return problems_; }

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
    // After a commit, undo or redo that changed something, once the viewport shows it.
    void document_changed(const cs::doc::ChangeList& changes);
    // After a page is added, with its index in inspector_pages().
    void inspector_page_added(int index);
    // The viewport drew a frame.
    void viewport_frame_drawn();

  private:
    friend std::unique_ptr<MainWindow>
    make_main_window(const std::optional<std::filesystem::path>& project);

    void set_gpu(std::unique_ptr<GpuContext> gpu, bool headless, QString problem);
    // A new project or a changed one: compile the world and show it. A new one starts at EV100 8
    // and from its first viewpoint; a changed one keeps the camera and the EV100.
    void show_world(bool new_project);
    doc::ChangeList document_edited(doc::ChangeList changes);
    // assets_folder() without making one: empty for an untitled project with no image yet.
    std::filesystem::path current_assets_folder() const;
    void update_title();
    void update_status();

    std::unique_ptr<doc::History> history_; // History neither copies nor moves
    std::optional<std::filesystem::path> folder_;
    std::unique_ptr<QTemporaryDir> untitled_assets_; // an untitled project's assets_folder()
    std::vector<std::string> problems_;
    QString problem_message_; // the status bar message show_world() last showed
    std::vector<InspectorPage> pages_;
    std::unique_ptr<GpuContext> gpu_; // outlives the viewport: ~MainWindow deletes it first
    Viewport* viewport_ = nullptr;    // the central widget's, when there is one
    QString viewport_problem_;
    QLabel* exposure_label_ = nullptr;
    QLabel* validation_label_ = nullptr;
};

// The application's window: a Vulkan device (headless on the offscreen platform, where the
// viewport draws into an image instead of a window surface), the viewport, every feature of
// libs/ui/src/features/, and the project folder given, or else an empty project. Without a
// Vulkan device the window still opens, with viewport() null and viewport_problem() saying why;
// it throws when CS_REQUIRE_GPU is set (as the GPU tests do), as cs::gpu::Device does for any
// other failure, and as open_project() does.
std::unique_ptr<MainWindow>
make_main_window(const std::optional<std::filesystem::path>& project = std::nullopt);

} // namespace cs::ui
