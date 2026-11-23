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
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

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

void warn(MainWindow& window, const QString& title, const QString& text) {
    QMessageBox::warning(&window, title, text);
}

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
}

bool has_room(const doc::Document& document) {
    return std::ranges::any_of(document.entities(),
                               [](const auto& entry) { return entry.second.room.has_value(); });
}

// ---- Works > Add painting --------------------------------------------------------------------

struct PaintingChoice {
    std::filesystem::path file;
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
    auto* choose = new QPushButton(MainWindow::tr("Choose…"), &dialog);
    choose->setObjectName(QStringLiteral("painting_choose"));
    QObject::connect(choose, &QPushButton::clicked, &dialog, [&dialog, file] {
        // Every tier loads only PNG and JPEG (assets::asset_file); both cases of the extension,
        // since some platforms' dialogs match patterns case by case.
        const QString chosen = QFileDialog::getOpenFileName(
            &dialog, MainWindow::tr("Choose the painting's photograph"), file->text(),
            MainWindow::tr("PNG and JPEG images (*.png *.jpg *.jpeg *.PNG *.JPG *.JPEG)"));
        if (!chosen.isEmpty()) {
            file->setText(chosen);
        }
    });
    auto* file_row = new QHBoxLayout;
    file_row->addWidget(file, 1);
    file_row->addWidget(choose);

    // The unframed work, as a catalogue gives it; up to 20 m, millimetres the finest step.
    const QString centimetres = QStringLiteral(" cm");
    QDoubleSpinBox* width =
        size_box(&dialog, "painting_width", default_painting_width_cm, 1.0, 2000.0, 1, centimetres);
    QDoubleSpinBox* height = size_box(&dialog, "painting_height", default_painting_height_cm, 1.0,
                                      2000.0, 1, centimetres);

    QDialogButtonBox* buttons = ok_cancel(dialog);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);
    QObject::connect(file, &QLineEdit::textChanged, ok,
                     [ok](const QString& text) { ok->setEnabled(!text.trimmed().isEmpty()); });

    auto* form = new QFormLayout(&dialog);
    form->addRow(MainWindow::tr("Photograph:"), file_row);
    form->addRow(MainWindow::tr("Width, unframed:"), width);
    form->addRow(MainWindow::tr("Height, unframed:"), height);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted || file->text().trimmed().isEmpty()) {
        return std::nullopt;
    }
    return PaintingChoice{std::filesystem::path(file->text().trimmed().toStdU16String()),
                          width->value() / 100.0, height->value() / 100.0};
}

// The file's name as bytes, UTF-8 where the system's names are: title_from_file_name and
// source_from_file_name make it valid and bounded.
std::string file_name_bytes(const std::filesystem::path& file) {
    const std::u8string name = file.filename().u8string();
    return {name.begin(), name.end()};
}

void add_painting(MainWindow& window) {
    // Asked first, so that the curator does not pick a photograph for nothing.
    if (!editor::find_wall_labelled(window.document(), editor::default_wall_label)) {
        warn(window, MainWindow::tr("Cannot add a painting"),
             MainWindow::tr("A painting hangs on wall A, and this project has no wall labelled A. "
                            "Make a room first: Room > New room."));
        return;
    }
    const std::optional<PaintingChoice> choice = ask_painting(window);
    if (!choice) {
        return;
    }
    const QString cannot_add = MainWindow::tr("Cannot add the painting");
    assets::AssetName asset;
    try {
        asset = assets::store_image(choice->file, window.assets_folder());
    } catch (const std::invalid_argument& error) {
        // Not an image the tiers can show: too large, not PNG or JPEG, or not decodable.
        warn(window, cannot_add, message_of(error));
        return;
    } catch (const std::exception& error) {
        warn(window, cannot_add,
             MainWindow::tr("The photograph cannot be read or stored in the project: %1")
                 .arg(message_of(error)));
        return;
    }
    const std::string name = file_name_bytes(choice->file);
    try {
        editor::AddedWork added = editor::add_work_in_row(
            window.document(), window.ids(), doc::AssetRef{asset.sha256, asset.extension},
            choice->width_m, choice->height_m, editor::title_from_file_name(name));
        doc::Work work = *added.transaction.find_work(added.work);
        work.source = editor::source_from_file_name(name);
        added.transaction.set_work(added.work, std::move(work));
        window.commit(std::move(added.transaction));
    } catch (const std::exception& error) {
        warn(window, cannot_add, message_of(error));
    }
}

// ---- Lights > Add spot -----------------------------------------------------------------------

// The exhibit added last, or none.
std::optional<doc::Uuid> newest_exhibit(const doc::Document& document) {
    std::optional<doc::Uuid> newest;
    for (const auto& [id, entity] : document.entities()) { // in order of id
        if (entity.exhibit) {
            newest = id;
        }
    }
    return newest;
}

void add_spot(MainWindow& window) {
    const std::optional<doc::Uuid> exhibit = newest_exhibit(window.document());
    if (!exhibit) {
        warn(window, MainWindow::tr("Cannot add a spot"),
             MainWindow::tr("Add a painting first: Add spot lights the painting added last."));
        return;
    }
    try {
        editor::AddedSpot spot = editor::add_spot_for(window.document(), window.ids(), *exhibit);
        window.commit(std::move(spot.transaction));
    } catch (const std::exception& error) {
        warn(window, MainWindow::tr("Cannot add a spot"), message_of(error));
    }
}

} // namespace

void register_skeleton_commands(MainWindow& window) {
    QMenu* room_menu = window.add_menu(MainWindow::tr("&Room"), room_menu_order);
    QAction* new_room_item = window.add_menu_item(room_menu, 0, MainWindow::tr("&New room…"));
    QObject::connect(new_room_item, &QAction::triggered, &window, [&window] { new_room(window); });

    QMenu* works_menu = window.add_menu(MainWindow::tr("&Works"), works_menu_order);
    QAction* add_painting_item =
        window.add_menu_item(works_menu, 0, MainWindow::tr("Add &painting…"));
    QObject::connect(add_painting_item, &QAction::triggered, &window,
                     [&window] { add_painting(window); });

    QMenu* lights_menu = window.add_menu(MainWindow::tr("&Lights"), lights_menu_order);
    QAction* add_spot_item = window.add_menu_item(lights_menu, 0, MainWindow::tr("Add &spot"));
    add_spot_item->setStatusTip(MainWindow::tr("Lights the painting added last, as the Canadian "
                                               "Conservation Institute advises"));
    QObject::connect(add_spot_item, &QAction::triggered, &window, [&window] { add_spot(window); });

    // A second room would be built at the origin too, overlapping the first, until rooms can be
    // placed (M2): New room makes a project's one room.
    const auto update_new_room = [&window, new_room_item] {
        const bool can = !has_room(window.document());
        new_room_item->setEnabled(can);
        new_room_item->setStatusTip(can ? MainWindow::tr("Builds a room with walls lettered A to D")
                                        : MainWindow::tr("This project has its room already"));
    };
    update_new_room();
    QObject::connect(&window, &MainWindow::document_changed, new_room_item, update_new_room);
    QObject::connect(&window, &MainWindow::project_changed, new_room_item, update_new_room);
}

} // namespace cs::ui
