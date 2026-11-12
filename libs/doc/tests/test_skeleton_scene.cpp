// SPDX-License-Identifier: Apache-2.0

// The skeleton test scene, tests/scenes/skeleton/ (M1-32): the room, the framed test chart, the
// spot and the viewpoint most of milestone 1 is tested on. It loads and saves back byte for byte,
// and it holds the numbers the task gives, which later tasks (geom, world, cs-render, the editor
// commands) rely on.

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/library.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/work.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

using namespace cs::doc;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double degrees = std::numbers::pi / 180.0;

std::filesystem::path scene_folder() {
    return std::filesystem::path(CS_SOURCE_DIR "/tests/scenes/skeleton");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Empty when the texts are equal; otherwise where they first differ, for the failure message.
std::string first_difference(std::string_view actual, std::string_view expected) {
    const auto [a, e] =
        std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
    if (a == actual.end() && e == expected.end()) {
        return {};
    }
    const auto at = static_cast<std::size_t>(a - actual.begin());
    const auto line = std::count(actual.begin(), a, '\n') + 1;
    return "first difference at byte " + std::to_string(at) + ", line " + std::to_string(line) +
           ": saved \"" + std::string(actual.substr(at, 40)) + "\", committed \"" +
           std::string(expected.substr(at, 40)) + "\"";
}

// The SHA-256 of `data` in lowercase hexadecimal (FIPS 180-4), the name ADR 0007 gives an asset.
// The test's own, since no module offers one yet; one small file a test is all it hashes.
std::string sha256_hex(std::string_view data) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    };
    std::array<std::uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // Padding: a 1 bit, zeros to 56 bytes modulo 64, then the length in bits, big-endian.
    std::string message(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<char>((bits >> shift) & 0xff));
    }

    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t t = 0; t < 16; ++t) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                w[t] = (w[t] << 8) | static_cast<unsigned char>(message[block + 4 * t + byte]);
            }
        }
        for (std::size_t t = 16; t < 64; ++t) {
            const std::uint32_t s0 =
                std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 =
                std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        std::array<std::uint32_t, 8> v = hash; // a, b, c, d, e, f, g, h
        for (std::size_t t = 0; t < 64; ++t) {
            const std::uint32_t s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
            const std::uint32_t choice = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + s1 + choice + k[t] + w[t];
            const std::uint32_t s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
            const std::uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            v = {t1 + s0 + majority, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) {
            hash[i] += v[i];
        }
    }

    static constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    for (const std::uint32_t word : hash) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(digits[(word >> shift) & 0xf]);
        }
    }
    return hex;
}

// A folder of its own in the working directory, which CTest sets to the test's build folder, so
// that two presets or two worktrees testing at once do not share it. Removed before and after.
class TempFolder {
  public:
    explicit TempFolder(std::string_view name)
        : path_(std::filesystem::current_path() / "skeleton_scene_scratch" / name) {
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

const Document& skeleton() {
    static const Document document = load_project_folder(scene_folder());
    return document;
}

// The ids of the skeleton's entities that hold a component of this kind, in id order.
std::vector<Uuid> entities_with(ComponentKind kind) {
    std::vector<Uuid> ids;
    for (const auto& [id, entity] : skeleton().entities()) {
        if (entity.has(kind)) {
            ids.push_back(id);
        }
    }
    return ids;
}

const Entity& only_entity_with(ComponentKind kind) {
    const std::vector<Uuid> ids = entities_with(kind);
    REQUIRE(ids.size() == 1);
    return *skeleton().find_entity(ids.front());
}

// ---- Plan geometry ---------------------------------------------------------------------------

Vec2 operator+(Vec2 a, Vec2 b) {
    return {a.x + b.x, a.y + b.y};
}
Vec2 operator-(Vec2 a, Vec2 b) {
    return {a.x - b.x, a.y - b.y};
}
Vec2 operator*(Vec2 a, double s) {
    return {a.x * s, a.y * s};
}
double dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}
double length(Vec2 a) {
    return std::hypot(a.x, a.y);
}

// Face A of a wall, in plan: the segment on the left of the line from start to end, half the
// thickness away from it, and the unit normal it faces along.
struct Face {
    Vec2 start;
    Vec2 end;
    Vec2 normal;

    Vec2 direction() const { return (end - start) * (1.0 / length(end - start)); }
    // Signed distance of a point in front of the face.
    double distance_to(Vec2 point) const { return dot(point - start, normal); }
};

Face face_a(const Wall& wall) {
    const Vec2 along = (wall.end_m - wall.start_m) * (1.0 / length(wall.end_m - wall.start_m));
    const Vec2 left{-along.y, along.x};
    const Vec2 offset = left * (wall.thickness_m / 2.0);
    return {wall.start_m + offset, wall.end_m + offset, left};
}

std::pair<Uuid, const Wall*> wall_labelled(std::string_view label) {
    for (const Uuid& id : entities_with(ComponentKind::wall)) {
        const Wall& wall = *skeleton().find_entity(id)->wall;
        if (wall.label == label) {
            return {id, &wall};
        }
    }
    FAIL("no wall labelled " << label);
    return {};
}

void check_near(Vec2 actual, Vec2 expected, double tolerance) {
    CHECK_THAT(actual.x, WithinAbs(expected.x, tolerance));
    CHECK_THAT(actual.y, WithinAbs(expected.y, tolerance));
}

// The PNG's width and height from its IHDR chunk, which the PNG specification puts first.
std::pair<std::uint32_t, std::uint32_t> png_size(const std::string& png) {
    REQUIRE(png.size() >= 24);
    REQUIRE(png.substr(0, 8) == std::string("\x89PNG\r\n\x1a\n", 8));
    REQUIRE(png.substr(12, 4) == "IHDR");
    const auto big_endian = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value = (value << 8) | static_cast<unsigned char>(png[at + i]);
        }
        return value;
    };
    return {big_endian(16), big_endian(20)};
}

} // namespace

TEST_CASE("sha256_hex gives the FIPS 180-4 example digests", "[doc][skeleton]") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Two blocks once padded.
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("the skeleton scene's assets folder holds only the files the document names",
          "[doc][skeleton]") {
    // So that an old chart left behind after a change cannot linger beside the new one.
    std::set<std::string> named;
    for (const auto& [id, work] : skeleton().works()) {
        if (work.image) {
            named.insert(work.image->sha256 + "." + work.image->extension);
        }
    }
    std::set<std::string> present;
    for (const auto& file : std::filesystem::directory_iterator(scene_folder() / "assets")) {
        present.insert(file.path().filename().string());
    }
    CHECK(present == named);
}

TEST_CASE("the skeleton scene loads with load_project_folder and saves back byte-identical",
          "[doc][skeleton]") {
    const Document document = load_project_folder(scene_folder());

    const TempFolder folder("skeleton_round_trip");
    save_project_folder(document, folder.path());
    // Empty when equal; otherwise the message shows where the saved text first differs.
    CHECK(first_difference(read_file(folder.path() / "project.json"),
                           read_file(scene_folder() / "project.json")) == "");
    CHECK(load_project_folder(folder.path()) == document);
}

TEST_CASE("the skeleton scene's room is 6 x 4 x 3.5 m, closed by walls A to D whose faces A are "
          "its outline",
          "[doc][skeleton]") {
    const Room& room = *only_entity_with(ComponentKind::room).room;
    REQUIRE(room.outline_m.size() == 4);

    // An axis-aligned rectangle 6 m along x and 4 m along y.
    double min_x = room.outline_m[0].x, max_x = min_x, min_y = room.outline_m[0].y, max_y = min_y;
    for (const Vec2& point : room.outline_m) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    CHECK(max_x - min_x == 6.0);
    CHECK(max_y - min_y == 4.0);
    for (const Vec2& point : room.outline_m) {
        CHECK((point.x == min_x || point.x == max_x));
        CHECK((point.y == min_y || point.y == max_y));
    }
    CHECK(room.floor_height_m == 0.0);
    CHECK(room.ceiling_height_m == 3.5);

    // Four walls, lettered A to D as the editor's new_room (M1-25) letters them.
    const std::vector<Uuid> walls = entities_with(ComponentKind::wall);
    std::set<std::string> labels;
    for (const Uuid& id : walls) {
        labels.insert(skeleton().find_entity(id)->wall->label);
    }
    CHECK(labels == std::set<std::string>{"A", "B", "C", "D"});

    // Each wall stands outside the room with its face A on one edge of the outline, facing in, so
    // that the room's surfaces and the faces works hang on are the same planes.
    const Vec2 centre{(min_x + max_x) / 2.0, (min_y + max_y) / 2.0};
    std::set<std::size_t> edges_covered;
    for (const Uuid& id : walls) {
        const Wall& wall = *skeleton().find_entity(id)->wall;
        INFO("wall " << wall.label);
        CHECK(wall.height_m == room.ceiling_height_m - room.floor_height_m);
        const Face face = face_a(wall);
        CHECK(face.distance_to(centre) > 0.0);
        bool on_an_edge = false;
        for (std::size_t i = 0; i < room.outline_m.size(); ++i) {
            const Vec2 a = room.outline_m[i];
            const Vec2 b = room.outline_m[(i + 1) % room.outline_m.size()];
            const auto near = [](Vec2 p, Vec2 q) { return length(p - q) < 1e-12; };
            if ((near(face.start, a) && near(face.end, b)) ||
                (near(face.start, b) && near(face.end, a))) {
                on_an_edge = true;
                edges_covered.insert(i);
            }
        }
        CHECK(on_an_edge);
    }
    CHECK(edges_covered.size() == 4);
}

TEST_CASE("the skeleton scene hangs one 0.73 x 0.92 m work showing the test chart on wall A at "
          "u = 3.0 m, centre height 1.50 m, stand-off 0.04 m, in a box frame",
          "[doc][skeleton]") {
    REQUIRE(skeleton().works().size() == 1);
    const auto& [work_id, work] = *skeleton().works().begin();
    CHECK(work.form.width_m == 0.73);
    CHECK(work.form.height_m == 0.92);

    // The chart is in the scene's assets, named by its hash, at most 50 KB, with the work's
    // proportions so that it is not stretched. (make_test_chart.py --check verifies that it is
    // the chart the script writes, pixel values included.)
    REQUIRE(work.image.has_value());
    CHECK(work.image->extension == "png");
    const std::filesystem::path chart =
        scene_folder() / "assets" / (work.image->sha256 + "." + work.image->extension);
    REQUIRE(std::filesystem::is_regular_file(chart));
    const std::string png = read_file(chart);
    CHECK(sha256_hex(png) == work.image->sha256);
    CHECK(png.size() <= 50'000);
    const auto [width_px, height_px] = png_size(png);
    CHECK_THAT(static_cast<double>(width_px) / height_px,
               WithinAbs(work.form.width_m / work.form.height_m, 1.0 / height_px));

    const Entity& exhibit = only_entity_with(ComponentKind::exhibit);
    CHECK(exhibit.exhibit->work == work_id);
    REQUIRE(exhibit.framing.has_value());
    CHECK(exhibit.framing->style == "box");
    // Bare, whatever the preset's default: the lux tests of milestone 1 expect no glazing.
    CHECK(exhibit.framing->glazing == Glazing::none);

    REQUIRE(exhibit.placement.has_value());
    REQUIRE(std::holds_alternative<OnWall>(*exhibit.placement));
    const OnWall& on_wall = std::get<OnWall>(*exhibit.placement);
    const auto [wall_a_id, wall_a] = wall_labelled("A");
    CHECK(on_wall.wall == wall_a_id);
    CHECK(on_wall.face == WallFace::a);
    CHECK(on_wall.distance_m == 3.0);
    CHECK(on_wall.centre_height_m == 1.5);
    CHECK(on_wall.stand_off_m == 0.04);

    // Centred on the wall, on the project's centre line.
    CHECK(on_wall.distance_m == length(wall_a->end_m - wall_a->start_m) / 2.0);
    CHECK(on_wall.centre_height_m == skeleton().settings().centre_line_m);
}

TEST_CASE("the skeleton scene's spot is a generic 800 lm spot on a ceiling track 1.097 m from "
          "wall A's face, aimed at the work 30 degrees from vertical",
          "[doc][skeleton]") {
    const Entity& spot = only_entity_with(ComponentKind::luminaire);
    const Luminaire& luminaire = *spot.luminaire;
    const FixtureType* type = skeleton().library().find_fixture_type(luminaire.fixture_type);
    REQUIRE(type != nullptr);
    CHECK(type->flux_lm == 800.0);
    // Full angles, written in radians; 1e-12 allows the last bit of a degree conversion.
    CHECK_THAT(type->beam_angle_rad, WithinAbs(15.0 * degrees, 1e-12));
    CHECK_THAT(type->field_angle_rad, WithinAbs(25.0 * degrees, 1e-12));
    CHECK(luminaire.dimming == 1.0);
    CHECK(type->colour_temperature_k == 3000.0);
    CHECK_FALSE(luminaire.colour_temperature_k.has_value());

    // On the ceiling, along wall A, at the Canadian Conservation Institute's distance from its
    // face (ADR 0006): (ceiling height - 1.6 m) x tan 30 degrees = 1.0970 m, written to the mm.
    const Room& room = *only_entity_with(ComponentKind::room).room;
    const Entity& track_entity = only_entity_with(ComponentKind::track);
    const Track& track = *track_entity.track;
    CHECK(track.height_m == room.ceiling_height_m);
    REQUIRE(track.points_m.size() == 2);
    const Face face = face_a(*wall_labelled("A").second);
    for (const Vec2& point : track.points_m) {
        CHECK_THAT(face.distance_to(point), WithinAbs(1.097, 1e-12));
    }
    const double cci_distance_m = (room.ceiling_height_m - 1.6) * std::tan(30.0 * degrees);
    CHECK_THAT(face.distance_to(track.points_m[0]), WithinAbs(cci_distance_m, 0.001));

    REQUIRE(spot.placement.has_value());
    REQUIRE(std::holds_alternative<OnTrack>(*spot.placement));
    const OnTrack& on_track = std::get<OnTrack>(*spot.placement);
    CHECK(skeleton().find_entity(on_track.track) == &track_entity);

    REQUIRE(std::holds_alternative<AimAtEntity>(luminaire.aim));
    const Uuid exhibit_id = entities_with(ComponentKind::exhibit).front();
    CHECK(std::get<AimAtEntity>(luminaire.aim).target == exhibit_id);

    // The aim, from the spot to the centre of the work's face (stand-off included, as for the
    // editor's add_spot_for, M1-25), is 30 degrees from vertical. The tolerance, 0.01 degree,
    // allows for the track distance and the slide being written to the millimetre: the exact
    // solution is 0.0013 degree away.
    const Vec2 along_track = track.points_m[1] - track.points_m[0];
    const Vec2 spot_plan =
        track.points_m[0] + along_track * (on_track.distance_m / length(along_track));
    const OnWall& on_wall = std::get<OnWall>(*only_entity_with(ComponentKind::exhibit).placement);
    const Vec2 work_plan =
        (face.start + face.end) * 0.5 + face.normal * on_wall.stand_off_m; // u is the middle
    const double horizontal_m = length(spot_plan - work_plan);
    const double drop_m = track.height_m - on_wall.centre_height_m;
    CHECK_THAT(std::atan2(horizontal_m, drop_m) / degrees, WithinAbs(30.0, 0.01));

    // Which is 0.465 m along the track from the point opposite the work (M1-25 checks its
    // add_spot_for against this number, within 1 mm).
    CHECK_THAT(std::abs(dot(spot_plan - work_plan, face.direction())), WithinAbs(0.465, 1e-12));
}

TEST_CASE("the skeleton scene's viewpoint is at eye height, 3 m from wall A, facing the work",
          "[doc][skeleton]") {
    const Viewpoint& viewpoint = *only_entity_with(ComponentKind::viewpoint).viewpoint;
    CHECK(viewpoint.position_m.z == skeleton().settings().eye_height_m);
    CHECK(viewpoint.position_m.z == 1.6);

    const Face face = face_a(*wall_labelled("A").second);
    const Vec2 plan{viewpoint.position_m.x, viewpoint.position_m.y};
    CHECK_THAT(face.distance_to(plan), WithinAbs(3.0, 1e-12));
    // Opposite the work, which hangs in the middle of the wall.
    check_near(plan, (face.start + face.end) * 0.5 + face.normal * 3.0, 1e-12);

    // Looking horizontally at the wall.
    check_near({std::cos(viewpoint.yaw_rad), std::sin(viewpoint.yaw_rad)}, face.normal * -1.0,
               1e-12);
    CHECK(viewpoint.pitch_rad == 0.0);
    CHECK(viewpoint.field_of_view_rad > 0.0);
    CHECK(viewpoint.field_of_view_rad < std::numbers::pi);
}
