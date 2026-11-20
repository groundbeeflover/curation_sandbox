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
// made if it does not exist. Nothing is copied when the two are the same folder; when `from` is
// empty (an untitled project that stored no image), nothing is copied and the log names each
// image.
// - Each name is checked by assets::asset_file, since the document may come from a project
//   someone sent: a name it refuses throws std::invalid_argument.
// - A file already at the destination as a regular file is kept: assets are named by their
//   content.
// - Each copy is made by assets::store_image, which checks it as every tier reads it and writes it
//   through a temporary file renamed once whole. A source that is missing, is not a regular file
//   (a symbolic link someone planted), is not an image store_image accepts, or whose content is
//   not its name's, is not copied, and the log says so; the project already shows that image as
//   a problem.
// Throws std::runtime_error (std::filesystem::filesystem_error among them) when a folder cannot
// be made or a copy cannot be written.
void copy_project_assets(const doc::Document& document, const std::filesystem::path& from,
                         const std::filesystem::path& to);

} // namespace cs::ui
