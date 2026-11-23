// SPDX-License-Identifier: Apache-2.0
//
// The walking skeleton (#88, M1-27), from the issue's acceptance criteria and the owner's comments
// on it: Room > New room (width, depth and height, 6, 4 and 3.5 m by default); Works > Add painting
// (a file, a width and a height, hung on wall A at the centre line in a box frame, side by side
// with the works already there, the file stored under its SHA-256, the work titled from the file's
// name); Lights > Add spot (lights the work just added); orbit by default, Tab toggles walk (W, A,
// S, D and mouse look with the cursor captured; Esc, Tab and focus loss end it); and a lux panel
// showing each work's mean direct and total lux from engine::work_lux_grid, computed on a worker
// thread and up to date within 1 s of a change, equal to cs-render's lux report for the saved
// project within 1%.
//
// The window's parts are found by object name, as the task's contract names them: the dialogs
// "new_room_dialog" (QDoubleSpinBox "room_width", "room_depth", "room_height", metres) and
// "add_painting_dialog" (QLineEdit "painting_file", QPushButton "painting_choose", QDoubleSpinBox
// "painting_width" and "painting_height", centimetres); the dock "lux_panel" titled "Lux" with the
// QTreeWidget "lux_table" (title, "Direct (lx)", "Total (lx)", the exact means as doubles in
// Qt::UserRole) and the QLabel "lux_status" ("Calculating..." while a computation is pending or
// running). Errors are modal QMessageBoxes.
//
// Every dialog and message box is modal, so the tests answer them from a timer that runs inside
// the dialog's event loop (ModalAnswerer, below): it records what it needs and never asserts, and
// the test checks what it recorded once the menu item's trigger() has returned.
//
// The tests that need no viewport use the application's window (make_main_window: features
// registered, and with no Vulkan device no viewport) and run on any machine; those that drive the
// camera need the viewport and are tagged [gpu]. Only one window of make_main_window is alive at a
// time (ui_test.hpp).

#include "ui_test.hpp"
#include "viewport/viewport.hpp"

#include <cs/core/thread_pool.hpp>
#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>
#include <cs/editor/camera_controller.hpp>
#include <cs/editor/commands/hanging.hpp>
#include <cs/editor/commands/room.hpp>
#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/textures.hpp>
#include <cs/engine/work_lux.hpp>
#include <cs/ui/main_window.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaType>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QWidget>
#include <QWindow>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::ui::MainWindow;
using cs::ui::Viewport;
namespace doc = cs::doc;
namespace fs = std::filesystem;

// ---- Waiting ---------------------------------------------------------------------------------

// How long a dialog or message box may take to appear and be answered, and how long the lux panel
// may take to be up to date before a test stops waiting. Generous, for lavapipe and the
// sanitizers of the debug build; qWaitFor returns as soon as the condition holds, so the bound
// costs only when a test fails. The 1 s of the issue is checked separately, by the clock.
constexpr int modal_timeout_ms = 20000;
constexpr int lux_timeout_ms = 60000;
// How long a frame may take beyond the viewport's settling (as test_document_changes.cpp).
constexpr int frame_margin_ms = 15000;

// ---- The test chart --------------------------------------------------------------------------

// The skeleton's test chart: its file name is its SHA-256 (ADR 0007), so it is also the name the
// project stores it under.
const std::string chart_sha256 = "26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a";

fs::path skeleton_chart() {
    return cs::ui::test::skeleton_folder() / "assets" / (chart_sha256 + ".png");
}

// The chart copied into a fresh folder of ui-out/ under another name: the photograph a curator
// picks from anywhere.
fs::path picked_chart(const std::string& folder_name, const std::string& file_name) {
    const fs::path picked = cs::ui::test::fresh_out_folder(folder_name) / file_name;
    fs::copy_file(skeleton_chart(), picked);
    return picked;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Every file in a folder, by name, not recursing; none when there is no folder.
std::vector<std::string> file_names(const fs::path& folder) {
    std::vector<std::string> names;
    if (!fs::is_directory(folder)) {
        return names;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

QString qstring(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

// ---- Modal dialogs and message boxes ---------------------------------------------------------

// A modal widget as it was when it was answered.
struct SeenModal {
    std::string class_name;
    std::string object_name;
    bool message_box = false;
    std::string text;        // a message box's text and informative text
    bool unexpected = false; // beyond the answers given: rejected unread
};

std::string describe(const SeenModal& seen) {
    return seen.class_name + " \"" + seen.object_name + "\"" +
           (seen.message_box ? " saying \"" + seen.text + "\"" : std::string()) +
           (seen.unexpected ? " (unexpected)" : "");
}

SeenModal describe_modal(QWidget& modal) {
    SeenModal seen;
    seen.class_name = modal.metaObject()->className();
    seen.object_name = modal.objectName().toStdString();
    if (const auto* box = qobject_cast<QMessageBox*>(&modal)) {
        seen.message_box = true;
        seen.text = (box->text() + box->informativeText()).toStdString();
    }
    return seen;
}

void close_modal(QWidget& modal, bool accept) {
    if (auto* dialog = qobject_cast<QDialog*>(&modal)) {
        if (accept) {
            dialog->accept();
        } else {
            dialog->reject();
        }
    } else {
        modal.close();
    }
}

// Reads what a test needs from a modal widget and fills it in; returns true to accept it, false to
// reject it. Never asserts: it runs inside the modal's event loop.
using Answer = std::function<bool(QWidget& modal)>;

// Answers the modal widgets that open while it lives, in order: the first with answers[0], the
// next with answers[1], and any beyond them by rejecting it, marked unexpected, so that a test
// never hangs on a modal it did not expect. A timer polls QApplication::activeModalWidget(), so
// it answers a dialog run by exec() (inside trigger()) and one opened by open() (in finish()).
// The modal is accepted or rejected from a queued call, after the answer has returned, so that a
// message box the dialog's accept() opens is answered in turn.
class ModalAnswerer {
  public:
    explicit ModalAnswerer(std::vector<Answer> answers = {}) : state_(std::make_shared<State>()) {
        state_->answers = std::move(answers);
        timer_.setInterval(5);
        QObject::connect(&timer_, &QTimer::timeout, [state = state_] { poll(state); });
        timer_.start();
    }
    ~ModalAnswerer() {
        timer_.stop();
        close_open_modals();
    }
    ModalAnswerer(const ModalAnswerer&) = delete;
    ModalAnswerer& operator=(const ModalAnswerer&) = delete;
    ModalAnswerer(ModalAnswerer&&) = delete;
    ModalAnswerer& operator=(ModalAnswerer&&) = delete;

    // Waits until every answer has been given and no modal is open; then stops answering, and
    // rejects any modal still open. True when every answer was given and none had to be closed.
    bool finish(int timeout_ms = modal_timeout_ms) {
        const bool answered = QTest::qWaitFor(
            [this] {
                return state_->used >= state_->answers.size() &&
                       QApplication::activeModalWidget() == nullptr;
            },
            timeout_ms);
        timer_.stop();
        const bool left_open = close_open_modals();
        return answered && !left_open;
    }

    [[nodiscard]] const std::vector<SeenModal>& seen() const { return state_->seen; }

    [[nodiscard]] std::size_t unexpected() const {
        return static_cast<std::size_t>(
            std::count_if(state_->seen.begin(), state_->seen.end(),
                          [](const SeenModal& s) { return s.unexpected; }));
    }

    // The first message box answered, if any.
    [[nodiscard]] std::optional<SeenModal> message_box() const {
        const auto it = std::find_if(state_->seen.begin(), state_->seen.end(),
                                     [](const SeenModal& s) { return s.message_box; });
        return it == state_->seen.end() ? std::nullopt : std::optional<SeenModal>(*it);
    }

    [[nodiscard]] std::string log() const {
        std::string text;
        for (const SeenModal& seen : state_->seen) {
            text += describe(seen) + "\n";
        }
        return text.empty() ? "(no modal)" : text;
    }

  private:
    struct State {
        std::vector<Answer> answers;
        std::size_t used = 0;
        std::vector<SeenModal> seen;
        std::vector<QPointer<QWidget>> closing; // answered, their close queued
    };

    static void poll(const std::shared_ptr<State>& state) {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr || !modal->isVisible()) {
            return;
        }
        for (const QPointer<QWidget>& closing : state->closing) {
            if (closing == modal) {
                return;
            }
        }
        SeenModal seen = describe_modal(*modal);
        bool accept = false;
        if (state->used < state->answers.size()) {
            accept = state->answers[state->used](*modal);
        } else {
            seen.unexpected = true;
        }
        ++state->used;
        state->seen.push_back(std::move(seen));
        state->closing.emplace_back(modal);
        const QPointer<QWidget> target(modal);
        QTimer::singleShot(0, modal, [state, target, accept] {
            if (target.isNull()) {
                return;
            }
            std::erase_if(state->closing,
                          [&](const QPointer<QWidget>& widget) { return widget == target; });
            close_modal(*target, accept);
        });
    }

    // Rejects every modal still open, recording each as unexpected. True when there was one.
    bool close_open_modals() {
        bool any = false;
        for (int i = 0; i < 16; ++i) {
            QWidget* modal = QApplication::activeModalWidget();
            if (modal == nullptr) {
                break;
            }
            any = true;
            SeenModal seen = describe_modal(*modal);
            seen.unexpected = true;
            state_->seen.push_back(std::move(seen));
            close_modal(*modal, false);
            QApplication::processEvents();
        }
        return any;
    }

    std::shared_ptr<State> state_;
    QTimer timer_;
};

// Accepts a message box (or any modal) as it is.
Answer accept_as_is() {
    return [](QWidget&) { return true; };
}

// What New room's dialog showed.
struct NewRoomDialog {
    bool fields = false; // the three spin boxes were there
    double width_m = 0.0;
    double depth_m = 0.0;
    double height_m = 0.0;
};

// Reads New room's fields and accepts their defaults.
Answer new_room_answer(NewRoomDialog& seen) {
    return [&seen](QWidget& modal) {
        const auto* width = modal.findChild<QDoubleSpinBox*>(QStringLiteral("room_width"));
        const auto* depth = modal.findChild<QDoubleSpinBox*>(QStringLiteral("room_depth"));
        const auto* height = modal.findChild<QDoubleSpinBox*>(QStringLiteral("room_height"));
        seen.fields = width != nullptr && depth != nullptr && height != nullptr;
        if (!seen.fields) {
            return false;
        }
        seen.width_m = width->value();
        seen.depth_m = depth->value();
        seen.height_m = height->value();
        return true;
    };
}

// What Add painting's dialog showed.
struct PaintingDialog {
    bool fields = false; // the line edit, the button and both spin boxes were there
    double width_cm = 0.0;
    double height_cm = 0.0;
};

struct SizeCm {
    double width_cm = 0.0;
    double height_cm = 0.0;
};

// Fills in Add painting's dialog with the file, and the size when one is given (otherwise its
// defaults), and accepts it. The file dialog (painting_choose) is not used: the path is typed.
Answer painting_answer(const fs::path& file, PaintingDialog& seen,
                       std::optional<SizeCm> size = std::nullopt) {
    return [&seen, file, size](QWidget& modal) {
        auto* path = modal.findChild<QLineEdit*>(QStringLiteral("painting_file"));
        const auto* choose = modal.findChild<QPushButton*>(QStringLiteral("painting_choose"));
        auto* width = modal.findChild<QDoubleSpinBox*>(QStringLiteral("painting_width"));
        auto* height = modal.findChild<QDoubleSpinBox*>(QStringLiteral("painting_height"));
        seen.fields = path != nullptr && choose != nullptr && width != nullptr && height != nullptr;
        if (!seen.fields) {
            return false;
        }
        seen.width_cm = width->value();
        seen.height_cm = height->value();
        path->setText(qstring(file));
        if (size) {
            width->setValue(size->width_cm);
            height->setValue(size->height_cm);
        }
        return true;
    };
}

// ---- Menus -----------------------------------------------------------------------------------

QString new_room_text() {
    return QStringLiteral("New room") + QChar(0x2026);
}
QString add_painting_text() {
    return QStringLiteral("Add painting") + QChar(0x2026);
}
QString add_spot_text() {
    return QStringLiteral("Add spot");
}

// The item of a menu of the menu bar, by their texts as a reader sees them (plain_text).
QAction* menu_action(MainWindow& window, const QString& menu_title, const QString& item) {
    INFO("the menu bar:\n" << cs::ui::test::to_string(cs::ui::test::menu_bar_layout(window)));
    QMenu* menu = cs::ui::test::find_menu(window, menu_title);
    REQUIRE(menu != nullptr);
    for (QAction* action : menu->actions()) {
        if (!action->isSeparator() && cs::ui::test::plain_text(action->text()) == item) {
            return action;
        }
    }
    FAIL("no item \"" << item.toStdString() << "\" in the menu \"" << menu_title.toStdString()
                      << "\"");
    return nullptr;
}

// ---- The document ----------------------------------------------------------------------------

template <class Predicate>
std::vector<doc::Uuid> entities_where(const doc::Document& document, Predicate predicate) {
    std::vector<doc::Uuid> ids;
    for (const auto& [id, entity] : document.entities()) {
        if (predicate(entity)) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::vector<doc::Uuid> rooms_of(const doc::Document& document) {
    return entities_where(document, [](const doc::Entity& e) { return e.room.has_value(); });
}
std::vector<doc::Uuid> luminaires_of(const doc::Document& document) {
    return entities_where(document, [](const doc::Entity& e) { return e.luminaire.has_value(); });
}

// The entity that shows a work.
doc::Uuid exhibit_of_work(const doc::Document& document, const doc::Uuid& work) {
    const std::vector<doc::Uuid> exhibits = entities_where(document, [&](const doc::Entity& e) {
        return e.exhibit.has_value() && e.exhibit->work == work;
    });
    REQUIRE(exhibits.size() == 1);
    return exhibits.front();
}

const doc::OnWall& on_wall_of(const doc::Document& document, const doc::Uuid& entity_id) {
    const doc::Entity* entity = document.find_entity(entity_id);
    REQUIRE(entity != nullptr);
    REQUIRE(entity->placement.has_value());
    const auto* at = std::get_if<doc::OnWall>(&*entity->placement);
    REQUIRE(at != nullptr);
    return *at;
}

std::set<doc::Uuid> work_ids(const doc::Document& document) {
    std::set<doc::Uuid> ids;
    for (const auto& [id, work] : document.works()) {
        ids.insert(id);
    }
    return ids;
}

// ---- The three commands, through the menus ---------------------------------------------------

// Room > New room..., accepted with its defaults.
void build_room(MainWindow& window) {
    QAction* action = menu_action(window, QStringLiteral("Room"), new_room_text());
    NewRoomDialog dialog;
    ModalAnswerer answers({new_room_answer(dialog)});
    action->trigger();
    const bool finished = answers.finish();
    INFO("modals:\n" << answers.log());
    REQUIRE(finished);
    REQUIRE(answers.unexpected() == 0);
    REQUIRE(dialog.fields);
    REQUIRE(rooms_of(window.document()).size() == 1);
}

// Works > Add painting... with the file, at the dialog's default size or the one given. Returns
// the new work's id.
doc::Uuid add_painting(MainWindow& window, const fs::path& file,
                       std::optional<SizeCm> size = std::nullopt) {
    QAction* action = menu_action(window, QStringLiteral("Works"), add_painting_text());
    const std::set<doc::Uuid> before = work_ids(window.document());
    PaintingDialog dialog;
    ModalAnswerer answers({painting_answer(file, dialog, size)});
    action->trigger();
    const bool finished = answers.finish();
    INFO("modals:\n" << answers.log());
    REQUIRE(finished);
    REQUIRE(answers.unexpected() == 0);
    REQUIRE(dialog.fields);
    std::vector<doc::Uuid> added;
    for (const doc::Uuid& id : work_ids(window.document())) {
        if (!before.contains(id)) {
            added.push_back(id);
        }
    }
    REQUIRE(added.size() == 1);
    return added.front();
}

// Lights > Add spot, which asks nothing.
void add_spot(MainWindow& window) {
    QAction* action = menu_action(window, QStringLiteral("Lights"), add_spot_text());
    const std::size_t before = luminaires_of(window.document()).size();
    ModalAnswerer answers;
    action->trigger();
    const bool finished = answers.finish();
    INFO("modals:\n" << answers.log());
    REQUIRE(finished);
    REQUIRE(answers.seen().empty());
    REQUIRE(luminaires_of(window.document()).size() == before + 1);
}

// ---- The windows -----------------------------------------------------------------------------

// The application's window, with or without a viewport (no Vulkan device): its menus and lux panel
// work either way. Fails when make_main_window throws, as it does under CS_REQUIRE_GPU without a
// device.
std::unique_ptr<MainWindow> app_window() {
    cs::ui::test::require_offscreen();
    std::unique_ptr<MainWindow> window;
    try {
        window = cs::ui::make_main_window();
    } catch (const std::exception& e) {
        FAIL("make_main_window() threw: " << e.what());
    }
    REQUIRE(window != nullptr);
    return window;
}

// The application's window with its viewport, headless, shown and active, with a line edit as an
// inspector page (another widget that takes the keyboard focus, and Tab's next stop), as in
// test_viewport_input.cpp.
struct Shell {
    std::unique_ptr<MainWindow> window;
    Viewport* viewport = nullptr;
    QLineEdit* line_edit = nullptr;
};

// The input tests add their line edit as an inspector page and use a size of their own; the
// documentation's screenshot shows the window as a curator sees it, at its own size.
enum class ShellFor { input_tests, documentation };

Shell shown_shell(ShellFor use = ShellFor::input_tests) {
    cs::ui::test::require_offscreen();
    Shell shell;
    shell.window = cs::ui::test::make_shell();
    shell.viewport = shell.window->viewport();
    REQUIRE(shell.viewport->headless());
    if (use == ShellFor::input_tests) {
        shell.line_edit = new QLineEdit;
        shell.window->add_inspector_page(10, QStringLiteral("Test input"), shell.line_edit);
        shell.window->resize(1024, 700);
    }
    shell.window->show();
    shell.window->activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(shell.window.get()));
    REQUIRE(shell.viewport->width() > 300);
    REQUIRE(shell.viewport->height() > 200);
    return shell;
}

QPoint in_top_level(const Shell& shell, QPoint viewport_position) {
    return shell.viewport->mapTo(shell.window.get(), viewport_position);
}

// A click on the viewport, which gives it the keyboard focus (viewport.hpp). The offscreen platform
// has no window manager to make the window active again once a command's dialog has closed, as a
// desktop does, and a widget of an inactive window has no focus: so the window is activated first.
void focus_viewport(Shell& shell) {
    shell.window->activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(shell.window.get()));
    QTest::mouseClick(shell.viewport, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    REQUIRE(QTest::qWaitFor([&] { return shell.viewport->hasFocus(); }, 1000));
}

// ---- The lux panel ---------------------------------------------------------------------------

struct LuxPanel {
    QDockWidget* dock = nullptr;
    QTreeWidget* table = nullptr;
    QLabel* status = nullptr;
};

LuxPanel lux_panel(MainWindow& window) {
    LuxPanel panel;
    panel.dock = window.findChild<QDockWidget*>(QStringLiteral("lux_panel"));
    REQUIRE(panel.dock != nullptr);
    panel.table = panel.dock->findChild<QTreeWidget*>(QStringLiteral("lux_table"));
    REQUIRE(panel.table != nullptr);
    panel.status = panel.dock->findChild<QLabel*>(QStringLiteral("lux_status"));
    REQUIRE(panel.status != nullptr);
    return panel;
}

bool calculating(const LuxPanel& panel) {
    return panel.status->text().startsWith(QStringLiteral("Calculating"));
}

bool wait_for_lux(const LuxPanel& panel) {
    return QTest::qWaitFor([&] { return !calculating(panel); }, lux_timeout_ms);
}

struct LuxRow {
    std::string title;
    bool doubles = false; // both columns hold a double in Qt::UserRole
    double direct_lx = 0.0;
    double total_lx = 0.0;
    std::string direct_text;
    std::string total_text;
};

std::vector<LuxRow> lux_rows(const LuxPanel& panel) {
    std::vector<LuxRow> rows;
    for (int i = 0; i < panel.table->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* item = panel.table->topLevelItem(i);
        const QVariant direct = item->data(1, Qt::UserRole);
        const QVariant total = item->data(2, Qt::UserRole);
        LuxRow row;
        row.title = item->text(0).toStdString();
        row.doubles = direct.metaType().id() == QMetaType::Double &&
                      total.metaType().id() == QMetaType::Double;
        row.direct_lx = direct.toDouble();
        row.total_lx = total.toDouble();
        row.direct_text = item->text(1).toStdString();
        row.total_text = item->text(2).toStdString();
        rows.push_back(std::move(row));
    }
    return rows;
}

// cs-render's lux report for one exhibit of a saved project, as apps/cs-render does it: load and
// compile the folder, load the photographs of its assets/, and read work_lux_grid with 1024
// samples and seed 1 (cs-render's defaults). Results do not depend on the thread count.
cs::engine::WorkLux cs_render_lux(const fs::path& folder) {
    const doc::Document document = doc::load_project_folder(folder);
    const cs::world::World world = cs::world::compile(document);
    const cs::engine::LoadedTextures textures = cs::engine::load_textures(world, folder / "assets");
    CHECK(textures.problems.empty());
    CHECK(textures.textures.size() == 1);
    REQUIRE(world.exhibits.size() == 1);
    const cs::engine::Scene scene(world);
    cs::ThreadPool pool(std::max(1u, std::thread::hardware_concurrency()));
    cs::engine::LuxOptions options;
    options.samples = 1024;
    options.seed = 1;
    options.pool = &pool;
    options.textures = &textures.textures;
    return cs::engine::work_lux_grid(scene, world.exhibits.front(), options);
}

// ---- Cameras ---------------------------------------------------------------------------------

std::string to_string(glm::dvec3 v) {
    return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) +
           ")";
}

void check_near(glm::dvec3 actual, glm::dvec3 expected, double tolerance) {
    INFO("actual " << to_string(actual) << ", expected " << to_string(expected));
    CHECK_THAT(actual.x, WithinAbs(expected.x, tolerance));
    CHECK_THAT(actual.y, WithinAbs(expected.y, tolerance));
    CHECK_THAT(actual.z, WithinAbs(expected.z, tolerance));
}

// The signed angle in plan from a's direction to b's, about +z: negative when b is turned to the
// right of a, seen from above.
double turn_in_plan_rad(glm::dvec3 a, glm::dvec3 b) {
    return std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
}

// A camera that a controller builds is exact but for the rounding of its trigonometry, about
// 1e-15; a micrometre (and a microradian for unit vectors) is far below anything a curator could
// see and far above that rounding.
constexpr double camera_m = 1e-6;
constexpr double camera_unit = 1e-6;

// The issue's room, new_room(6, 4, 3.5): wall A along y = 0. Standing inside it at eye height
// (the default project's 1.6 m above the floor at 0) and looking along -y, towards wall A.
void check_camera_faces_wall_a(const cs::world::Camera& camera, const doc::Document& document) {
    INFO("camera at " << to_string(camera.position_m) << " looking along "
                      << to_string(camera.forward));
    CHECK(camera.position_m.x > 0.0);
    CHECK(camera.position_m.x < 6.0);
    CHECK(camera.position_m.y > 0.0);
    CHECK(camera.position_m.y < 4.0);
    CHECK_THAT(camera.position_m.z, WithinAbs(document.settings().eye_height_m, camera_m));
    check_near(camera.forward, {0.0, -1.0, 0.0}, camera_unit);
}

} // namespace

// ==== Menus ===================================================================================

// Acceptance criteria: Room > New room, Works > Add painting, Lights > Add spot. The menus come
// after File, in that order.
TEST_CASE("Walking skeleton: Room, Works and Lights follow File, with New room..., Add "
          "painting... and Add spot",
          "[ui]") {
    std::unique_ptr<MainWindow> window = app_window();
    const auto layout = cs::ui::test::menu_bar_layout(*window);
    INFO("the menu bar:\n" << cs::ui::test::to_string(layout));
    const auto index_of = [&](const QString& title) {
        const auto it = std::find_if(layout.begin(), layout.end(),
                                     [&](const auto& menu) { return menu.title == title; });
        return it == layout.end() ? -1 : static_cast<int>(it - layout.begin());
    };
    const int file = index_of(QStringLiteral("File"));
    const int room = index_of(QStringLiteral("Room"));
    const int works = index_of(QStringLiteral("Works"));
    const int lights = index_of(QStringLiteral("Lights"));
    REQUIRE(file == 0);
    REQUIRE(room > file);
    REQUIRE(works > room);
    REQUIRE(lights > works);
    CHECK(layout[static_cast<std::size_t>(room)].items.contains(new_room_text()));
    CHECK(layout[static_cast<std::size_t>(works)].items.contains(add_painting_text()));
    CHECK(layout[static_cast<std::size_t>(lights)].items.contains(add_spot_text()));
}

// ==== Room > New room =========================================================================

// Acceptance criterion: "Room > New room: width, depth and height fields (defaults 6, 4, 3.5 m)
// create the room". The owner's comment: a second New room overlaps the first, so it is disabled
// once the document has a room. After it, the camera stands in the room facing wall A.
TEST_CASE("Walking skeleton: New room... asks for width, depth and height, 6, 4 and 3.5 m by "
          "default, and makes that room; then New room... is disabled",
          "[ui]") {
    std::unique_ptr<MainWindow> window = app_window();
    QAction* new_room = menu_action(*window, QStringLiteral("Room"), new_room_text());
    REQUIRE(new_room->isEnabled());

    NewRoomDialog dialog;
    ModalAnswerer answers({new_room_answer(dialog)});
    new_room->trigger();
    const bool finished = answers.finish();
    INFO("modals:\n" << answers.log());
    REQUIRE(finished);
    REQUIRE(answers.seen().size() == 1);
    CHECK(answers.seen()[0].object_name == "new_room_dialog");
    REQUIRE(dialog.fields);
    // The defaults, exactly: spin boxes in metres.
    CHECK(dialog.width_m == 6.0);
    CHECK(dialog.depth_m == 4.0);
    CHECK(dialog.height_m == 3.5);

    // One room, 6 x 4 m, its ceiling at 3.5 m, and walls A to D: editor::new_room(6, 4, 3.5).
    const doc::Document& document = window->document();
    const std::vector<doc::Uuid> rooms = rooms_of(document);
    REQUIRE(rooms.size() == 1);
    const doc::Room& room = *document.find_entity(rooms.front())->room;
    const std::vector<glm::dvec2> outline{{0.0, 0.0}, {6.0, 0.0}, {6.0, 4.0}, {0.0, 4.0}};
    REQUIRE(room.outline_m.size() == outline.size());
    for (std::size_t i = 0; i < outline.size(); ++i) {
        CAPTURE(i);
        // The sizes as given, exact but for a unit conversion of a few ulps.
        CHECK_THAT(room.outline_m[i].x, WithinAbs(outline[i].x, 1e-9));
        CHECK_THAT(room.outline_m[i].y, WithinAbs(outline[i].y, 1e-9));
    }
    CHECK(room.floor_height_m == 0.0);
    CHECK_THAT(room.ceiling_height_m, WithinAbs(3.5, 1e-9));
    for (const char* label : {"A", "B", "C", "D"}) {
        CAPTURE(label);
        CHECK(cs::editor::find_wall_labelled(document, label).has_value());
    }
    CHECK(document.entities().size() == 5);
    // One step to undo, named as the command names it.
    CHECK(window->history().undo_count() == 1);
    CHECK(window->history().undo_name().value_or("") == "New room");

    // A second room would overlap the first: New room is disabled while the document has a room.
    CHECK_FALSE(new_room->isEnabled());

    // The camera, when there is a viewport, stands in the room and faces wall A.
    if (Viewport* viewport = window->viewport()) {
        check_camera_faces_wall_a(viewport->camera(), window->document());
    }

    // "Once the document has a room": with the room undone there is none, and New room is
    // available again; a project opened with a room has one.
    window->undo();
    REQUIRE(rooms_of(window->document()).empty());
    CHECK(new_room->isEnabled());
    window->open_project(cs::ui::test::skeleton_folder());
    REQUIRE_FALSE(rooms_of(window->document()).empty());
    CHECK_FALSE(new_room->isEnabled());
    window->new_project();
    CHECK(new_room->isEnabled());
}

// ==== Works > Add painting ====================================================================

// Acceptance criteria: Add painting's file dialog and width and height fields hang the work on
// wall A at the centre line in a box frame; the picked file is stored with assets::store_image in
// window.assets_folder() (the owner's comments of M1-47 and M1-26: "the offscreen test adds a
// painting from Photo.PNG and finds one file in assets/"), named <sha256>.png; the work is titled
// from the file's name and its source is FromImport with that name.
TEST_CASE("Walking skeleton: Add painting... stores Photo.PNG under its SHA-256 and hangs a "
          "73 x 92 cm work titled Photo on wall A at the centre line in a box frame",
          "[ui]") {
    std::unique_ptr<MainWindow> window = app_window();
    build_room(*window);
    const fs::path photo = picked_chart("walking-skeleton-photo", "Photo.PNG");

    QAction* action = menu_action(*window, QStringLiteral("Works"), add_painting_text());
    PaintingDialog dialog;
    ModalAnswerer answers({painting_answer(photo, dialog)});
    action->trigger();
    const bool finished = answers.finish();
    INFO("modals:\n" << answers.log());
    REQUIRE(finished);
    REQUIRE(answers.seen().size() == 1);
    CHECK(answers.seen()[0].object_name == "add_painting_dialog");
    REQUIRE(dialog.fields);
    // The defaults, in centimetres: the skeleton chart's 73 x 92 cm.
    CHECK(dialog.width_cm == 73.0);
    CHECK(dialog.height_cm == 92.0);

    // The file, stored once under its content's name, its extension in lower case.
    const fs::path assets = window->assets_folder();
    CHECK(file_names(assets) == std::vector<std::string>{chart_sha256 + ".png"});
    CHECK(read_bytes(assets / (chart_sha256 + ".png")) == read_bytes(photo));
    // The picked file is left where it was.
    CHECK(fs::is_regular_file(photo));

    const doc::Document& document = window->document();
    REQUIRE(document.works().size() == 1);
    const auto& [work_id, work] = *document.works().begin();
    CHECK(work.title == "Photo");
    CHECK(work.source == doc::WorkSource{doc::FromImport{"Photo.PNG"}});
    CHECK(work.image == doc::AssetRef{chart_sha256, "png"});
    // 73 and 92 cm in metres: a division by 100, one rounding.
    CHECK_THAT(work.form.width_m, WithinAbs(0.73, 1e-12));
    CHECK_THAT(work.form.height_m, WithinAbs(0.92, 1e-12));

    const doc::Uuid exhibit = exhibit_of_work(document, work_id);
    const doc::Entity& entity = *document.find_entity(exhibit);
    REQUIRE(entity.framing.has_value());
    CHECK(entity.framing->style == cs::editor::default_frame_style);
    const doc::OnWall& at = on_wall_of(document, exhibit);
    CHECK(at.wall == cs::editor::find_wall_labelled(document, "A"));
    CHECK(at.face == doc::WallFace::a);
    CHECK(at.centre_height_m == document.settings().centre_line_m);
    // Alone on wall A: in the middle of its 6 m face (add_work_in_row), exact but for rounding.
    CHECK_THAT(at.distance_m, WithinAbs(3.0, 1e-9));

    CHECK(window->history().undo_count() == 2);
    CHECK(window->history().undo_name().value_or("") == "Add Photo");
}

// Acceptance criterion: "Several paintings hang side by side: ... Works > Add painting uses it
// [add_work_in_row]". Two paintings of the same photograph: one file in assets/.
TEST_CASE("Walking skeleton: two paintings added hang side by side on wall A, 0.4 m apart, the "
          "row centred",
          "[ui]") {
    std::unique_ptr<MainWindow> window = app_window();
    build_room(*window);
    const fs::path first_file = picked_chart("walking-skeleton-row-1", "Photo.PNG");
    const fs::path second_file = picked_chart("walking-skeleton-row-2", "Second_Photo.png");
    const doc::Uuid first = add_painting(*window, first_file);
    const doc::Uuid second = add_painting(*window, second_file, SizeCm{50.0, 40.0});

    const doc::Document& document = window->document();
    CHECK(document.find_work(first)->title == "Photo");
    CHECK(document.find_work(second)->title == "Second Photo");
    CHECK(document.find_work(second)->source ==
          doc::WorkSource{doc::FromImport{"Second_Photo.png"}});
    // The same photograph, stored once.
    CHECK(file_names(window->assets_folder()) == std::vector<std::string>{chart_sha256 + ".png"});

    const doc::Uuid first_exhibit = exhibit_of_work(document, first);
    const doc::Uuid second_exhibit = exhibit_of_work(document, second);
    const doc::OnWall& a = on_wall_of(document, first_exhibit);
    const doc::OnWall& b = on_wall_of(document, second_exhibit);
    const auto wall_a = cs::editor::find_wall_labelled(document, "A");
    CHECK(a.wall == wall_a);
    CHECK(b.wall == wall_a);
    CHECK(a.face == doc::WallFace::a);
    CHECK(b.face == doc::WallFace::a);
    CHECK(a.centre_height_m == document.settings().centre_line_m);
    CHECK(b.centre_height_m == document.settings().centre_line_m);

    // Box-framed, they take 0.73 + 0.11 = 0.84 m and 0.5 + 0.11 = 0.61 m of the wall; the row,
    // 0.84 + 0.4 + 0.61 = 1.85 m, starts at (6 - 1.85) / 2 = 2.075 m: centres at 2.495 m and
    // 3.62 m, the first added on the left. Exact but for rounding.
    CHECK_THAT(a.distance_m, WithinAbs(2.495, 1e-9));
    CHECK_THAT(b.distance_m, WithinAbs(3.62, 1e-9));
    const double gap_m = (b.distance_m - 0.61 / 2.0) - (a.distance_m + 0.84 / 2.0);
    CHECK_THAT(gap_m, WithinAbs(cs::editor::row_gap_m, 1e-9));
}

// Hostile input (CLAUDE.md) and the owner's M1-47 comment: "Show a std::invalid_argument (too
// large, not PNG or JPEG, not an image) to the curator as its message and add nothing."
TEST_CASE("Walking skeleton: a file that is not a PNG or JPEG image is refused with a message, "
          "and nothing is added",
          "[ui]") {
    std::unique_ptr<MainWindow> window = app_window();
    build_room(*window);
    const fs::path folder = cs::ui::test::fresh_out_folder("walking-skeleton-not-an-image");

    fs::path file;
    SECTION("a text file named notes.png") {
        file = folder / "notes.png";
        std::ofstream notes(file, std::ios::binary);
        notes << "These are notes, not an image.\n";
    }
    SECTION("a PNG named Photo.gif, an extension no tier loads") {
        file = folder / "Photo.gif";
        fs::copy_file(skeleton_chart(), file);
    }
    SECTION("a file that does not exist") {
        file = folder / "missing.png";
    }
    CAPTURE(file.string());
    const std::size_t entities = window->document().entities().size();
    const std::size_t steps = window->history().undo_count();

    QAction* action = menu_action(*window, QStringLiteral("Works"), add_painting_text());
    PaintingDialog dialog;
    // The dialog, then the message.
    ModalAnswerer answers({painting_answer(file, dialog), accept_as_is()});
    action->trigger();
    // A dialog that stays open after the message, for another try, is rejected by the answerer
    // as the curator would cancel it: either is allowed.
    answers.finish();
    INFO("modals:\n" << answers.log());
    REQUIRE_FALSE(answers.seen().empty());
    CHECK(answers.seen()[0].object_name == "add_painting_dialog");
    REQUIRE(dialog.fields);
    const std::optional<SeenModal> message = answers.message_box();
    REQUIRE(message.has_value());
    CHECK_FALSE(message->text.empty());

    CHECK(window->document().works().empty());
    CHECK(window->document().entities().size() == entities);
    CHECK(window->history().undo_count() == steps);
    // Nothing stored: store_image writes nothing when it refuses.
    CHECK(file_names(window->assets_folder()).empty());
}

// The owner's comment: "add_work throws if no wall is labelled A, for example after a rename;
// show that message." And Add spot before a painting has nothing to light.
TEST_CASE("Walking skeleton: Add painting with no wall A, and Add spot with no painting, show a "
          "message and add nothing",
          "[ui]") {
    std::unique_ptr<MainWindow> window = app_window();
    QString menu;
    QString item;

    SECTION("Add painting in an empty project") {
        menu = QStringLiteral("Works");
        item = add_painting_text();
    }
    SECTION("Add painting with wall A renamed") {
        build_room(*window);
        const std::optional<doc::Uuid> wall_a =
            cs::editor::find_wall_labelled(window->document(), "A");
        REQUIRE(wall_a.has_value());
        doc::Wall wall = *window->document().find_entity(*wall_a)->wall;
        wall.label = "Lobby";
        doc::Transaction rename(window->document(), "Rename wall");
        rename.set(*wall_a, wall);
        window->commit(std::move(rename));
        REQUIRE_FALSE(cs::editor::find_wall_labelled(window->document(), "A").has_value());
        menu = QStringLiteral("Works");
        item = add_painting_text();
    }
    SECTION("Add spot in an empty project") {
        menu = QStringLiteral("Lights");
        item = add_spot_text();
    }
    SECTION("Add spot in a room with no painting") {
        build_room(*window);
        menu = QStringLiteral("Lights");
        item = add_spot_text();
    }
    CAPTURE(menu.toStdString(), item.toStdString());
    const doc::Document before = window->document();
    const std::size_t steps = window->history().undo_count();

    QAction* action = menu_action(*window, menu, item);
    ModalAnswerer answers({accept_as_is()});
    action->trigger();
    const bool finished = answers.finish();
    INFO("modals:\n" << answers.log());
    CHECK(finished);
    // The message, and no dialog before it.
    REQUIRE(answers.seen().size() == 1);
    CHECK(answers.seen()[0].message_box);
    CHECK_FALSE(answers.seen()[0].text.empty());
    CHECK(window->document() == before);
    CHECK(window->history().undo_count() == steps);
}

// ==== Lights > Add spot =======================================================================

// Acceptance criterion: "Lights > Add spot lights the work just added (selection arrives with
// M2-14)": editor::add_spot_for on its exhibit, a luminaire aimed at it.
TEST_CASE("Walking skeleton: Add spot lights the work just added with a luminaire aimed at its "
          "exhibit",
          "[ui]") {
    std::unique_ptr<MainWindow> window = app_window();
    build_room(*window);
    add_painting(*window, picked_chart("walking-skeleton-spot-1", "Photo.PNG"));
    const doc::Uuid last = add_painting(
        *window, picked_chart("walking-skeleton-spot-2", "Second.png"), SizeCm{50.0, 40.0});
    const doc::Uuid last_exhibit = exhibit_of_work(window->document(), last);
    add_spot(*window);

    const doc::Document& document = window->document();
    const std::vector<doc::Uuid> luminaires = luminaires_of(document);
    REQUIRE(luminaires.size() == 1);
    const doc::Luminaire& luminaire = *document.find_entity(luminaires.front())->luminaire;
    CHECK(luminaire.aim == doc::Aim{doc::AimAtEntity{last_exhibit}});
    CHECK(window->history().undo_name().value_or("") == "Add spot");
}

// ==== The lux panel ===========================================================================

// Acceptance criterion: "A lux panel shows the work's mean direct and total lux from
// engine::work_lux_grid, computed on a worker thread and updated within 1 s of a change; [ui] an
// offscreen test runs the three commands and checks the panel's numbers equal cs-render's lux
// report for the saved project within 1%." The owner's M1-17 comment: with the photographs, loaded
// with engine::load_textures from the window's assets folder, the same seed and sample count.
//
// Tolerances: 1%, the issue's. The panel and cs-render read the same world (the saved project
// reads back equal), the same photograph, seed 1 and 1024 samples, so the numbers agree far more
// closely; the 1% allows for nothing but that. The 1 s is measured from the commit's return to
// the panel saying it is up to date, by the wall clock, polled every few milliseconds.
TEST_CASE("Walking skeleton: the lux panel shows the work's mean direct and total lux, computed "
          "off the UI thread within 1 s of a change, equal to cs-render's report within 1%",
          "[ui][accuracy]") {
    std::unique_ptr<MainWindow> window = app_window();
    const LuxPanel panel = lux_panel(*window);
    CHECK(panel.dock->windowTitle() == QStringLiteral("Lux"));
    REQUIRE(panel.table->columnCount() == 3);
    CHECK(panel.table->headerItem()->text(1) == QStringLiteral("Direct (lx)"));
    CHECK(panel.table->headerItem()->text(2) == QStringLiteral("Total (lx)"));

    build_room(*window);
    add_painting(*window, picked_chart("walking-skeleton-lux-photo", "Photo.PNG"));
    {
        INFO("lux status: " << panel.status->text().toStdString());
        REQUIRE(wait_for_lux(panel));
    }
    {
        // Unlit: no light at all, so exactly 0 lux, direct and total.
        const std::vector<LuxRow> rows = lux_rows(panel);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].title == "Photo");
        CHECK(rows[0].doubles);
        CHECK(rows[0].direct_lx == 0.0);
        CHECK(rows[0].total_lx == 0.0);
    }

    // The spot: the change whose lux the panel must show within 1 s.
    QAction* spot = menu_action(*window, QStringLiteral("Lights"), add_spot_text());
    REQUIRE_FALSE(calculating(panel));
    ModalAnswerer answers;
    spot->trigger();
    const auto committed = std::chrono::steady_clock::now();
    // Right after the commit the computation is pending or running, not done: it is not on the
    // UI thread, which has not run since.
    {
        INFO("lux status right after the commit: " << panel.status->text().toStdString());
        CHECK(calculating(panel));
    }
    const bool updated = wait_for_lux(panel);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - committed)
                                .count();
    WARN("the lux panel was up to date " << elapsed_ms << " ms after the spot's commit");
    REQUIRE(answers.finish());
    REQUIRE(answers.seen().empty());
    REQUIRE(luminaires_of(window->document()).size() == 1);
    REQUIRE(updated);
    CHECK(elapsed_ms <= 1000);

    const std::vector<LuxRow> rows = lux_rows(panel);
    REQUIRE(rows.size() == 1);
    const LuxRow& row = rows.front();
    CHECK(row.title == "Photo");
    CHECK(row.doubles);
    CHECK_FALSE(row.direct_text.empty());
    CHECK_FALSE(row.total_text.empty());

    // cs-render's report for the project as saved.
    const fs::path saved = cs::ui::test::fresh_out_folder("walking-skeleton-lux-saved");
    window->save_project(saved);
    const cs::engine::WorkLux report = cs_render_lux(saved);
    INFO("panel: direct " << row.direct_lx << " lx, total " << row.total_lx
                          << " lx; cs-render: direct " << report.direct_lux.mean << " lx, total "
                          << report.total_lux.mean << " lx");
    CHECK_THAT(row.direct_lx, WithinRel(report.direct_lux.mean, 0.01));
    CHECK_THAT(row.total_lx, WithinRel(report.total_lux.mean, 0.01));
    // Plausibly lit: an 800 lm spot about 2.3 m from the work gives hundreds of lux; 10 lx would
    // be a light that missed it.
    CHECK(report.direct_lux.mean > 10.0);
    CHECK(row.direct_lx > 10.0);
    CHECK(row.total_lx >= row.direct_lx);
}

// The panel recomputes on document_changed (undo is a change) and on project_changed (New, Open).
TEST_CASE("Walking skeleton: the lux panel follows undo, New and Open", "[ui][accuracy]") {
    std::unique_ptr<MainWindow> window = app_window();
    const LuxPanel panel = lux_panel(*window);
    build_room(*window);
    add_painting(*window, picked_chart("walking-skeleton-lux-follow", "Photo.PNG"));
    add_spot(*window);
    REQUIRE(wait_for_lux(panel));
    REQUIRE(lux_rows(panel).size() == 1);
    REQUIRE(lux_rows(panel).front().direct_lx > 10.0);
    const fs::path saved = cs::ui::test::fresh_out_folder("walking-skeleton-lux-follow-saved");
    window->save_project(saved);
    REQUIRE(wait_for_lux(panel));
    const cs::engine::WorkLux report = cs_render_lux(saved);

    // Undo the spot: no light, 0 lux. (Saving keeps the history.)
    REQUIRE(window->history().undo_name().value_or("") == "Add spot");
    window->undo();
    REQUIRE(wait_for_lux(panel));
    {
        const std::vector<LuxRow> rows = lux_rows(panel);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].direct_lx == 0.0);
        CHECK(rows[0].total_lx == 0.0);
    }

    // New: no work, no row.
    window->new_project();
    REQUIRE(wait_for_lux(panel));
    CHECK(panel.table->topLevelItemCount() == 0);

    // Open the saved project: its work, lit as cs-render reads it (the issue's 1%).
    window->open_project(saved);
    REQUIRE(wait_for_lux(panel));
    const std::vector<LuxRow> rows = lux_rows(panel);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].title == "Photo");
    CHECK_THAT(rows[0].direct_lx, WithinRel(report.direct_lux.mean, 0.01));
    CHECK_THAT(rows[0].total_lx, WithinRel(report.total_lux.mean, 0.01));
}

// ==== The camera ==============================================================================

// Acceptance criterion: "Orbit by default". Dragging with the left button turns the camera round
// the point it looks at; the wheel zooms towards it (editor::OrbitController).
TEST_CASE("Walking skeleton: orbit by default, dragging turns the camera round its target and "
          "the wheel zooms towards it",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    Viewport* viewport = shell.viewport;
    build_room(*shell.window);
    check_camera_faces_wall_a(viewport->camera(), shell.window->document());
    focus_viewport(shell);
    const cs::world::Camera start = viewport->camera();

    // 60 pixels to the right with the left button held.
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
    QTest::mouseMove(viewport, QPoint(260, 150));
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(260, 150));
    REQUIRE(QTest::qWaitFor([&] { return viewport->camera().forward != start.forward; }, 1000));
    const cs::world::Camera turned = viewport->camera();
    INFO("before: at " << to_string(start.position_m) << " looking along "
                       << to_string(start.forward) << "; after: at " << to_string(turned.position_m)
                       << " looking along " << to_string(turned.forward));
    // Turned to the right, as the pointer moved, about +z only.
    CHECK(turn_in_plan_rad(start.forward, turned.forward) < 0.0);
    CHECK_THAT(turned.forward.z, WithinAbs(start.forward.z, camera_unit));
    // Round one target at one distance: the two lines of sight meet, as far from each camera.
    const glm::dvec3 w = start.position_m - turned.position_m;
    const double b = glm::dot(start.forward, turned.forward);
    const double d = glm::dot(start.forward, w);
    const double e = glm::dot(turned.forward, w);
    REQUIRE(1.0 - b * b > 1e-6);
    const double t = (b * e - d) / (1.0 - b * b); // along the first line
    const double s = (e - b * d) / (1.0 - b * b); // along the second
    const glm::dvec3 target = start.position_m + t * start.forward;
    CHECK(t > 0.0);
    CHECK_THAT(s, WithinAbs(t, camera_m));
    check_near(turned.position_m + s * turned.forward, target, camera_m);

    // One notch of the wheel towards the target: 10% nearer (OrbitController::zoom_per_notch),
    // looking the same way.
    QTest::wheelEvent(shell.window->windowHandle(), QPointF(in_top_level(shell, {200, 150})),
                      QPoint(0, 120));
    REQUIRE(
        QTest::qWaitFor([&] { return viewport->camera().position_m != turned.position_m; }, 1000));
    const cs::world::Camera zoomed = viewport->camera();
    check_near(zoomed.forward, turned.forward, camera_unit);
    const glm::dvec3 moved = zoomed.position_m - turned.position_m;
    CHECK(glm::dot(moved, turned.forward) > 0.0);
    CHECK(glm::length(glm::cross(moved, turned.forward)) < camera_m);
    const double nearer_m = std::clamp(s * cs::editor::OrbitController::zoom_per_notch,
                                       cs::editor::OrbitController::min_distance_m,
                                       cs::editor::OrbitController::max_distance_m);
    CHECK_THAT(glm::length(moved), WithinAbs(s - nearer_m, camera_m));
    check_near(zoomed.position_m + nearer_m * zoomed.forward, target, camera_m);

    cs::ui::test::require_no_validation_messages(*shell.window);
}

// Acceptance criterion: "Tab toggles walk (W, A, S, D and mouse look with the cursor captured;
// Esc releases it)", with the owner's comments: walk mode uses the viewport's pointer lock and
// says "Esc to stop walking"; it ends on Esc, on Tab, and on focus loss (ADR 0011, point 10);
// Tab reaches the viewport rather than moving the focus; the view keeps its direction both ways
// (a WalkController from the orbit's camera, an OrbitController from the walk's).
TEST_CASE("Walking skeleton: Tab walks with the pointer captured, W walks forward, the pointer "
          "looks round, and Esc, Tab or focus loss end it, the view keeping its direction",
          "[ui][gpu]") {
    Shell shell = shown_shell();
    Viewport* viewport = shell.viewport;
    MainWindow& window = *shell.window;
    QWindow* top = window.windowHandle();
    REQUIRE(top != nullptr);
    build_room(window);
    focus_viewport(shell);
    const double eye_z_m = window.document().settings().eye_height_m;
    const QString walking_message = QStringLiteral("Esc to stop walking");

    const auto walking = [&] {
        return viewport->pointer_locked() &&
               window.statusBar()->currentMessage().contains(walking_message);
    };
    const auto check_focus_kept = [&](const char* after) {
        INFO("after " << after << ", the focus widget is "
                      << (QApplication::focusWidget() != nullptr
                              ? QApplication::focusWidget()->metaObject()->className()
                              : "none"));
        CHECK(viewport->hasFocus());
        CHECK_FALSE(shell.line_edit->hasFocus());
    };
    const auto tab = [&](const char* why) {
        INFO("Tab: " << why);
        QTest::keyClick(top, Qt::Key_Tab);
        check_focus_kept("Tab");
    };

    // ---- Orbit to walk: Tab. The view keeps its direction; the eye is at eye height.
    const cs::world::Camera orbiting = viewport->camera();
    REQUIRE_FALSE(viewport->pointer_locked());
    tab("start walking");
    REQUIRE(QTest::qWaitFor(walking, 1000));
    check_near(viewport->camera().forward, orbiting.forward, camera_unit);
    CHECK_THAT(viewport->camera().position_m.z, WithinAbs(eye_z_m, camera_m));

    // ---- Mouse look: the captured pointer moved 40 pixels right turns the view 40 x 0.005 =
    // 0.2 rad to the right (editor::turn_rad_per_px). Within 0.01 rad, two pixels, for any
    // rounding of the pointer's position.
    QApplication::processEvents();
    QScreen* screen = top->screen();
    const QPoint held_at = QCursor::pos(screen);
    const cs::world::Camera before_look = viewport->camera();
    QCursor::setPos(screen, held_at + QPoint(40, 0));
    QApplication::processEvents();
    REQUIRE(
        QTest::qWaitFor([&] { return viewport->camera().forward != before_look.forward; }, 1000));
    CHECK_THAT(turn_in_plan_rad(before_look.forward, viewport->camera().forward),
               WithinAbs(-40.0 * cs::editor::turn_rad_per_px, 0.01));
    CHECK(viewport->pointer_locked());

    // ---- W held walks forward in plan at 1.4 m/s. Generous: timers in a debug build under the
    // sanitizers fire late and a last partial step may be lost, so at least half the distance,
    // and never faster than walking (plus 5 cm for one step's rounding).
    const cs::world::Camera before_walk = viewport->camera();
    QElapsedTimer held;
    QTest::keyPress(top, Qt::Key_W);
    held.start();
    QTest::qWait(400);
    QTest::keyRelease(top, Qt::Key_W);
    const double held_s = static_cast<double>(held.elapsed()) / 1000.0;
    QApplication::processEvents();
    const cs::world::Camera after_walk = viewport->camera();
    const glm::dvec2 moved(after_walk.position_m.x - before_walk.position_m.x,
                           after_walk.position_m.y - before_walk.position_m.y);
    const glm::dvec2 ahead =
        glm::normalize(glm::dvec2(before_walk.forward.x, before_walk.forward.y));
    const double walked_m = glm::dot(moved, ahead);
    const double expected_m = cs::editor::WalkController::walk_speed_m_per_s * held_s;
    INFO("walked " << walked_m << " m in " << held_s << " s; at 1.4 m/s that is " << expected_m
                   << " m");
    CHECK(walked_m >= 0.5 * expected_m);
    CHECK(walked_m <= expected_m + 0.05);
    // Straight ahead, along the floor.
    CHECK(std::abs(moved.x * ahead.y - moved.y * ahead.x) < 1e-3);
    CHECK_THAT(after_walk.position_m.z, WithinAbs(eye_z_m, camera_m));
    check_near(after_walk.forward, before_walk.forward, camera_unit);
    check_focus_kept("walking");

    // ---- S, A and D: back, left and right of the view in plan, as generously. Left of a view
    // along (x, y), z up, is (-y, x). Short steps, so that the room's walls do not stop them.
    struct Step {
        Qt::Key key;
        const char* name;
        glm::dvec2 direction;
    };
    for (const Step& step :
         {Step{Qt::Key_S, "S", -ahead}, Step{Qt::Key_A, "A", {-ahead.y, ahead.x}},
          Step{Qt::Key_D, "D", {ahead.y, -ahead.x}}}) {
        INFO("key " << step.name);
        const glm::dvec3 from = viewport->camera().position_m;
        QElapsedTimer step_held;
        QTest::keyPress(top, step.key);
        step_held.start();
        QTest::qWait(200);
        QTest::keyRelease(top, step.key);
        const double step_s = static_cast<double>(step_held.elapsed()) / 1000.0;
        QApplication::processEvents();
        const glm::dvec3 to = viewport->camera().position_m;
        const glm::dvec2 step_moved(to.x - from.x, to.y - from.y);
        const double along_m = glm::dot(step_moved, step.direction);
        const double step_expected_m = cs::editor::WalkController::walk_speed_m_per_s * step_s;
        INFO("moved " << along_m << " m that way in " << step_s << " s");
        CHECK(along_m >= 0.5 * step_expected_m);
        CHECK(along_m <= step_expected_m + 0.05);
        CHECK(std::abs(step_moved.x * step.direction.y - step_moved.y * step.direction.x) < 1e-3);
        CHECK_THAT(to.z, WithinAbs(eye_z_m, camera_m));
    }

    // ---- Walk to orbit: Tab again. The view keeps its direction.
    const cs::world::Camera walked = viewport->camera();
    tab("stop walking");
    REQUIRE(QTest::qWaitFor([&] { return !viewport->pointer_locked(); }, 1000));
    CHECK_FALSE(window.statusBar()->currentMessage().contains(walking_message));
    check_near(viewport->camera().forward, walked.forward, camera_unit);

    // ---- Esc ends walk mode: sent to the top-level window, where the pointer lock takes it
    // (test_pointer_lock.cpp). The view keeps its direction here too.
    tab("start walking again");
    REQUIRE(QTest::qWaitFor(walking, 1000));
    const cs::world::Camera before_esc = viewport->camera();
    QTest::keyClick(top, Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !viewport->pointer_locked(); }, 1000));
    CHECK_FALSE(window.statusBar()->currentMessage().contains(walking_message));
    check_near(viewport->camera().forward, before_esc.forward, camera_unit);
    check_focus_kept("Esc");

    // ---- Focus loss ends walk mode (ADR 0011, point 10): the focus moves to another widget of
    // the same window, so the window itself stays active.
    tab("start walking a third time");
    REQUIRE(QTest::qWaitFor(walking, 1000));
    shell.line_edit->setFocus(Qt::OtherFocusReason);
    REQUIRE(QTest::qWaitFor([&] { return shell.line_edit->hasFocus(); }, 1000));
    CHECK(QTest::qWaitFor([&] { return !viewport->pointer_locked(); }, 1000));
    CHECK_FALSE(window.statusBar()->currentMessage().contains(walking_message));

    cs::ui::test::require_no_validation_messages(window);
}

// ==== The screenshot ==========================================================================

// Acceptance criterion: "docs/product/screens/m1-walking-skeleton.png (at most 300 KB) shows the
// result". The test makes it, to build/<preset>/ui-out/m1-walking-skeleton.png: the room built
// through the menus, the test chart added as a painting and lit, a few steps walked towards it,
// the lux panel up to date. Copying it to docs/ (and keeping it under 300 KB) is done by hand.
TEST_CASE("Walking skeleton: the screenshot of a room, a lit painting and its lux", "[ui][gpu]") {
    Shell shell = shown_shell(ShellFor::documentation);
    Viewport* viewport = shell.viewport;
    MainWindow& window = *shell.window;
    QWindow* top = window.windowHandle();
    REQUIRE(top != nullptr);
    const LuxPanel panel = lux_panel(window);

    build_room(window);
    add_painting(window, picked_chart("walking-skeleton-screenshot", "Test_chart.png"));
    add_spot(window);

    // A few steps towards wall A, then back to orbit.
    focus_viewport(shell);
    const int frames_before_walk = viewport->frames();
    const double y_before_m = viewport->camera().position_m.y;
    QTest::keyClick(top, Qt::Key_Tab);
    REQUIRE(QTest::qWaitFor([&] { return viewport->pointer_locked(); }, 1000));
    QTest::keyPress(top, Qt::Key_W);
    QTest::qWait(300);
    QTest::keyRelease(top, Qt::Key_W);
    QTest::keyClick(top, Qt::Key_Escape);
    REQUIRE(QTest::qWaitFor([&] { return !viewport->pointer_locked(); }, 1000));
    CHECK(viewport->camera().position_m.y < y_before_m);

    REQUIRE(wait_for_lux(panel));
    const std::vector<LuxRow> rows = lux_rows(panel);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].title == "Test chart");
    CHECK(rows[0].direct_lx > 10.0);
    CHECK(panel.dock->isVisible());

    // A frame of the walked view: drawn after the walk began, then a moment for the last one.
    REQUIRE(QTest::qWaitFor([&] { return viewport->frames() > frames_before_walk; },
                            Viewport::settle_ms + frame_margin_ms));
    QTest::qWait(Viewport::settle_ms);
    REQUIRE_FALSE(viewport->grab_frame().isNull());

    const QImage screenshot = window.screenshot();
    REQUIRE_FALSE(screenshot.isNull());
    const fs::path png = cs::ui::test::ui_out_dir() / "m1-walking-skeleton.png";
    fs::remove(png);
    REQUIRE(screenshot.save(qstring(png), "PNG"));
    CHECK_FALSE(QImage(qstring(png)).isNull());
    WARN("the screenshot is " << png.string() << ", " << fs::file_size(png) / 1024 << " KB");

    cs::ui::test::require_no_validation_messages(window);
}
