// SPDX-License-Identifier: Apache-2.0

// project.json: the schema (one describe() per type, listing its members with their keys and
// checks), a strict reader that names the JSON path of every error, and the canonical writer.
// Reading and writing share the describe() functions, so they cannot disagree about a key.

#include <cs/doc/project_io.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cs::doc {

ProjectError::ProjectError(const std::string& where, const std::string& json_path,
                           const std::string& problem)
    : std::runtime_error(where + ": " + (json_path.empty() ? "" : json_path + ": ") + problem),
      json_path_(json_path) {}

ReferenceError::ReferenceError(const Uuid& entity, const std::string& member,
                               const std::string& problem)
    : std::runtime_error("entity " + entity.to_string() + ": " + member + ": " + problem),
      entity_(entity), member_(member) {}

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json; // keeps keys in the order they are written

// ---- Checks on values, given with a member in describe() ---------------------------------------

constexpr double infinity = std::numeric_limits<double>::infinity();

struct NoCheck {};

// An interval of allowed numbers.
struct Range {
    double low;
    double high;
    bool low_open = false;
    bool high_open = false;
};

constexpr Range positive{0.0, infinity, true, false};
constexpr Range non_negative{0.0, infinity};
constexpr Range unit_interval{0.0, 1.0};
constexpr Range full_angle{0.0, 2.0 * std::numbers::pi, true, false};

// How many elements an array may have.
struct Count {
    std::size_t min;
    std::size_t max;
};

constexpr std::size_t max_points = 1000;

// A string that must be a LibraryId, a SHA-256 in hexadecimal, or an asset's file extension.
struct LibraryIdText {};
struct Sha256Text {};
struct ExtensionText {};

// A reference to something else in the document, checked once everything is read (and on every
// commit of a transaction) by check_references: a work, an entity (that has `component`, when one
// is given), or a library definition.
enum class Target { work, entity, fixture_type };

struct Ref {
    Target target;
    std::optional<ComponentKind> component = std::nullopt;
};

// ---- Spellings in the file ---------------------------------------------------------------------
// Each enumeration and variant used in the schema has its names here; the empty primary templates
// are refused by static_asserts where the names are used.

template <class E> constexpr std::array<std::string_view, 0> enum_names{};
template <> constexpr std::array<std::string_view, 2> enum_names<WallFace>{"A", "B"};
template <>
constexpr std::array<std::string_view, 3> enum_names<OpeningKind>{"door", "passage", "window"};
template <>
constexpr std::array<std::string_view, 7> enum_names<Glazing>{
    "none",          "floatGlass",          "lowIronGlass",
    "acrylic",       "antiReflectiveGlass", "antiReflectiveAcrylic",
    "nonGlareGlass",
};
template <>
constexpr std::array<std::string_view, 4> enum_names<Responsivity>{"irresponsive", "low", "medium",
                                                                   "high"};
template <>
constexpr std::array<std::string_view, 2> enum_names<DisplayUnits>{"centimetres", "inches"};

// The "kind" of each alternative of a variant, in the variant's order.
template <class V> constexpr std::array<std::string_view, 0> variant_kinds{};
template <>
constexpr std::array<std::string_view, 5> variant_kinds<Placement>{"onWall", "onFloor", "onHost",
                                                                   "onTrack", "onCeiling"};
template <> constexpr std::array<std::string_view, 2> variant_kinds<Aim>{"entity", "point"};
template <>
constexpr std::array<std::string_view, 3> variant_kinds<WorkSource>{"catalogue", "import",
                                                                    "checklist"};

// ---- Geometry rules ----------------------------------------------------------------------------
// Later code divides by these lengths, so a length whose square is zero (or underflows to zero)
// is refused: it has no direction.

double squared_length(const Vec2& v) {
    return v.x * v.x + v.y * v.y;
}

double squared_length(const Vec3& v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

bool has_direction(const Vec2& from, const Vec2& to) {
    return squared_length(Vec2{to.x - from.x, to.y - from.y}) > 0.0;
}

// Neighbouring points of a polyline differ, and so do the last and first of a closed one.
bool neighbours_differ(const std::vector<Vec2>& points, bool closed) {
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        if (!has_direction(points[i], points[i + 1])) {
            return false;
        }
    }
    return !closed || points.size() < 2 || has_direction(points.back(), points.front());
}

// ---- The schema --------------------------------------------------------------------------------
// describe(v, s) calls v(key, member[, check]) for every member of s, in the order they are
// written, then v.check(condition, key, problem) for each rule between members, which the reader
// enforces once the members are read and the writer ignores. `s` is const when writing and not
// when reading.

template <class S, class T>
concept Is = std::same_as<std::remove_const_t<S>, T>;

void describe(auto& v, Is<ProjectSettings> auto& s) {
    v("centreLine_m", s.centre_line_m, positive);
    v("eyeHeight_m", s.eye_height_m, positive);
    v("openingHoursPerYear", s.opening_hours_per_year, Range{0.0, 366.0 * 24.0});
    v("displayUnits", s.display_units);
}

void describe(auto& v, Is<FixtureType> auto& s) {
    v("name", s.name);
    v("flux_lm", s.flux_lm, positive);
    v("beamAngle_rad", s.beam_angle_rad, full_angle);
    v("fieldAngle_rad", s.field_angle_rad, full_angle);
    v("colourTemperature_K", s.colour_temperature_k, positive);
    v.check(s.field_angle_rad >= s.beam_angle_rad, "fieldAngle_rad",
            "must be at least beamAngle_rad");
}

void describe(auto& v, Is<Library> auto& s) {
    v("fixtureTypes", s.fixture_types, Count{0, max_fixture_types});
}

void describe(auto& v, Is<AssetRef> auto& s) {
    v("sha256", s.sha256, Sha256Text{});
    v("extension", s.extension, ExtensionText{});
}

void describe(auto& v, Is<FlatForm> auto& s) {
    v.kind("flat");
    v("width_m", s.width_m, positive);
    v("height_m", s.height_m, positive);
    v("depth_m", s.depth_m, positive);
}

void describe(auto& v, Is<WorkMaterial> auto& s) {
    v("preset", s.preset, LibraryIdText{});
    v("baseRoughness", s.base_roughness, unit_interval);
    v("coatWeight", s.coat_weight, unit_interval);
    v("coatRoughness", s.coat_roughness, unit_interval);
}

void describe(auto& v, Is<Sensitivity> auto& s) {
    v("responsivity", s.responsivity);
    v("maxLux", s.max_lux, non_negative);
    v("maxLuxHours", s.max_lux_hours, non_negative);
}

void describe(auto& v, Is<FromCatalogue> auto& s) {
    v("catalogueId", s.catalogue_id);
    v("imageSha256", s.image_sha256, Sha256Text{});
}

void describe(auto& v, Is<FromImport> auto& s) {
    v("originalName", s.original_name);
}

void describe(auto& v, Is<FromChecklist> auto& s) {
    v("row", s.row, Range{1.0, 1'000'000.0});
}

void describe(auto& v, Is<ExternalId> auto& s) {
    v("system", s.system);
    v("recordId", s.record_id);
}

void describe(auto& v, Is<Work> auto& s) {
    v("number", s.number);
    v("title", s.title);
    v("artist", s.artist);
    v("date", s.date);
    v("medium", s.medium);
    v("accessionNumber", s.accession_number);
    v("lender", s.lender);
    v("creditLine", s.credit_line);
    v("licence", s.licence);
    v("form", s.form);
    v("image", s.image);
    v("material", s.material);
    v("sensitivity", s.sensitivity);
    v("source", s.source);
    v("externalIds", s.external_ids, Count{0, 64});
}

void describe(auto& v, Is<Wall> auto& s) {
    v("start_m", s.start_m);
    v("end_m", s.end_m);
    v("height_m", s.height_m, positive);
    v("thickness_m", s.thickness_m, positive);
    v("finishA", s.finish_a, LibraryIdText{});
    v("finishB", s.finish_b, LibraryIdText{});
    v("label", s.label);
    v.check(has_direction(s.start_m, s.end_m), "end_m", "must differ from start_m");
}

void describe(auto& v, Is<Opening> auto& s) {
    v("wall", s.wall, Ref{Target::entity, ComponentKind::wall});
    v("offset_m", s.offset_m, non_negative);
    v("width_m", s.width_m, positive);
    v("height_m", s.height_m, positive);
    v("sill_m", s.sill_m, non_negative);
    v("kind", s.kind);
}

void describe(auto& v, Is<Room> auto& s) {
    v("outline_m", s.outline_m, Count{3, max_points});
    v("floorHeight_m", s.floor_height_m);
    v("ceilingHeight_m", s.ceiling_height_m);
    v("floorFinish", s.floor_finish, LibraryIdText{});
    v("ceilingFinish", s.ceiling_finish, LibraryIdText{});
    v.check(neighbours_differ(s.outline_m, true), "outline_m",
            "must not repeat a point next to itself, nor the first point at the end");
    v.check(s.ceiling_height_m > s.floor_height_m, "ceilingHeight_m",
            "must be greater than floorHeight_m");
}

void describe(auto& v, Is<Exhibit> auto& s) {
    v("work", s.work, Ref{Target::work});
}

void describe(auto& v, Is<Mat> auto& s) {
    v("border_m", s.border_m, positive);
    v("bottomExtra_m", s.bottom_extra_m, non_negative);
    v("overlap_m", s.overlap_m, non_negative);
    v("colour", s.colour);
}

void describe(auto& v, Is<Framing> auto& s) {
    v("style", s.style, LibraryIdText{});
    v("width_m", s.width_m, positive);
    v("colour", s.colour);
    v("mat", s.mat);
    v("glazing", s.glazing);
}

void describe(auto& v, Is<OnWall> auto& s) {
    v("wall", s.wall, Ref{Target::entity, ComponentKind::wall});
    v("face", s.face);
    v("distance_m", s.distance_m);
    v("centreHeight_m", s.centre_height_m);
    v("standOff_m", s.stand_off_m, non_negative);
}

void describe(auto& v, Is<OnFloor> auto& s) {
    v("room", s.room, Ref{Target::entity, ComponentKind::room});
    v("position_m", s.position_m);
    v("rotation_rad", s.rotation_rad);
}

void describe(auto& v, Is<OnHost> auto& s) {
    v("host", s.host, Ref{Target::entity});
    v("offset_m", s.offset_m);
    v("rotation_rad", s.rotation_rad);
}

void describe(auto& v, Is<OnTrack> auto& s) {
    v("track", s.track, Ref{Target::entity, ComponentKind::track});
    v("distance_m", s.distance_m, non_negative);
}

void describe(auto& v, Is<OnCeiling> auto& s) {
    v("room", s.room, Ref{Target::entity, ComponentKind::room});
    v("position_m", s.position_m);
}

void describe(auto& v, Is<Track> auto& s) {
    v("points_m", s.points_m, Count{2, max_points});
    v("height_m", s.height_m);
    v.check(neighbours_differ(s.points_m, false), "points_m",
            "must not repeat a point next to itself");
}

void describe(auto& v, Is<AimAtEntity> auto& s) {
    v("target", s.target, Ref{Target::entity});
}

void describe(auto& v, Is<AimAtPoint> auto& s) {
    v("point_m", s.point_m);
}

void describe(auto& v, Is<Luminaire> auto& s) {
    v("fixtureType", s.fixture_type, Ref{Target::fixture_type});
    v("dimming", s.dimming, unit_interval);
    v("aim", s.aim);
    v("colourTemperature_K", s.colour_temperature_k, positive);
}

void describe(auto& v, Is<LuxSensor> auto& s) {
    v("position_m", s.position_m);
    v("normal", s.normal);
    v.check(squared_length(s.normal) > 0.0, "normal", "must not be zero or nearly so");
}

void describe(auto& v, Is<Viewpoint> auto& s) {
    v("position_m", s.position_m);
    v("yaw_rad", s.yaw_rad);
    v("pitch_rad", s.pitch_rad, Range{-std::numbers::pi / 2.0, std::numbers::pi / 2.0});
    v("fieldOfView_rad", s.field_of_view_rad, Range{0.0, std::numbers::pi, true, true});
    v("ev100", s.ev100);
}

void describe(auto& v, Is<Anchor> auto& s) {
    v("entity", s.entity, Ref{Target::entity});
    v("point_m", s.point_m);
}

void describe(auto& v, Is<Dimension> auto& s) {
    v("from", s.from);
    v("to", s.to);
}

void describe(auto& v, Is<Entity> auto& s) {
    v("name", s.name);
    v(component_key(ComponentKind::wall), s.wall);
    v(component_key(ComponentKind::opening), s.opening);
    v(component_key(ComponentKind::room), s.room);
    v(component_key(ComponentKind::exhibit), s.exhibit);
    v(component_key(ComponentKind::framing), s.framing);
    v(component_key(ComponentKind::placement), s.placement);
    v(component_key(ComponentKind::track), s.track);
    v(component_key(ComponentKind::luminaire), s.luminaire);
    v(component_key(ComponentKind::lux_sensor), s.lux_sensor);
    v(component_key(ComponentKind::viewpoint), s.viewpoint);
    v(component_key(ComponentKind::dimension), s.dimension);
}

// The whole file, after its "schemaVersion" (read_document and write_document). A map is written
// as an array of objects, each with "id" first.
void describe(auto& v, Is<DocumentData> auto& s) {
    v("settings", s.settings);
    v("library", s.library);
    v("works", s.works, Count{0, max_works});
    v("entities", s.entities, Count{0, max_entities});
}

// ---- Type traits for the generic reader and writer ---------------------------------------------

template <class T> constexpr bool is_optional = false;
template <class T> constexpr bool is_optional<std::optional<T>> = true;

template <class T> constexpr bool is_vector = false;
template <class T> constexpr bool is_vector<std::vector<T>> = true;

template <class T> constexpr bool is_map = false;
template <class K, class T> constexpr bool is_map<std::map<K, T>> = true;

template <class T> constexpr bool is_variant = false;
template <class... T> constexpr bool is_variant<std::variant<T...>> = true;

// ---- Reading -----------------------------------------------------------------------------------

// The state of one read: where it is in the file, for messages; and the entities in the order of
// the file.
class Reader {
  public:
    explicit Reader(std::string where) : where_(std::move(where)) {}

    [[noreturn]] void fail(const std::string& problem) const { fail_at(path(), problem); }

    [[noreturn]] void fail_at(const std::string& path, const std::string& problem) const {
        throw ProjectError(where_, path, problem);
    }

    // Enters a member or an array element for the life of the returned object.
    class Scope {
      public:
        explicit Scope(Reader& r) : r_(r) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { r_.path_.pop_back(); }

      private:
        Reader& r_;
    };

    [[nodiscard]] Scope enter(std::string_view key) {
        path_.push_back(Segment{key, 0, false});
        return Scope(*this);
    }

    [[nodiscard]] Scope enter(std::size_t index) {
        path_.push_back(Segment{{}, index, true});
        return Scope(*this);
    }

    // Keys come from the file and may be long (an unknown key); messages show the first 64 bytes.
    std::string path() const {
        constexpr std::size_t shown = 64;
        std::string text;
        for (const Segment& segment : path_) {
            if (segment.is_index) {
                text += '[' + std::to_string(segment.index) + ']';
            } else {
                if (&segment != &path_.front()) {
                    text += '.';
                }
                text += segment.key.substr(0, shown);
                if (segment.key.size() > shown) {
                    text += "...";
                }
            }
        }
        return text;
    }

    std::vector<Uuid> entity_order; // entity ids in the order of the file

  private:
    struct Segment {
        std::string_view key; // a member's key, which may be empty
        std::size_t index;    // an array element's index
        bool is_index;
    };

    std::string where_;
    std::vector<Segment> path_;
};

// A number as the file writes it, for messages.
std::string number_text(double x) {
    return Json(x).dump();
}

double read_number(Reader& r, const Json& j) {
    if (!j.is_number()) {
        r.fail("expected a number");
    }
    const double x = j.get<double>();
    if (!std::isfinite(x)) {
        r.fail("expected a finite number");
    }
    return x;
}

void check_range(Reader& r, double x, const Range& range) {
    const bool low_ok = range.low_open ? x > range.low : x >= range.low;
    const bool high_ok = range.high_open ? x < range.high : x <= range.high;
    if (low_ok && high_ok) {
        return;
    }
    std::string problem = "must be ";
    problem += range.low_open ? "greater than " : "at least ";
    problem += number_text(range.low);
    if (range.high != infinity) {
        problem += range.high_open ? " and less than " : " and at most ";
        problem += number_text(range.high);
    }
    r.fail(problem + ", not " + number_text(x));
}

std::int64_t read_whole_number(Reader& r, const Json& j) {
    if (!j.is_number_integer()) {
        r.fail("expected a whole number");
    }
    if (j.is_number_unsigned() && j.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
        r.fail("the number is too large");
    }
    return j.get<std::int64_t>();
}

const std::string& read_string(Reader& r, const Json& j) {
    if (!j.is_string()) {
        r.fail("expected a string");
    }
    const auto& text = j.get_ref<const std::string&>();
    if (text.size() > max_string_bytes) {
        r.fail("longer than " + std::to_string(max_string_bytes) + " bytes");
    }
    return text;
}

bool is_lower_or_digit(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool is_library_id(std::string_view text) {
    return !text.empty() && text.size() <= 128 && is_lower_or_digit(text.front()) &&
           std::all_of(text.begin(), text.end(), [](char c) {
               return is_lower_or_digit(c) || c == '.' || c == '_' || c == '-';
           });
}

bool is_sha256(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool is_extension(std::string_view text) {
    return !text.empty() && text.size() <= 8 &&
           std::all_of(text.begin(), text.end(), is_lower_or_digit);
}

Uuid read_uuid(Reader& r, const Json& j) {
    if (!j.is_string()) {
        r.fail("expected a UUID");
    }
    const auto id = Uuid::parse(j.get_ref<const std::string&>());
    if (!id || id->is_nil()) {
        r.fail("expected a UUID such as \"0190f2a4-5b6c-7d8e-9f01-23456789abcd\"");
    }
    return *id;
}

// The position of `name` in `names`; fails, listing them, when it is not there.
template <std::size_t N>
std::size_t index_of(Reader& r, const std::array<std::string_view, N>& names,
                     std::string_view name) {
    static_assert(N > 0, "every enumeration and variant has its names");
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) {
        std::string problem = "expected one of ";
        for (std::size_t i = 0; i < N; ++i) {
            problem += (i == 0 ? "\"" : ", \"") + std::string(names[i]) + '"';
        }
        r.fail(problem);
    }
    return static_cast<std::size_t>(it - names.begin());
}

template <std::size_t N> std::array<double, N> read_numbers(Reader& r, const Json& j) {
    if (!j.is_array() || j.size() != N) {
        r.fail("expected an array of " + std::to_string(N) + " numbers");
    }
    std::array<double, N> numbers{};
    for (std::size_t i = 0; i < N; ++i) {
        const auto scope = r.enter(i);
        numbers[i] = read_number(r, j[i]);
    }
    return numbers;
}

template <class T, class C> void read_value(Reader& r, const Json& j, T& out, const C& check);

// Reads the members of one JSON object through describe(); finish() then fails on any key that
// describe() did not name.
class ObjectReader {
  public:
    ObjectReader(Reader& r, const Json& j, std::string_view unknown = "unknown field")
        : r_(r), j_(j), unknown_(unknown) {
        if (!j.is_object()) {
            r.fail("expected an object");
        }
    }

    template <class T, class C = NoCheck>
    void operator()(std::string_view key, T& member, const C& check = {}) {
        const Json* value = find(key);
        const auto scope = r_.enter(key);
        if (value) {
            read_value(r_, *value, member, check);
        } else if constexpr (!is_optional<T>) {
            r_.fail("missing");
        }
    }

    // A rule between members, called by describe() after the members it compares.
    void check(bool holds, std::string_view key, std::string_view problem) {
        if (!holds) {
            const auto scope = r_.enter(key);
            r_.fail(std::string(problem));
        }
    }

    // The "kind" of a variant's object.
    std::string_view read_kind() {
        const Json* value = find("kind");
        const auto scope = r_.enter("kind");
        if (!value) {
            r_.fail("missing");
        }
        return read_string(r_, *value);
    }

    // The "kind" of an object that has only one kind so far.
    void kind(std::string_view expected) {
        if (read_kind() != expected) {
            const auto scope = r_.enter("kind");
            r_.fail("expected \"" + std::string(expected) + '"');
        }
    }

    void finish() const {
        if (used_count_ == j_.size()) {
            return;
        }
        const auto used_end = used_.begin() + static_cast<std::ptrdiff_t>(used_count_);
        for (auto it = j_.begin(); it != j_.end(); ++it) {
            if (std::find(used_.begin(), used_end, it.key()) == used_end) {
                const auto scope = r_.enter(it.key());
                r_.fail(std::string(unknown_));
            }
        }
    }

  private:
    Reader& r_;
    const Json& j_;
    std::string_view unknown_;
    // The keys describe() found, which outlive the reader; a fixed array, because one is made for
    // every object of the file.
    std::array<std::string_view, 32> used_;
    std::size_t used_count_ = 0;

    const Json* find(std::string_view key) {
        const auto it = j_.find(key);
        if (it == j_.end()) {
            return nullptr;
        }
        if (used_count_ == used_.size()) {
            throw std::logic_error("describe() names more keys than ObjectReader holds");
        }
        used_[used_count_++] = key;
        return &*it;
    }
};

template <class V, std::size_t... I>
void emplace_alternative(V& v, std::size_t index, std::index_sequence<I...>) {
    ((index == I ? (void)v.template emplace<I>() : (void)0), ...);
}

template <class V> void read_variant(Reader& r, const Json& j, V& out) {
    ObjectReader object(r, j);
    const std::string_view kind = object.read_kind();
    std::size_t index = 0;
    {
        const auto scope = r.enter("kind");
        index = index_of(r, variant_kinds<V>, kind);
    }
    emplace_alternative(out, index, std::make_index_sequence<std::variant_size_v<V>>{});
    std::visit([&](auto& alternative) { describe(object, alternative); }, out);
    object.finish();
}

// A map is an array of objects, each with an "id" and the members of the mapped type.
template <class Key, class T> void read_map(Reader& r, const Json& j, std::map<Key, T>& out) {
    for (std::size_t i = 0; i < j.size(); ++i) {
        const auto scope = r.enter(i);
        ObjectReader object(r, j[i],
                            std::is_same_v<T, Entity> ? "unknown component" : "unknown field");
        Key id{};
        if constexpr (std::is_same_v<Key, Uuid>) {
            object("id", id);
        } else {
            object("id", id, LibraryIdText{});
        }
        T value{};
        describe(object, value);
        object.finish();
        if (!out.emplace(id, std::move(value)).second) {
            const auto id_scope = r.enter("id");
            r.fail("another element has the same id");
        }
        if constexpr (std::is_same_v<T, Entity>) {
            r.entity_order.push_back(id);
        }
    }
}

template <class T, class C> void read_value(Reader& r, const Json& j, T& out, const C& check) {
    if constexpr (is_optional<T>) {
        read_value(r, j, out.emplace(), check);
    } else if constexpr (std::is_same_v<T, double>) {
        out = read_number(r, j);
        if constexpr (std::is_same_v<C, Range>) {
            check_range(r, out, check);
        }
    } else if constexpr (std::is_same_v<T, int>) {
        static_assert(std::is_same_v<C, Range>, "every whole number has a range");
        const std::int64_t x = read_whole_number(r, j);
        check_range(r, static_cast<double>(x), check);
        out = static_cast<int>(x);
    } else if constexpr (std::is_same_v<T, std::string>) {
        out = read_string(r, j);
        if constexpr (std::is_same_v<C, LibraryIdText> || std::is_same_v<C, Ref>) {
            if (!is_library_id(out)) {
                r.fail("expected a library id: 1 to 128 characters from a-z, 0-9, '.', '_' and "
                       "'-', starting with a letter or digit");
            }
        }
        if constexpr (std::is_same_v<C, Sha256Text>) {
            if (!is_sha256(out)) {
                r.fail("expected a SHA-256 hash: 64 lowercase hexadecimal digits");
            }
        }
        if constexpr (std::is_same_v<C, ExtensionText>) {
            if (!is_extension(out)) {
                r.fail("expected a file extension: 1 to 8 characters from a-z and 0-9");
            }
        }
    } else if constexpr (std::is_same_v<T, Uuid>) {
        out = read_uuid(r, j);
    } else if constexpr (std::is_enum_v<T>) {
        out = static_cast<T>(index_of(r, enum_names<T>, read_string(r, j)));
    } else if constexpr (std::is_same_v<T, Vec2>) {
        const auto n = read_numbers<2>(r, j);
        out = {n[0], n[1]};
    } else if constexpr (std::is_same_v<T, Vec3>) {
        const auto n = read_numbers<3>(r, j);
        out = {n[0], n[1], n[2]};
    } else if constexpr (std::is_same_v<T, LinearRgb>) {
        const auto n = read_numbers<3>(r, j);
        for (std::size_t i = 0; i < n.size(); ++i) {
            const auto scope = r.enter(i);
            check_range(r, n[i], non_negative);
        }
        out = {n[0], n[1], n[2]};
    } else if constexpr (is_vector<T> || is_map<T>) {
        static_assert(std::is_same_v<C, Count>, "every array has a count");
        if (!j.is_array()) {
            r.fail("expected an array");
        }
        if (j.size() < check.min || j.size() > check.max) {
            r.fail("expected " + std::to_string(check.min) + " to " + std::to_string(check.max) +
                   " elements, not " + std::to_string(j.size()));
        }
        if constexpr (is_map<T>) {
            read_map(r, j, out);
        } else {
            out.resize(j.size());
            for (std::size_t i = 0; i < j.size(); ++i) {
                const auto scope = r.enter(i);
                read_value(r, j[i], out[i], NoCheck{});
            }
        }
    } else if constexpr (is_variant<T>) {
        read_variant(r, j, out);
    } else {
        ObjectReader object(r, j);
        describe(object, out);
        object.finish();
    }
}

// ---- References --------------------------------------------------------------------------------
// The rules are the Ref checks describe() gives, walked over the values rather than the file, so
// that reading and check_references (on every commit) share them.

struct ReferenceProblem {
    Uuid entity;
    std::string member; // as in ReferenceError::member()
    std::string problem;
};

// Walks one entity through describe() and stops at the first reference that does not resolve.
class ReferenceFinder {
  public:
    ReferenceFinder(const DocumentData& data, const Uuid& entity) : data_(data), entity_(entity) {}

    template <class T, class C = NoCheck>
    void operator()(std::string_view key, const T& member, const C& check = {}) {
        if (problem) {
            return;
        }
        path_.push_back(key);
        visit(member, check);
        path_.pop_back();
    }

    void check(bool, std::string_view, std::string_view) {}
    void kind(std::string_view) {}

    std::optional<ReferenceProblem> problem;

  private:
    const DocumentData& data_;
    Uuid entity_;
    std::vector<std::string_view> path_;

    template <class T, class C> void visit(const T& member, const C& check) {
        if constexpr (is_optional<T>) {
            if (member) {
                visit(*member, check);
            }
        } else if constexpr (std::is_same_v<C, Ref>) {
            resolve(check, member);
        } else if constexpr (is_variant<T>) {
            std::visit([&](const auto& alternative) { describe(*this, alternative); }, member);
        } else if constexpr (requires { describe(*this, member); }) {
            describe(*this, member);
        }
    }

    void resolve(const Ref& ref, const LibraryId& id) {
        if (ref.target == Target::fixture_type && !data_.library.find_fixture_type(id)) {
            fail("the library has no fixture type \"" + id + '"');
        }
    }

    void resolve(const Ref& ref, const Uuid& id) {
        if (ref.target == Target::work) {
            if (!data_.works.contains(id)) {
                fail("no work has the id " + id.to_string());
            }
            return;
        }
        const auto it = data_.entities.find(id);
        if (it == data_.entities.end()) {
            fail("no entity has the id " + id.to_string());
        } else if (ref.component && !it->second.has(*ref.component)) {
            fail("the entity " + id.to_string() + " has no " +
                 std::string(component_key(*ref.component)));
        }
    }

    void fail(std::string text) {
        std::string member;
        for (const std::string_view key : path_) {
            member += (member.empty() ? "" : ".") + std::string(key);
        }
        problem = ReferenceProblem{entity_, std::move(member), std::move(text)};
    }
};

// The first reference of `data` that does not resolve, taking entities in `order` (every id of
// data.entities, once each); then the first cycle of OnHost placements.
std::optional<ReferenceProblem> find_reference_problem(const DocumentData& data,
                                                       const std::vector<Uuid>& order) {
    for (const Uuid& id : order) {
        ReferenceFinder finder(data, id);
        describe(finder, data.entities.at(id));
        if (finder.problem) {
            return finder.problem;
        }
    }
    // Following OnHost placements from any entity must end. Only entities placed on a host are
    // walked, each once.
    const auto host_of = [&](const Uuid& id) -> const Uuid* {
        const Entity& entity = data.entities.at(id); // references are checked already
        const auto* on_host = entity.placement ? std::get_if<OnHost>(&*entity.placement) : nullptr;
        return on_host ? &on_host->host : nullptr;
    };
    enum class State { walking, done };
    std::map<Uuid, State> state;
    std::vector<Uuid> chain;
    for (const Uuid& start : order) {
        chain.clear();
        for (Uuid id = start; const Uuid* host = host_of(id); id = *host) {
            const auto [it, inserted] = state.try_emplace(id, State::walking);
            if (!inserted) {
                if (it->second == State::walking) {
                    return ReferenceProblem{id, "placement.host",
                                            "the placement's hosts form a cycle"};
                }
                break;
            }
            chain.push_back(id);
        }
        for (const Uuid& walked : chain) {
            state[walked] = State::done;
        }
    }
    return std::nullopt;
}

// Fails if arrays and objects nest deeper than max_json_depth, counting brackets outside strings.
// This runs before the parser, in one pass: nlohmann's own parser needs no limit for its stack,
// but its depth callback rescans each parent container whenever an object ends, which is
// quadratic in the length of an array of objects. Malformed text is left to the parser.
void check_depth(const std::string& where, std::string_view text) {
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max_json_depth) {
                throw ProjectError(
                    where, "", "nested deeper than " + std::to_string(max_json_depth) + " levels");
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
}

ProjectError too_large(const std::string& where) {
    return ProjectError(where, "",
                        "larger than " + std::to_string(max_project_json_bytes) + " bytes");
}

Json parse_json(const std::string& where, std::string_view text) {
    if (text.size() > max_project_json_bytes) {
        throw too_large(where);
    }
    check_depth(where, text);
    try {
        return Json::parse(text.begin(), text.end());
    } catch (const Json::exception& e) {
        // A syntax error, or a number too large for a double ("number overflow"). nlohmann's
        // message starts with "[json.exception.parse_error.101] ".
        std::string message = e.what();
        if (const auto end = message.find("] "); end != std::string::npos) {
            message.erase(0, end + 2);
        }
        throw ProjectError(where, "", "invalid JSON: " + message);
    }
}

DocumentData read_document(const std::string& where, std::string_view text) {
    const Json root = parse_json(where, text);
    Reader r(where);
    ObjectReader top(r, root);
    // The version comes first: nothing else is read from a file of another version. (Migrations
    // from older versions will run here, on the JSON, before the typed read.)
    int version = 0;
    top("schemaVersion", version,
        Range{std::numeric_limits<int>::min(), std::numeric_limits<int>::max()});
    if (version != schema_version) {
        const auto scope = r.enter("schemaVersion");
        r.fail("this build reads schema version " + std::to_string(schema_version) + ", not " +
               std::to_string(version));
    }
    DocumentData data;
    describe(top, data);
    top.finish();
    if (const auto problem = find_reference_problem(data, r.entity_order)) {
        const auto it = std::find(r.entity_order.begin(), r.entity_order.end(), problem->entity);
        r.fail_at("entities[" + std::to_string(it - r.entity_order.begin()) + "]." +
                      problem->member,
                  problem->problem);
    }
    return data;
}

// ---- Writing -----------------------------------------------------------------------------------

template <class T> OrderedJson write_value(const T& value);

// Writes the members describe() names, in its order, leaving out absent optional members.
class ObjectWriter {
  public:
    explicit ObjectWriter(OrderedJson& out) : out_(out) {}

    template <class T, class C = NoCheck>
    void operator()(std::string_view key, const T& member, const C& = {}) {
        if constexpr (is_optional<T>) {
            if (member) {
                out_[std::string(key)] = write_value(*member);
            }
        } else {
            out_[std::string(key)] = write_value(member);
        }
    }

    void check(bool, std::string_view, std::string_view) {}

    void kind(std::string_view kind) { out_["kind"] = std::string(kind); }

  private:
    OrderedJson& out_;
};

template <class T> OrderedJson write_value(const T& value) {
    if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int> ||
                  std::is_same_v<T, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<T, Uuid>) {
        return value.to_string();
    } else if constexpr (std::is_enum_v<T>) {
        static_assert(enum_names<T>.size() > 0, "every enumeration has its names");
        const auto index = static_cast<std::size_t>(value);
        // An out-of-range value writes "", which fails to read back, so save refuses it.
        return std::string(index < enum_names<T>.size() ? enum_names<T>[index]
                                                        : std::string_view{});
    } else if constexpr (std::is_same_v<T, Vec2>) {
        return OrderedJson::array({value.x, value.y});
    } else if constexpr (std::is_same_v<T, Vec3>) {
        return OrderedJson::array({value.x, value.y, value.z});
    } else if constexpr (std::is_same_v<T, LinearRgb>) {
        return OrderedJson::array({value.r, value.g, value.b});
    } else if constexpr (is_vector<T>) {
        OrderedJson array = OrderedJson::array();
        for (const auto& element : value) {
            array.push_back(write_value(element));
        }
        return array;
    } else if constexpr (is_map<T>) {
        OrderedJson array = OrderedJson::array();
        for (const auto& [id, element] : value) {
            OrderedJson object = OrderedJson::object();
            object["id"] = write_value(id);
            ObjectWriter writer(object);
            describe(writer, element);
            array.push_back(std::move(object));
        }
        return array;
    } else if constexpr (is_variant<T>) {
        static_assert(variant_kinds<T>.size() == std::variant_size_v<T>,
                      "every alternative has its kind");
        OrderedJson object = OrderedJson::object();
        object["kind"] = std::string(variant_kinds<T>[value.index()]);
        ObjectWriter writer(object);
        std::visit([&](const auto& alternative) { describe(writer, alternative); }, value);
        return object;
    } else {
        OrderedJson object = OrderedJson::object();
        ObjectWriter writer(object);
        describe(writer, value);
        return object;
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error("cannot read " + path.string() + ": " + error.message());
    }
    if (size > max_project_json_bytes) {
        throw too_large(path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in || in.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error("cannot read " + path.string() + ": it changed while reading");
    }
    return text;
}

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

} // namespace

void check_references(const DocumentData& data) {
    std::vector<Uuid> order;
    order.reserve(data.entities.size());
    for (const auto& [id, entity] : data.entities) {
        order.push_back(id);
    }
    if (const auto problem = find_reference_problem(data, order)) {
        throw ReferenceError(problem->entity, problem->member, problem->problem);
    }
}

Document parse_project_json(std::string_view text) {
    return Document(read_document("project.json", text));
}

std::string to_project_json(const Document& document) {
    OrderedJson root = OrderedJson::object();
    root["schemaVersion"] = schema_version;
    ObjectWriter writer(root);
    describe(writer, document.data());
    try {
        return root.dump(2) + '\n';
    } catch (const OrderedJson::type_error&) {
        throw ProjectError("project.json", "", "cannot write: a string is not valid UTF-8");
    }
}

Document load_project_folder(const std::filesystem::path& folder) {
    const std::filesystem::path file = folder / "project.json";
    return Document(read_document(file.string(), read_file(file)));
}

void save_project_folder(const Document& document, const std::filesystem::path& folder) {
    const std::filesystem::path file = folder / "project.json";
    const std::string text = to_project_json(document);
    // What is saved must open again: read it back before anything is written.
    read_document(file.string(), text);
    std::filesystem::create_directories(folder);
    // A project.json.tmp already there, left by a failed save or planted in a folder someone sent,
    // is removed first: remove() deletes a symbolic link itself, so the write below can never go
    // through a link to a file outside the folder.
    const std::filesystem::path temporary = folder / "project.json.tmp";
    std::filesystem::remove(temporary);
    try {
        write_file(temporary, text);
        std::filesystem::rename(temporary, file);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace cs::doc
