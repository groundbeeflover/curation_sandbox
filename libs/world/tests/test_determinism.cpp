// SPDX-License-Identifier: Apache-2.0

// The same document always compiles to the same world (#61): equal worlds from two compiles, in a
// fixed order by entity id; images named by asset hash and extension, never loaded; and the world's
// ids, which trace every result back to the document's entities.

#include "world_checks.hpp"

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/work.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace cs::world;
using namespace cs::world::test;

namespace doc = cs::doc;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Ids that sort before every id of the skeleton: byte 5 is 0xdb, the skeleton's 0xdc.
constexpr std::string_view early_exhibit_text = "01a1cd0d-dbff-7000-8000-000000000001";
constexpr std::string_view early_spot_text = "01a1cd0d-dbff-7000-8000-000000000002";
constexpr std::string_view early_viewpoint_text = "01a1cd0d-dbff-7000-8000-000000000003";
constexpr std::string_view early_work_text = "01a1cd0d-dbff-7000-8000-000000000004";

// The skeleton plus an unframed work on wall C, a second spot on the track and a second
// viewpoint, each with an id that sorts before the room's, so that a world listing entities by
// kind (rooms first) would be out of order.
doc::Document with_early_entities() {
    auto data = skeleton_data();

    doc::Work second_work = work(data, chart_work_text);
    second_work.number = "2";
    second_work.title = "Second";
    second_work.image.reset();
    data.works.emplace(uuid(early_work_text), second_work);

    doc::Entity exhibit;
    exhibit.name = "Second";
    exhibit.exhibit = doc::Exhibit{.work = uuid(early_work_text)};
    exhibit.placement = doc::OnWall{.wall = uuid(wall_c_text),
                                    .face = doc::WallFace::a,
                                    .distance_m = 3.0,
                                    .centre_height_m = 1.5,
                                    .stand_off_m = 0.04};
    data.entities.emplace(uuid(early_exhibit_text), exhibit);

    doc::Entity spot;
    spot.name = "Spot 0";
    spot.placement = doc::OnTrack{.track = uuid(track_text), .distance_m = 0.5};
    spot.luminaire = doc::Luminaire{.fixture_type = "generic-spot-800",
                                    .dimming = 1.0,
                                    .aim = doc::AimAtPoint{.point_m = {1.5, 0.0, 1.5}},
                                    .colour_temperature_k = std::nullopt};
    data.entities.emplace(uuid(early_spot_text), spot);

    doc::Entity viewpoint;
    viewpoint.name = "From the corner";
    viewpoint.viewpoint = doc::Viewpoint{.position_m = {1.0, 1.0, 1.6},
                                         .yaw_rad = 0.0,
                                         .pitch_rad = 0.0,
                                         .field_of_view_rad = 50.0 * degrees,
                                         .ev100 = std::nullopt};
    data.entities.emplace(uuid(early_viewpoint_text), viewpoint);

    return accepted(data);
}

} // namespace

TEST_CASE("determinism: compiling the skeleton twice gives equal worlds", "[world]") {
    const doc::Document document = skeleton();
    const World first = compile(document);
    const World second = compile(document);
    CHECK(first == second);

    // And from a second load of the same folder.
    CHECK(compile(skeleton()) == first);
}

TEST_CASE("determinism: compiling a larger document twice gives equal worlds", "[world]") {
    const doc::Document document = with_early_entities();
    CHECK(compile(document) == compile(document));
}

TEST_CASE("determinism: worlds from different documents are not equal", "[world]") {
    // operator== looks at the values, not only the counts: each change below keeps every count.
    const World skeleton_world = compile(skeleton());
    auto data = skeleton_data();
    SECTION("a dimmed spot") {
        entity(data, spot_text).luminaire->dimming = 0.5;
    }
    SECTION("a spot aimed elsewhere") {
        entity(data, spot_text).luminaire->aim =
            doc::AimAtPoint{.point_m = {.x = 3.0, .y = 0.0, .z = 1.0}};
    }
    SECTION("a warmer spot") {
        entity(data, spot_text).luminaire->colour_temperature_k = 2700.0;
    }
    SECTION("a work hung higher") {
        std::get<doc::OnWall>(*entity(data, chart_text).placement).centre_height_m = 1.6;
    }
    SECTION("another image") {
        work(data, chart_work_text).image->sha256 = std::string(64, 'a');
    }
    SECTION("another exposure") {
        entity(data, viewpoint_text).viewpoint->ev100 = 9.0;
    }
    SECTION("another frame colour") {
        entity(data, chart_text).framing->colour = doc::LinearRgb{.r = 0.5, .g = 0.5, .b = 0.5};
    }
    CHECK_FALSE(compile(accepted(data)) == skeleton_world);
}

TEST_CASE("ordering: instances, exhibits, lights and cameras are ordered by entity id", "[world]") {
    const World world = compile(with_early_entities());
    check_well_formed(world);
    CHECK(world.problems.empty());

    // Instances by entity id, and within an entity by part (world.hpp: room, painting, frame).
    const std::vector<Id> instance_entities = {id(early_exhibit_text), id(gallery_text),
                                               id(chart_text), id(chart_text)};
    const std::vector<Part> instance_parts = {Part::painting, Part::room, Part::painting,
                                              Part::frame};
    REQUIRE(world.instances.size() == instance_entities.size());
    for (std::size_t i = 0; i < world.instances.size(); ++i) {
        INFO("instance " << i);
        CHECK(world.instances[i].entity == instance_entities[i]);
        CHECK(world.instances[i].part == instance_parts[i]);
    }

    REQUIRE(world.exhibits.size() == 2);
    CHECK(world.exhibits[0].entity == id(early_exhibit_text));
    CHECK(world.exhibits[0].work == id(early_work_text));
    CHECK(world.exhibits[1].entity == id(chart_text));

    REQUIRE(world.lights.size() == 2);
    CHECK(world.lights[0].entity == id(early_spot_text));
    CHECK(world.lights[1].entity == id(spot_text));

    REQUIRE(world.cameras.size() == 2);
    CHECK(world.cameras[0].entity == std::optional<Id>(id(early_viewpoint_text)));
    CHECK(world.cameras[1].entity == std::optional<Id>(id(viewpoint_text)));
}

TEST_CASE("ordering: the skeleton's instances run room, painting, frame", "[world]") {
    // The room's id (...02) sorts before the chart's (...07); the chart's painting before its
    // frame.
    const World world = compile(skeleton());
    REQUIRE(world.instances.size() == 3);
    CHECK(world.instances[0].part == Part::room);
    CHECK(world.instances[1].part == Part::painting);
    CHECK(world.instances[2].part == Part::frame);
}

TEST_CASE("images: the world names images by asset hash and extension and loads none", "[world]") {
    SECTION("an asset that is not on disk compiles to the same reference") {
        // No file anywhere is named by this hash, so the world cannot have read one.
        const std::string hash(64, '0');
        auto data = skeleton_data();
        work(data, chart_work_text).image = doc::AssetRef{.sha256 = hash, .extension = "exr"};
        const World world = compile(accepted(data));
        check_well_formed(world);
        CHECK(world.problems.empty());

        const Instance& painting = the_instance(world, id(chart_text), Part::painting);
        const auto* surface = std::get_if<PaintingSurface>(&material_of(world, painting));
        REQUIRE(surface != nullptr);
        REQUIRE(surface->image.has_value());
        CHECK(*surface->image == AssetRef{.sha256 = hash, .extension = "exr"});
    }

    SECTION("a document read from text, with no folder of assets, compiles to the same world") {
        const doc::Document from_text =
            doc::parse_project_json(read_file(skeleton_folder() / "project.json"));
        CHECK(compile(from_text) == compile(skeleton()));
    }
}

TEST_CASE("ids: Id::to_string gives the entity's canonical lowercase UUID text", "[world]") {
    SECTION("from the bytes of a document id") {
        for (const auto text : {chart_work_text, gallery_text, chart_text, viewpoint_text}) {
            CHECK(id(text).to_string() == text);
            CHECK(id(text).to_string() == uuid(text).to_string());
        }
    }
    SECTION("nil") {
        CHECK(Id{}.to_string() == "00000000-0000-0000-0000-000000000000");
    }
    SECTION("every hexadecimal digit, in lowercase") {
        const Id::Bytes bytes = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
        CHECK(Id(bytes).to_string() == "01234567-89ab-cdef-fedc-ba9876543210");
    }
    SECTION("all ones") {
        Id::Bytes bytes{};
        bytes.fill(0xff);
        CHECK(Id(bytes).to_string() == "ffffffff-ffff-ffff-ffff-ffffffffffff");
    }
}

TEST_CASE("ids: the world's ids are the document's, in the document's order", "[world]") {
    const World world = compile(skeleton());
    REQUIRE(world.exhibits.size() == 1);
    CHECK(world.exhibits.front().entity.to_string() == chart_text);
    CHECK(world.exhibits.front().work.to_string() == chart_work_text);
    REQUIRE(world.lights.size() == 1);
    CHECK(world.lights.front().entity.to_string() == spot_text);
    REQUIRE(world.cameras.size() == 1);
    REQUIRE(world.cameras.front().entity.has_value());
    CHECK(world.cameras.front().entity->to_string() == viewpoint_text);

    // Byte by byte, as the document orders its ids.
    CHECK(id(gallery_text) < id(chart_text));
    CHECK(id("01a1cd0d-dbff-7000-8000-000000000001") < id(gallery_text));
}
