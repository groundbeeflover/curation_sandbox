// SPDX-License-Identifier: Apache-2.0

// Tests of store_image (issue #172, M1-47): a photograph a curator picks is checked, named by its
// SHA-256 and copied into the project's assets folder, so that Add painting (M1-27) can hang it.
// Every refusal also checks that nothing was written: the assets folder of each test does not
// exist until a store succeeds.

#include <cs/assets/asset_file.hpp>
#include <cs/assets/project_assets.hpp>
#include <cs/assets/texture.hpp>

// The hash is private to libs/assets, so its header is included by its place in the tree; it has
// tests of its own in test_sha256.cpp.
#include "../src/sha256.hpp"

#include <catch2/catch_test_macros.hpp>

// The tests write their images in memory with stb_image_write. STATIC keeps its functions inside
// this file, apart from core's copy.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using cs::assets::asset_file;
using cs::assets::AssetName;
using cs::assets::store_image;

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<char>;

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<Bytes*>(context);
    const auto* begin = static_cast<const char*>(data);
    out->insert(out->end(), begin, begin + size);
}

// A 2 x 1 image, as a PNG and as a JPEG.
const std::vector<std::uint8_t> two_pixels = {200, 100, 50, 10, 20, 30};

Bytes png() {
    Bytes out;
    REQUIRE(stbi_write_png_to_func(append_bytes, &out, 2, 1, 3, two_pixels.data(), 2 * 3) != 0);
    return out;
}

Bytes jpeg() {
    Bytes out;
    REQUIRE(stbi_write_jpg_to_func(append_bytes, &out, 2, 1, 3, two_pixels.data(), 100) != 0);
    return out;
}

// A PNG one pixel wider than load_texture accepts: a few kilobytes, so the file is small and only
// its image is too large.
Bytes too_wide_png() {
    const int width = cs::assets::max_texture_side + 1;
    const std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3, 128);
    Bytes out;
    REQUIRE(stbi_write_png_to_func(append_bytes, &out, width, 1, 3, row.data(), width * 3) != 0);
    return out;
}

void write_file(const fs::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
}

Bytes read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string sha256_of(const Bytes& bytes) {
    return cs::assets::sha256(std::as_bytes(std::span(bytes)));
}

// The names in a folder, sorted.
std::vector<std::string> entries(const fs::path& folder) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

// A directory of its own under the working directory, which ctest sets to this build's directory,
// so that other presets and worktrees running at the same time do not share it. Emptied when made
// and removed when the test ends.
class TempDir {
  public:
    explicit TempDir(const std::string& name)
        : path_(fs::current_path() / ("cs_assets_test_project_assets_" + name)) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

  private:
    fs::path path_;
};

// store_image refuses the source with std::invalid_argument and a message, and writes nothing:
// the assets folder, which does not exist before, does not exist after.
void check_refused(const fs::path& source, const fs::path& assets_folder) {
    REQUIRE_FALSE(fs::exists(assets_folder));
    try {
        (void)store_image(source, assets_folder);
        FAIL_CHECK("store_image accepted " << source.filename().string());
    } catch (const std::invalid_argument& error) {
        CHECK_FALSE(std::string(error.what()).empty());
    }
    CHECK_FALSE(fs::exists(assets_folder));
}

} // namespace

// ---- Storing -----------------------------------------------------------------------------------

TEST_CASE("store_image: the file is copied to asset_file(folder, sha256, extension), named by "
          "the SHA-256 of its bytes",
          "[assets][store_image]") {
    const TempDir dir("copied");
    const fs::path source = dir.path() / "Photo.png";
    const Bytes image = png();
    write_file(source, image);
    const fs::path assets = dir.path() / "project" / "assets";

    const AssetName name = store_image(source, assets);
    CHECK(name.sha256 == sha256_of(image));
    CHECK(name.extension == "png");

    const fs::path stored = asset_file(assets, name.sha256, name.extension);
    CHECK(read_file(stored) == image);
    // Only the asset is left: no temporary file.
    CHECK(entries(assets) == std::vector<std::string>{name.sha256 + ".png"});
    // The source is untouched, and the asset loads as every tier will load it.
    CHECK(read_file(source) == image);
    const cs::assets::Texture texture = cs::assets::load_texture(stored);
    CHECK(texture.width() == 2);
    CHECK(texture.height() == 1);
}

TEST_CASE("store_image: the extension is the source's, in lower case", "[assets][store_image]") {
    struct Case {
        const char* file_name;
        const char* extension;
        bool is_png;
    };
    const Case cases[] = {
        {"a.png", "png", true},    {"b.PNG", "png", true},      {"c.Png", "png", true},
        {"d.jpg", "jpg", false},   {"e.JPG", "jpg", false},     {"f.jpeg", "jpeg", false},
        {"g.JPEG", "jpeg", false}, {"h.old.Jpg", "jpg", false},
    };
    const TempDir dir("lower_case");
    const fs::path assets = dir.path() / "assets";
    for (const Case& c : cases) {
        INFO(c.file_name);
        const fs::path source = dir.path() / c.file_name;
        const Bytes image = c.is_png ? png() : jpeg();
        write_file(source, image);
        const AssetName name = store_image(source, assets);
        CHECK(name == AssetName{sha256_of(image), c.extension});
        CHECK(fs::is_regular_file(asset_file(assets, name.sha256, name.extension)));
    }
}

TEST_CASE("store_image: the same image stored twice, under any file name, gives the same name "
          "and one file",
          "[assets][store_image]") {
    const TempDir dir("twice");
    const Bytes image = png();
    fs::create_directories(dir.path() / "elsewhere");
    const fs::path first = dir.path() / "portrait.png";
    const fs::path second = dir.path() / "elsewhere" / "Copy of the portrait.PNG";
    write_file(first, image);
    write_file(second, image);
    const fs::path assets = dir.path() / "assets";

    const AssetName name = store_image(first, assets);
    CHECK(store_image(second, assets) == name);
    CHECK(store_image(first, assets) == name);
    CHECK(entries(assets) == std::vector<std::string>{name.sha256 + ".png"});
    CHECK(read_file(asset_file(assets, name.sha256, name.extension)) == image);

    // A different image is a second file.
    const fs::path other = dir.path() / "other.jpg";
    write_file(other, jpeg());
    const AssetName other_name = store_image(other, assets);
    CHECK(other_name.sha256 != name.sha256);
    CHECK(entries(assets).size() == 2);
}

TEST_CASE("store_image: a folder that does not exist yet is made", "[assets][store_image]") {
    const TempDir dir("made");
    const fs::path source = dir.path() / "photo.jpg";
    write_file(source, jpeg());
    const fs::path assets = dir.path() / "a new project" / "assets";
    REQUIRE_FALSE(fs::exists(assets.parent_path()));

    const AssetName name = store_image(source, assets);
    CHECK(fs::is_directory(assets));
    CHECK(fs::is_regular_file(asset_file(assets, name.sha256, name.extension)));
}

// ---- Refused, writing nothing ------------------------------------------------------------------

TEST_CASE("store_image: a file larger than max_texture_file_bytes is refused and nothing is "
          "written",
          "[assets][store_image]") {
    const TempDir dir("too_large");
    // A real PNG, extended with zeros to one byte over the limit: the file system leaves the
    // zeros unwritten (a sparse file), so the test does not write 512 MiB.
    const fs::path source = dir.path() / "huge.png";
    write_file(source, png());
    fs::resize_file(source, cs::assets::max_texture_file_bytes + 1);
    check_refused(source, dir.path() / "assets");
}

TEST_CASE("store_image: an extension asset_file refuses is refused and nothing is written",
          "[assets][store_image]") {
    const TempDir dir("extension");
    const Bytes image = png();
    // Each is a real PNG: only its name is wrong.
    for (const char* file_name : {"photo.tif", "photo.TIFF", "photo.gif", "photo.webp", "photo",
                                  "photo.", ".png", "photo.png.txt", "photo.p ng"}) {
        INFO(file_name);
        const fs::path source = dir.path() / file_name;
        write_file(source, image);
        check_refused(source, dir.path() / "assets");
    }
}

TEST_CASE("store_image: a file load_texture cannot decode is refused and nothing is written",
          "[assets][store_image]") {
    const TempDir dir("undecodable");
    const Bytes image = png();
    struct Case {
        const char* file_name;
        Bytes bytes;
    };
    const std::string text = "not an image";
    const Case cases[] = {
        {"empty.png", {}},
        {"text.png", Bytes(text.begin(), text.end())},
        {"truncated.png", Bytes(image.begin(), image.begin() + 20)},
        {"wider than max_texture_side.png", too_wide_png()},
    };
    for (const Case& c : cases) {
        INFO(c.file_name);
        const fs::path source = dir.path() / c.file_name;
        write_file(source, c.bytes);
        // The case is fair: load_texture itself refuses the file.
        REQUIRE_THROWS_AS(cs::assets::load_texture(source), std::runtime_error);
        check_refused(source, dir.path() / "assets");
    }
}

TEST_CASE("store_image: a source that is missing or not a file is refused and nothing is "
          "written",
          "[assets][store_image]") {
    const TempDir dir("not_a_file");
    const fs::path assets = dir.path() / "assets";
    CHECK_THROWS_AS(store_image(dir.path() / "missing.png", assets), std::runtime_error);
    CHECK_FALSE(fs::exists(assets));

    const fs::path folder = dir.path() / "folder.png";
    fs::create_directory(folder);
    check_refused(folder, assets);
}

// ---- Failures and what is already there --------------------------------------------------------

TEST_CASE("store_image: a failed copy leaves no partial asset", "[assets][store_image]") {
    // A directory, not empty, where the asset would go: the rename cannot replace it, so the
    // store fails after the temporary file is written.
    const TempDir dir("failed_copy");
    const fs::path source = dir.path() / "photo.png";
    const Bytes image = png();
    write_file(source, image);
    const fs::path assets = dir.path() / "assets";
    const fs::path in_the_way = asset_file(assets, sha256_of(image), "png");
    fs::create_directories(in_the_way);
    write_file(in_the_way / "inside", Bytes{'x'});

    CHECK_THROWS_AS(store_image(source, assets), std::runtime_error);
    // Neither a temporary file nor a partial asset is left; what was there is as it was.
    CHECK(entries(assets) == std::vector<std::string>{in_the_way.filename().string()});
    CHECK(entries(in_the_way) == std::vector<std::string>{"inside"});
}

TEST_CASE("store_image: a file at the asset's name that is not its image is replaced",
          "[assets][store_image]") {
    // Left by a store that did not finish or planted in a project someone sent: the name is
    // content's, so whatever holds it is overwritten with the content.
    const TempDir dir("replaced");
    const fs::path source = dir.path() / "photo.png";
    const Bytes image = png();
    write_file(source, image);
    const fs::path assets = dir.path() / "assets";
    fs::create_directories(assets);
    const fs::path stored = asset_file(assets, sha256_of(image), "png");
    write_file(stored, Bytes{'n', 'o', 't'});

    CHECK(store_image(source, assets) == AssetName{sha256_of(image), "png"});
    CHECK(read_file(stored) == image);
    CHECK(entries(assets) == std::vector<std::string>{stored.filename().string()});
}

TEST_CASE("store_image: a symbolic link at the temporary or the asset's name is replaced, never "
          "written through",
          "[assets][store_image]") {
    const TempDir dir("links");
    const fs::path source = dir.path() / "photo.png";
    const Bytes image = png();
    write_file(source, image);
    const fs::path assets = dir.path() / "assets";
    fs::create_directories(assets);
    const fs::path stored = asset_file(assets, sha256_of(image), "png");
    fs::path temporary = stored;
    temporary += ".tmp";

    // Two files outside the folder, each with a link to it planted at one of the two names.
    const Bytes kept = {'k', 'e', 'e', 'p'};
    const fs::path outside_temporary = dir.path() / "outside_temporary.txt";
    const fs::path outside_stored = dir.path() / "outside_stored.txt";
    write_file(outside_temporary, kept);
    write_file(outside_stored, kept);
    std::error_code error;
    fs::create_symlink(outside_temporary, temporary, error);
    if (!error) {
        fs::create_symlink(outside_stored, stored, error);
    }
    if (error) {
        SKIP("symbolic links cannot be made here: " << error.message());
    }

    CHECK(store_image(source, assets) == AssetName{sha256_of(image), "png"});
    CHECK(read_file(outside_temporary) == kept);
    CHECK(read_file(outside_stored) == kept);
    CHECK_FALSE(fs::is_symlink(stored));
    CHECK(read_file(stored) == image);
    CHECK(entries(assets) == std::vector<std::string>{stored.filename().string()});
}
