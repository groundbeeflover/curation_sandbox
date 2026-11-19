// SPDX-License-Identifier: Apache-2.0

#pragma once

// A project's assets
// ------------------
// A photograph a curator picks becomes part of the project when it is stored in the project's
// assets folder, named by its content: <sha256>.<extension> (ADR 0007). The same photograph
// stored twice, under any file name, is one file. libs/assets does not know the document, so
// store_image returns the two parts of the name and the caller makes the doc::AssetRef.
//
// The picked file comes from outside and is hostile (CLAUDE.md). It is read once, in chunks of
// 1 MiB, and never more than max_texture_file_bytes of it; the bytes that are hashed are the
// bytes that are checked and copied, so a file that changes while it is stored cannot give an
// asset whose name is not its content's. It is checked as every tier will read it: its name by
// asset_file, its content by load_texture. Nothing more is decoded (colour profiles are M2-09).
//
// The copy is written to a temporary file next to the asset, <sha256>.<extension>.tmp, and
// renamed to the asset's name once it is whole: a failed copy leaves no partial asset. Whatever
// is already at either name, a file left by a failed store, a symbolic link or a file planted in
// a project someone sent, is replaced, never written through.

#include <filesystem>
#include <string>

namespace cs::assets {

// The name of an asset in a project's assets folder, the file asset_file(folder, sha256,
// extension): the two fields of doc::AssetRef.
struct AssetName {
    std::string sha256;    // 64 lowercase hexadecimal digits
    std::string extension; // in lower case: "png", "jpg" or "jpeg"
    bool operator==(const AssetName&) const = default;
};

// Stores the image file `source` in `assets_folder`, which is made if it does not exist, and
// returns its name. The name is the SHA-256 of the file's bytes and the source's extension in
// lower case: "Photo.JPG" gives "jpg".
//
// Throws std::invalid_argument, with a message saying why, and writes nothing, if the source is
// not a regular file, is larger than max_texture_file_bytes, has an extension asset_file refuses
// or is not an image load_texture decodes. Throws std::runtime_error (std::filesystem's
// filesystem_error is one) if the source cannot be read, and then writes nothing, or if the asset
// cannot be written, and then leaves no partial asset.
AssetName store_image(const std::filesystem::path& source,
                      const std::filesystem::path& assets_folder);

} // namespace cs::assets
