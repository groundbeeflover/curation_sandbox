// SPDX-License-Identifier: Apache-2.0

#pragma once

// Library definitions (ADR 0008): reusable definitions that entities refer to by LibraryId. A
// project stores copies of the definitions it uses, so that it opens identically on another
// machine; the built-in library that ships with the app has the same shape. Fixture types are
// here from milestone 1; frame styles and finishes join them with milestone 2. Conventions as in
// components.hpp.

#include <cs/doc/components.hpp>

#include <map>
#include <string>

namespace cs::doc {

// A fixture type: the light a kind of fitting emits. A generic spot is a cone given by lumens and
// two full angles, as in IES files; IES distributions arrive with milestone 3.
struct FixtureType {
    std::string name;
    double flux_lm = 0.0;                 // > 0: at dimming 1
    double beam_angle_rad = 0.0;          // > 0: full angle at 50% of the peak intensity
    double field_angle_rad = 0.0;         // >= beam_angle_rad: full angle at 10% of the peak
    double colour_temperature_k = 3000.0; // > 0
    bool operator==(const FixtureType&) const = default;
};

// In project.json, an object with one array per kind of definition, each element an object with
// "id" first: { "fixtureTypes": [ { "id": "generic-spot-800", "name": ..., ... } ] }.
struct Library {
    std::map<LibraryId, FixtureType> fixture_types;

    // nullptr when there is none with that id.
    const FixtureType* find_fixture_type(const LibraryId& id) const {
        const auto it = fixture_types.find(id);
        return it == fixture_types.end() ? nullptr : &it->second;
    }

    bool operator==(const Library&) const = default;
};

} // namespace cs::doc
