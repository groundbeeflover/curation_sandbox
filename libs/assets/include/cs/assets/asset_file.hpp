// SPDX-License-Identifier: Apache-2.0

#pragma once

// An asset's file
// ---------------
// A project keeps each asset as assets/<sha256>.<extension>, named by its content (ADR 0007).
// Both names come from a project file, which is hostile (CLAUDE.md): a separator, a "..", a NUL or
// an absolute path in one would make a path outside the folder. asset_file is the one place that
// checks them before a path is made, for every tier: the engine (engine::load_textures) and the
// GPU (gpu::project_texture_loader) both call it, so a photograph loads in all of them or in none.
//
// Only the extensions of the images load_texture decodes are accepted, in lowercase, from one
// list in asset_file.cpp: png, jpg and jpeg, to which TIFF decoding (M2-32) adds tif and tiff.
// The document allows any of 1 to 8 characters from a-z and 0-9 (doc/components.hpp);
// normalising them on import (".JPG" to "jpg") is M2-11.

#include <filesystem>
#include <string_view>

namespace cs::assets {

// The file of an asset in a project's assets folder: <folder>/<sha256>.<extension>. Throws
// std::invalid_argument, with a message saying which name is wrong, unless sha256 is 64 lowercase
// hexadecimal digits and the extension is one load_texture decodes (above). The folder is the
// caller's, not the project file's, and is not checked.
std::filesystem::path asset_file(const std::filesystem::path& folder, std::string_view sha256,
                                 std::string_view extension);

} // namespace cs::assets
