// SPDX-License-Identifier: Apache-2.0

#include <cs/assets/project_assets.hpp>

#include <cs/assets/asset_file.hpp>
#include <cs/assets/texture.hpp>

#include "sha256.hpp"

#include <cstddef>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace cs::assets {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t read_chunk_bytes = std::size_t{1} << 20;

std::invalid_argument too_large(const fs::path& source) {
    return std::invalid_argument(source.string() + " is larger than the " +
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
        throw std::runtime_error("cannot read " + source.string());
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
        throw std::runtime_error("cannot read " + source.string());
    }
    return bytes;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

} // namespace

AssetName store_image(const fs::path& source, const fs::path& assets_folder) {
    // Every check comes before anything is written.
    std::error_code error;
    const bool regular = fs::is_regular_file(source, error);
    if (error) {
        throw std::runtime_error("cannot read " + source.string() + ": " + error.message());
    }
    if (!regular) {
        throw std::invalid_argument(source.string() + " is not a file");
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
        throw std::invalid_argument(source.string() + ": " + e.what());
    } catch (const std::runtime_error& e) { // load_texture: not an image it decodes
        throw std::invalid_argument(source.string() + ": " + e.what());
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
