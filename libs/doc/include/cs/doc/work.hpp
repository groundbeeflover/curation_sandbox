// SPDX-License-Identifier: Apache-2.0

#pragma once

// A Work is what is exhibited, independent of where it hangs (ADR 0008): one record can be placed,
// removed and placed again without losing its data. Entities show a work through an Exhibit
// component. Conventions as in components.hpp.

#include <cs/doc/components.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cs::doc {

// The shape of a flat work: a painting, drawing or print. In project.json, an object whose "kind"
// is "flat" (sculpture's "object" form arrives after 1.0).
struct FlatForm {
    double width_m = 0.0;          // > 0: the unframed work
    double height_m = 0.0;         // > 0
    std::optional<double> depth_m; // > 0: a canvas's stretcher or a panel's thickness
    bool operator==(const FlatForm&) const = default;
};

// The optical material of a work (ADR 0003): a preset by medium and finish, with overrides of its
// parameters. Absent overrides take the preset's values.
struct WorkMaterial {
    LibraryId preset;                     // "oil-canvas-gloss", "watercolour-paper", ...
    std::optional<double> base_roughness; // 0 to 1: GGX perceptual roughness of the paint
    std::optional<double> coat_weight;    // 0 to 1: how much varnish
    std::optional<double> coat_roughness; // 0 to 1: GGX perceptual roughness of the varnish
    bool operator==(const WorkMaterial&) const = default;
};

// Conservation classes (ADR 0003): CIE 157's categories of responsivity to light. In
// project.json, "irresponsive", "low", "medium" or "high".
enum class Responsivity { irresponsive, low, medium, high };

// How much light a work may receive. Absent limits take the project policy's values for its
// responsivity.
struct Sensitivity {
    Responsivity responsivity = Responsivity::low;
    std::optional<double> max_lux;       // >= 0: illuminance
    std::optional<double> max_lux_hours; // >= 0: yearly exposure
    bool operator==(const Sensitivity&) const = default;
};

// Where a work's record came from. In project.json, an object whose "kind" comes first:
// "catalogue", "import" or "checklist".
struct FromCatalogue {
    std::string catalogue_id; // the work's id in the shipped catalogue
    std::string image_sha256; // 64 lowercase hexadecimal digits: the catalogue image's hash
    bool operator==(const FromCatalogue&) const = default;
};

struct FromImport {
    std::string original_name; // the imported image's file name, for display only
    bool operator==(const FromImport&) const = default;
};

struct FromChecklist {
    int row = 0; // >= 1: the row in the imported checklist
    bool operator==(const FromChecklist&) const = default;
};

using WorkSource = std::variant<FromCatalogue, FromImport, FromChecklist>;

// The work's id in another system, such as a collections database.
struct ExternalId {
    std::string system;
    std::string record_id;
    bool operator==(const ExternalId&) const = default;
};

// Text fields may be empty; every string is at most 4096 bytes of UTF-8.
struct Work {
    std::string number; // the checklist number, "12" or "12a"
    std::string title;
    std::string artist;
    std::string date;   // as catalogued: "1872", "c. 1650", "1890-1895"
    std::string medium; // as catalogued: "Oil on canvas"
    std::optional<std::string> accession_number;
    std::optional<std::string> lender;
    std::string credit_line;
    std::optional<std::string> licence; // of the image
    FlatForm form;
    std::optional<AssetRef> image; // absent: shown as a labelled placeholder
    WorkMaterial material;
    Sensitivity sensitivity;
    std::optional<WorkSource> source;     // absent: entered by hand
    std::vector<ExternalId> external_ids; // at most 64
    bool operator==(const Work&) const = default;
};

} // namespace cs::doc
