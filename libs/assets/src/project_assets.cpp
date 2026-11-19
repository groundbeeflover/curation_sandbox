// SPDX-License-Identifier: Apache-2.0

#include <cs/assets/project_assets.hpp>

#include <cs/assets/asset_file.hpp>
#include <cs/assets/texture.hpp>

#include "sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace cs::assets {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t read_chunk_bytes = std::size_t{1} << 20;

// Held while an asset is written, so that two stores of one image at once, from two threads,
// never share the temporary file: the second finds the asset whole and writes nothing.
std::mutex writing;

// A path for a message, in UTF-8: path::string() can throw on Windows for a name outside the
// code page, which would replace the error being reported.
std::string shown(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::invalid_argument too_large(const fs::path& source) {
    return std::invalid_argument(shown(source) + " is larger than the " +
                                 std::to_string(max_texture_file_bytes >> 20) +
                                 " MiB an image file may be");
}

// The source's extension without its dot, in lower case: "Photo.JPG" gives "jpg". Only ASCII
// letters change; anything else is left for asset_file to refuse.
std::string lower_case_extension(const fs::path& source) {
    const std::u8string dotted = source.extension().u8string(); // ".JPG", or empty
    std::string extension;
    for (std::size_t i = 1; i < dotted.size(); ++i) {
        const auto c = static_cast<char>(dotted[i]);
        extension.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return extension;
}

// Reads the whole file in chunks, hashing each, and stops with std::invalid_argument as soon as
// it holds more than max_texture_file_bytes: the size checked before may have changed since.
std::vector<std::byte> read_and_hash(const fs::path& source, std::uintmax_t size, Sha256& hash) {
    std::ifstream file(source, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read " + shown(source));
    }
    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(size));
    std::vector<std::byte> chunk(read_chunk_bytes);
    while (file) {
        file.read(reinterpret_cast<char*>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        const auto got = std::span(chunk).first(static_cast<std::size_t>(file.gcount()));
        if (bytes.size() + got.size() > max_texture_file_bytes) {
            throw too_large(source);
        }
        hash.update(got);
        bytes.insert(bytes.end(), got.begin(), got.end());
    }
    if (file.bad()) {
        throw std::runtime_error("cannot read " + shown(source));
    }
    return bytes;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + shown(path));
    }
}

// Whether the asset is already stored: a regular file, not a link, holding exactly these bytes.
bool already_stored(const fs::path& file, std::span<const std::byte> bytes) {
    std::error_code error;
    if (fs::symlink_status(file, error).type() != fs::file_type::regular ||
        fs::file_size(file, error) != bytes.size()) {
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    std::vector<std::byte> chunk(read_chunk_bytes);
    while (!bytes.empty()) {
        const auto part = std::span(chunk).first(std::min(bytes.size(), chunk.size()));
        if (!in.read(reinterpret_cast<char*>(part.data()),
                     static_cast<std::streamsize>(part.size())) ||
            !std::ranges::equal(part, bytes.first(part.size()))) {
            return false;
        }
        bytes = bytes.subspan(part.size());
    }
    return true;
}

} // namespace

AssetName store_image(const fs::path& source, const fs::path& assets_folder) {
    // Every check comes before anything is written.
    std::error_code error;
    const bool regular = fs::is_regular_file(source, error);
    if (error) {
        throw std::runtime_error("cannot read " + shown(source) + ": " + error.message());
    }
    if (!regular) {
        throw std::invalid_argument(shown(source) + " is not a file");
    }
    const std::uintmax_t size = fs::file_size(source);
    if (size > max_texture_file_bytes) {
        throw too_large(source);
    }
    Sha256 hash;
    const std::vector<std::byte> bytes = read_and_hash(source, size, hash);
    AssetName name{hash.digest(), lower_case_extension(source)};

    fs::path file;
    try {
        file = asset_file(assets_folder, name.sha256, name.extension);
        (void)load_texture_from_memory(bytes);
    } catch (const std::invalid_argument& e) { // asset_file: the extension
        throw std::invalid_argument(shown(source) + ": " + e.what());
    } catch (const std::runtime_error& e) { // load_texture: not an image it decodes
        throw std::invalid_argument(shown(source) + ": " + e.what());
    }

    const std::scoped_lock lock(writing);
    if (already_stored(file, bytes)) {
        return name;
    }
    // A file at the temporary name, left by a failed store or planted in a project someone sent,
    // is removed first: remove() deletes a symbolic link itself, so the write never goes through
    // one. The rename replaces whatever is at the asset's name, a link too, never its target.
    fs::create_directories(assets_folder);
    fs::path temporary = file;
    temporary += ".tmp";
    fs::remove(temporary);
    try {
        write_file(temporary, bytes);
        fs::rename(temporary, file);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
    return name;
}

} // namespace cs::assets
