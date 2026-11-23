// SPDX-License-Identifier: Apache-2.0
//
// The lux panel (M1-27): a dock that shows, for every work hung, the mean direct and total lux on
// its face, from the calculation engine (engine::work_lux_grid, the reading cs-render's lux report
// gives), never from rendered pixels (CLAUDE.md).
//
// Each change of the document (document_changed, project_changed) compiles the world on the UI
// thread and reads the meter on a worker thread, so that the window stays responsive. One reading
// runs at a time; when the document changed again while it ran, its result is dropped and the
// newest document read instead, so the panel shows only readings of the document as it is. The
// paintings' photographs are loaded from the project's assets folder, once while the folder and
// the set of images stay the same: without them every painting would reflect its 0.8 grey base
// colour and the total would differ from cs-render's.
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

#include <QDockWidget>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMetaObject>
#include <QStringList>
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
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cs::ui {

namespace {

// The meter's settings: cs-render's defaults (--lux-samples 1024, --seed 1), so that the panel
// reads what cs-render's lux report gives for the saved project.
constexpr std::uint32_t lux_samples = 1024;
constexpr std::uint64_t lux_seed = 1;

constexpr double no_reading = std::numeric_limits<double>::quiet_NaN();

// What a reading needs, copied on the UI thread: the worker never sees the document.
struct LuxJob {
    std::uint64_t generation = 0;
    world::World world;
    std::vector<std::string> titles; // per exhibit, in the world's order
    std::filesystem::path assets_folder;
};

struct WorkReading {
    std::string title;
    double direct_lux = no_reading; // means over the work's 5 x 5 grid
    double total_lux = no_reading;
    std::string problem; // why there is no reading
};

struct LuxResult {
    std::uint64_t generation = 0;
    std::vector<WorkReading> works;
    std::vector<std::string> problems;
    double seconds = 0.0;
};

using AssetKey = std::pair<std::string, std::string>; // (sha256, extension)

// The photographs a world names, each once, in order.
std::vector<AssetKey> photographs(const world::World& world) {
    std::vector<AssetKey> keys;
    for (const world::Material& material : world.materials) {
        const auto* painting = std::get_if<world::PaintingSurface>(&material);
        if (painting != nullptr && painting->image) {
            keys.emplace_back(painting->image->sha256, painting->image->extension);
        }
    }
    std::ranges::sort(keys);
    const auto [first, last] = std::ranges::unique(keys);
    keys.erase(first, last);
    return keys;
}

// The photographs last loaded, which the next reading reuses while they are still the ones it
// needs. Only the worker thread touches it, one reading at a time.
class TextureCache {
  public:
    const engine::LoadedTextures& load(const world::World& world,
                                       const std::filesystem::path& folder) {
        std::vector<AssetKey> keys = photographs(world);
        if (!textures_ || folder != folder_ || keys != keys_) {
            textures_ =
                std::make_unique<engine::LoadedTextures>(engine::load_textures(world, folder));
            folder_ = folder;
            keys_ = std::move(keys);
        }
        return *textures_;
    }

  private:
    std::unique_ptr<engine::LoadedTextures> textures_;
    std::filesystem::path folder_;
    std::vector<AssetKey> keys_;
};

LuxResult read_lux(const LuxJob& job, TextureCache& cache, ThreadPool& pool) {
    QElapsedTimer clock;
    clock.start();
    LuxResult result;
    result.generation = job.generation;
    try {
        const engine::LoadedTextures& loaded = cache.load(job.world, job.assets_folder);
        for (const engine::TextureProblem& problem : loaded.problems) {
            result.problems.push_back("The photograph " + problem.asset.sha256 + "." +
                                      problem.asset.extension +
                                      " is read as plain grey: " + problem.reason);
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
    result.seconds = static_cast<double>(clock.nsecsElapsed()) * 1e-9;
    return result;
}

// Lux as a curator reads it: whole lux, with a tenth below 10 lx, where limits such as 50 lx are
// set and one lux is a large part.
QString lux_text(double lux) {
    return QLocale().toString(lux, 'f', lux < 10.0 ? 1 : 0);
}

class LuxPanel : public QWidget {
  public:
    explicit LuxPanel(MainWindow& window)
        : window_(window), pool_(std::max(1u, std::thread::hardware_concurrency())) {
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
        ++wanted_;
        if (!running_) {
            start();
        }
        if (running_) {
            status_->setText(MainWindow::tr("Calculating…"));
        }
    }

  private:
    void start() {
        auto job = std::make_shared<LuxJob>();
        job->generation = wanted_;
        try {
            const doc::Document& document = window_.document();
            job->world = world::compile(document);
            for (const world::Exhibit& exhibit : job->world.exhibits) {
                const doc::Work* work = document.find_work(doc::Uuid(exhibit.work.bytes()));
                job->titles.push_back(work != nullptr ? work->title : std::string());
            }
            if (!photographs(job->world).empty()) {
                job->assets_folder = window_.assets_folder();
            }
        } catch (const std::exception& error) {
            LuxResult failed;
            failed.generation = wanted_;
            failed.problems.push_back(std::string("The lux cannot be calculated: ") + error.what());
            display(failed);
            return;
        }
        if (job->world.exhibits.empty()) {
            display(LuxResult{wanted_, {}, {}, 0.0});
            return;
        }
        running_ = true;
        jobs_.start([this, job] {
            auto result = std::make_shared<LuxResult>(read_lux(*job, cache_, pool_));
            QMetaObject::invokeMethod(
                this, [this, result] { finished(std::move(*result)); }, Qt::QueuedConnection);
        });
    }

    void finished(LuxResult result) {
        running_ = false;
        if (result.generation != wanted_) {
            start(); // the document changed while it ran
            return;
        }
        display(result);
    }

    void display(const LuxResult& result) {
        table_->clear();
        for (const WorkReading& reading : result.works) {
            auto* item = new QTreeWidgetItem(table_);
            item->setText(0, reading.title.empty() ? MainWindow::tr("Untitled work")
                                                   : QString::fromStdString(reading.title));
            item->setToolTip(0, item->text(0));
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
            lines << MainWindow::tr("No works hung yet");
        } else if (!result.works.empty()) {
            lines << MainWindow::tr("Mean of 25 points on each work's face · %1 s")
                         .arg(QLocale().toString(result.seconds, 'f', 1));
        }
        for (const std::string& problem : result.problems) {
            lines << QString::fromStdString(problem);
        }
        status_->setText(lines.join(QLatin1Char('\n')));
    }

    MainWindow& window_;
    // Declared before jobs_, so that they outlive it.
    ThreadPool pool_;
    TextureCache cache_;
    QTreeWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
    std::uint64_t wanted_ = 0; // the generation of the document as it is
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
    QObject::connect(&window, &MainWindow::document_changed, panel, [panel] { panel->request(); });
    QObject::connect(&window, &MainWindow::project_changed, panel, [panel] { panel->request(); });
    panel->request();
}

} // namespace cs::ui
