// SPDX-License-Identifier: Apache-2.0

#pragma once

// What the commands share: their errors, and the step from the document's walls to geom's faces.

#include <cs/doc/components.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/geom/wall_face.hpp>

#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cs::editor::detail {

// Throws std::invalid_argument: "<command>: <problem>".
[[noreturn]] inline void refuse(std::string_view command, const std::string& problem) {
    throw std::invalid_argument(std::string(command) + ": " + problem);
}

// Refuses unless value_m is finite and greater than 0.
inline void check_length(std::string_view command, std::string_view what, double value_m) {
    if (!std::isfinite(value_m) || !(value_m > 0.0)) {
        refuse(command, std::string(what) + " must be finite and greater than 0, not " +
                            std::to_string(value_m));
    }
}

// Refuses text the project file could not hold: more than doc::max_string_bytes.
inline void check_text(std::string_view command, std::string_view what, std::string_view text) {
    if (text.size() > doc::max_string_bytes) {
        refuse(command, std::string(what) + " is longer than " +
                            std::to_string(doc::max_string_bytes) + " bytes");
    }
}

// Refuses an asset the project file could not name (components.hpp): a SHA-256 of 64 lowercase
// hexadecimal digits, an extension of 1 to 8 characters from a-z and 0-9.
inline void check_asset(std::string_view command, const doc::AssetRef& asset) {
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    const auto alphanumeric = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
    };
    if (asset.sha256.size() != 64 || !std::ranges::all_of(asset.sha256, hex) ||
        asset.extension.empty() || asset.extension.size() > 8 ||
        !std::ranges::all_of(asset.extension, alphanumeric)) {
        refuse(command, "the image is not an asset name: assets/<64 lowercase hexadecimal "
                        "digits>.<1 to 8 of a-z and 0-9>");
    }
}

inline glm::dvec2 to_glm(const doc::Vec2& v) {
    return {v.x, v.y};
}

// The face of the wall; throws std::invalid_argument for a wall geom::wall_face_frame refuses.
inline geom::FaceFrame face_frame(const doc::Wall& wall, doc::WallFace face) {
    return geom::wall_face_frame(to_glm(wall.start_m), to_glm(wall.end_m), wall.thickness_m,
                                 face == doc::WallFace::a ? geom::WallFace::a : geom::WallFace::b);
}

} // namespace cs::editor::detail
