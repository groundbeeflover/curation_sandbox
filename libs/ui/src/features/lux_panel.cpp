// SPDX-License-Identifier: Apache-2.0
//
// The lux panel (M1-27): a dock that shows, for every work hung, the mean direct and total lux on
// its face, from the calculation engine (engine::work_lux_grid, the reading cs-render's lux report
// gives), never from rendered pixels (CLAUDE.md).
//
// Each change of the document (document_changed, project_changed) compiles the world on the UI
// thread and reads the meter on a worker thread, so that the window stays responsive. One reading
// runs at a time; a change while it runs is read once it ends, and until then the status says
// "Calculating…". A change that leaves the world, the titles and the assets folder as they were (a
// save in place) reads nothing. The paintings' photographs are loaded from the project's assets
// folder and kept while they are all a reading needs from that folder: without them every painting
// would reflect its 0.8 grey base colour and the total would differ from cs-render's.
//
// M2-37 replaces the computation with the engine's lux service (M2-28).

#include "features/feature.hpp"

#include <cs/core/thread_pool.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>
#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/textures.hpp>
#include <cs/engine/work_lux.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <QAction>
#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMetaObject>
#include <QStringList>
#include <QTextDocument>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cs::ui {

namespace {

// The meter's settings: cs-render's defaults (--lux-samples 1024, --seed 1), written out rather
// than taken from LuxOptions' own, so that the panel reads what cs-render's lux report gives for
// the saved project.
constexpr std::uint32_t lux_samples = 1024;
constexpr std::uint64_t lux_seed = 1;

constexpr double no_reading = std::numeric_limits<double>::quiet_NaN();

// View > Lux, among the panels after View > Walk.
constexpr int lux_item_order = 50;

// What a reading needs, made on the UI thread from the document: the worker never sees it.
struct LuxJob {
    world::World world;
    std::vector<std::string> titles;     // per exhibit, in the world's order
    std::filesystem::path assets_folder; // empty when the world names no photograph
    // The document has a luminaire, on or off: the world leaves out one dimmed to 0 or one that
    // cannot be placed.
    bool has_luminaire = false;

    // Equal worlds and titles, with photographs from the same folder, read the same: a save in
    // place reads nothing. A project whose photographs are elsewhere now (Open, or the first save
    // of an untitled project) is read again from there, as cs-render would read it.
    [[nodiscard]] bool same_reading(const LuxJob& other) const {
        return world == other.world && titles == other.titles &&
               assets_folder == other.assets_folder && has_luminaire == other.has_luminaire;
    }
};

// Why a reading of hung works traced nothing.
enum class Unlit {
    no,           // it traced, or there is no work
    no_luminaire, // the document has no luminaire
    none_on,      // every luminaire is dimmed to 0 or cannot be placed
};

struct WorkReading {
    std::string title;
    double direct_lux = no_reading; // means over the work's 5 x 5 grid
    double total_lux = no_reading;
    std::string problem; // why there is no reading
};

struct LuxResult {
    std::vector<WorkReading> works;
    std::vector<std::string> problems;
    Unlit unlit = Unlit::no;
};

const world::AssetRef* photograph_of(const world::Material& material) {
    const auto* painting = std::get_if<world::PaintingSurface>(&material);
    return painting != nullptr && painting->image ? &*painting->image : nullptr;
}

// The photographs last loaded, and those that could not be, kept for later readings from the same
// folder while they account for every photograph a reading needs: an undo, a redo or a save in
// place then loads none again, nor does an edit while one photograph cannot be read. Only the
// worker thread touches it.
class TextureCache {
  public:
    const engine::LoadedTextures& load(const world::World& world,
                                       const std::filesystem::path& folder) {
        if (!textures_ || folder != folder_ || !holds_all(world)) {
            textures_.emplace(engine::load_textures(world, folder));
            folder_ = folder;
            failed_ = textures_->problems;
        }
        // The problems of the photographs this world names, in the order of its materials.
        textures_->problems.clear();
        for (const world::Material& material : world.materials) {
            const world::AssetRef* photograph = photograph_of(material);
            const engine::TextureProblem* problem =
                photograph != nullptr ? failed(*photograph) : nullptr;
            if (problem != nullptr &&
                std::ranges::find(textures_->problems, *photograph,
                                  &engine::TextureProblem::asset) == textures_->problems.end()) {
                textures_->problems.push_back(*problem);
            }
        }
        return *textures_;
    }

  private:
    const engine::TextureProblem* failed(const world::AssetRef& photograph) const {
        const auto found = std::ranges::find(failed_, photograph, &engine::TextureProblem::asset);
        return found != failed_.end() ? &*found : nullptr;
    }

    bool holds_all(const world::World& world) const {
        return std::ranges::all_of(world.materials, [this](const world::Material& material) {
            const world::AssetRef* photograph = photograph_of(material);
            return photograph == nullptr || textures_->textures.find(*photograph) != nullptr ||
                   failed(*photograph) != nullptr;
        });
    }

    std::optional<engine::LoadedTextures> textures_;
    std::vector<engine::TextureProblem> failed_; // as load_textures reported them
    std::filesystem::path folder_;               // where textures_ came from
};

// The works whose face shows the photograph, by title, for the curator: "“Photo”, “Second”".
std::string works_showing(const LuxJob& job, const world::AssetRef& photograph) {
    std::string named;
    for (std::size_t i = 0; i < job.world.exhibits.size(); ++i) {
        const world::Instance& face =
            job.world.instances.at(job.world.exhibits[i].painting_instance);
        const world::AssetRef* shown = photograph_of(job.world.materials.at(face.material));
        if (shown == nullptr || *shown != photograph) {
            continue;
        }
        named += named.empty() ? "" : ", ";
        named +=
            job.titles[i].empty() ? std::string("an untitled work") : "“" + job.titles[i] + "”";
    }
    return named.empty() ? std::string("a work") : named;
}

LuxResult read_lux(const LuxJob& job, TextureCache& cache, ThreadPool& pool) {
    LuxResult result;
    try {
        const engine::LoadedTextures& loaded = cache.load(job.world, job.assets_folder);
        for (const engine::TextureProblem& problem : loaded.problems) {
            result.problems.push_back("The photograph of " + works_showing(job, problem.asset) +
                                      " cannot be read, so its lux is calculated as if it were "
                                      "plain grey (" +
                                      problem.reason + ")");
        }
        const engine::Scene scene(job.world);
        engine::LuxOptions options;
        options.samples = lux_samples;
        options.seed = lux_seed;
        options.pool = &pool;
        options.textures = &loaded.textures;
        for (std::size_t i = 0; i < job.world.exhibits.size(); ++i) {
            WorkReading reading;
            reading.title = job.titles[i];
            try {
                const engine::WorkLux lux =
                    engine::work_lux_grid(scene, job.world.exhibits[i], options);
                reading.direct_lux = lux.direct_lux.mean;
                reading.total_lux = lux.total_lux.mean;
            } catch (const std::exception& error) {
                reading.problem = error.what();
            }
            result.works.push_back(std::move(reading));
        }
    } catch (const std::exception& error) {
        result.works.clear();
        result.problems.push_back(std::string("The lux cannot be calculated: ") + error.what());
    }
    return result;
}

// The reading of a world with no work, or with no light, where every work reads 0 lx: no path need
// be traced. The engine gives the same (its test "work_lux_grid: with no lights, every point and
// summary reads 0"), since no material emits and a path that leaves the room finds nothing. When
// daylight or an emitting material arrives, this must ask the engine instead.
static_assert(std::variant_size_v<world::Material> == 2,
              "a new kind of material may emit light: check untraced()");
LuxResult untraced(const LuxJob& job) {
    LuxResult result;
    if (job.world.lights.empty()) {
        result.unlit = job.has_luminaire ? Unlit::none_on : Unlit::no_luminaire;
    }
    for (const std::string& title : job.titles) {
        result.works.push_back({title, 0.0, 0.0, {}});
    }
    return result;
}

// Lux as a curator reads it: whole lux, with a tenth below 10 lx, where limits such as 50 lx are
// set and one lux is a large part.
QString lux_text(double lux) {
    return QLocale().toString(lux, 'f', lux < 10.0 ? 1 : 0);
}

class LuxPanel : public QWidget {
  public:
    explicit LuxPanel(MainWindow& window) : window_(window) {
        // One reading at a time, in order.
        jobs_.setMaxThreadCount(1);

        table_ = new QTreeWidget(this);
        table_->setObjectName(QStringLiteral("lux_table"));
        table_->setColumnCount(3);
        table_->setHeaderLabels(
            {MainWindow::tr("Work"), MainWindow::tr("Direct (lx)"), MainWindow::tr("Total (lx)")});
        table_->setRootIsDecorated(false);
        table_->setUniformRowHeights(true);
        table_->setSelectionMode(QAbstractItemView::NoSelection);
        table_->setFocusPolicy(Qt::NoFocus);
        QTreeWidgetItem* header = table_->headerItem();
        header->setToolTip(1, MainWindow::tr("Light that reaches the work straight from the "
                                             "fixtures: the mean of 25 points on its face"));
        header->setToolTip(2, MainWindow::tr("Direct light plus the light the room reflects onto "
                                             "the work: the mean of 25 points on its face"));
        header->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        header->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        table_->header()->setStretchLastSection(false);
        table_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("lux_status"));
        status_->setWordWrap(true);
        status_->setTextFormat(Qt::PlainText); // it may name works by their titles
        status_->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto* layout = new QVBoxLayout(this);
        layout->addWidget(table_, 1);
        layout->addWidget(status_);
    }

    ~LuxPanel() override {
        // The reading uses the pool, the cache and this panel: it ends before they go.
        jobs_.waitForDone();
    }

    LuxPanel(const LuxPanel&) = delete;
    LuxPanel& operator=(const LuxPanel&) = delete;
    LuxPanel(LuxPanel&&) = delete;
    LuxPanel& operator=(LuxPanel&&) = delete;

    // The document changed: read it again.
    void request() {
        if (running_) {
            // finished() reads the document as it is then.
            calculating();
            return;
        }
        start();
    }

  private:
    // The numbers shown are of the document as it was: greyed until the new reading.
    void calculating() {
        table_->setEnabled(false);
        status_->setText(MainWindow::tr("Calculating…"));
    }

    std::shared_ptr<const LuxJob> make_job() {
        auto job = std::make_shared<LuxJob>();
        const doc::Document& document = window_.document();
        job->world = world::compile(document);
        for (const world::Exhibit& exhibit : job->world.exhibits) {
            const doc::Work* work = document.find_work(doc::Uuid(exhibit.work.bytes()));
            job->titles.push_back(work != nullptr ? work->title : std::string());
        }
        job->has_luminaire = std::ranges::any_of(document.entities(), [](const auto& entry) {
            return entry.second.luminaire.has_value();
        });
        // Only then, since an untitled project's folder is made the first time it is asked for.
        if (std::ranges::any_of(job->world.materials, photograph_of)) {
            job->assets_folder = window_.assets_folder();
        }
        return job;
    }

    void start() {
        std::shared_ptr<const LuxJob> job;
        try {
            job = make_job();
        } catch (const std::exception& error) {
            shown_.reset();
            LuxResult failed;
            failed.problems.push_back(std::string("The lux cannot be calculated: ") + error.what());
            display(failed);
            return;
        }
        if (shown_ && job->same_reading(*shown_)) {
            return;
        }
        if (job->world.exhibits.empty() || job->world.lights.empty()) {
            show_reading(job, untraced(*job));
            return;
        }
        running_ = true;
        calculating();
        jobs_.start([this, job] {
            if (!pool_) {
                pool_.emplace(std::max(1u, std::thread::hardware_concurrency()));
            }
            auto result = std::make_shared<LuxResult>(read_lux(*job, cache_, *pool_));
            QMetaObject::invokeMethod(
                this, [this, job, result] { finished(job, *result); }, Qt::QueuedConnection);
        });
    }

    // Shows the reading, then reads the document again if it changed meanwhile: until that one is
    // shown, the status says "Calculating…".
    void finished(const std::shared_ptr<const LuxJob>& job, const LuxResult& result) {
        running_ = false;
        show_reading(job, result);
        start();
    }

    void show_reading(std::shared_ptr<const LuxJob> job, const LuxResult& result) {
        shown_ = std::move(job);
        display(result);
    }

    void display(const LuxResult& result) {
        table_->setEnabled(true);
        table_->clear();
        for (const WorkReading& reading : result.works) {
            auto* item = new QTreeWidgetItem(table_);
            item->setText(0, reading.title.empty() ? MainWindow::tr("Untitled work")
                                                   : QString::fromStdString(reading.title));
            // As written: a title comes from a project file, and Qt reads markup in a tooltip as
            // rich text, which loads <img> sources (on Windows from network shares).
            item->setToolTip(0, Qt::convertFromPlainText(item->text(0), Qt::WhiteSpaceNormal));
            for (const int column : {1, 2}) {
                const double lux = column == 1 ? reading.direct_lux : reading.total_lux;
                item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
                if (std::isnan(lux)) {
                    item->setText(column, QStringLiteral("—"));
                    item->setToolTip(column, QString::fromStdString(reading.problem));
                } else {
                    item->setText(column, lux_text(lux));
                    item->setData(column, Qt::UserRole, lux);
                }
            }
        }
        QStringList lines;
        if (result.works.empty() && result.problems.empty()) {
            lines << MainWindow::tr("No works hung yet: Works > Add painting…");
        } else if (result.unlit == Unlit::no_luminaire) {
            lines << MainWindow::tr("No light in the room yet: Lights > Add spot");
        } else if (result.unlit == Unlit::none_on) {
            lines << MainWindow::tr("No light reaches the works: every light is off or cannot be "
                                    "placed");
        } else if (!result.works.empty()) {
            lines << MainWindow::tr("Mean of 25 points on each work's face");
        }
        for (const std::string& problem : result.problems) {
            lines << QString::fromStdString(problem);
        }
        status_->setText(lines.join(QLatin1Char('\n')));
    }

    MainWindow& window_;
    // The worker's alone, and declared before jobs_, so that they outlive it. The pool is made
    // with the first reading, so that a window that reads nothing starts no threads.
    std::optional<ThreadPool> pool_;
    TextureCache cache_;
    QTreeWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
    std::shared_ptr<const LuxJob> shown_; // the reading the panel shows
    bool running_ = false;
    QThreadPool jobs_;
};

} // namespace

void register_lux_panel(MainWindow& window) {
    auto* dock = new QDockWidget(MainWindow::tr("Lux"), &window);
    dock->setObjectName(QStringLiteral("lux_panel"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* panel = new LuxPanel(window);
    dock->setWidget(panel);
    window.addDockWidget(Qt::RightDockWidgetArea, dock);
    // View > Lux brings the dock back after its title bar's x closed it, and closes it too.
    QAction* item = window.add_menu_item(window.add_menu(MainWindow::tr("&View"), menu_order::view),
                                         lux_item_order, MainWindow::tr("&Lux"));
    item->setCheckable(true);
    item->setChecked(true);
    item->setStatusTip(MainWindow::tr("Shows or hides the lux on every work"));
    QObject::connect(item, &QAction::toggled, dock, &QDockWidget::setVisible);
    QObject::connect(dock->toggleViewAction(), &QAction::toggled, item, &QAction::setChecked);
    QObject::connect(&window, &MainWindow::document_changed, panel, [panel] { panel->request(); });
    QObject::connect(&window, &MainWindow::project_changed, panel, [panel] { panel->request(); });
    panel->request();
}

} // namespace cs::ui
