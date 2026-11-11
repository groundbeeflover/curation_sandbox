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

// A reference to something else in the document, checked once everything is read.
enum class Target { work, entity, wall, room, track, fixture_type };

struct Ref {
    Target target;
};

// The file's schemaVersion, which must be schema_version.
struct SchemaVersionCheck {};

// ---- Spellings in the file ---------------------------------------------------------------------

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

// ---- The schema --------------------------------------------------------------------------------
// describe(v, s) calls v(key, member[, check]) for every member of s, in the order they are
// written. `s` is const when writing and not when reading.

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
}

void describe(auto& v, Is<Opening> auto& s) {
    v("wall", s.wall, Ref{Target::wall});
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
    v("wall", s.wall, Ref{Target::wall});
    v("face", s.face);
    v("distance_m", s.distance_m);
    v("centreHeight_m", s.centre_height_m);
    v("standOff_m", s.stand_off_m, non_negative);
}

void describe(auto& v, Is<OnFloor> auto& s) {
    v("room", s.room, Ref{Target::room});
    v("position_m", s.position_m);
    v("rotation_rad", s.rotation_rad);
}

void describe(auto& v, Is<OnHost> auto& s) {
    v("host", s.host, Ref{Target::entity});
    v("offset_m", s.offset_m);
    v("rotation_rad", s.rotation_rad);
}

void describe(auto& v, Is<OnTrack> auto& s) {
    v("track", s.track, Ref{Target::track});
    v("distance_m", s.distance_m, non_negative);
}

void describe(auto& v, Is<OnCeiling> auto& s) {
    v("room", s.room, Ref{Target::room});
    v("position_m", s.position_m);
}

void describe(auto& v, Is<Track> auto& s) {
    v("points_m", s.points_m, Count{2, max_points});
    v("height_m", s.height_m);
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

// The whole file. A map is written as an array of objects, each with "id" first; section() is a
// nested object with no type of its own.
void describe(auto& v, Is<DocumentData> auto& s) {
    int version = schema_version;
    v("schemaVersion", version, SchemaVersionCheck{});
    v("settings", s.settings);
    v.section("library", [&](auto& library) {
        library("fixtureTypes", s.fixture_types, Count{0, max_fixture_types});
    });
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

struct PendingRef {
    std::string path;
    Target target;
    Uuid id;              // for every target but fixture types
    LibraryId library_id; // for fixture types
};

// The state of one read: where it is in the file, for messages; the references to check once
// everything is read; and the entities in the order of the file.
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
        path_.push_back(Segment{key, 0});
        return Scope(*this);
    }

    [[nodiscard]] Scope enter(std::size_t index) {
        path_.push_back(Segment{{}, index});
        return Scope(*this);
    }

    std::string path() const {
        std::string text;
        for (const Segment& segment : path_) {
            if (segment.key.empty()) {
                text += '[' + std::to_string(segment.index) + ']';
            } else {
                if (!text.empty()) {
                    text += '.';
                }
                text += segment.key;
            }
        }
        return text;
    }

    void add_ref(Target target, const Uuid& id) { refs_.push_back({path(), target, id, {}}); }
    void add_ref(const LibraryId& id) { refs_.push_back({path(), Target::fixture_type, {}, id}); }
    const std::vector<PendingRef>& refs() const { return refs_; }

    std::vector<Uuid> entity_order; // entity ids in the order of the file

  private:
    struct Segment {
        std::string_view key; // empty for an array element
        std::size_t index;
    };

    std::string where_;
    std::vector<Segment> path_;
    std::vector<PendingRef> refs_;
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

template <std::size_t N>
[[noreturn]] void fail_one_of(Reader& r, const std::array<std::string_view, N>& names) {
    std::string problem = "expected one of ";
    for (std::size_t i = 0; i < N; ++i) {
        problem += (i == 0 ? "\"" : ", \"") + std::string(names[i]) + '"';
    }
    r.fail(problem);
}

template <class E> E read_enum(Reader& r, const Json& j) {
    const std::string& text = read_string(r, j);
    const auto& names = enum_names<E>;
    const auto it = std::find(names.begin(), names.end(), text);
    if (it == names.end()) {
        fail_one_of(r, names);
    }
    return static_cast<E>(it - names.begin());
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

    template <class F> void section(std::string_view key, F&& read_members) {
        const Json* value = find(key);
        const auto scope = r_.enter(key);
        if (!value) {
            r_.fail("missing");
        }
        ObjectReader inner(r_, *value);
        read_members(inner);
        inner.finish();
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
        if (used_.size() == j_.size()) {
            return;
        }
        for (auto it = j_.begin(); it != j_.end(); ++it) {
            if (std::find(used_.begin(), used_.end(), it.key()) == used_.end()) {
                const auto scope = r_.enter(it.key());
                r_.fail(std::string(unknown_));
            }
        }
    }

  private:
    Reader& r_;
    const Json& j_;
    std::string_view unknown_;
    std::vector<std::string_view> used_; // keys from describe(), which outlive the reader

    const Json* find(std::string_view key) {
        const auto it = j_.find(key);
        if (it == j_.end()) {
            return nullptr;
        }
        used_.push_back(key);
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
    const auto& kinds = variant_kinds<V>;
    const auto it = std::find(kinds.begin(), kinds.end(), kind);
    if (it == kinds.end()) {
        const auto scope = r.enter("kind");
        fail_one_of(r, kinds);
    }
    emplace_alternative(out, static_cast<std::size_t>(it - kinds.begin()),
                        std::make_index_sequence<std::variant_size_v<V>>{});
    std::visit([&](auto& alternative) { describe(object, alternative); }, out);
    object.finish();
}

// Rules between members of one object, checked once the object is read.

template <class T> void check_members(Reader&, const T&) {}

void check_members(Reader& r, const FixtureType& s) {
    if (s.field_angle_rad < s.beam_angle_rad) {
        const auto scope = r.enter("fieldAngle_rad");
        r.fail("must be at least the beam angle, " + number_text(s.beam_angle_rad));
    }
}

void check_members(Reader& r, const Room& s) {
    if (s.ceiling_height_m <= s.floor_height_m) {
        const auto scope = r.enter("ceilingHeight_m");
        r.fail("must be greater than floorHeight_m, " + number_text(s.floor_height_m));
    }
}

void check_members(Reader& r, const LuxSensor& s) {
    if (s.normal == Vec3{}) {
        const auto scope = r.enter("normal");
        r.fail("must not be zero");
    }
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
        check_members(r, value);
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
    } else if constexpr (std::is_same_v<T, int> && std::is_same_v<C, SchemaVersionCheck>) {
        const std::int64_t version = read_whole_number(r, j);
        if (version != schema_version) {
            r.fail("this build reads schema version " + std::to_string(schema_version) + ", not " +
                   std::to_string(version));
        }
        out = schema_version;
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
        if constexpr (std::is_same_v<C, Ref>) {
            r.add_ref(out);
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
        if constexpr (std::is_same_v<C, Ref>) {
            r.add_ref(check.target, out);
        }
    } else if constexpr (std::is_enum_v<T>) {
        out = read_enum<T>(r, j);
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
        check_members(r, out);
    }
}

std::string entity_path(const Reader& r, const Uuid& id, std::string_view rest) {
    const auto it = std::find(r.entity_order.begin(), r.entity_order.end(), id);
    return "entities[" + std::to_string(it - r.entity_order.begin()) + "]." + std::string(rest);
}

void check_references(const Reader& r, const DocumentData& data) {
    for (const PendingRef& ref : r.refs()) {
        if (ref.target == Target::fixture_type) {
            if (!data.fixture_types.contains(ref.library_id)) {
                r.fail_at(ref.path, "the library has no fixture type \"" + ref.library_id + '"');
            }
            continue;
        }
        if (ref.target == Target::work) {
            if (!data.works.contains(ref.id)) {
                r.fail_at(ref.path, "no work has the id " + ref.id.to_string());
            }
            continue;
        }
        const auto it = data.entities.find(ref.id);
        if (it == data.entities.end()) {
            r.fail_at(ref.path, "no entity has the id " + ref.id.to_string());
        }
        const auto require = [&](ComponentKind kind) {
            if (!it->second.has(kind)) {
                r.fail_at(ref.path, "the entity " + ref.id.to_string() + " has no " +
                                        std::string(component_key(kind)));
            }
        };
        if (ref.target == Target::wall) {
            require(ComponentKind::wall);
        } else if (ref.target == Target::room) {
            require(ComponentKind::room);
        } else if (ref.target == Target::track) {
            require(ComponentKind::track);
        }
    }
}

// Following OnHost placements from any entity must end; each entity is walked once.
void check_hosts(const Reader& r, const DocumentData& data) {
    enum class State { walking, done };
    std::map<Uuid, State> state;
    std::vector<Uuid> chain;
    for (const Uuid& start : r.entity_order) {
        chain.clear();
        Uuid id = start;
        while (true) {
            const auto [it, inserted] = state.try_emplace(id, State::walking);
            if (!inserted) {
                if (it->second == State::walking) {
                    r.fail_at(entity_path(r, id, "placement.host"),
                              "the placement's hosts form a cycle");
                }
                break;
            }
            chain.push_back(id);
            const Entity& entity = data.entities.at(id); // references are checked already
            const auto* on_host =
                entity.placement ? std::get_if<OnHost>(&*entity.placement) : nullptr;
            if (!on_host) {
                break;
            }
            id = on_host->host;
        }
        for (const Uuid& walked : chain) {
            state[walked] = State::done;
        }
    }
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

Json parse_json(const std::string& where, std::string_view text) {
    if (text.size() > max_project_json_bytes) {
        throw ProjectError(where, "",
                           "larger than " + std::to_string(max_project_json_bytes) + " bytes");
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
    DocumentData data;
    read_value(r, root, data, NoCheck{});
    check_references(r, data);
    check_hosts(r, data);
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

    template <class F> void section(std::string_view key, F&& write_members) {
        OrderedJson inner = OrderedJson::object();
        ObjectWriter writer(inner);
        write_members(writer);
        out_[std::string(key)] = std::move(inner);
    }

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
        throw ProjectError(path.string(), "",
                           "larger than " + std::to_string(max_project_json_bytes) + " bytes");
    }
    std::ifstream in(path, std::ios::binary);
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

Document parse_project_json(std::string_view text) {
    return Document(read_document("project.json", text));
}

std::string to_project_json(const Document& document) {
    try {
        return write_value(document.data()).dump(2) + '\n';
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
    const std::filesystem::path temporary = folder / "project.json.tmp";
    write_file(temporary, text);
    std::filesystem::rename(temporary, file);
}

} // namespace cs::doc
