// SPDX-License-Identifier: Apache-2.0
//
// Works > Add sample painting (M1-50): real paintings from museums' open-access collections, of
// very different sizes, to try the room, the hanging and the light without finding images first.
//
// Each sample is a manifest, data/samples/paintings/<id>.json, naming the work, its unframed size
// in millimetres, its source and licence, and its image's URL and SHA-256. The images are not
// committed (ADR 0004): scripts/fetch-samples.py downloads them into build/samples/paintings/, as
// <id>.<extension of the URL>. The app finds both folders through compile definitions until M4
// packages them, and never loads an image from a museum's server. A sample whose image is not
// there is listed but disabled, with a tip naming the script.
//
// Choosing a sample hangs it as Works > Add painting does (skeleton_commands.cpp): asked first with
// a stand-in image, so that one wall A cannot take leaves no copy in the assets folder; then the
// image is stored through assets::store_image and the work committed in one transaction, with its
// artist, date, medium and catalogue source.
//
// Manifests are input from outside (CLAUDE.md): Qt's parser reads them, and every field this file
// uses is checked and bounded before it is shown or used to make a path. A manifest that fails is
// left out of the menu.

#include "features/feature.hpp"

#include <cs/assets/project_assets.hpp>
#include <cs/assets/texture.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>
#include <cs/editor/commands/hanging.hpp>
#include <cs/editor/commands/room.hpp>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QString>
#include <QTextDocument>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cs::ui {

namespace fs = std::filesystem;

namespace {

// Works, the menu skeleton_commands.cpp makes at the same order; the samples come after Add
// painting (order 0).
constexpr int works_menu_order = 40;
constexpr int add_sample_order = 10;
// How long the status bar says what the command did.
constexpr int passing_message_ms = 10000;

// Bounds on what is read (CLAUDE.md): a manifest is about 1 KiB.
constexpr qint64 max_manifest_bytes = 64 * 1024;
constexpr std::size_t max_samples = 64;
constexpr std::size_t max_folder_entries = 4096;
constexpr std::size_t max_id_length = 64;
// The largest side Add painting takes, 20 m.
constexpr double max_side_mm = 20000.0;
constexpr qint64 hash_chunk_bytes = qint64{1} << 20;

const QString fetch_command = QStringLiteral("python3 scripts/fetch-samples.py");

struct Sample {
    std::string id;
    std::string title;
    std::string artist;
    std::string date;
    std::string medium;
    std::string institution;
    std::string accession_number;
    std::string credit_line;
    std::string licence;
    std::string image_sha256; // 64 lowercase hexadecimal digits
    std::string extension;    // of the image: "jpg", "jpeg" or "png"
    double width_mm = 0.0;
    double height_mm = 0.0;

    [[nodiscard]] fs::path image_in(const fs::path& images) const {
        return images / (id + "." + extension); // id and extension are checked ASCII
    }
};

// ---- Reading the manifests -------------------------------------------------------------------

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > max_id_length || id.front() == '-') {
        return false;
    }
    return std::ranges::all_of(
        id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool valid_sha256(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// A string field of at most doc::max_string_bytes of UTF-8 (the project file's bound, so that the
// work can be saved); an absent field is empty. Nothing when the field is there but is not such a
// string.
std::optional<std::string> text(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        return std::string();
    }
    if (!value.isString()) {
        return std::nullopt;
    }
    std::string utf8 = value.toString().toStdString();
    if (utf8.size() > doc::max_string_bytes) {
        return std::nullopt;
    }
    return utf8;
}

// A side in millimetres: a finite number greater than 0, at most max_side_mm.
std::optional<double> side_mm(const QJsonObject& object, const QString& key) {
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double mm = value.toDouble();
    if (!std::isfinite(mm) || mm <= 0.0 || mm > max_side_mm) {
        return std::nullopt;
    }
    return mm;
}

// The image's extension from its URL, which must be https: what scripts/fetch-samples.py names
// the downloaded file by.
std::optional<std::string> image_extension(const std::string& url_text) {
    const QUrl url(QString::fromStdString(url_text), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty()) {
        return std::nullopt;
    }
    const QString path = url.path(QUrl::FullyEncoded);
    for (const char* extension : {"jpg", "jpeg", "png"}) {
        if (path.endsWith(QLatin1Char('.') + QLatin1String(extension))) {
            return std::string(extension);
        }
    }
    return std::nullopt;
}

// The sample a manifest describes, or nothing when it cannot be used (the reason is logged).
std::optional<Sample> read_manifest(const fs::path& path) {
    const QString name = QString::fromStdU16String(path.filename().u16string());
    const auto refuse = [&name](const char* why) {
        qWarning("Sample painting %s left out: %s", qUtf8Printable(name), why);
        return std::nullopt;
    };
    QFile file(QString::fromStdU16String(path.u16string()));
    if (!file.open(QIODevice::ReadOnly)) {
        return refuse("cannot be read");
    }
    const QByteArray bytes = file.read(max_manifest_bytes + 1);
    if (bytes.size() > max_manifest_bytes) {
        return refuse("larger than 64 KiB");
    }
    QJsonParseError error{};
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return refuse("not a JSON object");
    }
    const QJsonObject object = json.object();

    Sample sample;
    const std::optional<std::string> id = text(object, QStringLiteral("id"));
    // The file is named after the id, which is ASCII, so the comparison converts nothing.
    if (!id || !valid_id(*id) || path.filename() != fs::path(*id + ".json")) {
        return refuse("its id is not 1 to 64 of a-z, 0-9 and '-', or not its file's name");
    }
    sample.id = *id;
    const QJsonValue artist = object.value(QStringLiteral("artist"));
    const std::optional<std::string> artist_name =
        artist.isObject() ? text(artist.toObject(), QStringLiteral("name")) : std::nullopt;
    const std::optional<std::string> title = text(object, QStringLiteral("title"));
    if (!title || title->empty() || !artist_name || artist_name->empty()) {
        return refuse("it needs a title and an artist's name, each at most 4096 bytes");
    }
    sample.title = *title;
    sample.artist = *artist_name;
    for (auto [key, field] :
         {std::pair{"date", &sample.date}, std::pair{"medium", &sample.medium},
          std::pair{"institution", &sample.institution},
          std::pair{"accession_number", &sample.accession_number},
          std::pair{"credit_line", &sample.credit_line}, std::pair{"licence", &sample.licence}}) {
        std::optional<std::string> value = text(object, QString::fromLatin1(key));
        if (!value) {
            return refuse("a text field is not a string of at most 4096 bytes");
        }
        *field = std::move(*value);
    }
    const std::optional<double> width = side_mm(object, QStringLiteral("width_mm"));
    const std::optional<double> height = side_mm(object, QStringLiteral("height_mm"));
    if (!width || !height) {
        return refuse("width_mm and height_mm must be greater than 0 and at most 20000");
    }
    sample.width_mm = *width;
    sample.height_mm = *height;
    const std::optional<std::string> url = text(object, QStringLiteral("image_url"));
    std::optional<std::string> extension = url ? image_extension(*url) : std::nullopt;
    if (!extension) {
        return refuse("image_url is not an https URL ending in .jpg, .jpeg or .png");
    }
    sample.extension = std::move(*extension);
    const std::optional<std::string> sha256 = text(object, QStringLiteral("image_sha256"));
    if (!sha256 || !valid_sha256(*sha256)) {
        return refuse("image_sha256 is not 64 lowercase hexadecimal digits");
    }
    sample.image_sha256 = *sha256;
    return sample;
}

// The samples of a folder's manifests (*.json), in order of file name; at most max_samples.
std::vector<Sample> read_samples(const fs::path& manifests) {
    std::vector<fs::path> files;
    std::error_code error;
    fs::directory_iterator entries(manifests, error);
    std::size_t seen = 0;
    for (; !error && entries != fs::directory_iterator() && seen < max_folder_entries;
         entries.increment(error), ++seen) {
        const fs::directory_entry& entry = *entries;
        std::error_code type_error;
        if (entry.path().extension() == ".json" && entry.is_regular_file(type_error)) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    if (files.size() > max_samples) {
        files.resize(max_samples);
    }
    std::vector<Sample> samples;
    for (const fs::path& file : files) {
        try {
            if (std::optional<Sample> sample = read_manifest(file)) {
                samples.push_back(std::move(*sample));
            }
        } catch (const std::exception& e) {
            // A name std::filesystem cannot convert (not valid UTF-8, or UTF-16 on Windows).
            qWarning("A sample painting's manifest left out: %s", e.what());
        }
    }
    return samples;
}

// Whether a file's SHA-256 is `expected`. Reads at most max_texture_file_bytes, more than
// store_image would take.
bool has_sha256(const fs::path& path, const std::string& expected) {
    QFile file(QString::fromStdU16String(path.u16string()));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::uintmax_t total = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(hash_chunk_bytes);
        if (chunk.isEmpty()) {
            return false; // a read error
        }
        total += static_cast<std::uintmax_t>(chunk.size());
        if (total > assets::max_texture_file_bytes) {
            return false;
        }
        hash.addData(chunk);
    }
    return hash.result().toHex().toStdString() == expected;
}

// ---- The menu --------------------------------------------------------------------------------

QString from_utf8(const std::string& text) {
    return QString::fromStdString(text);
}

// A menu item's text shows '&' only as "&&"; a single one marks a mnemonic.
QString menu_text(const std::string& text) {
    return from_utf8(text).replace(QLatin1Char('&'), QStringLiteral("&&"));
}

// Millimetres as centimetres, to the millimetre: 400 gives "40", 181 gives "18.1".
QString centimetres(double mm, const QLocale& locale) {
    const double rounded_mm = std::round(mm);
    const bool whole = std::fmod(rounded_mm, 10.0) == 0.0;
    return locale.toString(rounded_mm / 10.0, 'f', whole ? 0 : 1);
}

// "Title, Artist (W × H cm)".
QString item_text(const Sample& sample) {
    QLocale locale;
    locale.setNumberOptions(QLocale::OmitGroupSeparator);
    return MainWindow::tr("%1, %2 (%3 × %4 cm)")
        .arg(menu_text(sample.title), menu_text(sample.artist),
             centimetres(sample.width_mm, locale), centimetres(sample.height_mm, locale));
}

// A tooltip shown as written: Qt reads markup in a tooltip that looks like rich text.
QString plain_tooltip(const QString& text) {
    return Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal);
}

// ---- Choosing a sample -----------------------------------------------------------------------

// A message box that shows its text as written, as skeleton_commands.cpp's: titles come from
// manifests, which are hostile (CLAUDE.md).
void warn(MainWindow& window, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, &window);
    box.setTextFormat(Qt::PlainText);
    box.exec();
}

// The wait cursor while the window cannot answer (the image is hashed, decoded and uploaded on the
// UI thread).
class Busy {
  public:
    Busy() { QApplication::setOverrideCursor(Qt::WaitCursor); }
    ~Busy() { QApplication::restoreOverrideCursor(); }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
    Busy(Busy&&) = delete;
    Busy& operator=(Busy&&) = delete;
};

QString not_fetched_tip() {
    return MainWindow::tr("Its image is not downloaded yet: run %1 in the source folder")
        .arg(fetch_command);
}

void add_sample(MainWindow& window, const Sample& sample, const fs::path& images) {
    const QString cannot_add = MainWindow::tr("Cannot add the sample painting");
    const QString title = from_utf8(sample.title);
    // Asked first, before anything is read, as Add painting asks.
    if (!editor::find_wall_labelled(window.document(), editor::default_wall_label)) {
        warn(window, cannot_add,
             MainWindow::tr("A painting hangs on wall A, and this project has no wall labelled A. "
                            "Make a room first: Room > New room."));
        return;
    }
    // The image may have gone since the menu was shown.
    const fs::path image = sample.image_in(images);
    std::error_code error;
    if (!fs::is_regular_file(image, error)) {
        warn(window, cannot_add, MainWindow::tr("“%1”: %2").arg(title, not_fetched_tip()));
        return;
    }
    const double width_m = sample.width_mm / 1000.0;
    const double height_m = sample.height_mm / 1000.0;
    // Tried first with a stand-in, so that a sample wall A cannot take leaves no copy of its image
    // in the project's assets.
    try {
        const doc::AssetRef stand_in{.sha256 = std::string(64, '0'), .extension = "png"};
        [[maybe_unused]] const editor::AddedWork trial = editor::add_work_in_row(
            window.document(), window.ids(), stand_in, width_m, height_m, sample.title);
    } catch (const std::exception& e) {
        warn(window, cannot_add, QString::fromUtf8(e.what()));
        return;
    }
    const QString changed =
        MainWindow::tr(
            "The image of “%1” is not the one its manifest names: it was changed after it was "
            "downloaded. Run %2 in the source folder to download it again.")
            .arg(title, fetch_command);
    // Checked before it is stored, so that an image that is not the sample's leaves no copy; and
    // after, in case it changed in between.
    bool unchanged = false;
    assets::AssetName asset;
    try {
        const Busy busy;
        unchanged = has_sha256(image, sample.image_sha256);
        if (unchanged) {
            asset = assets::store_image(image, window.assets_folder());
        }
    } catch (const std::exception& e) {
        warn(window, cannot_add,
             MainWindow::tr("The image of “%1” cannot be stored in the project: %2")
                 .arg(title, QString::fromUtf8(e.what())));
        return;
    }
    if (!unchanged || asset.sha256 != sample.image_sha256) {
        warn(window, cannot_add, changed);
        return;
    }
    try {
        editor::AddedWork added = editor::add_work_in_row(
            window.document(), window.ids(), doc::AssetRef{asset.sha256, asset.extension}, width_m,
            height_m, sample.title);
        doc::Work work = *added.transaction.find_work(added.work);
        work.artist = sample.artist;
        work.date = sample.date;
        work.medium = sample.medium;
        work.credit_line = sample.credit_line;
        if (!sample.accession_number.empty()) {
            work.accession_number = sample.accession_number;
        }
        if (!sample.licence.empty()) {
            work.licence = sample.licence;
        }
        work.source = doc::FromCatalogue{sample.id, sample.image_sha256};
        added.transaction.set_work(added.work, std::move(work));
        {
            const Busy busy; // the image goes to the GPU
            window.commit(std::move(added.transaction));
        }
        window.statusBar()->showMessage(
            MainWindow::tr("“%1” hangs on wall A. Lights > Add spot lights it").arg(title),
            passing_message_ms);
    } catch (const std::exception& e) {
        warn(window, cannot_add, QString::fromUtf8(e.what()));
    }
}

// Lists the samples in the submenu again, each enabled when its image is there.
void fill(MainWindow& window, QAction& item, QMenu& menu, const fs::path& manifests,
          const fs::path& images) {
    menu.clear(); // deletes the items it made
    const std::vector<Sample> samples = read_samples(manifests);
    item.setEnabled(!samples.empty());
    for (const Sample& sample : samples) {
        QAction* action = menu.addAction(item_text(sample));
        action->setData(QString::fromStdString(sample.id));
        std::error_code error;
        if (fs::is_regular_file(sample.image_in(images), error)) {
            const QString source =
                MainWindow::tr("%1, %2. %3, %4")
                    .arg(from_utf8(sample.medium), from_utf8(sample.date),
                         from_utf8(sample.institution), from_utf8(sample.accession_number));
            action->setStatusTip(source);
            action->setToolTip(plain_tooltip(source));
            QObject::connect(action, &QAction::triggered, &window,
                             [&window, sample, images] { add_sample(window, sample, images); });
        } else {
            // In a tooltip too: some styles (Fusion, Breeze) show no status tip for a disabled
            // item.
            action->setEnabled(false);
            action->setStatusTip(not_fetched_tip());
            action->setToolTip(plain_tooltip(not_fetched_tip()));
        }
    }
}

} // namespace

// Works > Add sample painting, listing the manifests (*.json) of `manifests`, whose images are
// looked for in `images`. The ui tests call it with folders of their own.
void register_sample_paintings_from(MainWindow& window, const fs::path& manifests,
                                    const fs::path& images) {
    QMenu* works = window.add_menu(MainWindow::tr("&Works"), works_menu_order);
    QAction* item =
        window.add_menu_item(works, add_sample_order, MainWindow::tr("Add &sample painting"));
    item->setStatusTip(MainWindow::tr("Hangs a painting from a museum's open-access collection at "
                                      "its true size, beside the others on wall A"));
    auto* menu = new QMenu(works);
    menu->setObjectName(QStringLiteral("sample_paintings_menu"));
    menu->setToolTipsVisible(true);
    item->setMenu(menu);
    // Read again whenever Works or the submenu opens, so that images downloaded while the app runs
    // are offered.
    const auto refresh = [&window, item, menu, manifests, images] {
        fill(window, *item, *menu, manifests, images);
    };
    refresh();
    QObject::connect(works, &QMenu::aboutToShow, menu, refresh);
    QObject::connect(menu, &QMenu::aboutToShow, menu, refresh);
}

// The samples of the source tree (libs/ui/CMakeLists.txt); packaging them is M4's.
void register_sample_paintings(MainWindow& window) {
    register_sample_paintings_from(window, fs::path(u8"" CS_SAMPLE_MANIFESTS_DIR),
                                   fs::path(u8"" CS_SAMPLE_IMAGES_DIR));
}

} // namespace cs::ui
