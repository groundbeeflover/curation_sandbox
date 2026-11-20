// SPDX-License-Identifier: Apache-2.0
#include "project_assets_copy.hpp"

#include <cs/assets/asset_file.hpp>
#include <cs/assets/texture.hpp>
#include <cs/doc/document.hpp>

#include <iostream>
#include <system_error>

namespace cs::ui {

namespace {

namespace fs = std::filesystem;

// Whether the two folders are one, existing or not.
bool same_folder(const fs::path& a, const fs::path& b) {
    std::error_code error;
    if (fs::exists(a, error) && fs::exists(b, error)) {
        return fs::equivalent(a, b);
    }
    return fs::weakly_canonical(a) == fs::weakly_canonical(b);
}

void copy_asset(const fs::path& source, const fs::path& target) {
    if (fs::is_regular_file(fs::symlink_status(target))) {
        return; // the same name holds the same content
    }
    const fs::file_status status = fs::symlink_status(source);
    if (!fs::is_regular_file(status)) {
        std::cerr << "curation: warning: " << source.string()
                  << " is missing or not a regular file, so it is not copied\n";
        return;
    }
    if (fs::file_size(source) > assets::max_texture_file_bytes) {
        std::cerr << "curation: warning: " << source.string()
                  << " is larger than an image may be, so it is not copied\n";
        return;
    }
    fs::path temporary = target;
    temporary += ".tmp";
    // remove() takes a link away rather than following it, so the copy never writes through one.
    fs::remove(temporary);
    fs::copy_file(source, temporary);
    try {
        fs::rename(temporary, target);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace

void copy_project_assets(const doc::Document& document, const fs::path& from, const fs::path& to) {
    if (document.works().empty() || same_folder(from, to)) {
        return;
    }
    fs::create_directories(to);
    for (const auto& [id, work] : document.works()) {
        if (!work.image) {
            continue;
        }
        const doc::AssetRef& image = *work.image;
        copy_asset(assets::asset_file(from, image.sha256, image.extension),
                   assets::asset_file(to, image.sha256, image.extension));
    }
}

} // namespace cs::ui
