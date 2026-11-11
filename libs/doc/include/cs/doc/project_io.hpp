// SPDX-License-Identifier: Apache-2.0

#pragma once

// Reading and writing a project as a folder (ADR 0007): <folder>/project.json holds the document;
// assets/ beside it holds the files the document names by hash. The .curation zip (milestone 2)
// holds the same files.
//
// project.json
// ------------
// UTF-8 JSON, written in one canonical form so that a project saved twice is byte-identical and
// diffs well:
// - 2-space indentation, one member or array element per line, a newline at the end;
// - object keys in a fixed order: the order members are declared in the C++ types, with "id" and
//   "name" first in works, entities and fixture types, and "kind" first in a variant's object;
// - optional members are left out when absent; arrays are always written, even when empty;
// - works, entities and fixture types sorted by id (for UUIDv7 ids, the order of creation);
// - numbers in the shortest form that reads back to the same double, with a fractional part for
//   whole numbers ("3.0"); the integers schemaVersion and a checklist row without one.
//
// The top level, in this order:
//
//   {
//     "schemaVersion": 1,
//     "settings": { "centreLine_m": 1.5, "eyeHeight_m": 1.6, "openingHoursPerYear": 2184.0,
//                   "displayUnits": "centimetres" },
//     "library": { "fixtureTypes": [ { "id": "generic-spot-800", "name": ..., "flux_lm": ...,
//                                      "beamAngle_rad": ..., "fieldAngle_rad": ...,
//                                      "colourTemperature_K": ... } ] },
//     "works": [ { "id": "<uuid>", "number": ..., "title": ..., ... } ],
//     "entities": [ { "id": "<uuid>", "name": ..., "wall": { ... }, "placement": { ... } } ]
//   }
//
// Members of each type are named as components.hpp describes; work.hpp and components.hpp give
// the spelling of every enumeration and variant kind.
//
// Reading is strict, because a project file is untrusted input: an unknown key, a missing
// member, a value of the wrong type or out of its range, a reference to a work, entity or
// fixture type the document does not hold, an entity referenced as a wall (or a room, or a track)
// that has no such component, a cycle of OnHost placements, a duplicated id, or a schemaVersion
// other than 1 is an error. So is anything over the limits below.

#include <cs/doc/document.hpp>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cs::doc {

// The only schema version this build reads and writes.
inline constexpr int schema_version = 1;

// Limits on what a project file may hold.
inline constexpr std::size_t max_project_json_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t max_works = 100'000;
inline constexpr std::size_t max_entities = 100'000;
inline constexpr std::size_t max_fixture_types = 10'000;
inline constexpr std::size_t max_string_bytes = 4096;
inline constexpr std::size_t max_json_depth = 32;

// A project file that cannot be read. what() is "<where>: <json path>: <problem>", where <where>
// is the file (or "project.json" for text not read from a file) and the JSON path is written as
// in "entities[3].placement.wall": keys after dots, array indices in brackets, counting from 0 in
// the order of the file. An error in the file's syntax gives the line and column instead of a
// path.
class ProjectError : public std::runtime_error {
  public:
    ProjectError(const std::string& where, const std::string& json_path,
                 const std::string& problem);

    // The JSON path of the value at fault; empty for the top level and for syntax errors.
    const std::string& json_path() const { return json_path_; }

  private:
    std::string json_path_;
};

// Reads <folder>/project.json. Throws ProjectError when it is invalid or larger than
// max_project_json_bytes, and std::runtime_error when it cannot be read.
Document load_project_folder(const std::filesystem::path& folder);

// Writes <folder>/project.json in the canonical form, creating the folder if needed. The text is
// written to project.json.tmp first and then renamed over project.json, so a failed save leaves
// the previous file whole. Other files in the folder are left alone. Throws ProjectError, before
// anything is written, if the document would not read back (a dangling reference, a value out of
// range, a number that is not finite); std::runtime_error when the folder cannot be written.
void save_project_folder(const Document& document, const std::filesystem::path& folder);

// The same, without files: parse_project_json reads the text of a project.json (throwing
// ProjectError as load_project_folder does), and to_project_json writes the canonical text,
// including the final newline, without checking the document (it throws ProjectError only for a
// string that is not valid UTF-8).
Document parse_project_json(std::string_view text);
std::string to_project_json(const Document& document);

} // namespace cs::doc
