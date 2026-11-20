// SPDX-License-Identifier: Apache-2.0
//
// Copying a project's assets to the folder it is saved to (MainWindow::save_project): an untitled
// project keeps its images in a temporary folder, and a project saved to a new folder leaves its
// old one behind, so a save first copies every asset the document names into the new folder.
#pragma once

#include <filesystem>

namespace cs::doc {
class Document;
}

namespace cs::ui {

// Copies every asset `document` names (works' images) from the folder `from` into `to`, which is
// made if it does not exist. Nothing is copied when the two are the same folder.
// - Each name is checked by assets::asset_file, since the document may come from a project
//   someone sent: a name it refuses throws std::invalid_argument.
// - A file already at the destination as a regular file is kept: assets are named by their
//   content.
// - A source that is missing, is not a regular file (a symbolic link someone planted), or is
//   larger than assets::max_texture_file_bytes is not copied, and the log says so; the project
//   already shows that image as a problem.
// - Each copy goes to <name>.tmp beside the asset, replacing whatever is there without writing
//   through a link, and is renamed once whole, so a failed save leaves no partial asset.
// Throws std::filesystem::filesystem_error when a folder cannot be made or a copy fails.
void copy_project_assets(const doc::Document& document, const std::filesystem::path& from,
                         const std::filesystem::path& to);

} // namespace cs::ui
