// SPDX-License-Identifier: Apache-2.0
//
// Sample paintings (#189, M1-50), from the issue's acceptance criteria and the owner's comment on
// it: Works > Add sample painting lists the samples as "Title, Artist (W × H cm)"; choosing one
// hangs it with editor::add_work_in_row at its true size beside the works on wall A, with its
// title, artist, date and medium and the source FromCatalogue{id, image hash}, its image stored
// with assets::store_image in window.assets_folder(); a sample whose image has not been fetched is
// disabled, with a tip naming scripts/fetch-samples.py. And the [ui] criterion: "an offscreen test
// uses a test manifest and a tiny committed image in libs/ui/tests/data/samples/: the submenu
// lists it, choosing it hangs a work of the manifest's size beside an existing one, with its title
// and artist, and a manifest whose image is missing gives a disabled item".
//
// The feature (src/features/sample_paintings.cpp) has no header. The registry's
// register_sample_paintings reads the folders two compile definitions name (data/samples/paintings
// for the manifests, build/samples/paintings for the images); register_sample_paintings_from
// takes them as arguments, so that these tests give it libs/ui/tests/data/samples/ as both
// (test-sample.json with its image test-sample.png, and missing-sample.json, whose image is never
// committed), or folders of their own in ui-out/. Both are declared below, as the generated
// registry declares features (cmake/CsRegistry.cmake), with register_skeleton_commands: Room > New
// room, Works > Add painting and Lights > Add spot.
//
// The contract the tests check, beyond the issue's words:
// - A manifest is one JSON object per file, <id>.json. The menu lists it only when the file is at
//   most 64 KiB and a JSON object; its id is 1 to 64 characters of a-z, 0-9 and '-', starts with a
//   letter or digit and is the file's stem; its title and artist.name are non-empty strings; every
//   string is at most 4096 bytes of UTF-8; width_mm and height_mm are finite numbers > 0 and
//   <= 20000; image_url starts with "https://" and its path ends with ".jpg", ".jpeg" or ".png";
//   image_sha256 is 64 lowercase hexadecimal digits. At most 64 manifests are read, in file-name
//   order. One that breaks a rule is left out, with no message box.
// - A sample's image is <images>/<id>.<extension of image_url>; its item is disabled when the file
//   does not exist. With no manifest listed, Add sample painting is disabled. The submenu, named
//   "sample_paintings_menu", reads the folders again each time it is about to show.
// - An item's data is its sample's id. Its text is "Title, Artist (W × H cm)", W and H in
//   centimetres to one decimal with a trailing ".0" dropped, in QLocale()'s decimal separator, with
//   no group separator; an '&' of the title or the artist doubled, so that it shows as '&'.
// - Choosing a sample opens no modal and adds the work in one undo step; it refuses with a message
//   box, adding and storing nothing, when there is no wall A, when wall A is full and when the
//   image's bytes do not hash to image_sha256.
//
// Most tests use a bare MainWindow (no feature but those registered, no viewport, no GPU: they run
// anywhere), build the room with editor::new_room and hang the work already there with
// editor::add_work_in_row, both committed directly: Lights > Add spot prefers the painting Works >
// Add painting hung last, so Add painting is not used. The last test uses the application's window
// and the real data, data/samples/paintings/. Every message box is modal, so the tests answer it
// from a timer that runs inside its event loop (ModalAnswerer, as in test_walking_skeleton.cpp).

#include "ui_test.hpp"

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>
#include <cs/editor/commands/hanging.hpp>
#include <cs/editor/commands/room.hpp>
#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QChar>
#include <QCryptographicHash>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVariant>
#include <QWidget>

#include <algorithm>
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
#include <utility>
#include <variant>
#include <vector>

namespace cs::ui {
// Works > Add sample painting, listing the manifests (*.json) of `manifests`, whose images are
// looked for in `images`.
void register_sample_paintings_from(MainWindow& window, const std::filesystem::path& manifests,
                                    const std::filesystem::path& images);
// Room > New room, Works > Add painting and Lights > Add spot (skeleton_commands.cpp).
void register_skeleton_commands(MainWindow& window);
} // namespace cs::ui

namespace {

using Catch::Matchers::WithinAbs;
using cs::ui::MainWindow;
namespace doc = cs::doc;
namespace fs = std::filesystem;

// How long a message box may take to appear and be answered before a test stops waiting.
// Generous, for the sanitizers of the debug build; qWaitFor returns as soon as it is answered.
constexpr int modal_timeout_ms = 20000;

// ---- The test data ---------------------------------------------------------------------------

// The committed test samples: manifests and images in one folder.
fs::path test_samples() {
    return fs::path(CS_SOURCE_DIR) / "libs" / "ui" / "tests" / "data" / "samples";
}

// The SHA-256 of test-sample.png: test-sample.json's image_sha256, and the name the project
// stores the image under.
const std::string test_sample_sha256 =
    "b5abea55388b570c16ba8e5bc6a97283e2202518e1ba58add19bdc91eb8fad16";

// The skeleton's test chart, named by its SHA-256: a valid PNG of other bytes than the sample's.
const std::string chart_sha256 = "26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a";

fs::path skeleton_chart() {
    return cs::ui::test::skeleton_folder() / "assets" / (chart_sha256 + ".png");
}

const QString fetch_script = QStringLiteral("scripts/fetch-samples.py");

std::string read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
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

QByteArray qbytes(const std::string& bytes) {
    return {bytes.data(), static_cast<qsizetype>(bytes.size())};
}

std::string sha256_hex(const std::string& bytes) {
    return QCryptographicHash::hash(qbytes(bytes), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

// Copies test-sample.json and its image, test-sample.png, into a folder.
void copy_test_sample(const fs::path& folder) {
    fs::copy_file(test_samples() / "test-sample.json", folder / "test-sample.json");
    fs::copy_file(test_samples() / "test-sample.png", folder / "test-sample.png");
}

// ---- Manifests -------------------------------------------------------------------------------

QJsonObject read_manifest(const fs::path& file) {
    QFile json_file(qstring(file));
    REQUIRE(json_file.open(QIODevice::ReadOnly));
    QJsonParseError error;
    const QJsonDocument json = QJsonDocument::fromJson(json_file.readAll(), &error);
    INFO(file.string() << ": " << error.errorString().toStdString());
    REQUIRE(error.error == QJsonParseError::NoError);
    REQUIRE(json.isObject());
    return json.object();
}

QJsonObject test_manifest() {
    return read_manifest(test_samples() / "test-sample.json");
}

// test-sample.json under another id, its title and its image URL following: "Sample <id>",
// https://example.org/samples/<id>.png. Its image_sha256 stays the test image's.
QJsonObject sample_manifest(const std::string& id) {
    QJsonObject manifest = test_manifest();
    const QString name = QString::fromStdString(id);
    manifest[QStringLiteral("id")] = name;
    manifest[QStringLiteral("title")] = QStringLiteral("Sample ") + name;
    manifest[QStringLiteral("image_url")] =
        QStringLiteral("https://example.org/samples/") + name + QStringLiteral(".png");
    return manifest;
}

QJsonObject with(QJsonObject manifest, const QString& key, const QJsonValue& value) {
    manifest[key] = value;
    return manifest;
}

QJsonObject without(QJsonObject manifest, const QString& key) {
    manifest.remove(key);
    return manifest;
}

QJsonObject with_size_mm(QJsonObject manifest, double width_mm, double height_mm) {
    manifest[QStringLiteral("width_mm")] = width_mm;
    manifest[QStringLiteral("height_mm")] = height_mm;
    return manifest;
}

// The manifest with its artist's name set to `name`, or removed when `name` is undefined.
QJsonObject with_artist_name(QJsonObject manifest, const QJsonValue& name) {
    QJsonObject artist = manifest.value(QStringLiteral("artist")).toObject();
    if (name.isUndefined()) {
        artist.remove(QStringLiteral("name"));
    } else {
        artist[QStringLiteral("name")] = name;
    }
    manifest[QStringLiteral("artist")] = artist;
    return manifest;
}

std::string json_text(const QJsonObject& manifest) {
    return QJsonDocument(manifest).toJson().toStdString();
}

// The manifest's JSON with the value of `key` written as `raw`, which may be anything: a number
// JSON cannot hold as a double, bytes that are not UTF-8.
std::string json_text_with_raw(QJsonObject manifest, const QString& key, const std::string& raw) {
    const std::string placeholder = "raw-value-placeholder";
    manifest[key] = QString::fromStdString(placeholder);
    std::string text = json_text(manifest);
    const std::string quoted = "\"" + placeholder + "\"";
    const std::size_t at = text.find(quoted);
    REQUIRE(at != std::string::npos);
    text.replace(at, quoted.size(), raw);
    return text;
}

// The manifest's JSON with spaces after its opening brace, `bytes` bytes long.
std::string padded_json(const QJsonObject& manifest, std::size_t bytes) {
    std::string text = QJsonDocument(manifest).toJson(QJsonDocument::Compact).toStdString();
    REQUIRE(text.size() <= bytes);
    REQUIRE(text.front() == '{');
    text.insert(1, bytes - text.size(), ' ');
    REQUIRE(text.size() == bytes);
    return text;
}

void write_manifest(const fs::path& folder, const std::string& stem, const QJsonObject& manifest) {
    write_file(folder / (stem + ".json"), json_text(manifest));
}

// ---- The menu's texts ------------------------------------------------------------------------

QString add_painting_text() {
    return QStringLiteral("Add painting") + QChar(0x2026);
}
QString add_sample_text() {
    return QStringLiteral("Add sample painting");
}
QString add_spot_text() {
    return QStringLiteral("Add spot");
}

// A size in whole millimetres as the menu shows it, in centimetres: to one decimal, with a
// trailing ".0" dropped, QLocale()'s decimal separator and no group separator. 400 mm gives "40",
// 181 mm "18.1", 20000 mm "2000". A whole number of millimetres is exact to one decimal of a
// centimetre, so no rounding is involved.
QString cm_text(int mm) {
    QString text = QString::number(mm / 10);
    if (mm % 10 != 0) {
        text += QLocale().decimalPoint() + QString::number(mm % 10);
    }
    return text;
}

// An item's text as a reader sees it (plain_text): "Title, Artist (W × H cm)", × being U+00D7.
std::string item_text(const QString& title, const QString& artist, int width_mm, int height_mm) {
    const QString times = QStringLiteral(" ") + QChar(0x00D7) + QStringLiteral(" ");
    return (title + QStringLiteral(", ") + artist + QStringLiteral(" (") + cm_text(width_mm) +
            times + cm_text(height_mm) + QStringLiteral(" cm)"))
        .toStdString();
}

// ---- Menus -----------------------------------------------------------------------------------

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

// Works > Add sample painting.
QAction* add_sample_action(MainWindow& window) {
    return menu_action(window, QStringLiteral("Works"), add_sample_text());
}

// Its submenu.
QMenu& samples_menu(MainWindow& window) {
    QAction* action = add_sample_action(window);
    QMenu* menu = action->menu();
    REQUIRE(menu != nullptr);
    return *menu;
}

std::vector<QAction*> sample_items(const QMenu& menu) {
    std::vector<QAction*> items;
    for (QAction* action : menu.actions()) {
        if (!action->isSeparator()) {
            items.push_back(action);
        }
    }
    return items;
}

// The ids the submenu's items hold as their data, in order.
std::vector<std::string> item_ids(const QMenu& menu) {
    std::vector<std::string> ids;
    for (const QAction* action : sample_items(menu)) {
        ids.push_back(action->data().toString().toStdString());
    }
    return ids;
}

// The submenu's items as a reader sees them, in order.
std::vector<std::string> item_texts(const QMenu& menu) {
    std::vector<std::string> texts;
    for (const QAction* action : sample_items(menu)) {
        texts.push_back(cs::ui::test::plain_text(action->text()).toStdString());
    }
    return texts;
}

// The submenu's item of the sample `id`.
QAction* sample_item(const QMenu& menu, const std::string& id) {
    for (QAction* action : sample_items(menu)) {
        if (action->data().toString().toStdString() == id) {
            return action;
        }
    }
    FAIL("no item for the sample \"" << id << "\"; the items' ids are "
                                     << Catch::Detail::stringify(item_ids(menu)));
    return nullptr;
}

// What a curator's opening the submenu does: it emits aboutToShow, and the submenu reads its
// folders again. Items read before may be gone: look them up again.
void reread(QMenu& menu) {
    emit menu.aboutToShow();
    QApplication::processEvents();
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

// A copy: the document a reference points into changes with the next commit or undo.
doc::OnWall on_wall_of(const doc::Document& document, const doc::Uuid& entity_id) {
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

// The width an exhibit's work takes on the wall, from outer edge to outer edge.
double hung_width_of(const doc::Document& document, const doc::Uuid& work,
                     const doc::Uuid& exhibit) {
    const doc::Work* w = document.find_work(work);
    const doc::Entity* e = document.find_entity(exhibit);
    REQUIRE(w != nullptr);
    REQUIRE(e != nullptr);
    return cs::editor::hung_width_m(*w, e->framing);
}

// ---- Modal dialogs and message boxes ---------------------------------------------------------

// A modal widget as it was when it was answered.
struct SeenModal {
    std::string class_name;
    std::string object_name;
    bool message_box = false;
    std::string text; // a message box's text and informative text
    Qt::TextFormat text_format = Qt::AutoText;
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
        seen.text_format = box->textFormat();
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

// Reads what a test needs from a modal widget; returns true to accept it, false to reject it.
// Never asserts: it runs inside the modal's event loop.
using Answer = std::function<bool(QWidget& modal)>;

// Answers the modal widgets that open while it lives, in order: the first with answers[0], the
// next with answers[1], and any beyond them by rejecting it, marked unexpected, so that a test
// never hangs on a modal it did not expect. A timer polls QApplication::activeModalWidget(). The
// modal is accepted or rejected from a queued call, after the answer has returned. (As
// test_walking_skeleton.cpp's.)
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

// ---- The window and the commands -------------------------------------------------------------

// A bare window's features for these tests: the sample paintings of `manifests` and `images`,
// then the skeleton's commands, in the order the application's registry calls them (by file
// name).
void register_samples(MainWindow& window, const fs::path& manifests, const fs::path& images) {
    cs::ui::register_sample_paintings_from(window, manifests, images);
    cs::ui::register_skeleton_commands(window);
}

// The issue's room, editor::new_room(6, 4, 3.5), committed: face A of wall A is 6 m long.
void build_room(MainWindow& window) {
    cs::editor::NewRoom room = cs::editor::new_room(window.document(), window.ids(), 6.0, 4.0, 3.5);
    window.commit(std::move(room.transaction));
    REQUIRE(cs::editor::find_wall_labelled(window.document(), "A").has_value());
}

struct Hung {
    doc::Uuid work;
    doc::Uuid exhibit;
};

// A work already on wall A, hung in the row as Add painting hangs one, committed. Its image is an
// asset the project names, never looked for on disk (add_work), so nothing is stored.
Hung hang_existing(MainWindow& window, double width_m, double height_m) {
    const doc::AssetRef image{.sha256 = std::string(64, 'a'), .extension = "png"};
    cs::editor::AddedWork added = cs::editor::add_work_in_row(
        window.document(), window.ids(), image, width_m, height_m, "Already there");
    const Hung hung{added.work, added.exhibit};
    window.commit(std::move(added.transaction));
    return hung;
}

// Chooses the sample `id`, which must be enabled, open no modal and add one work. Returns the new
// work's id.
doc::Uuid choose_sample(MainWindow& window, const std::string& id) {
    QAction* item = sample_item(samples_menu(window), id);
    REQUIRE(item->isEnabled());
    const std::set<doc::Uuid> before = work_ids(window.document());
    ModalAnswerer answers;
    item->trigger();
    const bool finished = answers.finish();
    INFO("modals:\n" << answers.log());
    REQUIRE(finished);
    REQUIRE(answers.seen().empty());
    std::vector<doc::Uuid> added;
    for (const doc::Uuid& work : work_ids(window.document())) {
        if (!before.contains(work)) {
            added.push_back(work);
        }
    }
    REQUIRE(added.size() == 1);
    return added.front();
}

// Chooses the sample `id`, which must be enabled, accepting the message box it shows; any modal
// after it is rejected and marked unexpected. Returns the modals seen.
std::vector<SeenModal> choose_refused(MainWindow& window, const std::string& id) {
    QAction* item = sample_item(samples_menu(window), id);
    REQUIRE(item->isEnabled());
    ModalAnswerer answers({accept_as_is()});
    item->trigger();
    answers.finish();
    return answers.seen();
}

// A refusal as the contract has it: one modal, a message box saying something, as plain text. The
// owner's comment: the editor's message is shown "as it is, in a plain-text message box", since
// titles come from files (hostile, CLAUDE.md) and Qt reads markup in a message box as rich text.
void check_one_message(const std::vector<SeenModal>& seen) {
    std::string log;
    for (const SeenModal& modal : seen) {
        log += describe(modal) + "\n";
    }
    INFO("modals:\n" << (log.empty() ? "(no modal)" : log));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].message_box);
    CHECK_FALSE(seen[0].unexpected);
    CHECK_FALSE(seen[0].text.empty());
    CHECK(seen[0].text_format == Qt::PlainText);
}

// Lights > Add spot, which asks nothing while no light aims at the work it lights.
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

// The application's window, with or without a viewport (no Vulkan device): its menus work either
// way. Fails when make_main_window throws, as it does under CS_REQUIRE_GPU without a device.
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

// ---- Manifests that break a rule -------------------------------------------------------------

struct BrokenManifest {
    std::string why;
    std::string stem; // the file is <stem>.json
    std::string content;
};

// One manifest per rule broken, each otherwise the test sample's under its own id.
std::vector<BrokenManifest> broken_manifests() {
    const auto m = [](const std::string& id) { return sample_manifest(id); };
    const auto json = [](const QJsonObject& manifest) { return json_text(manifest); };
    const QString title = QStringLiteral("title");
    const QString artist = QStringLiteral("artist");
    const QString width = QStringLiteral("width_mm");
    const QString height = QStringLiteral("height_mm");
    const QString url = QStringLiteral("image_url");
    const QString hash = QStringLiteral("image_sha256");
    const QString medium = QStringLiteral("medium");
    const QString sha = QString::fromStdString(test_sample_sha256);
    const std::string not_utf8{
        '"', 'O', 'i', 'l', ' ', static_cast<char>(0xff), static_cast<char>(0xfe),
        ' ', 'o', 'n', '"'};
    return {
        // The file
        {"it is empty", "empty", ""},
        {"it is not JSON", "not-json", "This is not JSON.\n"},
        {"it is cut short", "cut-short", json(m("cut-short")).substr(0, 200)},
        {"it is a JSON array, not an object", "array", "[" + json(m("array")) + "]"},
        {"it is JSON null", "null", "null"},
        {"it is 64 KiB and 1 byte long", "too-large", padded_json(m("too-large"), 65537)},
        // The id
        {"its id is not its file's name", "renamed", json(m("not-renamed"))},
        {"its id has a capital letter", "Capital-Id", json(m("Capital-Id"))},
        {"its id starts with '-'", "-dash-first", json(m("-dash-first"))},
        {"its id has a '_'", "under_score", json(m("under_score"))},
        {"its id has a '.'", "dot.ted", json(m("dot.ted"))},
        {"its id is 65 characters long", std::string(65, 'a'), json(m(std::string(65, 'a')))},
        {"its id is a path", "dot-dot", json(m("../dot-dot"))},
        {"its id is the number 7, not a string", "7", json(with(m("7"), QStringLiteral("id"), 7))},
        {"it has no id", "no-id", json(without(m("no-id"), QStringLiteral("id")))},
        // The title and the artist
        {"its title is empty", "empty-title",
         json(with(m("empty-title"), title, QStringLiteral("")))},
        {"its title is a number", "number-title", json(with(m("number-title"), title, 1925))},
        {"it has no title", "no-title", json(without(m("no-title"), title))},
        {"its artist is a string, not an object with a name", "artist-string",
         json(with(m("artist-string"), artist, QStringLiteral("Curation Sandbox")))},
        {"its artist's name is empty", "empty-artist",
         json(with_artist_name(m("empty-artist"), QStringLiteral("")))},
        {"its artist has no name", "artist-no-name",
         json(with_artist_name(m("artist-no-name"), QJsonValue(QJsonValue::Undefined)))},
        {"it has no artist", "no-artist", json(without(m("no-artist"), artist))},
        // The strings
        {"its title is 4097 bytes long", "long-title",
         json(with(m("long-title"), title, QString(4097, QLatin1Char('x'))))},
        {"its artist's name is 4097 bytes long", "long-artist",
         json(with_artist_name(m("long-artist"), QString(4097, QLatin1Char('x'))))},
        {"its medium, an optional string, is 4097 bytes long", "long-medium",
         json(with(m("long-medium"), medium, QString(4097, QLatin1Char('x'))))},
        {"its medium is 2049 characters but 4098 bytes of UTF-8 (each 'é' is 2 bytes)",
         "long-multibyte", json(with(m("long-multibyte"), medium, QString(2049, QChar(0x00E9))))},
        {"its medium is not UTF-8", "not-utf8",
         json_text_with_raw(m("not-utf8"), medium, not_utf8)},
        // The size
        {"its width is 0", "zero-width", json(with_size_mm(m("zero-width"), 0.0, 300.0))},
        {"its height is 0", "zero-height", json(with_size_mm(m("zero-height"), 400.0, 0.0))},
        {"its width is negative", "negative-width",
         json(with_size_mm(m("negative-width"), -400.0, 300.0))},
        {"its width is over 20000 mm", "too-wide",
         json(with_size_mm(m("too-wide"), 20001.0, 300.0))},
        {"its height is over 20000 mm", "too-high",
         json(with_size_mm(m("too-high"), 400.0, 20001.0))},
        {"its width is the string \"400\"", "text-width",
         json(with(m("text-width"), width, QStringLiteral("400")))},
        {"its width is too large for a double", "infinite-width",
         json_text_with_raw(m("infinite-width"), width, "1e999")},
        {"it has no height", "no-height", json(without(m("no-height"), height))},
        // The image
        {"its image URL is http", "http-image",
         json(with(m("http-image"), url,
                   QStringLiteral("http://example.org/samples/http-image.png")))},
        {"its image URL is a file URL", "file-image",
         json(with(m("file-image"), url, QStringLiteral("file:///tmp/file-image.png")))},
        {"its image URL ends with .gif", "gif-image",
         json(with(m("gif-image"), url,
                   QStringLiteral("https://example.org/samples/gif-image.gif")))},
        {"its image URL ends with .PNG, in capitals", "capital-extension",
         json(with(m("capital-extension"), url,
                   QStringLiteral("https://example.org/samples/capital-extension.PNG")))},
        {"its image URL has no extension", "no-extension",
         json(with(m("no-extension"), url,
                   QStringLiteral("https://example.org/samples/no-extension")))},
        {"it has no image URL", "no-image-url", json(without(m("no-image-url"), url))},
        {"its image hash is 63 digits", "short-hash",
         json(with(m("short-hash"), hash, sha.left(63)))},
        {"its image hash is in capitals", "capital-hash",
         json(with(m("capital-hash"), hash, sha.toUpper()))},
        {"its image hash has a 'g'", "not-hex-hash",
         json(with(m("not-hex-hash"), hash, QStringLiteral("g") + sha.mid(1)))},
        {"it has no image hash", "no-hash", json(without(m("no-hash"), hash))},
    };
}

} // namespace

// ==== The menu ================================================================================

// Acceptance criterion: "Works > Add sample painting lists the samples". The contract: an item "Add
// &sample painting", no ellipsis since it opens a submenu, not a dialog, after Add painting... in
// the one Works menu, whose submenu is named "sample_paintings_menu". The window sorts what
// features add, so the order they register in does not matter; the application's registry calls
// the sample paintings first (file-name order).
TEST_CASE("Sample paintings: the Works menu has Add sample painting after Add painting..., with "
          "the submenu sample_paintings_menu, whichever feature registers first",
          "[ui]") {
    cs::ui::test::require_offscreen();
    MainWindow window;
    SECTION("the sample paintings first, as the registry calls them") {
        cs::ui::register_sample_paintings_from(window, test_samples(), test_samples());
        cs::ui::register_skeleton_commands(window);
    }
    SECTION("the skeleton's commands first") {
        cs::ui::register_skeleton_commands(window);
        cs::ui::register_sample_paintings_from(window, test_samples(), test_samples());
    }
    const auto layout = cs::ui::test::menu_bar_layout(window);
    INFO("the menu bar:\n" << cs::ui::test::to_string(layout));
    // One Works menu: the feature's and Add painting's are merged.
    CHECK(std::count_if(layout.begin(), layout.end(), [](const auto& menu) {
              return menu.title == QStringLiteral("Works");
          }) == 1);
    QMenu* works = cs::ui::test::find_menu(window, QStringLiteral("Works"));
    REQUIRE(works != nullptr);
    const QStringList items = cs::ui::test::menu_items(*works);
    const qsizetype add_painting = items.indexOf(add_painting_text());
    const qsizetype add_sample = items.indexOf(add_sample_text());
    REQUIRE(add_painting >= 0);
    REQUIRE(add_sample >= 0);
    CHECK(add_sample > add_painting);

    QAction* action = add_sample_action(window);
    CHECK(action->text() == QStringLiteral("Add &sample painting"));
    REQUIRE(action->menu() != nullptr);
    CHECK(action->menu()->objectName() == QStringLiteral("sample_paintings_menu"));
    // The test folder holds two manifests, so the submenu has something to show.
    CHECK(action->isEnabled());
}

// Acceptance criteria: the submenu "lists the samples as 'Title, Artist (W × H cm)'"; the [ui]
// criterion: "the submenu lists it". The test folder's manifests, in file-name order:
// missing-sample (300 x 500 mm) and test-sample (400 x 300 mm). Each item holds its sample's id as
// its data. Reading the folders again (aboutToShow) lists the same items, not each twice.
TEST_CASE("Sample paintings: the submenu lists each manifest as 'Title, Artist (W x H cm)', in "
          "file-name order, with its id as the item's data",
          "[ui]") {
    cs::ui::test::require_offscreen();
    MainWindow window;
    register_samples(window, test_samples(), test_samples());
    const std::vector<std::string> ids{"missing-sample", "test-sample"};
    const std::vector<std::string> texts{
        item_text(QStringLiteral("Missing Sample"), QStringLiteral("Curation Sandbox"), 300, 500),
        item_text(QStringLiteral("Test Sample"), QStringLiteral("Curation Sandbox"), 400, 300)};
    {
        const QMenu& menu = samples_menu(window);
        CHECK(item_ids(menu) == ids);
        CHECK(item_texts(menu) == texts);
    }
    reread(samples_menu(window));
    reread(samples_menu(window));
    const QMenu& menu = samples_menu(window);
    CHECK(item_ids(menu) == ids);
    CHECK(item_texts(menu) == texts);
}

// The contract's sizes: W and H are width_mm / 10 and height_mm / 10 in centimetres, to one
// decimal with a trailing ".0" dropped, in QLocale()'s decimal separator and with no group
// separator (2000, never 2,000). The expected texts are built with the decimal separator of the
// locale the test runs in, so they hold in any. 1 mm is the smallest whole size a manifest may give
// and 20000 mm the largest.
TEST_CASE("Sample paintings: sizes show in centimetres to one decimal, '.0' dropped, with the "
          "locale's decimal separator and no group separator",
          "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-sizes");
    write_manifest(folder, "a-small",
                   with(with_size_mm(sample_manifest("a-small"), 181.0, 240.0),
                        QStringLiteral("title"), QStringLiteral("Small")));
    write_manifest(folder, "b-largest",
                   with(with_size_mm(sample_manifest("b-largest"), 20000.0, 12345.0),
                        QStringLiteral("title"), QStringLiteral("Largest")));
    write_manifest(folder, "c-tiny",
                   with(with_size_mm(sample_manifest("c-tiny"), 1.0, 9.0), QStringLiteral("title"),
                        QStringLiteral("Tiny")));
    MainWindow window;
    register_samples(window, folder, folder);
    const QString artist = QStringLiteral("Curation Sandbox");
    CHECK(item_texts(samples_menu(window)) ==
          std::vector<std::string>{item_text(QStringLiteral("Small"), artist, 181, 240),
                                   item_text(QStringLiteral("Largest"), artist, 20000, 12345),
                                   item_text(QStringLiteral("Tiny"), artist, 1, 9)});
    // Spelled out for a locale whose decimal separator is '.': "18.1 × 24", "2000 × 1234.5",
    // "0.1 × 0.9".
    if (QLocale().decimalPoint() == QStringLiteral(".")) {
        const QString times = QString(QChar(0x00D7));
        CHECK(item_texts(samples_menu(window)) ==
              std::vector<std::string>{(QStringLiteral("Small, Curation Sandbox (18.1 ") + times +
                                        QStringLiteral(" 24 cm)"))
                                           .toStdString(),
                                       (QStringLiteral("Largest, Curation Sandbox (2000 ") + times +
                                        QStringLiteral(" 1234.5 cm)"))
                                           .toStdString(),
                                       (QStringLiteral("Tiny, Curation Sandbox (0.1 ") + times +
                                        QStringLiteral(" 0.9 cm)"))
                                           .toStdString()});
    }
}

// The contract: an '&' in a title or an artist's name is doubled in the item's text, so that the
// menu shows it rather than reading it as a mnemonic.
TEST_CASE("Sample paintings: an '&' in a title or an artist's name shows as '&'", "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-ampersand");
    write_manifest(folder, "salt-and-pepper",
                   with_artist_name(with(sample_manifest("salt-and-pepper"),
                                         QStringLiteral("title"), QStringLiteral("Salt & Pepper")),
                                    QStringLiteral("Smith & Sons")));
    MainWindow window;
    register_samples(window, folder, folder);
    const QAction* item = sample_item(samples_menu(window), "salt-and-pepper");
    const QString times = QStringLiteral(" ") + QChar(0x00D7) + QStringLiteral(" ");
    CHECK(item->text().toStdString() ==
          (QStringLiteral("Salt && Pepper, Smith && Sons (40") + times + QStringLiteral("30 cm)"))
              .toStdString());
    CHECK(cs::ui::test::plain_text(item->text()).toStdString() ==
          item_text(QStringLiteral("Salt & Pepper"), QStringLiteral("Smith & Sons"), 400, 300));
}

// Acceptance criteria: "A sample whose image has not been fetched is disabled, with a tip naming
// the script"; the [ui] criterion: "a manifest whose image is missing gives a disabled item".
// missing-sample.png is never committed; test-sample.png is. A disabled item cannot be chosen:
// triggering it opens nothing and adds nothing (here in a project with no room, where choosing a
// sample would show a message).
TEST_CASE("Sample paintings: a sample whose image has not been fetched is disabled, its tips "
          "naming scripts/fetch-samples.py; one whose image is there is enabled",
          "[ui]") {
    cs::ui::test::require_offscreen();
    MainWindow window;
    register_samples(window, test_samples(), test_samples());
    REQUIRE_FALSE(fs::exists(test_samples() / "missing-sample.png"));
    REQUIRE(fs::exists(test_samples() / "test-sample.png"));
    const QMenu& menu = samples_menu(window);

    QAction* missing = sample_item(menu, "missing-sample");
    CHECK_FALSE(missing->isEnabled());
    INFO("tool tip: " << missing->toolTip().toStdString());
    INFO("status tip: " << missing->statusTip().toStdString());
    CHECK(missing->toolTip().contains(fetch_script));
    CHECK(missing->statusTip().contains(fetch_script));

    CHECK(sample_item(menu, "test-sample")->isEnabled());
    CHECK(add_sample_action(window)->isEnabled());

    const doc::Document before = window.document();
    ModalAnswerer answers;
    missing->trigger();
    CHECK(answers.finish());
    INFO("modals:\n" << answers.log());
    CHECK(answers.seen().empty());
    CHECK(window.document() == before);
}

// The contract: with no manifest listed, from an empty folder, a missing one or one whose files
// are no manifests, Add sample painting is disabled and its submenu holds no item; nothing says
// so in a message box.
TEST_CASE("Sample paintings: with no manifest to list, Add sample painting is disabled and its "
          "submenu empty",
          "[ui]") {
    cs::ui::test::require_offscreen();
    fs::path folder;
    SECTION("an empty folder") {
        folder = cs::ui::test::fresh_out_folder("sample-paintings-empty");
    }
    SECTION("a folder that does not exist") {
        folder = cs::ui::test::fresh_out_folder("sample-paintings-absent") / "absent";
    }
    SECTION("a folder of an image, a text file and a manifest that is not JSON") {
        folder = cs::ui::test::fresh_out_folder("sample-paintings-no-manifest");
        fs::copy_file(test_samples() / "test-sample.png", folder / "test-sample.png");
        write_file(folder / "notes.txt", "Not a manifest.\n");
        write_file(folder / "broken.json", "{\"id\": \"broken\",");
    }
    CAPTURE(folder.string());
    MainWindow window;
    ModalAnswerer answers;
    register_samples(window, folder, folder);
    QAction* action = add_sample_action(window);
    CHECK_FALSE(action->isEnabled());
    REQUIRE(action->menu() != nullptr);
    CHECK(action->menu()->objectName() == QStringLiteral("sample_paintings_menu"));
    CHECK(sample_items(*action->menu()).empty());
    CHECK(answers.finish());
    INFO("modals:\n" << answers.log());
    CHECK(answers.seen().empty());
}

// The contract: the submenu reads the folders again each time it is about to show. An image
// fetched after the window was made enables its item; a manifest added is listed; an image
// removed disables its item again. The item enabled so is chosen as any other.
TEST_CASE("Sample paintings: the submenu reads its folders again each time it is about to show",
          "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path manifests = cs::ui::test::fresh_out_folder("sample-paintings-later-manifests");
    const fs::path images = cs::ui::test::fresh_out_folder("sample-paintings-later-images");
    fs::copy_file(test_samples() / "test-sample.json", manifests / "test-sample.json");
    MainWindow window;
    register_samples(window, manifests, images);
    build_room(window);
    CHECK(add_sample_action(window)->isEnabled());
    REQUIRE(item_ids(samples_menu(window)) == std::vector<std::string>{"test-sample"});
    CHECK_FALSE(sample_item(samples_menu(window), "test-sample")->isEnabled());

    // The image fetched: enabled, listed once.
    fs::copy_file(test_samples() / "test-sample.png", images / "test-sample.png");
    reread(samples_menu(window));
    CHECK(item_ids(samples_menu(window)) == std::vector<std::string>{"test-sample"});
    CHECK(sample_item(samples_menu(window), "test-sample")->isEnabled());
    const doc::Uuid work = choose_sample(window, "test-sample");
    CHECK(window.document().find_work(work)->title == "Test Sample");

    // A manifest added: listed, in file-name order, disabled until its image is there.
    fs::copy_file(test_samples() / "missing-sample.json", manifests / "missing-sample.json");
    reread(samples_menu(window));
    CHECK(item_ids(samples_menu(window)) ==
          std::vector<std::string>{"missing-sample", "test-sample"});
    CHECK_FALSE(sample_item(samples_menu(window), "missing-sample")->isEnabled());
    CHECK(sample_item(samples_menu(window), "test-sample")->isEnabled());

    // The image removed: disabled again.
    fs::remove(images / "test-sample.png");
    reread(samples_menu(window));
    CHECK_FALSE(sample_item(samples_menu(window), "test-sample")->isEnabled());
}

// The contract: a sample's image is <images>/<id>.<extension of image_url>, in the images folder
// (not the manifests'), named by the sample's id (not by the URL's file name, which for a IIIF
// server is "default.jpg"), with the URL's extension (".jpg", ".jpeg" or ".png").
TEST_CASE("Sample paintings: a sample's image is <images>/<id>.<extension of its image URL>",
          "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path manifests = cs::ui::test::fresh_out_folder("sample-paintings-names-manifests");
    const fs::path images = cs::ui::test::fresh_out_folder("sample-paintings-names-images");
    const auto manifest_with_url = [&](const std::string& id, const QString& image_url) {
        write_manifest(manifests, id,
                       with(sample_manifest(id), QStringLiteral("image_url"), image_url));
    };
    const auto image = [](const fs::path& path) {
        fs::copy_file(test_samples() / "test-sample.png", path);
    };
    // Enabled: <id>.jpg for a IIIF URL ending in default.jpg, <id>.jpeg, <id>.png.
    manifest_with_url("iiif-style", QStringLiteral("https://example.org/iiif/2/iiif-style/full/"
                                                   "!4096,4096/0/default.jpg"));
    image(images / "iiif-style.jpg");
    manifest_with_url("jpeg-url", QStringLiteral("https://example.org/samples/jpeg-url.jpeg"));
    image(images / "jpeg-url.jpeg");
    manifest_with_url("png-url", QStringLiteral("https://example.org/samples/png-url.png"));
    image(images / "png-url.png");
    // Disabled: the image under the URL's file name, under another extension, and in the
    // manifests' folder.
    manifest_with_url("named-by-url", QStringLiteral("https://example.org/samples/picture.png"));
    image(images / "picture.png");
    manifest_with_url("other-extension",
                      QStringLiteral("https://example.org/samples/other-extension.png"));
    image(images / "other-extension.jpg");
    manifest_with_url("wrong-folder",
                      QStringLiteral("https://example.org/samples/wrong-folder.png"));
    image(manifests / "wrong-folder.png");

    MainWindow window;
    register_samples(window, manifests, images);
    const QMenu& menu = samples_menu(window);
    REQUIRE(item_ids(menu) == std::vector<std::string>{"iiif-style", "jpeg-url", "named-by-url",
                                                       "other-extension", "png-url",
                                                       "wrong-folder"});
    CHECK(sample_item(menu, "iiif-style")->isEnabled());
    CHECK(sample_item(menu, "jpeg-url")->isEnabled());
    CHECK(sample_item(menu, "png-url")->isEnabled());
    CHECK_FALSE(sample_item(menu, "named-by-url")->isEnabled());
    CHECK_FALSE(sample_item(menu, "other-extension")->isEnabled());
    CHECK_FALSE(sample_item(menu, "wrong-folder")->isEnabled());
}

// ==== Manifests from outside ===================================================================

// Input from outside is hostile (CLAUDE.md), and the contract's rules: a manifest that breaks one
// is left out of the menu, with no message box, and the others are still listed. Each case is a
// folder of the test sample and one broken manifest.
TEST_CASE("Sample paintings: a manifest that breaks a rule is left out of the menu, with no "
          "message",
          "[ui]") {
    cs::ui::test::require_offscreen();
    for (const BrokenManifest& broken : broken_manifests()) {
        INFO("the manifest " << broken.stem << ".json: " << broken.why);
        const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-broken");
        copy_test_sample(folder);
        write_file(folder / (broken.stem + ".json"), broken.content);
        MainWindow window;
        ModalAnswerer answers;
        register_samples(window, folder, folder);
        CHECK(item_ids(samples_menu(window)) == std::vector<std::string>{"test-sample"});
        reread(samples_menu(window));
        CHECK(item_ids(samples_menu(window)) == std::vector<std::string>{"test-sample"});
        CHECK(sample_item(samples_menu(window), "test-sample")->isEnabled());
        CHECK(answers.finish());
        INFO("modals:\n" << answers.log());
        CHECK(answers.seen().empty());
    }

    // A folder named like a manifest is no file: left out.
    const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-broken");
    copy_test_sample(folder);
    fs::create_directory(folder / "a-folder.json");
    MainWindow window;
    ModalAnswerer answers;
    register_samples(window, folder, folder);
    CHECK(item_ids(samples_menu(window)) == std::vector<std::string>{"test-sample"});
    CHECK(answers.finish());
    CHECK(answers.seen().empty());
}

// The contract's rules at their limits are kept, not broken: an id of 64 characters, or starting
// with a digit; a string of 4096 bytes, in ASCII or in 2-byte characters; a width and a height of
// 20000 mm; a file of exactly 64 KiB; image URLs ending in .jpg and .jpeg; a manifest with no
// optional field.
TEST_CASE("Sample paintings: manifests at the limits of the rules are listed", "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-limits");
    std::vector<std::string> stems;
    const auto add = [&](const std::string& stem, const std::string& content) {
        write_file(folder / (stem + ".json"), content);
        stems.push_back(stem);
    };
    const std::string id_64(64, 'z');
    add(id_64, json_text(sample_manifest(id_64)));
    add("0-starts-with-a-digit", json_text(sample_manifest("0-starts-with-a-digit")));
    add("medium-4096-bytes",
        json_text(with(sample_manifest("medium-4096-bytes"), QStringLiteral("medium"),
                       QString(4096, QLatin1Char('x')))));
    add("medium-4096-bytes-of-2",
        json_text(with(sample_manifest("medium-4096-bytes-of-2"), QStringLiteral("medium"),
                       QString(2048, QChar(0x00E9)))));
    add("largest", json_text(with_size_mm(sample_manifest("largest"), 20000.0, 20000.0)));
    add("file-of-64-kib", padded_json(sample_manifest("file-of-64-kib"), 65536));
    add("jpg-url", json_text(with(sample_manifest("jpg-url"), QStringLiteral("image_url"),
                                  QStringLiteral("https://example.org/samples/jpg-url.jpg"))));
    add("jpeg-url", json_text(with(sample_manifest("jpeg-url"), QStringLiteral("image_url"),
                                   QStringLiteral("https://example.org/samples/jpeg-url.jpeg"))));
    QJsonObject required;
    required[QStringLiteral("id")] = QStringLiteral("required-fields-only");
    required[QStringLiteral("title")] = QStringLiteral("Required fields only");
    QJsonObject artist;
    artist[QStringLiteral("name")] = QStringLiteral("Curation Sandbox");
    required[QStringLiteral("artist")] = artist;
    required[QStringLiteral("image_url")] =
        QStringLiteral("https://example.org/samples/required-fields-only.png");
    required[QStringLiteral("image_sha256")] = QString::fromStdString(test_sample_sha256);
    required[QStringLiteral("width_mm")] = 400;
    required[QStringLiteral("height_mm")] = 300;
    add("required-fields-only", json_text(required));
    // In order of file name, as the contract says: "medium-4096-bytes-of-2.json" comes before
    // "medium-4096-bytes.json", since '-' sorts before '.'.
    std::sort(stems.begin(), stems.end(),
              [](const std::string& a, const std::string& b) { return a + ".json" < b + ".json"; });

    MainWindow window;
    ModalAnswerer answers;
    register_samples(window, folder, folder);
    CHECK(item_ids(samples_menu(window)) == stems);
    CHECK(answers.finish());
    INFO("modals:\n" << answers.log());
    CHECK(answers.seen().empty());
}

// The contract: at most 64 manifests are read, in file-name order. Of 70 valid ones, written in
// reverse order so that the directory's own order is unlikely to be the file names', the first 64
// by name are listed.
TEST_CASE("Sample paintings: at most 64 manifests are read, the first in file-name order", "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-many");
    const auto id_of = [](int i) {
        return std::string("s-") + static_cast<char>('0' + i / 10) +
               static_cast<char>('0' + i % 10);
    };
    for (int i = 69; i >= 0; --i) {
        write_manifest(folder, id_of(i), sample_manifest(id_of(i)));
    }
    std::vector<std::string> first_64;
    for (int i = 0; i < 64; ++i) {
        first_64.push_back(id_of(i));
    }
    MainWindow window;
    register_samples(window, folder, folder);
    CHECK(item_ids(samples_menu(window)) == first_64);
}

// ==== Choosing a sample =======================================================================

// Acceptance criterion: "choosing one hangs it with editor::add_work_in_row at its true size,
// with its title, artist, date and medium, and source FromCatalogue{id, image hash}; the image
// goes through assets::store_image into window.assets_folder()"; the [ui] criterion: "choosing it
// hangs a work of the manifest's size beside an existing one, with its title and artist". The
// contract: no modal, one undo step, the manifest's credit line, accession number and licence too,
// and undo takes the work away and puts the one already there back.
//
// The row (add_work_in_row): the work already there, then the sample at its right end, centred on
// the 6 m face with row_gap_m = 0.4 m between their outer edges, which hung_width_m gives. In a box
// frame each is 0.11 m wider than the work (0.61 and 0.51 m): a row of 1.52 m from 2.24 m, centres
// at 2.545 and 3.505 m. Distances are exact but for rounding: 1e-9 m. The sizes are a division of
// millimetres by 1000, exact but for a unit conversion's few ulps: 1e-12 m.
TEST_CASE("Sample paintings: choosing the test sample hangs it at 40 x 30 cm beside the work on "
          "wall A, with the manifest's title, artist, date, medium and FromCatalogue source, its "
          "image stored, in one undo step",
          "[ui]") {
    cs::ui::test::require_offscreen();
    MainWindow window;
    register_samples(window, test_samples(), test_samples());
    build_room(window);
    const Hung existing = hang_existing(window, 0.5, 0.4);
    const double existing_alone_m = on_wall_of(window.document(), existing.exhibit).distance_m;
    const std::size_t steps = window.history().undo_count();
    const std::size_t entities = window.document().entities().size();

    const doc::Uuid sample = choose_sample(window, "test-sample");

    const doc::Document& document = window.document();
    CHECK(window.history().undo_count() == steps + 1);
    CHECK(document.works().size() == 2);
    CHECK(document.entities().size() == entities + 1);

    const doc::Work* found = document.find_work(sample);
    REQUIRE(found != nullptr);
    const doc::Work work = *found;
    CHECK(work.title == "Test Sample");
    CHECK(work.artist == "Curation Sandbox");
    CHECK(work.date == "1925");
    CHECK(work.medium == "Pixels on a test chart");
    CHECK(work.credit_line == "Made by the project for its tests");
    CHECK(work.accession_number == std::optional<std::string>("TEST.1"));
    CHECK(work.licence ==
          std::optional<std::string>("https://creativecommons.org/publicdomain/zero/1.0/"));
    CHECK_THAT(work.form.width_m, WithinAbs(0.4, 1e-12));
    CHECK_THAT(work.form.height_m, WithinAbs(0.3, 1e-12));
    CHECK(work.image == doc::AssetRef{test_sample_sha256, "png"});
    CHECK(work.source == doc::WorkSource{doc::FromCatalogue{"test-sample", test_sample_sha256}});

    // The image, stored once under its content's name, the same bytes; the committed file left
    // where it is.
    const fs::path assets = window.assets_folder();
    CHECK(file_names(assets) == std::vector<std::string>{test_sample_sha256 + ".png"});
    CHECK(read_bytes(assets / (test_sample_sha256 + ".png")) ==
          read_bytes(test_samples() / "test-sample.png"));
    CHECK(fs::is_regular_file(test_samples() / "test-sample.png"));

    // Hung as add_work_in_row hangs it: framed, on face A of wall A at the centre line, to the
    // right of the work already there, the row centred.
    const doc::Uuid sample_exhibit = exhibit_of_work(document, sample);
    const doc::Entity* exhibit_entity = document.find_entity(sample_exhibit);
    REQUIRE(exhibit_entity != nullptr);
    REQUIRE(exhibit_entity->framing.has_value());
    CHECK(exhibit_entity->framing->style == cs::editor::default_frame_style);
    const doc::OnWall left = on_wall_of(document, existing.exhibit);
    const doc::OnWall right = on_wall_of(document, sample_exhibit);
    const auto wall_a = cs::editor::find_wall_labelled(document, "A");
    CHECK(right.wall == wall_a);
    CHECK(right.face == doc::WallFace::a);
    CHECK(right.centre_height_m == document.settings().centre_line_m);
    CHECK(left.wall == wall_a);
    CHECK(left.centre_height_m == document.settings().centre_line_m);
    const double left_m = hung_width_of(document, existing.work, existing.exhibit);
    const double right_m = hung_width_of(document, sample, sample_exhibit);
    const double row_m = left_m + cs::editor::row_gap_m + right_m;
    const double start_m = (6.0 - row_m) / 2.0;
    INFO("hung widths " << left_m << " and " << right_m << " m; centres at " << left.distance_m
                        << " and " << right.distance_m << " m");
    CHECK_THAT(left.distance_m, WithinAbs(start_m + left_m / 2.0, 1e-9));
    CHECK_THAT(right.distance_m,
               WithinAbs(start_m + left_m + cs::editor::row_gap_m + right_m / 2.0, 1e-9));
    CHECK(right.distance_m > left.distance_m);

    // One undo takes the sample away and puts the work already there back where it was.
    window.undo();
    CHECK(window.document().works().size() == 1);
    CHECK(window.document().find_work(sample) == nullptr);
    CHECK(window.history().undo_count() == steps);
    CHECK(on_wall_of(window.document(), existing.exhibit).distance_m == existing_alone_m);
}

// The owner's comment: "Lights > Add spot lights the painting Add painting hung last, or else the
// exhibit with the greatest id. Within one run that is the sample just hung. The [ui] test can
// check Add spot aims at the sample."
TEST_CASE("Sample paintings: Lights > Add spot after choosing a sample aims at the sample",
          "[ui]") {
    cs::ui::test::require_offscreen();
    MainWindow window;
    register_samples(window, test_samples(), test_samples());
    build_room(window);
    hang_existing(window, 0.5, 0.4);
    const doc::Uuid sample = choose_sample(window, "test-sample");
    const doc::Uuid sample_exhibit = exhibit_of_work(window.document(), sample);

    add_spot(window);

    const doc::Document& document = window.document();
    const std::vector<doc::Uuid> luminaires = luminaires_of(document);
    REQUIRE(luminaires.size() == 1);
    CHECK(document.find_entity(luminaires.front())->luminaire->aim ==
          doc::Aim{doc::AimAtEntity{sample_exhibit}});
}

// Choosing a sample twice hangs it twice (a curator may try two places for one painting), and the
// image is stored once: the project names its assets by their content (ADR 0007).
TEST_CASE("Sample paintings: choosing a sample twice hangs two works and stores its image once",
          "[ui]") {
    cs::ui::test::require_offscreen();
    MainWindow window;
    register_samples(window, test_samples(), test_samples());
    build_room(window);
    const std::size_t steps = window.history().undo_count();
    const doc::Uuid first = choose_sample(window, "test-sample");
    const doc::Uuid second = choose_sample(window, "test-sample");
    CHECK(first != second);
    CHECK(window.document().works().size() == 2);
    CHECK(window.history().undo_count() == steps + 2);
    for (const doc::Uuid& work : {first, second}) {
        CHECK(window.document().find_work(work)->source ==
              doc::WorkSource{doc::FromCatalogue{"test-sample", test_sample_sha256}});
    }
    CHECK(file_names(window.assets_folder()) ==
          std::vector<std::string>{test_sample_sha256 + ".png"});
}

// ==== Choices refused =========================================================================

// The contract: with no wall labelled A (an empty project, or wall A renamed) choosing a sample
// shows a message box and adds nothing, and stores nothing (the owner's comment: add_work_in_row
// is asked first, with a stand-in image, so that a refused sample leaves no copy).
TEST_CASE("Sample paintings: choosing a sample with no wall labelled A shows a message, and adds "
          "and stores nothing",
          "[ui]") {
    cs::ui::test::require_offscreen();
    MainWindow window;
    register_samples(window, test_samples(), test_samples());
    SECTION("an empty project") {}
    SECTION("a room whose wall A is renamed") {
        build_room(window);
        const std::optional<doc::Uuid> wall_a =
            cs::editor::find_wall_labelled(window.document(), "A");
        REQUIRE(wall_a.has_value());
        doc::Wall wall = *window.document().find_entity(*wall_a)->wall;
        wall.label = "Lobby";
        doc::Transaction rename(window.document(), "Rename wall");
        rename.set(*wall_a, wall);
        window.commit(std::move(rename));
        REQUIRE_FALSE(cs::editor::find_wall_labelled(window.document(), "A").has_value());
    }
    const doc::Document before = window.document();
    const std::size_t steps = window.history().undo_count();

    check_one_message(choose_refused(window, "test-sample"));

    CHECK(window.document() == before);
    CHECK(window.history().undo_count() == steps);
    CHECK(file_names(window.assets_folder()).empty());
}

// The owner's comment: add_work_in_row "throws std::invalid_argument whose message, 'Wall A is
// full: …', is written for the curator. Show it as it is, in a plain-text message box", and call
// it before store_image, "so a sample that does not fit leaves no copy in the assets folder".
//
// Face A is 6 m long and a row must stay row_end_clearance_m = 0.3 m from each end: at most 5.4 m.
// A 4.6 m work in a box frame takes 4.71 m, which fits alone; with the 0.4 m gap and the sample's
// 0.51 m the row would be 5.62 m. A sample 20 m wide, the widest a manifest may give, fits on no
// wall of the room.
TEST_CASE("Sample paintings: a sample wall A has no room for is refused with the editor's 'Wall A "
          "is full' message, and nothing is added or stored",
          "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-wall-full");
    copy_test_sample(folder);
    write_manifest(folder, "widest", with_size_mm(sample_manifest("widest"), 20000.0, 15000.0));
    fs::copy_file(test_samples() / "test-sample.png", folder / "widest.png");
    MainWindow window;
    register_samples(window, folder, folder);
    build_room(window);
    std::string id;
    SECTION("a 4.6 m work already on wall A") {
        const Hung existing = hang_existing(window, 4.6, 1.0);
        // The premise, from hung_width_m: the two would not fit.
        doc::Work sample_alone;
        sample_alone.form.width_m = 0.4;
        sample_alone.form.height_m = 0.3;
        doc::Framing box;
        box.style = std::string(cs::editor::default_frame_style);
        const double sample_m = cs::editor::hung_width_m(sample_alone, box);
        const double existing_m = hung_width_of(window.document(), existing.work, existing.exhibit);
        REQUIRE(existing_m + cs::editor::row_gap_m + sample_m >
                6.0 - 2.0 * cs::editor::row_end_clearance_m);
        id = "test-sample";
    }
    SECTION("a sample 20 m wide on an empty wall A") {
        id = "widest";
    }
    CAPTURE(id);
    const doc::Document before = window.document();
    const std::size_t steps = window.history().undo_count();

    const std::vector<SeenModal> seen = choose_refused(window, id);
    check_one_message(seen);
    REQUIRE_FALSE(seen.empty());
    CHECK_THAT(seen[0].text, Catch::Matchers::StartsWith("Wall A is full"));

    CHECK(window.document() == before);
    CHECK(window.history().undo_count() == steps);
    CHECK(file_names(window.assets_folder()).empty());
}

// The contract: an image whose bytes do not hash to the manifest's image_sha256 (another PNG in
// its place, or the image cut short) is refused with a message box: nothing is added and the image
// is not stored. Input from outside is hostile (CLAUDE.md): an image file that hashes as the
// manifest says but is no image is refused in the same way, as store_image refuses it (the owner's
// comment of M1-47 for Add painting: show its message and add nothing).
TEST_CASE("Sample paintings: an image that is not the manifest's is refused with a message, and "
          "nothing is added or stored",
          "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path folder = cs::ui::test::fresh_out_folder("sample-paintings-bad-image");
    SECTION("another PNG in its place") {
        fs::copy_file(test_samples() / "test-sample.json", folder / "test-sample.json");
        fs::copy_file(skeleton_chart(), folder / "test-sample.png");
    }
    SECTION("the image cut short") {
        fs::copy_file(test_samples() / "test-sample.json", folder / "test-sample.json");
        const std::string bytes = read_bytes(test_samples() / "test-sample.png");
        write_file(folder / "test-sample.png", bytes.substr(0, bytes.size() / 2));
    }
    SECTION("text whose hash is the manifest's image_sha256") {
        const std::string text = "These are notes, not an image.\n";
        write_manifest(folder, "test-sample",
                       with(test_manifest(), QStringLiteral("image_sha256"),
                            QString::fromStdString(sha256_hex(text))));
        write_file(folder / "test-sample.png", text);
    }
    MainWindow window;
    register_samples(window, folder, folder);
    build_room(window);
    const doc::Document before = window.document();
    const std::size_t steps = window.history().undo_count();

    check_one_message(choose_refused(window, "test-sample"));

    CHECK(window.document() == before);
    CHECK(window.history().undo_count() == steps);
    CHECK(file_names(window.assets_folder()).empty());
}

// ==== The data ================================================================================

// Acceptance criterion: "the test image is made by the project". The committed test data agree
// with each other: test-sample.png is a 4 x 3 PNG, 4:3 as the work's 400 x 300 mm, whose SHA-256
// is test-sample.json's image_sha256; missing-sample.png is never committed.
TEST_CASE("Sample paintings: test-sample.png is a 4 x 3 PNG whose SHA-256 is test-sample.json's "
          "image_sha256",
          "[ui]") {
    cs::ui::test::require_offscreen();
    const QJsonObject manifest = test_manifest();
    CHECK(manifest.value(QStringLiteral("image_sha256")).toString().toStdString() ==
          test_sample_sha256);
    const std::string bytes = read_bytes(test_samples() / "test-sample.png");
    CHECK(sha256_hex(bytes) == test_sample_sha256);
    QImage image;
    REQUIRE(image.loadFromData(qbytes(bytes), "PNG"));
    CHECK(image.width() == 4);
    CHECK(image.height() == 3);
    CHECK(image.width() == manifest.value(QStringLiteral("image_width_px")).toInt());
    CHECK(image.height() == manifest.value(QStringLiteral("image_height_px")).toInt());
    CHECK(image.width() * manifest.value(QStringLiteral("height_mm")).toInt() ==
          image.height() * manifest.value(QStringLiteral("width_mm")).toInt());
    CHECK_FALSE(fs::exists(test_samples() / "missing-sample.png"));
}

// Acceptance criteria: "data/samples/paintings/ holds 6 to 8 paintings, one JSON file each";
// "Works > Add sample painting lists the samples ... A sample whose image has not been fetched is
// disabled, with a tip naming the script. The app finds the folder through a compile definition."
// The application's window (its registry's register_sample_paintings, with the compile
// definitions' folders) lists every manifest of data/samples/paintings/, in file-name order, each
// enabled exactly when build/samples/paintings/<id>.<extension of its image URL> exists, as
// scripts/fetch-samples.py leaves it. It holds whether or not the images have been fetched.
TEST_CASE("Sample paintings: the application lists data/samples/paintings/, each sample enabled "
          "exactly when its image is in build/samples/paintings/",
          "[ui]") {
    cs::ui::test::require_offscreen();
    const fs::path root(CS_SOURCE_DIR);
    const fs::path manifests = root / "data" / "samples" / "paintings";
    const fs::path images = root / "build" / "samples" / "paintings";
    std::vector<fs::path> files;
    if (fs::is_directory(manifests)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(manifests)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    INFO(manifests.string() << " holds " << files.size() << " manifests");
    REQUIRE(files.size() >= 6);
    REQUIRE(files.size() <= 8);

    struct Sample {
        std::string id;
        QString title;
        QString artist;
        bool fetched = false;
    };
    std::vector<Sample> samples;
    std::vector<std::string> ids;
    for (const fs::path& file : files) {
        const QJsonObject manifest = read_manifest(file);
        Sample sample;
        sample.id = manifest.value(QStringLiteral("id")).toString().toStdString();
        CHECK(sample.id == file.stem().string());
        sample.title = manifest.value(QStringLiteral("title")).toString();
        sample.artist = manifest.value(QStringLiteral("artist"))
                            .toObject()
                            .value(QStringLiteral("name"))
                            .toString();
        const QString path = QUrl(manifest.value(QStringLiteral("image_url")).toString()).path();
        const QString extension = path.mid(path.lastIndexOf(QLatin1Char('.')) + 1);
        sample.fetched = fs::exists(images / (sample.id + "." + extension.toStdString()));
        ids.push_back(sample.id);
        samples.push_back(std::move(sample));
    }

    std::unique_ptr<MainWindow> window = app_window();
    CHECK(add_sample_action(*window)->isEnabled());
    const QMenu& menu = samples_menu(*window);
    REQUIRE(item_ids(menu) == ids);
    std::size_t fetched = 0;
    for (const Sample& sample : samples) {
        INFO("the sample " << sample.id << (sample.fetched ? ", fetched" : ", not fetched"));
        const QAction* item = sample_item(menu, sample.id);
        CHECK(item->isEnabled() == sample.fetched);
        const QString text = cs::ui::test::plain_text(item->text());
        INFO("its item: " << text.toStdString());
        CHECK(text.startsWith(sample.title + QStringLiteral(", ") + sample.artist +
                              QStringLiteral(" (")));
        CHECK(text.contains(QChar(0x00D7)));
        CHECK(text.endsWith(QStringLiteral(" cm)")));
        if (!sample.fetched) {
            CHECK(item->toolTip().contains(fetch_script));
            CHECK(item->statusTip().contains(fetch_script));
        }
        fetched += sample.fetched ? 1 : 0;
    }
    WARN(fetched << " of " << samples.size()
                 << " samples' images are in build/samples/paintings/ (scripts/fetch-samples.py)");
}

// ==== The screenshot ==========================================================================

// Acceptance criterion: "the pull request shows a screenshot of three samples side by side and
// lit". The test makes it, to build/<preset>/ui-out/m1-sample-paintings.png, in the application's
// window: New room through the menu (which turns the camera to face wall A), then three samples
// of very different sizes, each lit by Lights > Add spot just after it is hung (Add spot lights the
// exhibit with the greatest id, the sample just hung), and the lux panel up to date. It needs the
// real images: it skips until scripts/fetch-samples.py has fetched them.
TEST_CASE("Sample paintings: the screenshot of three samples side by side, each lit", "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    const fs::path images = fs::path(CS_SOURCE_DIR) / "build" / "samples" / "paintings";
    // 22.8 cm, 91.8 cm and 1.63 m on their long sides: a 3.86 m row in its frames.
    const std::vector<std::string> ids{"vermeer-girl-with-the-red-hat",
                                       "van-gogh-the-large-plane-trees",
                                       "church-twilight-in-the-wilderness"};
    for (const std::string& id : ids) {
        if (!fs::exists(images / (id + ".jpg"))) {
            SKIP("the sample " << id << " is not fetched: run python3 scripts/fetch-samples.py");
        }
    }

    std::unique_ptr<MainWindow> window = cs::ui::test::make_shell();
    cs::ui::Viewport* viewport = window->viewport();
    REQUIRE(viewport->headless());
    window->show();
    window->activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(window.get()));
    auto* lux_dock = window->findChild<QDockWidget*>(QStringLiteral("lux_panel"));
    REQUIRE(lux_dock != nullptr);
    const auto* lux_table = lux_dock->findChild<QTreeWidget*>(QStringLiteral("lux_table"));
    const auto* lux_status = lux_dock->findChild<QLabel*>(QStringLiteral("lux_status"));
    REQUIRE(lux_table != nullptr);
    REQUIRE(lux_status != nullptr);

    {
        // Room > New room..., its dialog accepted with its defaults (6 x 4 x 3.5 m).
        QAction* new_room = menu_action(*window, QStringLiteral("Room"),
                                        QStringLiteral("New room") + QChar(0x2026));
        ModalAnswerer answers({accept_as_is()});
        new_room->trigger();
        const bool finished = answers.finish();
        INFO("modals:\n" << answers.log());
        REQUIRE(finished);
        REQUIRE(answers.seen().size() == 1);
        REQUIRE_FALSE(answers.seen()[0].unexpected);
    }
    for (const std::string& id : ids) {
        choose_sample(*window, id);
        add_spot(*window);
    }
    CHECK(luminaires_of(window->document()).size() == ids.size());

    // Generous, for lavapipe and the sanitizers; it returns as soon as the panel is up to date.
    constexpr int lux_timeout_ms = 120000;
    const int frames_before = viewport->frames();
    REQUIRE(QTest::qWaitFor(
        [&] { return !lux_status->text().startsWith(QStringLiteral("Calculating")); },
        lux_timeout_ms));
    REQUIRE(lux_table->topLevelItemCount() == static_cast<int>(ids.size()));
    for (int i = 0; i < lux_table->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* row = lux_table->topLevelItem(i);
        INFO(row->text(0).toStdString() << ": " << row->text(1).toStdString() << " lx direct");
        // Each lit by its own spot: tens of lux at least (the walking skeleton's spot gives about
        // 100 lx on a work).
        CHECK(row->data(1, Qt::UserRole).toDouble() > 10.0);
    }

    // A frame drawn after the last change, then a moment for the last one.
    REQUIRE(QTest::qWaitFor([&] { return viewport->frames() > frames_before; },
                            cs::ui::Viewport::settle_ms + 15000));
    QTest::qWait(cs::ui::Viewport::settle_ms);
    REQUIRE_FALSE(viewport->grab_frame().isNull());

    const QImage screenshot = window->screenshot();
    REQUIRE_FALSE(screenshot.isNull());
    const fs::path png = cs::ui::test::ui_out_dir() / "m1-sample-paintings.png";
    fs::remove(png);
    REQUIRE(screenshot.save(qstring(png), "PNG"));
    WARN("the screenshot is " << png.string() << ", " << fs::file_size(png) / 1024 << " KB");

    cs::ui::test::require_no_validation_messages(*window);
}
