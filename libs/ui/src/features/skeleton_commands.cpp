// SPDX-License-Identifier: Apache-2.0
//
// The walking skeleton's commands (M1-27): Room > New room, Works > Add painting and Lights > Add
// spot, each asking what it needs, building editor's transaction and committing it through the
// window, so that one undo takes it back. A command that cannot be done says why in a message box
// and changes nothing.
//
// Selection arrives with M2-14, so Add spot lights the painting added last: the exhibit with the
// greatest id, since ids sort in the order they were made (doc/uuid.hpp). That holds whoever added
// it, and after undo, redo and Open.

#include "features/feature.hpp"
#include "viewport/viewport.hpp"

#include <cs/assets/project_assets.hpp>
#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>
#include <cs/editor/camera_controller.hpp>
#include <cs/editor/commands/hanging.hpp>
#include <cs/editor/commands/lighting.hpp>
#include <cs/editor/commands/room.hpp>
#include <cs/editor/work_title.hpp>
#include <cs/world/world.hpp>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSize>
#include <QStandardPaths>
#include <QStatusBar>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace cs::ui {

namespace {

// The menus' places in the menu bar, after View (feature.hpp).
constexpr int room_menu_order = 30;
constexpr int works_menu_order = 40;
constexpr int lights_menu_order = 50;

// New room's defaults, the skeleton scene's room. [C]
constexpr double default_room_width_m = 6.0;
constexpr double default_room_depth_m = 4.0;
constexpr double default_room_height_m = 3.5;
// Add painting's defaults, the skeleton scene's test chart. [C]
constexpr double default_painting_width_cm = 73.0;
constexpr double default_painting_height_cm = 92.0;
// After New room the curator stands this far in from the wall opposite A, facing A. [C]
constexpr double facing_distance_from_far_wall_m = 0.5;
// How long the status bar says what a command did.
constexpr int passing_message_ms = 10000;

// A message box that shows its text as written. Titles come from project files and names from
// the file system, which are hostile (CLAUDE.md); Qt would read markup in them as rich text, which
// loads <img> sources (on Windows from network shares) and opens links.
QMessageBox::StandardButton ask(MainWindow& window, QMessageBox::Icon icon, const QString& title,
                                const QString& text, QMessageBox::StandardButtons buttons,
                                QMessageBox::StandardButton default_button) {
    QMessageBox box(icon, title, text, buttons, &window);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(default_button);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

void warn(MainWindow& window, const QString& title, const QString& text) {
    ask(window, QMessageBox::Warning, title, text, QMessageBox::Ok, QMessageBox::Ok);
}

// What a command did, in the status bar: the viewport may not show it (a room stays dark until a
// light is added).
void tell(MainWindow& window, const QString& text) {
    window.statusBar()->showMessage(text, passing_message_ms);
}

QString quoted(const std::string& title) {
    return title.empty() ? MainWindow::tr("the untitled work")
                         : MainWindow::tr("“%1”").arg(QString::fromStdString(title));
}

// The wait cursor while the window cannot answer (a large photograph is hashed, decoded and
// uploaded on the UI thread).
class Busy {
  public:
    Busy() { QApplication::setOverrideCursor(Qt::WaitCursor); }
    ~Busy() { QApplication::restoreOverrideCursor(); }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
    Busy(Busy&&) = delete;
    Busy& operator=(Busy&&) = delete;
};

QString message_of(const std::exception& error) {
    return QString::fromUtf8(error.what());
}

QDoubleSpinBox* size_box(QWidget* parent, const char* name, double value, double minimum,
                         double maximum, int decimals, const QString& suffix) {
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setDecimals(decimals);
    box->setRange(minimum, maximum);
    box->setSuffix(suffix);
    box->setValue(value);
    box->setAlignment(Qt::AlignRight);
    return box;
}

QDialogButtonBox* ok_cancel(QDialog& dialog) {
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return buttons;
}

// ---- Room > New room -------------------------------------------------------------------------

struct RoomSize {
    double width_m = 0.0;
    double depth_m = 0.0;
    double height_m = 0.0;
};

std::optional<RoomSize> ask_room_size(MainWindow& window) {
    QDialog dialog(&window);
    dialog.setObjectName(QStringLiteral("new_room_dialog"));
    dialog.setWindowTitle(MainWindow::tr("New room"));
    const QString metres = QStringLiteral(" m");
    // Sizes a curator could mean, from a cabinet to a hall; centimetres are the finest step.
    QDoubleSpinBox* width =
        size_box(&dialog, "room_width", default_room_width_m, 1.0, 200.0, 2, metres);
    QDoubleSpinBox* depth =
        size_box(&dialog, "room_depth", default_room_depth_m, 1.0, 200.0, 2, metres);
    QDoubleSpinBox* height =
        size_box(&dialog, "room_height", default_room_height_m, 2.0, 30.0, 2, metres);
    auto* form = new QFormLayout(&dialog);
    form->addRow(MainWindow::tr("Width, along wall A:"), width);
    form->addRow(MainWindow::tr("Depth:"), depth);
    form->addRow(MainWindow::tr("Ceiling height:"), height);
    form->addRow(ok_cancel(dialog));
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return RoomSize{width->value(), depth->value(), height->value()};
}

// Stands the viewport's camera inside a new room (new_room's outline from the origin), at eye
// height, facing wall A along -y, so that the curator sees the wall paintings hang on. The default
// camera stands in the room's corner, edge-on to wall A.
void face_wall_a(MainWindow& window, const RoomSize& size) {
    Viewport* viewport = window.viewport();
    if (viewport == nullptr) {
        return;
    }
    world::Camera camera = viewport->camera();
    camera.entity.reset();
    camera.projection = world::Projection::perspective;
    camera.position_m = {
        size.width_m / 2.0,
        std::max(size.depth_m / 2.0, size.depth_m - facing_distance_from_far_wall_m),
        window.document().settings().eye_height_m};
    camera.forward = {0.0, -1.0, 0.0};
    camera.up = {0.0, 0.0, 1.0};
    if (!(camera.vertical_fov_rad > 0.0 && camera.vertical_fov_rad < std::numbers::pi)) {
        camera.vertical_fov_rad = editor::default_vertical_fov_rad;
    }
    viewport->set_camera(camera);
}

void new_room(MainWindow& window) {
    const std::optional<RoomSize> size = ask_room_size(window);
    if (!size) {
        return;
    }
    try {
        editor::NewRoom room = editor::new_room(window.document(), window.ids(), size->width_m,
                                                size->depth_m, size->height_m);
        window.commit(std::move(room.transaction));
    } catch (const std::exception& error) {
        warn(window, MainWindow::tr("Cannot make the room"), message_of(error));
        return;
    }
    face_wall_a(window, *size);
    const QLocale locale;
    tell(window,
         MainWindow::tr("Room built, %1 × %2 m and %3 m high, with walls A to D; you face "
                        "wall A. It stays dark until a light is added: Works > Add "
                        "painting…, then Lights > Add spot")
             .arg(locale.toString(size->width_m, 'f', 2), locale.toString(size->depth_m, 'f', 2),
                  locale.toString(size->height_m, 'f', 2)));
}

bool has_room(const doc::Document& document) {
    return std::ranges::any_of(document.entities(),
                               [](const auto& entry) { return entry.second.room.has_value(); });
}

// ---- Works > Add painting --------------------------------------------------------------------

struct PaintingChoice {
    QString file; // as the curator chose or typed it
    double width_m = 0.0;
    double height_m = 0.0;
};

std::optional<PaintingChoice> ask_painting(MainWindow& window) {
    QDialog dialog(&window);
    dialog.setObjectName(QStringLiteral("add_painting_dialog"));
    dialog.setWindowTitle(MainWindow::tr("Add painting"));

    auto* file = new QLineEdit(&dialog);
    file->setObjectName(QStringLiteral("painting_file"));
    file->setPlaceholderText(MainWindow::tr("A PNG or JPEG photograph of the work"));
    file->setMinimumWidth(320);
    auto* pixels = new QLabel(&dialog);
    pixels->setObjectName(QStringLiteral("painting_pixels"));

    // The unframed work, as a catalogue gives it; up to 20 m, millimetres the finest step.
    const QString centimetres = QStringLiteral(" cm");
    QDoubleSpinBox* width =
        size_box(&dialog, "painting_width", default_painting_width_cm, 1.0, 2000.0, 1, centimetres);
    QDoubleSpinBox* height = size_box(&dialog, "painting_height", default_painting_height_cm, 1.0,
                                      2000.0, 1, centimetres);

    auto* choose = new QPushButton(MainWindow::tr("Choose…"), &dialog);
    choose->setObjectName(QStringLiteral("painting_choose"));
    QObject::connect(
        choose, &QPushButton::clicked, &dialog, [&dialog, file, pixels, width, height] {
            // Every tier loads only PNG and JPEG (assets::asset_file); both cases of the extension,
            // since some platforms' dialogs match patterns case by case.
            const QString start =
                file->text().trimmed().isEmpty()
                    ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                    : file->text();
            const QString chosen = QFileDialog::getOpenFileName(
                &dialog, MainWindow::tr("Choose the painting's photograph"), start,
                MainWindow::tr("PNG and JPEG images (*.png *.jpg *.jpeg *.PNG *.JPG *.JPEG)"));
            if (chosen.isEmpty()) {
                return;
            }
            file->setText(chosen);
            // The photograph's proportions give the height for the width, until the curator types
            // the catalogue's; reading the size reads only the file's header.
            const QSize size = QImageReader(chosen).size();
            if (size.width() > 0 && size.height() > 0) {
                height->setValue(width->value() * size.height() / size.width());
                pixels->setText(
                    MainWindow::tr("%1 × %2 pixels: type the catalogue's width and height")
                        .arg(size.width())
                        .arg(size.height()));
            } else {
                pixels->clear();
            }
        });
    auto* file_row = new QHBoxLayout;
    file_row->addWidget(file, 1);
    file_row->addWidget(choose);

    QDialogButtonBox* buttons = ok_cancel(dialog);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);
    QObject::connect(file, &QLineEdit::textChanged, ok,
                     [ok](const QString& text) { ok->setEnabled(!text.trimmed().isEmpty()); });

    auto* form = new QFormLayout(&dialog);
    form->addRow(MainWindow::tr("Photograph:"), file_row);
    form->addRow(QString(), pixels);
    form->addRow(MainWindow::tr("Width, unframed:"), width);
    form->addRow(MainWindow::tr("Height, unframed:"), height);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted || file->text().trimmed().isEmpty()) {
        return std::nullopt;
    }
    return PaintingChoice{file->text().trimmed(), width->value() / 100.0, height->value() / 100.0};
}

// Returns the exhibit it hung, or none.
std::optional<doc::Uuid> add_painting(MainWindow& window) {
    // Asked first, so that the curator does not pick a photograph for nothing.
    if (!editor::find_wall_labelled(window.document(), editor::default_wall_label)) {
        warn(window, MainWindow::tr("Cannot add a painting"),
             MainWindow::tr("A painting hangs on wall A, and this project has no wall labelled A. "
                            "Make a room first: Room > New room."));
        return std::nullopt;
    }
    const std::optional<PaintingChoice> choice = ask_painting(window);
    if (!choice) {
        return std::nullopt;
    }
    const QString cannot_add = MainWindow::tr("Cannot add the painting");
    const QFileInfo info(choice->file);
    // Asked here in a curator's words, before store_image would refuse it in its own.
    const QString kind = info.suffix().toLower();
    if (kind != QStringLiteral("png") && kind != QStringLiteral("jpg") &&
        kind != QStringLiteral("jpeg")) {
        warn(window, cannot_add,
             MainWindow::tr("“%1” is not a PNG or JPEG photograph, the kinds the app reads. Save "
                            "it as PNG or JPEG and try again.")
                 .arg(info.fileName()));
        return std::nullopt;
    }
    // From Qt's text, not std::filesystem::path::u8string(), which throws for a name that is not
    // valid UTF-16 (Windows allows one); toUtf8() never throws, and title_from_file_name and
    // source_from_file_name make what it gives valid and bounded.
    const std::string name = info.fileName().toUtf8().toStdString();
    const std::string title = editor::title_from_file_name(name);
    // Tried first with a stand-in for the photograph, so that a painting the row cannot take (wall
    // A full) leaves no copy of its photograph in the project's assets.
    try {
        const doc::AssetRef stand_in{.sha256 = std::string(64, '0'), .extension = "png"};
        [[maybe_unused]] const editor::AddedWork trial = editor::add_work_in_row(
            window.document(), window.ids(), stand_in, choice->width_m, choice->height_m, title);
    } catch (const std::exception& error) {
        warn(window, cannot_add, message_of(error));
        return std::nullopt;
    }
    assets::AssetName asset;
    try {
        const Busy busy;
        asset = assets::store_image(std::filesystem::path(choice->file.toStdU16String()),
                                    window.assets_folder());
    } catch (const std::invalid_argument& error) {
        // Not an image the tiers can show: too large, or not decodable.
        warn(window, cannot_add,
             MainWindow::tr("“%1” cannot be shown as a painting: %2")
                 .arg(info.fileName(), message_of(error)));
        return std::nullopt;
    } catch (const std::exception& error) {
        warn(window, cannot_add,
             MainWindow::tr("“%1” cannot be read or stored in the project: %2")
                 .arg(info.fileName(), message_of(error)));
        return std::nullopt;
    }
    try {
        editor::AddedWork added = editor::add_work_in_row(
            window.document(), window.ids(), doc::AssetRef{asset.sha256, asset.extension},
            choice->width_m, choice->height_m, title);
        doc::Work work = *added.transaction.find_work(added.work);
        work.source = editor::source_from_file_name(name);
        added.transaction.set_work(added.work, std::move(work));
        {
            const Busy busy; // the photograph goes to the GPU
            window.commit(std::move(added.transaction));
        }
        tell(window,
             MainWindow::tr("%1 hangs on wall A. Lights > Add spot lights it").arg(quoted(title)));
        return added.exhibit;
    } catch (const std::exception& error) {
        warn(window, cannot_add, message_of(error));
        return std::nullopt;
    }
}

// ---- Lights > Add spot -----------------------------------------------------------------------

// The painting Add spot lights: the one Add painting hung last in this project, while it is still
// there; else (after undoing it, or in a project just opened) the exhibit with the greatest id. Ids
// grow only within one window's run: an opened project may hold greater ones, made on a machine
// whose clock was ahead. Selection replaces this (M2-14).
std::optional<doc::Uuid> painting_to_light(const doc::Document& document,
                                           const std::optional<doc::Uuid>& hung_last) {
    if (hung_last) {
        const doc::Entity* entity = document.find_entity(*hung_last);
        if (entity != nullptr && entity->exhibit) {
            return hung_last;
        }
    }
    std::optional<doc::Uuid> newest;
    for (const auto& [id, entity] : document.entities()) { // in order of id
        if (entity.exhibit) {
            newest = id;
        }
    }
    return newest;
}

// Whether a luminaire is aimed at the exhibit already.
bool lit(const doc::Document& document, const doc::Uuid& exhibit) {
    return std::ranges::any_of(document.entities(), [&exhibit](const auto& entry) {
        const std::optional<doc::Luminaire>& luminaire = entry.second.luminaire;
        return luminaire && luminaire->aim == doc::Aim{doc::AimAtEntity{exhibit}};
    });
}

void add_spot(MainWindow& window, const std::optional<doc::Uuid>& hung_last) {
    const doc::Document& document = window.document();
    const std::optional<doc::Uuid> exhibit = painting_to_light(document, hung_last);
    if (!exhibit) {
        warn(window, MainWindow::tr("Cannot add a spot"),
             MainWindow::tr("Add a painting first: Add spot lights the painting added last."));
        return;
    }
    const doc::Work* work = document.find_work(document.find_entity(*exhibit)->exhibit->work);
    const QString painting = quoted(work != nullptr ? work->title : std::string());
    // A second spot goes exactly where the first is, doubling the light: asked, Cancel first.
    if (lit(document, *exhibit) &&
        ask(window, QMessageBox::Question, MainWindow::tr("Add spot"),
            MainWindow::tr("A spot lights %1 already. Add a second one in the same place, which "
                           "doubles the light on it?")
                .arg(painting),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    try {
        editor::AddedSpot spot = editor::add_spot_for(document, window.ids(), *exhibit);
        const QString name = QString::fromStdString(spot.transaction.find_entity(spot.spot)->name);
        window.commit(std::move(spot.transaction));
        tell(
            window,
            MainWindow::tr("%1 lights %2 from the ceiling, 30° from vertical").arg(name, painting));
    } catch (const std::exception& error) {
        warn(window, MainWindow::tr("Cannot add a spot"), message_of(error));
    }
}

} // namespace

void register_skeleton_commands(MainWindow& window) {
    QMenu* room_menu = window.add_menu(MainWindow::tr("&Room"), room_menu_order);
    QAction* new_room_item = window.add_menu_item(room_menu, 0, MainWindow::tr("&New room…"));
    QObject::connect(new_room_item, &QAction::triggered, &window, [&window] { new_room(window); });

    // The exhibit Add painting hung last, for Add spot (painting_to_light checks it is still
    // there).
    auto hung_last = std::make_shared<std::optional<doc::Uuid>>();

    QMenu* works_menu = window.add_menu(MainWindow::tr("&Works"), works_menu_order);
    QAction* add_painting_item =
        window.add_menu_item(works_menu, 0, MainWindow::tr("Add &painting…"));
    QObject::connect(add_painting_item, &QAction::triggered, &window, [&window, hung_last] {
        if (const std::optional<doc::Uuid> exhibit = add_painting(window)) {
            *hung_last = exhibit;
        }
    });

    QMenu* lights_menu = window.add_menu(MainWindow::tr("&Lights"), lights_menu_order);
    QAction* add_spot_item = window.add_menu_item(lights_menu, 0, MainWindow::tr("Add &spot"));
    add_spot_item->setStatusTip(
        MainWindow::tr("Puts a spot on a ceiling track and aims it at the painting added last, "
                       "30° from vertical (the Canadian Conservation Institute's rule)"));
    QObject::connect(add_spot_item, &QAction::triggered, &window,
                     [&window, hung_last] { add_spot(window, *hung_last); });

    // A second room would be built at the origin too, overlapping the first, until rooms can be
    // placed (M2): New room makes a project's one room. Why it is greyed is said in a tooltip too,
    // since some styles (Fusion, Breeze) show no status tip for a disabled item.
    room_menu->setToolTipsVisible(true);
    const auto update_new_room = [&window, new_room_item] {
        const bool can = !has_room(window.document());
        new_room_item->setEnabled(can);
        const QString why = can ? MainWindow::tr("Builds a room with walls lettered A to D")
                                : MainWindow::tr("This project has its room already. To build "
                                                 "another, start a new project: File > New");
        new_room_item->setStatusTip(why);
        new_room_item->setToolTip(why);
    };
    update_new_room();
    QObject::connect(&window, &MainWindow::document_changed, new_room_item, update_new_room);
    QObject::connect(&window, &MainWindow::project_changed, new_room_item, update_new_room);
}

} // namespace cs::ui
