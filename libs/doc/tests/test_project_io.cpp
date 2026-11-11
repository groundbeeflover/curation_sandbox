// SPDX-License-Identifier: Apache-2.0

// Reading and writing project folders (project_io.hpp): the canonical project.json, the
// every-component fixture in data/every_component/, and the strict reader's errors, each naming
// the JSON path at fault.

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

using namespace cs::doc;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

// Tests edit the fixture as JSON, keeping its key order, to make invalid documents.
using Json = nlohmann::ordered_json;

namespace {

// ---- The fixture -----------------------------------------------------------------------------

std::filesystem::path fixture_folder() {
    return std::filesystem::path(CS_SOURCE_DIR "/libs/doc/tests/data/every_component");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out);
}

const std::string& fixture_text() {
    static const std::string text = read_file(fixture_folder() / "project.json");
    return text;
}

Json fixture_json() {
    return Json::parse(fixture_text());
}

// The text of a JSON value as a project file: 2-space indentation and a final newline.
std::string text_of(const Json& document) {
    return document.dump(2) + "\n";
}

// The indices of the fixture's entities, in file order: entities[3] is the "Harbour at Dusk"
// painting. The fixture's entity at index i has the id 01932f5e-8a00-7000-8000-<i + 1 in hex>.
namespace ix {
constexpr std::size_t wall_a = 0;
constexpr std::size_t wall_b = 1;
constexpr std::size_t gallery = 2;
constexpr std::size_t harbour = 3;
constexpr std::size_t doorway = 4;
constexpr std::size_t study = 5;
constexpr std::size_t plinth = 6;
constexpr std::size_t seated_figure = 7;
constexpr std::size_t track = 8;
constexpr std::size_t spot = 9;
constexpr std::size_t downlight = 10;
constexpr std::size_t sensor = 11;
constexpr std::size_t entrance_view = 12;
constexpr std::size_t wall_a_width = 13;
constexpr std::size_t entity_count = 14;
} // namespace ix

Uuid uuid(std::string_view text) {
    const std::optional<Uuid> id = Uuid::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

std::string numbered_id(std::string_view prefix, std::size_t number) {
    char digits[32];
    std::snprintf(digits, sizeof digits, "%012zx", number);
    return std::string(prefix) + digits;
}

// The id of the fixture's entity at an index, and of its work at an index, in the file's order.
std::string entity_id_text(std::size_t index) {
    return numbered_id("01932f5e-8a00-7000-8000-", index + 1);
}
std::string work_id_text(std::size_t index) {
    return numbered_id("01932f5e-7f00-7000-8000-", index + 1);
}
Uuid entity_id(std::size_t index) {
    return uuid(entity_id_text(index));
}
Uuid work_id(std::size_t index) {
    return uuid(work_id_text(index));
}

// A version 7 id that nothing in the fixture has.
const std::string missing_id = "01932f5e-8a00-7000-8000-0000000000ff";

const std::string image_sha256 = "9efd8c22d4e910f296919762e790de1c0b225e41356c2b99653bd55a21c44f0e";

// The document data/every_component/project.json describes, built field by field from the
// headers, so that a reader and a writer that agree with each other but not with the
// specification still fail.
DocumentData every_component_data() {
    DocumentData data;
    data.settings = {.centre_line_m = 1.45,
                     .eye_height_m = 1.55,
                     .opening_hours_per_year = 2500.0,
                     .display_units = DisplayUnits::inches};

    data.library.fixture_types["generic-spot-800"] = {.name = "Generic spot 800 lm",
                                                      .flux_lm = 800.0,
                                                      .beam_angle_rad = 0.4,
                                                      .field_angle_rad = 0.7,
                                                      .colour_temperature_k = 3000.0};
    data.library.fixture_types["generic-wash-1200"] = {.name = "Generic wash 1200 lm",
                                                       .flux_lm = 1200.0,
                                                       .beam_angle_rad = 1.0,
                                                       .field_angle_rad = 1.6,
                                                       .colour_temperature_k = 4000.0};

    // Every member of Work, with a catalogue source.
    data.works[work_id(0)] = Work{
        .number = "1",
        .title = "Harbour at Dusk",
        .artist = "H\xc3\xa9l\xc3\xa8ne Dubois", // UTF-8 for e-acute and e-grave
        .date = "1872",
        .medium = "Oil on canvas",
        .accession_number = "1985.12",
        .lender = "Private collection",
        .credit_line = "Lent by a private collector",
        .licence = "CC0-1.0",
        .form = {.width_m = 0.73, .height_m = 0.6, .depth_m = 0.03},
        .image = AssetRef{.sha256 = image_sha256, .extension = "png"},
        .material = {.preset = "oil-canvas-gloss",
                     .base_roughness = 0.4,
                     .coat_weight = 0.8,
                     .coat_roughness = 0.1},
        .sensitivity = {.responsivity = Responsivity::medium,
                        .max_lux = 150.0,
                        .max_lux_hours = 150000.0},
        .source =
            FromCatalogue{.catalogue_id = "dubois-harbour-1872", .image_sha256 = image_sha256},
        .external_ids = {{.system = "TMS", .record_id = "48213"},
                         {.system = "Wikidata", .record_id = "Q123456789"}},
    };
    // Optional members absent, an import source, no external ids. The works below are built
    // member by member: every member not set keeps its default, so optionals are absent.
    Work& study_work = data.works[work_id(1)];
    study_work.number = "2a";
    study_work.title = "Study of a Head";
    study_work.artist = "Unknown";
    study_work.date = "c. 1650";
    study_work.medium = "Red chalk on paper";
    study_work.credit_line = "Museum purchase, 1902";
    study_work.form.width_m = 0.3;
    study_work.form.height_m = 0.4;
    study_work.material.preset = "paper-matte";
    study_work.sensitivity.responsivity = Responsivity::high;
    study_work.sensitivity.max_lux = 50.0;
    study_work.source = FromImport{.original_name = "study-of-a-head.jpg"};

    // Empty text fields, values at the bottom of their ranges, a checklist source.
    Work& figure_work = data.works[work_id(2)];
    figure_work.number = "3";
    figure_work.title = "Seated Figure";
    figure_work.form.width_m = 0.2;
    figure_work.form.height_m = 0.25;
    figure_work.material.preset = "oil-canvas-matte";
    figure_work.material.coat_weight = 0.0;
    figure_work.sensitivity.responsivity = Responsivity::irresponsive;
    figure_work.sensitivity.max_lux_hours = 0.0;
    figure_work.source = FromChecklist{.row = 12};

    // Entered by hand (no source), not placed, a title with quotes.
    Work& print_work = data.works[work_id(3)];
    print_work.number = "4";
    print_work.title = "Untitled (\"Blue\")";
    print_work.artist = "B. Printer";
    print_work.date = "1890-1895";
    print_work.medium = "Etching";
    print_work.credit_line = "Gift of the artist";
    print_work.form.width_m = 0.15;
    print_work.form.height_m = 0.1;
    print_work.material.preset = "paper-matte";
    print_work.sensitivity.responsivity = Responsivity::low;

    Entity& wall_a = data.entities[entity_id(ix::wall_a)];
    wall_a.name = "Wall A";
    wall_a.wall = Wall{.start_m = {0.0, 0.0},
                       .end_m = {8.0, 0.0},
                       .height_m = 3.5,
                       .thickness_m = 0.2,
                       .finish_a = "wall-white",
                       .finish_b = "wall-grey",
                       .label = "A"};

    Entity& wall_b = data.entities[entity_id(ix::wall_b)];
    wall_b.name = "Wall B";
    wall_b.wall = Wall{.start_m = {8.0, 6.0},
                       .end_m = {8.0, 0.0},
                       .height_m = 3.5,
                       .thickness_m = 0.2,
                       .finish_a = std::nullopt,
                       .finish_b = std::nullopt,
                       .label = "B"};

    Entity& gallery = data.entities[entity_id(ix::gallery)];
    gallery.name = "Gallery 1";
    gallery.room = Room{.outline_m = {{0.0, 0.0}, {8.0, 0.0}, {8.0, 6.0}, {0.0, 6.0}},
                        .floor_height_m = 0.0,
                        .ceiling_height_m = 3.5,
                        .floor_finish = "oak-floor",
                        .ceiling_finish = "ceiling-white"};

    Entity& harbour = data.entities[entity_id(ix::harbour)];
    harbour.name = "Harbour at Dusk";
    harbour.exhibit = Exhibit{.work = work_id(0)};
    harbour.framing = Framing{.style = "gilt-classic",
                              .width_m = 0.08,
                              .colour = LinearRgb{0.6, 0.45, 0.2},
                              .mat = Mat{.border_m = 0.05,
                                         .bottom_extra_m = 0.02,
                                         .overlap_m = 0.01,
                                         .colour = {0.9, 0.88, 0.85}},
                              .glazing = Glazing::anti_reflective_glass};
    harbour.placement = OnWall{.wall = entity_id(ix::wall_a),
                               .face = WallFace::a,
                               .distance_m = 2.5,
                               .centre_height_m = 1.45,
                               .stand_off_m = 0.02};

    Entity& doorway = data.entities[entity_id(ix::doorway)];
    doorway.name = "Doorway";
    doorway.opening = Opening{.wall = entity_id(ix::wall_b),
                              .offset_m = 1.0,
                              .width_m = 1.2,
                              .height_m = 2.2,
                              .sill_m = 0.0,
                              .kind = OpeningKind::door};

    Entity& study = data.entities[entity_id(ix::study)];
    study.name = "Study of a Head";
    study.exhibit = Exhibit{.work = work_id(1)};
    study.framing = Framing{.style = "box",
                            .width_m = std::nullopt,
                            .colour = std::nullopt,
                            .mat = std::nullopt,
                            .glazing = std::nullopt};
    study.placement = OnWall{.wall = entity_id(ix::wall_b),
                             .face = WallFace::b,
                             .distance_m = 4.0,
                             .centre_height_m = 1.45,
                             .stand_off_m = 0.0};

    Entity& plinth = data.entities[entity_id(ix::plinth)];
    plinth.name = "Plinth";
    plinth.placement =
        OnFloor{.room = entity_id(ix::gallery), .position_m = {4.0, 3.0}, .rotation_rad = 0.5};

    Entity& seated_figure = data.entities[entity_id(ix::seated_figure)];
    seated_figure.name = "Seated Figure";
    seated_figure.exhibit = Exhibit{.work = work_id(2)};
    seated_figure.placement =
        OnHost{.host = entity_id(ix::plinth), .offset_m = {0.0, 0.0, 1.0}, .rotation_rad = -0.25};

    Entity& track = data.entities[entity_id(ix::track)];
    track.name = "Track 1";
    track.track = Track{.points_m = {{1.0, 1.5}, {7.0, 1.5}}, .height_m = 3.2};

    Entity& spot = data.entities[entity_id(ix::spot)];
    spot.name = "Spot 1";
    spot.placement = OnTrack{.track = entity_id(ix::track), .distance_m = 1.5};
    spot.luminaire = Luminaire{.fixture_type = "generic-spot-800",
                               .dimming = 0.75,
                               .aim = AimAtEntity{.target = entity_id(ix::harbour)},
                               .colour_temperature_k = 2700.0};

    Entity& downlight = data.entities[entity_id(ix::downlight)];
    downlight.name = "Downlight 1";
    downlight.placement = OnCeiling{.room = entity_id(ix::gallery), .position_m = {6.0, 4.0}};
    downlight.luminaire = Luminaire{.fixture_type = "generic-wash-1200",
                                    .dimming = 1.0,
                                    .aim = AimAtPoint{.point_m = {6.0, 4.0, 0.0}},
                                    .colour_temperature_k = std::nullopt};

    Entity& sensor = data.entities[entity_id(ix::sensor)];
    sensor.name = "Sensor 1";
    sensor.lux_sensor = LuxSensor{.position_m = {2.5, 0.1, 1.45}, .normal = {0.0, 1.0, 0.0}};

    Entity& entrance_view = data.entities[entity_id(ix::entrance_view)];
    entrance_view.name = "Entrance view";
    entrance_view.viewpoint = Viewpoint{.position_m = {4.0, 5.0, 1.55},
                                        .yaw_rad = -1.5,
                                        .pitch_rad = -0.1,
                                        .field_of_view_rad = 0.9,
                                        .ev100 = 7.5};

    Entity& wall_a_width = data.entities[entity_id(ix::wall_a_width)];
    wall_a_width.name = "Width of wall A";
    wall_a_width.dimension =
        Dimension{.from = {.entity = entity_id(ix::wall_a), .point_m = {0.0, 0.0, 0.0}},
                  .to = {.entity = std::nullopt, .point_m = {8.0, 0.0, 0.0}}};

    return data;
}

// Where two texts first differ, as the line in each; empty when they are equal. A failed
// comparison of two 400-line files shows only this.
std::string difference(std::string_view written, std::string_view expected) {
    const std::size_t common = std::min(written.size(), expected.size());
    std::size_t i = 0;
    std::size_t line = 1;
    std::size_t line_start = 0;
    for (; i < common && written[i] == expected[i]; ++i) {
        if (written[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    if (i == common && written.size() == expected.size()) {
        return {};
    }
    const auto line_of = [line_start](std::string_view text) {
        const std::size_t end = text.find('\n', line_start);
        return std::string(text.substr(
            line_start, end == std::string_view::npos ? std::string_view::npos : end - line_start));
    };
    return "line " + std::to_string(line) + " differs:\n  written:  " + line_of(written) +
           "\n  expected: " + line_of(expected) + "\n  (sizes " + std::to_string(written.size()) +
           " and " + std::to_string(expected.size()) + ")";
}

// A folder of its own under the system's temporary folder, one name per test, since CTest runs
// test cases in parallel processes. Removed before and after; created by whoever needs it.
class TempFolder {
  public:
    explicit TempFolder(std::string_view name)
        : path_(std::filesystem::temp_directory_path() / "cs_doc_tests" / name) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_.parent_path());
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

// ---- Rejections ------------------------------------------------------------------------------

// The ProjectError parsing `text` throws; nullopt if it parses. Any other exception escapes and
// fails the test: invalid input must fail with a ProjectError, not a library's own exception.
std::optional<ProjectError> rejection(const std::string& text) {
    try {
        (void)parse_project_json(text);
    } catch (const ProjectError& error) {
        return error;
    }
    return std::nullopt;
}

// `document` is rejected with an error at exactly `path`, which what() names as
// "<where>: <json path>: <problem>", <where> being "project.json" for text not read from a file.
void check_rejected_at(const Json& document, const std::string& path) {
    INFO("expected a ProjectError at " << path);
    const std::optional<ProjectError> error = rejection(text_of(document));
    REQUIRE(error.has_value());
    CHECK(error->json_path() == path);
    CHECK_THAT(std::string(error->what()), StartsWith("project.json: " + path + ": "));
}

// Rejected with an error somewhere under `prefix`, where the header leaves the exact member open
// (a relation between two members, an array that is too long or too short).
void check_rejected_under(const Json& document, const std::string& prefix) {
    INFO("expected a ProjectError under " << prefix);
    const std::optional<ProjectError> error = rejection(text_of(document));
    REQUIRE(error.has_value());
    CHECK_THAT(error->json_path(), StartsWith(prefix));
    CHECK_THAT(std::string(error->what()), ContainsSubstring(prefix));
}

// One change to the fixture: the value at a JSON pointer (RFC 6901) replaced or added.
struct Change {
    std::string pointer;
    Json value;
    std::string path = {}; // the JSON path the error must name; unused when accepted
};

void check_each_rejected_at(const std::vector<Change>& changes) {
    for (const Change& change : changes) {
        CAPTURE(change.pointer, change.value.dump());
        Json document = fixture_json();
        document[Json::json_pointer(change.pointer)] = change.value;
        check_rejected_at(document, change.path);
    }
}

void check_each_accepted(const std::vector<Change>& changes) {
    for (const Change& change : changes) {
        CAPTURE(change.pointer, change.value.dump());
        Json document = fixture_json();
        document[Json::json_pointer(change.pointer)] = change.value;
        CHECK_NOTHROW(parse_project_json(text_of(document)));
    }
}

void erase(Json& document, const std::string& pointer) {
    const Json::json_pointer at(pointer);
    REQUIRE(document[at.parent_pointer()].erase(at.back()) == 1);
}

Json& entity(Json& document, std::size_t index) {
    return document["entities"][index];
}

// Whether the entity holds a component, from its members rather than Entity::has.
bool holds(const Entity& entity, ComponentKind kind) {
    switch (kind) {
    case ComponentKind::wall:
        return entity.wall.has_value();
    case ComponentKind::opening:
        return entity.opening.has_value();
    case ComponentKind::room:
        return entity.room.has_value();
    case ComponentKind::exhibit:
        return entity.exhibit.has_value();
    case ComponentKind::framing:
        return entity.framing.has_value();
    case ComponentKind::placement:
        return entity.placement.has_value();
    case ComponentKind::track:
        return entity.track.has_value();
    case ComponentKind::luminaire:
        return entity.luminaire.has_value();
    case ComponentKind::lux_sensor:
        return entity.lux_sensor.has_value();
    case ComponentKind::viewpoint:
        return entity.viewpoint.has_value();
    case ComponentKind::dimension:
        return entity.dimension.has_value();
    }
    return false;
}

} // namespace

// ---- The types -------------------------------------------------------------------------------

TEST_CASE("project settings default to a 1.50 m centre line, 1.60 m eye height, 2184 opening "
          "hours and centimetres",
          "[doc][project_io]") {
    // Acceptance criterion 1. 2184 hours a year is 52 weeks of 6 days of 7 hours.
    const ProjectSettings settings;
    CHECK(settings.centre_line_m == 1.50);
    CHECK(settings.eye_height_m == 1.60);
    CHECK(settings.opening_hours_per_year == 2184.0);
    CHECK(settings.opening_hours_per_year == 52.0 * 6.0 * 7.0);
    CHECK(settings.display_units == DisplayUnits::centimetres);

    const Document empty;
    CHECK(empty.settings() == settings);
    CHECK(empty.works().empty());
    CHECK(empty.entities().empty());
    CHECK(empty.library().fixture_types.empty());
}

TEST_CASE("work sizes are stored as width_m and height_m", "[doc][project_io]") {
    // Acceptance criterion 1: "Sizes are stored as width and height, named so". The names below
    // must compile, and the file uses them as keys.
    Work work;
    work.form = FlatForm{.width_m = 0.5, .height_m = 0.25, .depth_m = 0.04};
    work.material.preset = "oil-canvas-gloss";
    CHECK(work.form.width_m == 0.5);
    CHECK(work.form.height_m == 0.25);

    DocumentData data;
    data.works[work_id(0)] = work;
    const Json written = Json::parse(to_project_json(Document(data)));
    const Json& form = written.at("works").at(0).at("form");
    CHECK(form.at("kind") == "flat");
    CHECK(form.at("width_m") == 0.5);
    CHECK(form.at("height_m") == 0.25);
    CHECK(form.at("depth_m") == 0.04);

    // Openings, the other sized thing, are width by height too.
    Opening opening;
    opening.width_m = 0.9;
    opening.height_m = 2.1;
    CHECK(opening.width_m == 0.9);
    CHECK(opening.height_m == 2.1);
}

TEST_CASE("component keys are the camelCase names, in declaration order", "[doc][project_io]") {
    CHECK(component_key(ComponentKind::wall) == "wall");
    CHECK(component_key(ComponentKind::opening) == "opening");
    CHECK(component_key(ComponentKind::room) == "room");
    CHECK(component_key(ComponentKind::exhibit) == "exhibit");
    CHECK(component_key(ComponentKind::framing) == "framing");
    CHECK(component_key(ComponentKind::placement) == "placement");
    CHECK(component_key(ComponentKind::track) == "track");
    CHECK(component_key(ComponentKind::luminaire) == "luminaire");
    CHECK(component_key(ComponentKind::lux_sensor) == "luxSensor");
    CHECK(component_key(ComponentKind::viewpoint) == "viewpoint");
    CHECK(component_key(ComponentKind::dimension) == "dimension");

    const Entity nothing;
    for (const ComponentKind kind : all_component_kinds) {
        CAPTURE(component_key(kind));
        CHECK_FALSE(nothing.has(kind));
    }

    Entity painting;
    painting.exhibit = Exhibit{};
    painting.placement = OnWall{};
    for (const ComponentKind kind : all_component_kinds) {
        CAPTURE(component_key(kind));
        CHECK(painting.has(kind) ==
              (kind == ComponentKind::exhibit || kind == ComponentKind::placement));
    }
}

// ---- The fixture -----------------------------------------------------------------------------

TEST_CASE("the fixture is canonical JSON with the entities the tests expect at each index",
          "[doc][project_io]") {
    // A check of the fixture itself, not of the code under test: its text is what a 2-space
    // dump of its own JSON gives, with a final newline, and entities sit at the indices the
    // expected error paths below use.
    const std::string& text = fixture_text();
    CHECK(difference(text_of(Json::parse(text)), text).empty());
    CHECK(text.ends_with("}\n"));
    CHECK(text.find('\r') == std::string::npos);
    CHECK(text.find('\t') == std::string::npos);

    const Json document = fixture_json();
    REQUIRE(document["entities"].size() == ix::entity_count);
    for (std::size_t i = 0; i < ix::entity_count; ++i) {
        CAPTURE(i);
        CHECK(document["entities"][i]["id"] == entity_id_text(i));
    }
    CHECK(document["entities"][ix::harbour]["name"] == "Harbour at Dusk");
}

TEST_CASE("the every-component fixture loads with one of every component and placement kind",
          "[doc][project_io]") {
    // Acceptance criterion 3: "a fixture ... with one of every component".
    const Document document = load_project_folder(fixture_folder());
    CHECK(document.entities().size() == ix::entity_count);
    CHECK(document.works().size() == 4);
    CHECK(document.library().fixture_types.size() == 2);

    for (const ComponentKind kind : all_component_kinds) {
        CAPTURE(component_key(kind));
        CHECK(std::ranges::any_of(document.entities(),
                                  [kind](const auto& entry) { return entry.second.has(kind); }));
    }

    std::set<std::size_t> placement_kinds;
    std::set<std::size_t> aim_kinds;
    for (const auto& entry : document.entities()) {
        const Entity& entity = entry.second;
        CAPTURE(entity.name);
        for (const ComponentKind kind : all_component_kinds) {
            CAPTURE(component_key(kind));
            CHECK(entity.has(kind) == holds(entity, kind));
        }
        if (entity.placement) {
            placement_kinds.insert(entity.placement->index());
        }
        if (entity.luminaire) {
            aim_kinds.insert(entity.luminaire->aim.index());
        }
    }
    // OnWall, OnFloor, OnHost, OnTrack and OnCeiling; AimAtEntity and AimAtPoint.
    CHECK(std::variant_size_v<Placement> == 5);
    CHECK(placement_kinds.size() == 5);
    CHECK(aim_kinds.size() == 2);

    std::set<std::size_t> source_kinds;
    bool entered_by_hand = false;
    for (const auto& entry : document.works()) {
        const Work& work = entry.second;
        if (work.source) {
            source_kinds.insert(work.source->index());
        } else {
            entered_by_hand = true;
        }
    }
    CHECK(source_kinds.size() == 3);
    CHECK(entered_by_hand);
}

TEST_CASE("the every-component fixture reads as exactly the document it describes",
          "[doc][project_io]") {
    const Document loaded = load_project_folder(fixture_folder());
    const DocumentData expected = every_component_data();

    // Piece by piece first, so a failure says where.
    CHECK(loaded.settings() == expected.settings);
    CHECK(loaded.library() == expected.library);
    for (const auto& [id, work] : expected.works) {
        CAPTURE(id.to_string());
        const Work* found = loaded.find_work(id);
        REQUIRE(found != nullptr);
        CHECK(*found == work);
    }
    for (const auto& [id, entity] : expected.entities) {
        CAPTURE(id.to_string(), entity.name);
        const Entity* found = loaded.find_entity(id);
        REQUIRE(found != nullptr);
        CHECK(found->name == entity.name);
        CHECK(found->wall == entity.wall);
        CHECK(found->opening == entity.opening);
        CHECK(found->room == entity.room);
        CHECK(found->exhibit == entity.exhibit);
        CHECK(found->framing == entity.framing);
        CHECK(found->placement == entity.placement);
        CHECK(found->track == entity.track);
        CHECK(found->luminaire == entity.luminaire);
        CHECK(found->lux_sensor == entity.lux_sensor);
        CHECK(found->viewpoint == entity.viewpoint);
        CHECK(found->dimension == entity.dimension);
    }
    CHECK(loaded == Document(expected));
    CHECK(parse_project_json(fixture_text()) == loaded);
}

TEST_CASE("find_entity, find_work and find_fixture_type look things up by id",
          "[doc][project_io]") {
    const Document document(every_component_data());

    const Entity* wall = document.find_entity(entity_id(ix::wall_a));
    REQUIRE(wall != nullptr);
    CHECK(wall->name == "Wall A");
    CHECK(document.find_entity(uuid(missing_id)) == nullptr);
    CHECK(document.find_entity(work_id(0)) == nullptr); // a work's id is not an entity's
    CHECK(document.find_entity(Uuid{}) == nullptr);

    const Work* work = document.find_work(work_id(0));
    REQUIRE(work != nullptr);
    CHECK(work->title == "Harbour at Dusk");
    CHECK(document.find_work(entity_id(ix::wall_a)) == nullptr);

    const FixtureType* spot = document.library().find_fixture_type("generic-spot-800");
    REQUIRE(spot != nullptr);
    CHECK(spot->flux_lm == 800.0);
    CHECK(document.library().find_fixture_type("generic-spot-801") == nullptr);
    CHECK(document.library().find_fixture_type("") == nullptr);
}

// ---- Writing ---------------------------------------------------------------------------------

TEST_CASE("saving the every-component fixture writes its project.json byte for byte",
          "[doc][project_io]") {
    // Acceptance criterion 3: "reloads and saves byte for byte (stable key order, 2-space
    // indent, trailing newline)".
    const TempFolder folder("save_fixture_byte_for_byte");
    const Document document = load_project_folder(fixture_folder());

    save_project_folder(document, folder.path());
    const std::filesystem::path saved = folder.path() / "project.json";
    CHECK(difference(read_file(saved), fixture_text()).empty());
    CHECK_FALSE(std::filesystem::exists(folder.path() / "project.json.tmp"));

    // Saving over the first save gives the same bytes, and the save reads back the same.
    save_project_folder(document, folder.path());
    CHECK(difference(read_file(saved), fixture_text()).empty());
    CHECK(load_project_folder(folder.path()) == document);
}

TEST_CASE("to_project_json writes the fixture's text", "[doc][project_io]") {
    // From what the reader made, and from the document built in code, so the writer is checked
    // on its own.
    CHECK(
        difference(to_project_json(load_project_folder(fixture_folder())), fixture_text()).empty());
    CHECK(difference(to_project_json(Document(every_component_data())), fixture_text()).empty());
}

TEST_CASE("an empty document is written in the canonical form", "[doc][project_io]") {
    // From the header: the top-level keys in order, the default settings with whole numbers
    // written with a fractional part, and arrays written even when empty.
    const std::string expected = R"({
  "schemaVersion": 1,
  "settings": {
    "centreLine_m": 1.5,
    "eyeHeight_m": 1.6,
    "openingHoursPerYear": 2184.0,
    "displayUnits": "centimetres"
  },
  "library": {
    "fixtureTypes": []
  },
  "works": [],
  "entities": []
}
)";
    CHECK(difference(to_project_json(Document{}), expected).empty());
    CHECK(parse_project_json(expected) == Document{});
}

TEST_CASE("parse_project_json(to_project_json(document)) gives back the document",
          "[doc][project_io]") {
    SECTION("the every-component document") {
        const Document document(every_component_data());
        CHECK(parse_project_json(to_project_json(document)) == document);
    }
    SECTION("an empty document") {
        CHECK(parse_project_json(to_project_json(Document{})) == Document{});
    }
    SECTION("numbers that need all their digits, and strings that need escapes") {
        // Numbers are written in the shortest form that reads back to the same double, so
        // these survive exactly: 0.1 + 0.2 is 0.30000000000000004, the smallest subnormal
        // (written 5e-324) and the largest finite double. Strings keep quotes, backslashes, control
        // characters and characters outside the Basic Multilingual Plane.
        DocumentData data;
        Entity& wall = data.entities[entity_id(0)];
        wall.name = "Quote \" backslash \\ newline \n tab \t palette \xf0\x9f\x8e\xa8";
        wall.wall = Wall{.start_m = {0.1 + 0.2, 1.0 / 3.0},
                         .end_m = {123456789.123, -2.0 / 3.0},
                         .height_m = 1e-7,
                         .thickness_m = std::numeric_limits<double>::max(),
                         .finish_a = std::nullopt,
                         .finish_b = std::nullopt,
                         .label = ""};
        constexpr double tiny = std::numeric_limits<double>::denorm_min(); // about 4.9e-324
        Entity& room = data.entities[entity_id(1)];
        room.name = "";
        room.room = Room{.outline_m = {{tiny, -tiny}, {1e300, 0.0}, {0.0, -1e-300}},
                         .floor_height_m = -1.25,
                         .ceiling_height_m = 1e15,
                         .floor_finish = std::nullopt,
                         .ceiling_finish = std::nullopt};
        const Document document(data);

        const std::string text = to_project_json(document);
        CHECK(text.ends_with("}\n"));
        CHECK(parse_project_json(text) == document);
        CHECK(to_project_json(parse_project_json(text)) == text);
    }
}

TEST_CASE("entities, works and fixture types are written in id order whatever order they are "
          "read in, and keys in their fixed order",
          "[doc][project_io]") {
    // Reading takes an object's keys and a list's elements in any order (the header names the
    // errors, and order is not one of them); writing puts them in the canonical order.
    Json document = fixture_json();
    for (Json* list :
         {&document["entities"], &document["works"], &document["library"]["fixtureTypes"]}) {
        std::reverse(list->begin(), list->end());
    }

    // The painting (now entities[10]) with its components, its placement's members and the
    // top-level keys in reverse: "kind" last in the placement, "id" last in the entity.
    const auto reversed_keys = [](const Json& object) {
        Json reversed = Json::object();
        for (auto it = object.rbegin(); it != object.rend(); ++it) {
            reversed[it.key()] = it.value();
        }
        return reversed;
    };
    Json& painting = document["entities"][ix::entity_count - 1 - ix::harbour];
    REQUIRE(painting["name"] == "Harbour at Dusk");
    painting["placement"] = reversed_keys(painting["placement"]);
    painting = reversed_keys(painting);
    document = reversed_keys(document);

    const Document read = parse_project_json(text_of(document));
    CHECK(read == Document(every_component_data()));
    CHECK(difference(to_project_json(read), fixture_text()).empty());
}

// ---- What reading rejects: the five of acceptance criterion 4 --------------------------------

TEST_CASE("an unknown component is rejected with its JSON path", "[doc][project_io]") {
    // Acceptance criterion 4. Furniture is a component reserved for 1.0, so unknown here.
    Json document = fixture_json();
    entity(document, ix::gallery)["furniture"] = Json::object({{"type", "bench"}});
    check_rejected_at(document, "entities[2].furniture");
}

TEST_CASE("a missing field is rejected with its JSON path", "[doc][project_io]") {
    // Acceptance criterion 4. The path is the missing member's own.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"/entities/0/wall/thickness_m", "entities[0].wall.thickness_m"},
        {"/entities/0/name", "entities[0].name"},
        {"/entities/0/id", "entities[0].id"},
        {"/entities/3/placement/kind", "entities[3].placement.kind"},
        {"/entities/3/placement/centreHeight_m", "entities[3].placement.centreHeight_m"},
        {"/entities/9/luminaire/aim", "entities[9].luminaire.aim"},
        {"/works/0/form/width_m", "works[0].form.width_m"},
        {"/works/1/externalIds", "works[1].externalIds"}, // arrays are always written
        {"/library/fixtureTypes/0/flux_lm", "library.fixtureTypes[0].flux_lm"},
        {"/settings/displayUnits", "settings.displayUnits"},
        {"/settings", "settings"},
        {"/schemaVersion", "schemaVersion"},
    };
    for (const auto& [pointer, path] : cases) {
        CAPTURE(pointer);
        Json document = fixture_json();
        erase(document, pointer);
        check_rejected_at(document, path);
    }
}

TEST_CASE("a value of the wrong type is rejected with its JSON path", "[doc][project_io]") {
    // Acceptance criterion 4.
    check_each_rejected_at({
        {"/entities/0/wall/height_m", "3.5", "entities[0].wall.height_m"},
        {"/entities/0/name", 12, "entities[0].name"},
        {"/entities/2/room/outline_m/1", "x", "entities[2].room.outline_m[1]"},
        {"/entities/3/placement", Json::array({"onWall"}), "entities[3].placement"},
        {"/entities/3/placement/wall", 42, "entities[3].placement.wall"},
        {"/entities/9/luminaire/dimming", true, "entities[9].luminaire.dimming"},
        {"/entities/9/luminaire/aim/target", nullptr, "entities[9].luminaire.aim.target"},
        {"/works/0/number", 1, "works[0].number"},
        {"/works/0/externalIds", Json::object(), "works[0].externalIds"},
        {"/works/2/source/row", 12.5, "works[2].source.row"},
        {"/works/2/source/row", "12", "works[2].source.row"},
        {"/settings", Json::array(), "settings"},
        {"/schemaVersion", "1", "schemaVersion"},
    });

    // A top level that is not an object is at fault as a whole: the path is empty.
    const std::optional<ProjectError> error = rejection("[]\n");
    REQUIRE(error.has_value());
    CHECK(error->json_path().empty());
}

TEST_CASE("a dangling reference is rejected with its JSON path", "[doc][project_io]") {
    // Acceptance criterion 4, with the issue's own example path.
    Json document = fixture_json();
    entity(document, ix::harbour)["placement"]["wall"] = missing_id;
    check_rejected_at(document, "entities[3].placement.wall");
}

TEST_CASE("a schemaVersion other than 1 is rejected", "[doc][project_io]") {
    // Acceptance criterion 4: a newer file and a nonsensical one.
    for (const int version : {2, 0, -1}) {
        CAPTURE(version);
        Json document = fixture_json();
        document["schemaVersion"] = version;
        check_rejected_at(document, "schemaVersion");
    }
    CHECK(schema_version == 1);
}

// ---- What reading rejects: the rest of the header's list -------------------------------------

TEST_CASE("unknown keys are rejected wherever they are", "[doc][project_io]") {
    check_each_rejected_at({
        {"/extra", 1, "extra"},
        {"/settings/units", "metres", "settings.units"},
        {"/library/frameStyles", Json::array(), "library.frameStyles"},
        {"/entities/0/wall/colour", Json::array({1.0, 1.0, 1.0}), "entities[0].wall.colour"},
        // A member of another placement kind.
        {"/entities/3/placement/host", entity_id_text(ix::plinth), "entities[3].placement.host"},
        // The C++ spelling instead of the file's.
        {"/entities/3/placement/centre_height_m", 1.5, "entities[3].placement.centre_height_m"},
        {"/works/0/notes", "", "works[0].notes"},
        {"/works/0/form/bounds_m", Json::array({1.0, 1.0, 1.0}), "works[0].form.bounds_m"},
        {"/library/fixtureTypes/0/ies", "", "library.fixtureTypes[0].ies"},
    });
}

TEST_CASE("dangling references to works, entities and fixture types are rejected",
          "[doc][project_io]") {
    check_each_rejected_at({
        {"/entities/3/exhibit/work", work_id_text(9), "entities[3].exhibit.work"},
        // An entity's id where a work's belongs.
        {"/entities/3/exhibit/work", entity_id_text(ix::wall_a), "entities[3].exhibit.work"},
        {"/entities/4/opening/wall", missing_id, "entities[4].opening.wall"},
        {"/entities/6/placement/room", missing_id, "entities[6].placement.room"},
        {"/entities/7/placement/host", missing_id, "entities[7].placement.host"},
        {"/entities/9/placement/track", missing_id, "entities[9].placement.track"},
        {"/entities/9/luminaire/fixtureType", "generic-spot-801",
         "entities[9].luminaire.fixtureType"},
        {"/entities/9/luminaire/aim/target", missing_id, "entities[9].luminaire.aim.target"},
        {"/entities/10/placement/room", missing_id, "entities[10].placement.room"},
        {"/entities/13/dimension/from/entity", missing_id, "entities[13].dimension.from.entity"},
        // A work's id where an entity's belongs.
        {"/entities/13/dimension/to/entity", work_id_text(0), "entities[13].dimension.to.entity"},
    });
}

TEST_CASE("a reference to an entity without the needed component is rejected",
          "[doc][project_io]") {
    check_each_rejected_at({
        // The room where a wall belongs: the issue's example path again.
        {"/entities/3/placement/wall", entity_id_text(ix::gallery), "entities[3].placement.wall"},
        {"/entities/4/opening/wall", entity_id_text(ix::track), "entities[4].opening.wall"},
        {"/entities/6/placement/room", entity_id_text(ix::wall_a), "entities[6].placement.room"},
        {"/entities/9/placement/track", entity_id_text(ix::wall_a), "entities[9].placement.track"},
        {"/entities/10/placement/room", entity_id_text(ix::spot), "entities[10].placement.room"},
    });
}

TEST_CASE("a duplicated id is rejected at its second appearance", "[doc][project_io]") {
    SECTION("an entity") {
        // A copy of the lux sensor, which nothing references, appended as entities[14].
        Json document = fixture_json();
        const Json copy = entity(document, ix::sensor);
        document["entities"].push_back(copy);
        check_rejected_at(document, "entities[14].id");
    }
    SECTION("a work") {
        // A copy of the unplaced work, appended as works[4].
        Json document = fixture_json();
        const Json copy = document["works"][3];
        document["works"].push_back(copy);
        check_rejected_at(document, "works[4].id");
    }
    SECTION("a fixture type") {
        Json document = fixture_json();
        const Json copy = document["library"]["fixtureTypes"][0];
        document["library"]["fixtureTypes"].push_back(copy);
        check_rejected_at(document, "library.fixtureTypes[2].id");
    }
}

TEST_CASE("OnHost placements that form a cycle are rejected", "[doc][project_io]") {
    SECTION("an entity hosted on itself") {
        Json document = fixture_json();
        entity(document, ix::seated_figure)["placement"]["host"] =
            entity_id_text(ix::seated_figure);
        check_rejected_at(document, "entities[7].placement.host");
    }
    SECTION("two entities hosted on each other") {
        // The plinth goes onto the figure that stands on it. Which of the two the error names
        // is left open.
        Json document = fixture_json();
        entity(document, ix::plinth)["placement"] =
            Json::object({{"kind", "onHost"},
                          {"host", entity_id_text(ix::seated_figure)},
                          {"offset_m", Json::array({0.0, 0.0, 0.0})},
                          {"rotation_rad", 0.0}});
        const std::optional<ProjectError> error = rejection(text_of(document));
        REQUIRE(error.has_value());
        const std::string& path = error->json_path();
        CAPTURE(path);
        CHECK((path == "entities[6].placement.host" || path == "entities[7].placement.host"));
    }
}

TEST_CASE("a value out of its range is rejected with its JSON path", "[doc][project_io]") {
    check_each_rejected_at({
        {"/settings/centreLine_m", 0.0, "settings.centreLine_m"},
        {"/settings/eyeHeight_m", -1.6, "settings.eyeHeight_m"},
        {"/settings/openingHoursPerYear", 8785.0, "settings.openingHoursPerYear"},
        {"/settings/openingHoursPerYear", -1.0, "settings.openingHoursPerYear"},
        {"/library/fixtureTypes/0/flux_lm", 0.0, "library.fixtureTypes[0].flux_lm"},
        {"/library/fixtureTypes/0/beamAngle_rad", 0.0, "library.fixtureTypes[0].beamAngle_rad"},
        {"/library/fixtureTypes/0/colourTemperature_K", -3000.0,
         "library.fixtureTypes[0].colourTemperature_K"},
        {"/works/0/form/width_m", 0.0, "works[0].form.width_m"},
        {"/works/0/form/height_m", -0.6, "works[0].form.height_m"},
        {"/works/0/form/depth_m", 0.0, "works[0].form.depth_m"},
        {"/works/0/material/baseRoughness", 1.5, "works[0].material.baseRoughness"},
        {"/works/0/material/coatWeight", -0.1, "works[0].material.coatWeight"},
        {"/works/0/sensitivity/maxLux", -1.0, "works[0].sensitivity.maxLux"},
        {"/works/0/sensitivity/maxLuxHours", -1.0, "works[0].sensitivity.maxLuxHours"},
        {"/works/2/source/row", 0, "works[2].source.row"},
        {"/entities/0/wall/height_m", 0.0, "entities[0].wall.height_m"},
        {"/entities/0/wall/thickness_m", -0.2, "entities[0].wall.thickness_m"},
        {"/entities/3/framing/width_m", 0.0, "entities[3].framing.width_m"},
        {"/entities/3/framing/mat/border_m", 0.0, "entities[3].framing.mat.border_m"},
        {"/entities/3/framing/colour/1", -0.5, "entities[3].framing.colour[1]"},
        {"/entities/3/placement/standOff_m", -0.01, "entities[3].placement.standOff_m"},
        {"/entities/4/opening/offset_m", -0.5, "entities[4].opening.offset_m"},
        {"/entities/4/opening/width_m", 0.0, "entities[4].opening.width_m"},
        {"/entities/4/opening/sill_m", -0.1, "entities[4].opening.sill_m"},
        {"/entities/9/placement/distance_m", -1.0, "entities[9].placement.distance_m"},
        {"/entities/9/luminaire/dimming", 1.5, "entities[9].luminaire.dimming"},
        {"/entities/9/luminaire/dimming", -0.25, "entities[9].luminaire.dimming"},
        {"/entities/9/luminaire/colourTemperature_K", 0.0,
         "entities[9].luminaire.colourTemperature_K"},
        {"/entities/12/viewpoint/fieldOfView_rad", 0.0, "entities[12].viewpoint.fieldOfView_rad"},
        {"/entities/12/viewpoint/fieldOfView_rad", 3.5, "entities[12].viewpoint.fieldOfView_rad"},
        {"/entities/12/viewpoint/pitch_rad", 2.0, "entities[12].viewpoint.pitch_rad"},
    });

    SECTION("relations between members") {
        // A ceiling at the floor's height, a field angle narrower than the beam, a zero normal.
        Json document = fixture_json();
        entity(document, ix::gallery)["room"]["ceilingHeight_m"] = 0.0;
        check_rejected_under(document, "entities[2].room");

        document = fixture_json();
        document["library"]["fixtureTypes"][0]["fieldAngle_rad"] = 0.3;
        check_rejected_under(document, "library.fixtureTypes[0]");

        document = fixture_json();
        entity(document, ix::sensor)["luxSensor"]["normal"] = Json::array({0.0, 0.0, 0.0});
        check_rejected_under(document, "entities[11].luxSensor.normal");
    }
}

TEST_CASE("values at the edges of their ranges are accepted", "[doc][project_io]") {
    check_each_accepted({
        {"/settings/openingHoursPerYear", 0.0},
        {"/settings/openingHoursPerYear", 8784.0},
        {"/library/fixtureTypes/0/fieldAngle_rad", 0.4}, // equal to the beam angle
        {"/works/0/material/baseRoughness", 1.0},
        {"/works/0/material/coatRoughness", 0.0},
        {"/works/0/sensitivity/maxLux", 0.0},
        {"/works/2/source/row", 1},
        {"/entities/3/framing/mat/bottomExtra_m", 0.0},
        {"/entities/4/opening/offset_m", 0.0},
        {"/entities/9/placement/distance_m", 0.0},
        {"/entities/9/luminaire/dimming", 0.0},
        {"/entities/11/luxSensor/normal", Json::array({0.0, 0.0, -5.0})}, // not unit length
    });
}

TEST_CASE("arrays shorter or longer than their limits are rejected", "[doc][project_io]") {
    const auto points = [](std::size_t count) {
        Json list = Json::array();
        for (std::size_t i = 0; i < count; ++i) {
            list.push_back(Json::array({static_cast<double>(i), static_cast<double>(i * i)}));
        }
        return list;
    };
    const auto external_ids = [](std::size_t count) {
        Json list = Json::array();
        for (std::size_t i = 0; i < count; ++i) {
            list.push_back(Json::object({{"system", "TMS"}, {"recordId", std::to_string(i)}}));
        }
        return list;
    };

    // A room's outline has 3 to 1000 points, a track 2 to 1000; a work at most 64 external ids.
    Json document = fixture_json();
    entity(document, ix::gallery)["room"]["outline_m"] = points(2);
    check_rejected_under(document, "entities[2].room.outline_m");
    entity(document, ix::gallery)["room"]["outline_m"] = points(1001);
    check_rejected_under(document, "entities[2].room.outline_m");
    entity(document, ix::gallery)["room"]["outline_m"] = points(1000);
    CHECK_NOTHROW(parse_project_json(text_of(document)));
    entity(document, ix::gallery)["room"]["outline_m"] = points(3);
    CHECK_NOTHROW(parse_project_json(text_of(document)));

    document = fixture_json();
    entity(document, ix::track)["track"]["points_m"] = points(1);
    check_rejected_under(document, "entities[8].track.points_m");
    entity(document, ix::track)["track"]["points_m"] = points(1001);
    check_rejected_under(document, "entities[8].track.points_m");
    entity(document, ix::track)["track"]["points_m"] = points(1000);
    CHECK_NOTHROW(parse_project_json(text_of(document)));

    document = fixture_json();
    document["works"][0]["externalIds"] = external_ids(65);
    check_rejected_under(document, "works[0].externalIds");
    document["works"][0]["externalIds"] = external_ids(64);
    CHECK_NOTHROW(parse_project_json(text_of(document)));

    // A point with the wrong number of coordinates.
    document = fixture_json();
    entity(document, ix::wall_a)["wall"]["start_m"] = Json::array({0.0, 0.0, 0.0});
    check_rejected_under(document, "entities[0].wall.start_m");
    entity(document, ix::wall_a)["wall"]["start_m"] = Json::array({0.0});
    check_rejected_under(document, "entities[0].wall.start_m");
}

TEST_CASE("more fixture types than max_fixture_types are rejected", "[doc][project_io]") {
    // The fixture's two, plus enough generic ones to reach the limit, then one more.
    const auto with_fixture_types = [](std::size_t total) {
        Json document = fixture_json();
        Json& list = document["library"]["fixtureTypes"];
        for (std::size_t i = list.size(); i < total; ++i) {
            char id[32];
            std::snprintf(id, sizeof id, "ft-%05zu", i);
            list.push_back(Json::object({{"id", id},
                                         {"name", id},
                                         {"flux_lm", 100.0},
                                         {"beamAngle_rad", 0.5},
                                         {"fieldAngle_rad", 0.8},
                                         {"colourTemperature_K", 3000.0}}));
        }
        return document;
    };
    CHECK_NOTHROW(parse_project_json(text_of(with_fixture_types(max_fixture_types))));
    check_rejected_under(with_fixture_types(max_fixture_types + 1), "library.fixtureTypes");
}

TEST_CASE("a project with max_entities entities loads", "[doc][project_io]") {
    // A file within every limit must load in time proportional to its size. A reader that is
    // quadratic in the length of a list (as nlohmann's parser with a depth callback was) takes
    // about 25 minutes here in a debug build; a linear one takes seconds.
    std::string text = R"({"schemaVersion": 1, "settings": {"centreLine_m": 1.5,
        "eyeHeight_m": 1.6, "openingHoursPerYear": 2184.0, "displayUnits": "centimetres"},
        "library": {"fixtureTypes": []}, "works": [], "entities": [)";
    for (std::size_t i = 0; i < max_entities; ++i) {
        char entity[80];
        std::snprintf(entity, sizeof entity,
                      R"(%s{"id": "01932f5e-8a00-7000-8000-%012zx", "name": ""})",
                      i == 0 ? "" : ",", i + 1);
        text += entity;
    }
    text += "]}";
    CHECK(parse_project_json(text).entities().size() == max_entities);
}

TEST_CASE("a string longer than max_string_bytes is rejected", "[doc][project_io]") {
    // The limit counts bytes of UTF-8, not characters: 2048 two-byte characters are 4096 bytes.
    const std::string e_acute = "\xc3\xa9";
    std::string two_byte_limit;
    for (std::size_t i = 0; i < max_string_bytes / 2; ++i) {
        two_byte_limit += e_acute;
    }
    REQUIRE(two_byte_limit.size() == max_string_bytes);

    Json document = fixture_json();
    entity(document, ix::wall_a)["name"] = std::string(max_string_bytes + 1, 'x');
    check_rejected_at(document, "entities[0].name");
    entity(document, ix::wall_a)["name"] = two_byte_limit + "x";
    check_rejected_at(document, "entities[0].name");

    document = fixture_json();
    document["works"][0]["title"] = std::string(max_string_bytes + 1, 'x');
    check_rejected_at(document, "works[0].title");

    // At the limit, the string is read whole.
    document = fixture_json();
    entity(document, ix::wall_a)["name"] = std::string(max_string_bytes, 'x');
    document["works"][0]["title"] = two_byte_limit;
    const Document read = parse_project_json(text_of(document));
    CHECK(read.find_entity(entity_id(ix::wall_a))->name == std::string(max_string_bytes, 'x'));
    CHECK(read.find_work(work_id(0))->title == two_byte_limit);
}

TEST_CASE("malformed ids, asset names, library ids and enumerations are rejected",
          "[doc][project_io]") {
    // Untrusted text in fields with a fixed form, including attempts to reach outside assets/.
    check_each_rejected_at({
        {"/entities/11/id", "not-a-uuid", "entities[11].id"},
        {"/entities/11/id", "{01932f5e-8a00-7000-8000-00000000000c}", "entities[11].id"},
        {"/entities/3/placement/wall", "01932f5e8a00700080000000000000001",
         "entities[3].placement.wall"},
        {"/works/3/id", "", "works[3].id"},
        {"/works/0/image/extension", "../../../etc/passwd", "works[0].image.extension"},
        {"/works/0/image/extension", "png/../x", "works[0].image.extension"},
        {"/works/0/image/extension", "PNG", "works[0].image.extension"},
        {"/works/0/image/extension", "", "works[0].image.extension"},
        {"/works/0/image/extension", "abcdefghi", "works[0].image.extension"}, // 9 characters
        {"/works/0/image/sha256", "abc", "works[0].image.sha256"},
        {"/works/0/image/sha256",
         "9EFD8C22D4E910F296919762E790DE1C0B225E41356C2B99653BD55A21C44F0E",
         "works[0].image.sha256"},
        {"/works/0/image/sha256",
         "../../9762e790de1c0b225e41356c2b99653bd55a21c44f0e0000000000000000",
         "works[0].image.sha256"},
        {"/works/0/source/imageSha256", "", "works[0].source.imageSha256"},
        {"/entities/3/framing/style", "Gilt Classic", "entities[3].framing.style"},
        {"/entities/3/framing/style", "", "entities[3].framing.style"},
        {"/entities/3/framing/style", "-gilt", "entities[3].framing.style"},
        {"/entities/3/framing/style", std::string(129, 'a'), "entities[3].framing.style"},
        {"/entities/0/wall/finishA", "../wall-white", "entities[0].wall.finishA"},
        {"/works/0/material/preset", "oil/canvas", "works[0].material.preset"},
        {"/library/fixtureTypes/1/id", "Generic Wash", "library.fixtureTypes[1].id"},
        {"/works/0/sensitivity/responsivity", "extreme", "works[0].sensitivity.responsivity"},
        {"/entities/3/placement/kind", "onShelf", "entities[3].placement.kind"},
        {"/entities/3/placement/face", "a", "entities[3].placement.face"},
        {"/entities/3/framing/glazing", "anti_reflective_glass", "entities[3].framing.glazing"},
        {"/entities/4/opening/kind", "Door", "entities[4].opening.kind"},
        {"/entities/9/luminaire/aim/kind", "target", "entities[9].luminaire.aim.kind"},
        {"/works/0/form/kind", "object", "works[0].form.kind"}, // arrives after 1.0
        {"/works/0/source/kind", "loan", "works[0].source.kind"},
        {"/settings/displayUnits", "metres", "settings.displayUnits"},
    });

    // A library id at its longest is fine.
    Json document = fixture_json();
    entity(document, ix::harbour)["framing"]["style"] = std::string(128, 'a');
    CHECK_NOTHROW(parse_project_json(text_of(document)));
}

TEST_CASE("text that is not JSON is rejected with a line instead of a path", "[doc][project_io]") {
    SECTION("a missing comma, on a known line") {
        std::string text = fixture_text();
        const std::string needle = "\"label\": \"A\"";
        const std::size_t at = text.find(needle);
        REQUIRE(at != std::string::npos);
        text.insert(at + needle.size(), " \"B\"");
        const std::ptrdiff_t line =
            1 + std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n');

        const std::optional<ProjectError> error = rejection(text);
        REQUIRE(error.has_value());
        CHECK(error->json_path().empty());
        const std::string what = error->what();
        CHECK_THAT(what, StartsWith("project.json: "));
        CHECK_THAT(what, ContainsSubstring(std::to_string(line)));
    }
    SECTION("other broken text") {
        const std::string& fixture = fixture_text();
        std::string invalid_utf8 = fixture;
        const std::size_t name = invalid_utf8.find("H\xc3\xa9l");
        REQUIRE(name != std::string::npos);
        invalid_utf8[name + 1] = '\xff';

        std::string nan_number = fixture;
        const std::string height = "\"height_m\": 3.5";
        nan_number.replace(nan_number.find(height), height.size(), "\"height_m\": NaN");
        std::string huge_number = fixture;
        huge_number.replace(huge_number.find(height), height.size(), "\"height_m\": 1e999");

        const std::vector<std::string> texts = {
            "",
            "\n",
            "{",
            "{\"schemaVersion\": 1,}",
            "// a comment\n" + fixture,
            fixture.substr(0, fixture.size() / 2),
            fixture + "{}",
            std::string("{\0}", 3),
            invalid_utf8,
            nan_number,
            huge_number, // 1e999 overflows a double: not a finite number
        };
        for (std::size_t i = 0; i < texts.size(); ++i) {
            CAPTURE(i);
            CHECK(rejection(texts[i]).has_value());
        }
    }
}

TEST_CASE("JSON nested deeper than max_json_depth is rejected without exhausting the stack",
          "[doc][project_io]") {
    // 10^5 nested arrays would overflow the stack of a recursive reader.
    constexpr std::size_t deep = 100'000;
    CHECK(rejection(std::string(deep, '[') + std::string(deep, ']')).has_value());
    CHECK(rejection(std::string(deep, '{') + std::string(deep, '}')).has_value());

    // A valid file with one value nested past the limit.
    Json nested = "x";
    for (std::size_t i = 0; i < max_json_depth + 1; ++i) {
        nested = Json::array({nested});
    }
    Json document = fixture_json();
    entity(document, ix::wall_a)["name"] = nested;
    CHECK(rejection(text_of(document)).has_value());
}

// ---- Folders ---------------------------------------------------------------------------------

TEST_CASE("load_project_folder names the file and the JSON path in its errors",
          "[doc][project_io]") {
    const TempFolder folder("load_dangling_reference");
    Json document = fixture_json();
    entity(document, ix::harbour)["placement"]["wall"] = missing_id;
    write_file(folder.path() / "project.json", text_of(document));

    try {
        (void)load_project_folder(folder.path());
        FAIL("an invalid project loaded");
    } catch (const ProjectError& error) {
        CHECK(error.json_path() == "entities[3].placement.wall");
        const std::string what = error.what();
        CHECK_THAT(what, ContainsSubstring("project.json"));
        CHECK_THAT(what, ContainsSubstring(": entities[3].placement.wall: "));
    }
}

TEST_CASE("load_project_folder throws when there is no project.json to read", "[doc][project_io]") {
    const TempFolder folder("load_missing");
    CHECK_THROWS_AS(load_project_folder(folder.path()), std::runtime_error);
    std::filesystem::create_directories(folder.path());
    CHECK_THROWS_AS(load_project_folder(folder.path()), std::runtime_error);
}

TEST_CASE("load_project_folder rejects a project.json larger than max_project_json_bytes",
          "[doc][project_io]") {
    // Valid JSON padded with trailing spaces, so that only the size is wrong.
    const auto write_padded = [](const std::filesystem::path& file, std::size_t size) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        REQUIRE(out);
        const std::string& text = fixture_text();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        const std::string spaces(std::size_t{1} << 20, ' ');
        for (std::size_t written = text.size(); written < size;) {
            const std::size_t n = std::min(spaces.size(), size - written);
            out.write(spaces.data(), static_cast<std::streamsize>(n));
            written += n;
        }
        REQUIRE(out);
    };

    SECTION("one byte over the limit") {
        const TempFolder folder("load_too_large");
        write_padded(folder.path() / "project.json", max_project_json_bytes + 1);
        REQUIRE(std::filesystem::file_size(folder.path() / "project.json") ==
                max_project_json_bytes + 1);
        CHECK_THROWS_AS(load_project_folder(folder.path()), ProjectError);
    }
    SECTION("exactly at the limit") {
        // A file at the limit is read and parsed, not refused for its size. It starts with a byte
        // that is not JSON, so that the parser stops at once rather than reading 64 MiB.
        const TempFolder folder("load_at_size_limit");
        const std::filesystem::path file = folder.path() / "project.json";
        std::filesystem::create_directories(folder.path());
        {
            std::ofstream out(file, std::ios::binary);
            const std::string chunk(std::size_t{1} << 20, ' ');
            out.put('x');
            for (std::size_t written = 1; written < max_project_json_bytes;) {
                const std::size_t n = std::min(chunk.size(), max_project_json_bytes - written);
                out.write(chunk.data(), static_cast<std::streamsize>(n));
                written += n;
            }
            REQUIRE(out);
        }
        REQUIRE(std::filesystem::file_size(file) == max_project_json_bytes);
        std::string what;
        try {
            (void)load_project_folder(folder.path());
        } catch (const ProjectError& error) {
            what = error.what();
        }
        CHECK_THAT(what, ContainsSubstring("invalid JSON"));
        CHECK_THAT(what, !ContainsSubstring("larger than"));
    }
}

TEST_CASE("save_project_folder creates the folder, replaces project.json and leaves other "
          "files alone",
          "[doc][project_io]") {
    const TempFolder folder("save_into_folder");
    const std::filesystem::path& project = folder.path(); // does not exist yet

    save_project_folder(Document{}, project);
    REQUIRE(std::filesystem::exists(project / "project.json"));
    CHECK(parse_project_json(read_file(project / "project.json")) == Document{});

    write_file(project / "assets" / "notes.txt", "keep me");
    write_file(project / "README.txt", "keep me too");
    save_project_folder(load_project_folder(fixture_folder()), project);

    CHECK(difference(read_file(project / "project.json"), fixture_text()).empty());
    CHECK(read_file(project / "assets" / "notes.txt") == "keep me");
    CHECK(read_file(project / "README.txt") == "keep me too");
    CHECK_FALSE(std::filesystem::exists(project / "project.json.tmp"));
}

TEST_CASE("save_project_folder of a document with a dangling reference throws and writes nothing",
          "[doc][project_io]") {
    DocumentData data = every_component_data();
    std::get<OnWall>(*data.entities.at(entity_id(ix::harbour)).placement).wall = uuid(missing_id);
    const Document dangling(data);

    // to_project_json writes without checking.
    CHECK_NOTHROW(to_project_json(dangling));

    SECTION("into a new folder") {
        const TempFolder folder("save_dangling_new");
        CHECK_THROWS_AS(save_project_folder(dangling, folder.path()), ProjectError);
        CHECK_FALSE(std::filesystem::exists(folder.path() / "project.json"));
        CHECK_FALSE(std::filesystem::exists(folder.path() / "project.json.tmp"));
    }
    SECTION("over a saved project, which stays whole") {
        const TempFolder folder("save_dangling_existing");
        write_file(folder.path() / "project.json", fixture_text());
        CHECK_THROWS_AS(save_project_folder(dangling, folder.path()), ProjectError);
        CHECK(read_file(folder.path() / "project.json") == fixture_text());
        CHECK_FALSE(std::filesystem::exists(folder.path() / "project.json.tmp"));
    }
}

TEST_CASE("save_project_folder of a document with a number that is not finite throws and "
          "writes nothing",
          "[doc][project_io]") {
    // JSON has no NaN or infinity, so such a document would not read back.
    DocumentData nan_setting = every_component_data();
    nan_setting.settings.centre_line_m = std::numeric_limits<double>::quiet_NaN();
    DocumentData infinite_wall = every_component_data();
    infinite_wall.entities.at(entity_id(ix::wall_a)).wall->height_m =
        std::numeric_limits<double>::infinity();

    const TempFolder folder("save_not_finite");
    write_file(folder.path() / "project.json", fixture_text());
    CHECK_THROWS_AS(save_project_folder(Document(nan_setting), folder.path()), ProjectError);
    CHECK_THROWS_AS(save_project_folder(Document(infinite_wall), folder.path()), ProjectError);
    CHECK(read_file(folder.path() / "project.json") == fixture_text());
    CHECK_FALSE(std::filesystem::exists(folder.path() / "project.json.tmp"));
}
