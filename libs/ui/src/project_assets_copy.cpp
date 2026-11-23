// SPDX-License-Identifier: Apache-2.0
#include "project_assets_copy.hpp"

#include <cs/assets/asset_file.hpp>
#include <cs/assets/project_assets.hpp>
#include <cs/doc/document.hpp>

#include <iostream>
#include <stdexcept>
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

void copy_asset(const fs::path& source, const fs::path& target, const doc::AssetRef& name) {
    if (fs::is_regular_file(fs::symlink_status(target))) {
        return; // the same name holds the same content
    }
    if (!fs::is_regular_file(fs::symlink_status(source))) {
        std::cerr << "curation: warning: " << source.string()
                  << " is missing or not a regular file, so it is not copied\n";
        return;
    }
    // store_image checks the file as every tier will read it, and writes it whole or not at all.
    // Its name is the content's hash, so a file whose content is not its name's (in a project
    // someone sent) is stored under its true name, which the document does not use. That file is
    // left: it is a valid image, and may be another work's.
    try {
        const assets::AssetName stored = assets::store_image(source, target.parent_path());
        if (stored.sha256 != name.sha256 || stored.extension != name.extension) {
            std::cerr << "curation: warning: " << source.string()
                      << " does not hold the image its name says, so it is not copied\n";
        }
    } catch (const std::invalid_argument& error) {
        std::cerr << "curation: warning: " << source.string() << " is not copied: " << error.what()
                  << '\n';
    }
}

} // namespace

void copy_project_assets(const doc::Document& document, const fs::path& from, const fs::path& to) {
    if (from.empty()) {
        // An untitled project that has stored no image.
        for (const auto& [id, work] : document.works()) {
            if (work.image) {
                std::cerr << "curation: warning: image " << work.image->sha256 << '.'
                          << work.image->extension << " was never stored, so it is not saved\n";
            }
        }
        return;
    }
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
                   assets::asset_file(to, image.sha256, image.extension), image);
    }
}

} // namespace cs::ui
