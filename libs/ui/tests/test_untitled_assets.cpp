// SPDX-License-Identifier: Apache-2.0
//
// Images for an untitled project (issue #82, owner's addition of 2026-11-20): assets_folder() is
// <project folder>/assets, or for an untitled project a temporary folder the window owns; New and
// Open drop it; save_project(folder) first copies every asset the document names into
// <folder>/assets, each through a temporary file and a rename, skipping files already there; a
// project saved so reopens with its images and no texture problem.
//
// Most need no GPU (a bare MainWindow). Those that read the viewport's problems use the
// application's window and are tagged [gpu].

#include "ui_test.hpp"

#include <cs/assets/project_assets.hpp>
#include <cs/core/random.hpp>
#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/transaction.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/editor/commands/hanging.hpp>
#include <cs/editor/commands/room.hpp>
#include <cs/ui/main_window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cs::ui::MainWindow;
namespace fs = std::filesystem;

// The skeleton's test chart: its file name is its SHA-256 (ADR 0007, content addressing), which
// is therefore what store_image must name it.
constexpr const char* chart_sha256 =
    "26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a";

fs::path skeleton_chart() {
    return cs::ui::test::skeleton_folder() / "assets" / (std::string(chart_sha256) + ".png");
}

// The chart copied to a file of another name, "chart.png", in a fresh folder of ui-out/: the image
// a curator picks from anywhere.
fs::path picked_chart(const std::string& folder_name) {
    const fs::path folder = cs::ui::test::fresh_out_folder(folder_name);
    const fs::path picked = folder / "chart.png";
    fs::copy_file(skeleton_chart(), picked);
    return picked;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool same_path(const fs::path& a, const fs::path& b) {
    return fs::weakly_canonical(a) == fs::weakly_canonical(b);
}

// Ids from a fixed seed and a fixed clock, so that every run builds the same documents.
cs::doc::IdGenerator fixed_ids(std::uint32_t stream) {
    return cs::doc::IdGenerator(cs::hash_seed(82, 100 + stream, 0, 0, 0),
                                [] { return std::int64_t{1'800'000'000'000}; });
}

// A room, then a work showing `image` on its wall A, committed through the window. Returns the
// work's id.
cs::doc::Uuid hang(MainWindow& window, cs::doc::IdGenerator& ids, const cs::doc::AssetRef& image) {
    cs::editor::NewRoom room = cs::editor::new_room(window.document(), ids, 6.0, 4.0, 3.0);
    window.commit(std::move(room.transaction));
    cs::editor::AddedWork work =
        cs::editor::add_work(window.document(), ids, image, 0.5, 0.4, "Chart");
    window.commit(std::move(work.transaction));
    return work.work;
}

// Every file in a folder, by name, not recursing.
std::vector<std::string> file_names(const fs::path& folder) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

std::string lines(const std::vector<std::string>& problems) {
    std::string text;
    for (const std::string& problem : problems) {
        text += problem + "\n";
    }
    return text;
}

// make_main_window(project) with a viewport, as ui_test.hpp's make_shell() does for an empty
// project: skips without a Vulkan device, fails without one under CS_REQUIRE_GPU.
std::unique_ptr<MainWindow> shell_on(const std::optional<fs::path>& project) {
    std::unique_ptr<MainWindow> window;
    try {
        window = cs::ui::make_main_window(project);
    } catch (const std::exception& e) {
        FAIL("make_main_window() threw: " << e.what());
    }
    REQUIRE(window != nullptr);
    if (window->viewport() == nullptr) {
        if (cs::ui::test::require_gpu_env()) {
            FAIL("the window has no viewport, and CS_REQUIRE_GPU requires one: "
                 << window->viewport_problem().toStdString());
        } else {
            SKIP("no viewport (no Vulkan device): " << window->viewport_problem().toStdString());
        }
    }
    return window;
}

} // namespace

// Acceptance criterion: for an untitled project, assets_folder() is a temporary folder the window
// owns: it exists, and a second call gives the same one. Owned: it goes with the window.
TEST_CASE("MainWindow: an untitled project's assets_folder() is an existing temporary folder, the "
          "same on every call",
          "[ui]") {
    fs::path folder;
    {
        MainWindow window;
        REQUIRE_FALSE(window.project_folder().has_value());
        folder = window.assets_folder();
        CHECK_FALSE(folder.empty());
        CHECK(fs::is_directory(folder));
        CHECK(window.assets_folder() == folder);
        CHECK(fs::is_directory(folder));
        // Not inside the repository's test scenes or the folders tests save to.
        CHECK_FALSE(same_path(folder, cs::ui::test::skeleton_folder() / "assets"));
    }
    CHECK_FALSE(fs::exists(folder));
}

// Acceptance criteria: assets_folder() is <project folder>/assets for an opened or saved project;
// New and Open drop the untitled project's temporary folder, and so does saving it.
TEST_CASE("MainWindow: assets_folder() is <folder>/assets once opened or saved; New, Open and Save "
          "delete the temporary one",
          "[ui]") {
    MainWindow window;
    const fs::path temporary = window.assets_folder();
    REQUIRE(fs::is_directory(temporary));

    SECTION("open_project") {
        window.open_project(cs::ui::test::skeleton_folder());
        CHECK_FALSE(fs::exists(temporary));
        CHECK(same_path(window.assets_folder(), cs::ui::test::skeleton_folder() / "assets"));

        // New after Open: a new untitled project, with a temporary folder of its own.
        window.new_project();
        const fs::path again = window.assets_folder();
        CHECK(fs::is_directory(again));
        CHECK_FALSE(same_path(again, cs::ui::test::skeleton_folder() / "assets"));
    }
    SECTION("new_project") {
        window.new_project();
        CHECK_FALSE(fs::exists(temporary));
        CHECK_FALSE(window.project_folder().has_value());
        CHECK(fs::is_directory(window.assets_folder()));
    }
    SECTION("save_project") {
        const fs::path saved = cs::ui::test::fresh_out_folder("untitled-assets-save-folder");
        window.save_project(saved);
        CHECK_FALSE(fs::exists(temporary));
        CHECK(same_path(window.assets_folder(), saved / "assets"));
    }
}

// Acceptance criterion, the owner's test: store an image in an untitled project, add a work with
// it, save, reopen, and the image loads with no texture problem. The saved folder holds the image
// as assets/<sha256>.<extension>.
TEST_CASE("MainWindow: an image stored in an untitled project is saved with it and reopens "
          "without problems",
          "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    const fs::path picked = picked_chart("untitled-assets-picked");
    const fs::path saved = cs::ui::test::fresh_out_folder("untitled-assets-saved");
    cs::doc::Uuid work_id;
    {
        std::unique_ptr<MainWindow> window = shell_on(std::nullopt);
        const fs::path temporary = window->assets_folder();

        const cs::assets::AssetName name = cs::assets::store_image(picked, temporary);
        CHECK(name.sha256 == chart_sha256);
        CHECK(name.extension == "png");
        REQUIRE(fs::is_regular_file(temporary / (name.sha256 + ".png")));

        cs::doc::IdGenerator ids = fixed_ids(0);
        work_id = hang(*window, ids, cs::doc::AssetRef{name.sha256, name.extension});
        // The untitled project shows it already, from the temporary folder.
        INFO("problems before saving:\n" << lines(window->project_problems()));
        CHECK(window->project_problems().empty());

        window->save_project(saved);
        CHECK_FALSE(fs::exists(temporary));
    }

    // The saved folder holds the image, whole, under its content's name, and nothing half-copied.
    const fs::path asset = saved / "assets" / (std::string(chart_sha256) + ".png");
    REQUIRE(fs::is_regular_file(asset));
    CHECK(read_bytes(asset) == read_bytes(picked));
    CHECK(file_names(saved / "assets") == std::vector<std::string>{asset.filename().string()});

    std::unique_ptr<MainWindow> reopened;
    SECTION("reopened with make_main_window(folder)") {
        reopened = shell_on(saved);
    }
    SECTION("reopened with open_project") {
        reopened = shell_on(std::nullopt);
        reopened->open_project(saved);
    }
    REQUIRE(reopened->document().find_work(work_id) != nullptr);
    CHECK(reopened->document().find_work(work_id)->image == cs::doc::AssetRef{chart_sha256, "png"});
    INFO("problems after reopening:\n" << lines(reopened->project_problems()));
    CHECK(reopened->project_problems().empty());
    cs::ui::test::require_no_validation_messages(*reopened);
}

// The control of the test above: without the image in the saved folder, reopening shows a
// problem, so that an empty list there means the image loaded. An image the untitled project
// never stored is not copied, and the save still succeeds (main_window.hpp, save_project).
TEST_CASE("MainWindow: a saved project whose image was never stored reopens with a problem",
          "[ui][gpu]") {
    cs::ui::test::require_offscreen();
    const fs::path saved = cs::ui::test::fresh_out_folder("untitled-assets-missing");
    // A valid name of an image nobody stored.
    const cs::doc::AssetRef missing{std::string(64, 'a'), "png"};
    {
        std::unique_ptr<MainWindow> window = shell_on(std::nullopt);
        cs::doc::IdGenerator ids = fixed_ids(1);
        hang(*window, ids, missing);
        CHECK_FALSE(window->project_problems().empty());
        CHECK_NOTHROW(window->save_project(saved));
    }
    REQUIRE(fs::exists(saved / "project.json"));
    CHECK_FALSE(fs::exists(saved / "assets" / (missing.sha256 + ".png")));

    std::unique_ptr<MainWindow> reopened = shell_on(saved);
    INFO("problems after reopening:\n" << lines(reopened->project_problems()));
    CHECK_FALSE(reopened->project_problems().empty());
}

// Acceptance criterion: save_project skips a file already in <folder>/assets (it is not written
// again: same bytes, same modification time), and the temporary folder is gone after the save.
// Saving again to the same folder copies nothing (assets_folder() is that folder's now).
TEST_CASE("MainWindow: save_project leaves an image already in <folder>/assets and deletes the "
          "temporary folder",
          "[ui]") {
    const fs::path picked = picked_chart("untitled-assets-skip-picked");
    const fs::path saved = cs::ui::test::fresh_out_folder("untitled-assets-skip");
    MainWindow window;
    const fs::path temporary = window.assets_folder();
    const cs::assets::AssetName name = cs::assets::store_image(picked, temporary);
    cs::doc::IdGenerator ids = fixed_ids(2);
    hang(window, ids, cs::doc::AssetRef{name.sha256, name.extension});

    // The image is there already, a year old: a copy (temporary file and rename) would make it new.
    const fs::path asset = saved / "assets" / (name.sha256 + "." + name.extension);
    fs::create_directories(asset.parent_path());
    fs::copy_file(picked, asset);
    const fs::file_time_type old_time = fs::last_write_time(asset) - std::chrono::hours(24 * 365);
    fs::last_write_time(asset, old_time);

    window.save_project(saved);
    CHECK_FALSE(fs::exists(temporary));
    CHECK(fs::last_write_time(asset) == old_time);
    CHECK(read_bytes(asset) == read_bytes(picked));
    CHECK(file_names(asset.parent_path()) == std::vector<std::string>{asset.filename().string()});
    CHECK(cs::doc::load_project_folder(saved) == window.document());

    // Saved again to its own folder: nothing to copy, nothing rewritten.
    window.save_project(saved);
    CHECK(fs::last_write_time(asset) == old_time);
    CHECK(file_names(asset.parent_path()) == std::vector<std::string>{asset.filename().string()});
}

// Acceptance criterion: save_project copies every asset the document names from assets_folder()
// into the new folder's assets, also for an opened project saved elsewhere ("save as").
TEST_CASE("MainWindow: save_project of an opened project to another folder copies its images",
          "[ui]") {
    MainWindow window;
    window.open_project(cs::ui::test::skeleton_folder());
    // The skeleton's work shows the chart.
    bool names_chart = false;
    for (const auto& [id, work] : window.document().works()) {
        names_chart = names_chart || (work.image && work.image->sha256 == chart_sha256);
    }
    REQUIRE(names_chart);

    const fs::path saved = cs::ui::test::fresh_out_folder("untitled-assets-save-as");
    window.save_project(saved);
    const fs::path asset = saved / "assets" / (std::string(chart_sha256) + ".png");
    REQUIRE(fs::is_regular_file(asset));
    CHECK(read_bytes(asset) == read_bytes(skeleton_chart()));
    CHECK(file_names(saved / "assets") == std::vector<std::string>{asset.filename().string()});
    CHECK(same_path(window.assets_folder(), saved / "assets"));
    // The folder it came from is left as it was.
    CHECK(fs::is_regular_file(skeleton_chart()));
}

// main_window.hpp, save_project: an asset the document names but the folder lacks, or holds as
// anything but a regular file, is not copied; the document is still written.
TEST_CASE("MainWindow: save_project skips an image the assets folder lacks or holds as a "
          "directory, and writes the project",
          "[ui]") {
    MainWindow window;
    const fs::path temporary = window.assets_folder();
    cs::doc::AssetRef image{std::string(64, 'b'), "png"};

    SECTION("lacks") {}
    SECTION("holds as a directory") {
        fs::create_directories(temporary / (image.sha256 + ".png"));
    }
    cs::doc::IdGenerator ids = fixed_ids(3);
    hang(window, ids, image);

    const fs::path saved = cs::ui::test::fresh_out_folder("untitled-assets-not-a-file");
    CHECK_NOTHROW(window.save_project(saved));
    CHECK_FALSE(fs::exists(saved / "assets" / (image.sha256 + ".png")));
    REQUIRE(fs::exists(saved / "project.json"));
    CHECK(cs::doc::load_project_folder(saved) == window.document());
}

// main_window.hpp, save_project: when an asset cannot be copied it throws std::runtime_error
// before writing the document, and the project's folder stays as it was, so the untitled
// project's images stay where they were too. Here <folder>/assets is a file, not a folder.
TEST_CASE("MainWindow: save_project throws std::runtime_error when an image cannot be copied, "
          "and writes nothing",
          "[ui]") {
    const fs::path picked = picked_chart("untitled-assets-fail-picked");
    const fs::path saved = cs::ui::test::fresh_out_folder("untitled-assets-fail");
    {
        std::ofstream blocker(saved / "assets");
        blocker << "not a folder\n";
    }
    MainWindow window;
    const fs::path temporary = window.assets_folder();
    const cs::assets::AssetName name = cs::assets::store_image(picked, temporary);
    cs::doc::IdGenerator ids = fixed_ids(4);
    hang(window, ids, cs::doc::AssetRef{name.sha256, name.extension});
    const cs::doc::Document before = window.document();

    CHECK_THROWS_AS(window.save_project(saved), std::runtime_error);
    CHECK_FALSE(fs::exists(saved / "project.json"));
    CHECK(fs::is_regular_file(saved / "assets"));
    CHECK_FALSE(window.project_folder().has_value());
    CHECK(window.document() == before);
    CHECK(window.assets_folder() == temporary);
    CHECK(fs::is_regular_file(temporary / (name.sha256 + "." + name.extension)));
}
