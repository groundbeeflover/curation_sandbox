// SPDX-License-Identifier: Apache-2.0

// Tests of asset_file.hpp (issue #154, M1-43): the one check of an asset's name before a path is
// made of it, for every tier. The hostile names are those M1-17 (engine::asset_path) and M1-21
// (the GPU's project_texture_loader) tested on their own, now in one place.
//
// Every hostile name has a real image planted in the file it would name, made the way a loader
// that skipped the check would make it (folder / (sha256 + "." + extension)), and the test first
// shows that load_texture reads that file: so a check that let the name through would load an
// image rather than fail for want of a file. Two names cannot have one (see the last test).

#include <cs/assets/asset_file.hpp>
#include <cs/assets/texture.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

// The tests write their images in memory with stb_image_write. STATIC keeps its functions inside
// this file, apart from core's copy.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using cs::assets::asset_file;

namespace fs = std::filesystem;

namespace {

// 64 lowercase hexadecimal digits: a valid asset name.
const std::string valid_sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

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

void write_file(const fs::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
}

// A directory of its own under the working directory, which ctest sets to this build's directory,
// so that other presets and worktrees running at the same time do not share it. Emptied when made
// and removed when the test ends.
class TempDir {
  public:
    explicit TempDir(const std::string& name)
        : path_(fs::current_path() / ("cs_assets_test_asset_file_" + name)) {
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

// The path a loader without the check would make of a name.
fs::path unchecked_path(const fs::path& folder, const std::string& sha256,
                        const std::string& extension) {
    return folder / (sha256 + "." + extension);
}

// Writes `image` to the file `path` names, as the file system resolves it: every directory the
// path passes through is made first, so that ".." and separators inside a name lead to a real
// file. A NUL ends the name where the file system reads it, so the file is written there.
void plant(const fs::path& path, const Bytes& image) {
    fs::path prefix;
    for (auto it = path.begin(); it != path.end() && std::next(it) != path.end(); ++it) {
        prefix /= *it;
        if (*it != "." && *it != ".." && !fs::exists(prefix)) {
            fs::create_directory(prefix);
        }
    }
    write_file(path, image);
}

// asset_file refuses the name with std::invalid_argument and a message.
void check_refused(const fs::path& folder, const std::string& sha256,
                   const std::string& extension) {
    try {
        (void)asset_file(folder, sha256, extension);
        FAIL_CHECK("asset_file accepted the name");
    } catch (const std::invalid_argument& error) {
        CHECK_FALSE(std::string(error.what()).empty());
    }
}

struct HostileName {
    std::string why;
    std::string sha256;
    std::string extension;
};

// Each name is planted and checked in a project folder of its own, <root>/<n>/assets, so that no
// two plants collide (a name that needs <sha256>.png to be a directory, another that needs it to
// be a file) and a file a name points at next to the folder is inside the test's own directory.
void check_planted_and_refused(const std::string& test, const std::vector<HostileName>& names) {
    const TempDir root(test);
    for (std::size_t n = 0; n < names.size(); ++n) {
        const HostileName& name = names[n];
        INFO(name.why);
        const fs::path folder = root.path() / std::to_string(n) / "assets";
        fs::create_directories(folder);

        const fs::path unchecked = unchecked_path(folder, name.sha256, name.extension);
        plant(unchecked, png());
        // The file the name points at is a real image: a check that let the name through would
        // load it.
        REQUIRE_NOTHROW(cs::assets::load_texture(unchecked));

        check_refused(folder, name.sha256, name.extension);
    }
}

} // namespace

// ---- Valid names -------------------------------------------------------------------------------

TEST_CASE("asset_file: <folder>/<sha256>.<extension> for a valid name", "[assets][asset_file]") {
    const fs::path folder = "project/assets";
    for (const char* extension : {"png", "jpg", "jpeg"}) {
        INFO("extension " << extension);
        CHECK(asset_file(folder, valid_sha, extension) == folder / (valid_sha + "." + extension));
    }
    // Every hexadecimal digit is allowed.
    const std::string all_f(64, 'f');
    const std::string all_0(64, '0');
    CHECK(asset_file(folder, all_f, "png") == folder / (all_f + ".png"));
    CHECK(asset_file(folder, all_0, "jpg") == folder / (all_0 + ".jpg"));
}

TEST_CASE("asset_file: png, jpg and jpeg are accepted, in lowercase, and a real photograph under "
          "each loads",
          "[assets][asset_file]") {
    // load_texture decodes PNG and JPEG (texture.hpp). The JPEGs are JPEGs, so each extension is
    // shown with the kind of file it names.
    const TempDir dir("decoded");
    for (const char* extension : {"png", "jpg", "jpeg"}) {
        INFO("extension " << extension);
        fs::path path;
        REQUIRE_NOTHROW(path = asset_file(dir.path(), valid_sha, extension));
        write_file(path, std::string(extension) == "png" ? png() : jpeg());
        cs::assets::Texture texture;
        REQUIRE_NOTHROW(texture = cs::assets::load_texture(path));
        CHECK(texture.width() == 2);
        CHECK(texture.height() == 1);
    }
}

TEST_CASE("asset_file: tif and tiff are refused until load_texture decodes TIFF (M2-32)",
          "[assets][asset_file]") {
    // The engine used to take tif, only to fail to decode it. M2-32 adds both to the list in
    // asset_file.cpp and turns this test round.
    check_planted_and_refused("tiff", {
                                          {"tif", valid_sha, "tif"},
                                          {"tiff", valid_sha, "tiff"},
                                      });
}

// ---- Hostile names, each with a real image where it would point --------------------------------

TEST_CASE("asset_file: a sha256 that is not 64 lowercase hexadecimal digits is refused with "
          "std::invalid_argument, even when a real image is where it points",
          "[assets][asset_file]") {
    std::string one_upper = valid_sha;
    one_upper[10] = 'A'; // 'a' would be valid
    std::string nul_inside = valid_sha;
    nul_inside[20] = '\0';
    // An absolute path inside the test's own directory: folder / name is then the name alone, as
    // fs::path's operator/ makes it.
    const std::string absolute =
        (fs::current_path() / "cs_assets_test_asset_file_hash" / "absolute" / "image").string();

    check_planted_and_refused(
        "hash",
        {
            {"one uppercase digit", one_upper, "png"},
            {"all uppercase", "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF",
             "png"},
            {"63 digits", valid_sha.substr(1), "png"},
            {"65 digits", valid_sha + "0", "png"},
            {"empty", "", "png"},
            {"a letter past f", std::string(63, 'a') + "g", "png"},
            {"a space", " " + valid_sha.substr(1), "png"},
            // 64 characters, so only the hexadecimal check refuses it; it points next to the
            // folder.
            {"a parent directory, 64 characters long", "../" + valid_sha.substr(3), "png"},
            {"a parent directory, shorter", "../secret", "png"},
            {"a path separator", valid_sha.substr(0, 32) + "/" + valid_sha.substr(33), "png"},
            {"a backslash", valid_sha.substr(0, 32) + "\\" + valid_sha.substr(33), "png"},
            {"a NUL character inside", nul_inside, "png"},
            {"a NUL character at the end", std::string(63, 'a') + '\0', "png"},
            {"an absolute path", absolute, "png"},
        });
}

TEST_CASE("asset_file: an extension load_texture does not decode is refused with "
          "std::invalid_argument, even when a real image is where it points",
          "[assets][asset_file]") {
    check_planted_and_refused(
        "extension", {
                         {"empty", valid_sha, ""},
                         {"upper case", valid_sha, "PNG"},
                         {"mixed case", valid_sha, "Png"},
                         {"upper case JPEG", valid_sha, "JPEG"},
                         // An AssetRef's extension (world/ids.hpp) that load_texture does not
                         // decode: the engine used to take it, only to fail to decode it.
                         {"exr", valid_sha, "exr"},
                         {"gif", valid_sha, "gif"},
                         {"exe", valid_sha, "exe"},
                         {"a leading dot", valid_sha, ".png"},
                         {"a trailing space", valid_sha, "png "},
                         {"a parent directory before it", valid_sha, "../png"},
                         {"a parent directory after it", valid_sha, "png/../../x"},
                         {"a separator before it", valid_sha, "/png"},
                         {"a backslash and a parent directory", valid_sha, "exr\\..\\x"},
                         // The file system reads <sha256>.png, a valid name's own file.
                         {"a NUL character after it", valid_sha, std::string("png\0", 4)},
                     });
}

TEST_CASE("asset_file: names that cannot point at a file are refused all the same",
          "[assets][asset_file]") {
    // These name a directory, or a file at the root of the file system, where a test may not
    // write; nothing is planted.
    const fs::path folder = "project/assets";
    // 64 characters: only the hexadecimal check refuses it.
    check_refused(folder, "/" + std::string(63, 'a'), "png");
    // <sha256>.png/.. is the folder itself; exr\.. is the same on Windows.
    check_refused(folder, valid_sha, "png/..");
    check_refused(folder, valid_sha, "exr\\..");
}

TEST_CASE("asset_file: the message says which name is wrong", "[assets][asset_file]") {
    const fs::path folder = "project/assets";
    CHECK_THROWS_WITH(asset_file(folder, "../secret", "png"), ContainsSubstring("SHA-256"));
    // It names the extensions that are accepted, so a curator knows what to convert to.
    CHECK_THROWS_WITH(asset_file(folder, valid_sha, "exr"), ContainsSubstring("png") &&
                                                                ContainsSubstring("jpg") &&
                                                                ContainsSubstring("jpeg"));
}
