// SPDX-License-Identifier: Apache-2.0

// The compiled skeleton's lux grid and painting texture, from issue #81 (M1-17) and the owner's
// comments on it: the document loaded from tests/scenes/skeleton, compiled to a world, its
// chart's photograph loaded from the assets folder, and read by the engine.
//
// - work_lux_grid on the chart: the direct light at the centre within 1% of I cos / d^2 (the
//   issue's criterion); every one of the 25 points above 0 lux, and the minimum within the range
//   the analytic I cos / d^2 predicts at the corners of the sight opening (comment of
//   2026-11-12).
// - The chart's texels in the indirect light: the total illuminance on the wall beside the chart
//   changes when the chart's texture is black, its direct illuminance does not (2026-11-15),
//   through both lux_at and render (2026-11-16).
//
// It belongs here because the engine may not depend on the document (blueprint section 7), so
// its own tests cannot load a project. Written from the issue and the public headers only
// (test-author), without the implementation.

#include <cs/assets/texture.hpp>
#include <cs/core/colour.hpp>
#include <cs/core/image.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/engine/lux.hpp>
#include <cs/engine/path_tracer.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/textures.hpp>
#include <cs/engine/work_lux.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using glm::dvec2;
using glm::dvec3;

namespace engine = cs::engine;
namespace world = cs::world;

namespace {

// The issue's tolerance for the direct light against I cos / d^2: 1%.
constexpr double issue_tolerance = 0.01;

// The chart's photograph, as tests/scenes/skeleton/assets/ names it.
const std::string chart_sha = "26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a";

std::filesystem::path skeleton_folder() {
    return std::filesystem::path(CS_SOURCE_DIR) / "tests/scenes/skeleton";
}

struct Skeleton {
    world::World world;
    engine::LoadedTextures loaded;
    world::AssetRef chart;
};

Skeleton load_skeleton() {
    const cs::doc::Document document = cs::doc::load_project_folder(skeleton_folder());
    Skeleton s;
    s.world = world::compile(document);
    REQUIRE(s.world.problems.empty());
    REQUIRE(s.world.exhibits.size() == 1);
    REQUIRE(s.world.lights.size() == 1);
    REQUIRE(s.world.cameras.size() == 1);

    // The chart's painting names its photograph.
    const world::Exhibit& exhibit = s.world.exhibits.front();
    REQUIRE(exhibit.painting_instance < s.world.instances.size());
    const world::Instance& painting = s.world.instances[exhibit.painting_instance];
    REQUIRE(painting.material < s.world.materials.size());
    const auto* surface =
        std::get_if<world::PaintingSurface>(&s.world.materials[painting.material]);
    REQUIRE(surface != nullptr);
    REQUIRE(surface->image.has_value());
    s.chart = *surface->image;

    s.loaded = engine::load_textures(s.world, skeleton_folder() / "assets");
    return s;
}

cs::ThreadPool& shared_pool() {
    static cs::ThreadPool pool(std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    return pool;
}

// The direct illuminance the spot's I cos / d^2 predicts at a point of the chart's face, facing
// along its normal, from the compiled world's own light, ignoring anything in the way.
double analytic_direct_lux(const world::World& w, dvec3 point_m) {
    const world::Exhibit& chart = w.exhibits.front();
    const cs::light::Luminaire& spot = w.lights.front().luminaire;
    const dvec3 to_light = spot.position_m() - point_m;
    const double d = glm::length(to_light);
    const double cos_theta = glm::dot(chart.face_normal, to_light / d);
    return spot.intensity_cd_towards(point_m) * cos_theta / (d * d);
}

bool same_bits(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// The chart's photograph replaced by one black texel: the same asset, so the painting finds it,
// but it reflects nothing.
engine::Textures black_chart(const world::AssetRef& chart) {
    engine::Textures black;
    black.add(chart, cs::assets::Texture{{cs::Image(1, 1)}});
    return black;
}

} // namespace

// ---- The chart's photograph --------------------------------------------------------------------

TEST_CASE("the skeleton's chart photograph loads from its assets folder with no problems",
          "[integration]") {
    const Skeleton s = load_skeleton();
    CHECK(s.chart.sha256 == chart_sha);
    CHECK(s.chart.extension == "png");
    CHECK(s.loaded.problems.empty());
    for (const engine::TextureProblem& p : s.loaded.problems) {
        UNSCOPED_INFO(p.asset.sha256 << "." << p.asset.extension << ": " << p.reason);
    }
    CHECK(s.loaded.textures.size() == 1);
    const cs::assets::Texture* texture = s.loaded.textures.find(s.chart);
    REQUIRE(texture != nullptr);
    CHECK(texture->width() == 292); // the PNG's own size
    CHECK(texture->height() == 368);
}

// ---- The lux grid ------------------------------------------------------------------------------

TEST_CASE("work_lux_grid on the compiled skeleton: the direct light at the chart's centre is "
          "I cos / d^2 within 1%",
          "[integration][accuracy]") {
    // The issue's criterion. Point 12 is the centre of the 5 x 5 grid, and the sight opening is
    // centred on the work, so it is the face's centre. The direct part is a sum over point lights
    // (lux.hpp), not an estimate, so few samples do: only the direct part is checked.
    const Skeleton s = load_skeleton();
    const engine::Scene scene(s.world);
    const world::Exhibit& chart = s.world.exhibits.front();
    engine::LuxOptions options;
    options.samples = 16;
    options.textures = &s.loaded.textures;
    const engine::WorkLux grid = engine::work_lux_grid(scene, chart, options);

    const engine::WorkLuxPoint& centre = grid.points[12];
    CHECK_THAT(centre.face_m.x, WithinAbs(0.0, 1e-9));
    CHECK_THAT(centre.face_m.y, WithinAbs(0.0, 1e-9));
    CHECK_THAT(glm::length(centre.point_m - chart.face_centre_m), WithinAbs(0.0, 1e-9));

    // The scene README's geometry: the light arrives from 2.309 m at cos 0.4577.
    const dvec3 to_light = s.world.lights.front().luminaire.position_m() - chart.face_centre_m;
    CHECK_THAT(glm::length(to_light), WithinAbs(2.309, 1e-3));
    CHECK_THAT(glm::dot(chart.face_normal, glm::normalize(to_light)), WithinAbs(0.4577, 1e-3));

    const double expected = analytic_direct_lux(s.world, chart.face_centre_m);
    INFO("centre: direct " << centre.reading.direct_lux.value << " lux, I cos / d^2 " << expected);
    CHECK(centre.reading.direct_lux.standard_error == 0.0);
    REQUIRE(centre.reading.direct_lux.value > 0.0);
    CHECK_THAT(centre.reading.direct_lux.value, WithinRel(expected, issue_tolerance));
}

TEST_CASE("work_lux_grid on the compiled skeleton: all 25 points read direct light above 0, and "
          "the minimum lies in the range I cos / d^2 predicts for the sight opening's corners",
          "[integration][accuracy]") {
    // The owner's comment of 2026-11-12. The chart is framed with a 5 mm rebate, so the visible
    // area is the sight opening, 0.72 x 0.91 m, and the grid's points lie a tenth of it in from
    // its edges: columns at +-0.288 and +-0.144 m, rows at +-0.364 and +-0.182 m.
    //
    // No point is shadowed. The light comes from above: from the top row (0.364 m up) the ray to
    // the spot rises 1.636 m over 1.057 m out from the face, so at the frame's front, 4 cm out, it
    // is 6.2 cm higher, at 0.426 m, inside the opening's top edge at 0.455 m. Sideways it moves
    // 0.177 / 1.057 x 4 cm = 7 mm, far inside the opening's sides (0.36 m against the outer
    // columns' 0.288 m). So every point's direct light is the spot's I cos / d^2 there, and the
    // check holds each to the issue's 1%.
    //
    // The range for the minimum, as the owner put it: between the least and the greatest
    // I cos / d^2 at the four corners of the sight opening. The spot is aimed at the centre and
    // its intensity falls away from its axis (half the peak at 7.5 degrees), and it meets the
    // face obliquely, so the corners differ: worked out by hand from the README's geometry and
    // light::GenericSpot's profile, the darkest corner (bottom, nearest the light's side, 13.7
    // degrees off the axis) gets about a third of the grid's least I cos / d^2, and the brightest
    // (top, far side, 10.6 degrees) about 1.4 times it. So the range brackets the grid's minimum
    // with a wide margin both ways, and the premise (no grid point darker than the darkest
    // corner) is checked too. A grid whose points sat on the work's own edges, under the frame's
    // lip, would read about 0 and fall below the range; one bunched at the centre would rise above
    // it. Within that, the minimum is checked to 1% against the least I cos / d^2 at the 25
    // points themselves.
    const Skeleton s = load_skeleton();
    const engine::Scene scene(s.world);
    const world::Exhibit& chart = s.world.exhibits.front();

    // The compiled visible area: the 0.73 x 0.92 m work less 5 mm on every side (README, and
    // world::default_frame's rebate).
    CHECK_THAT(chart.visible.min_m.x, WithinAbs(-0.36, 1e-9));
    CHECK_THAT(chart.visible.max_m.x, WithinAbs(0.36, 1e-9));
    CHECK_THAT(chart.visible.min_m.y, WithinAbs(-0.455, 1e-9));
    CHECK_THAT(chart.visible.max_m.y, WithinAbs(0.455, 1e-9));

    engine::LuxOptions options;
    options.samples = 16; // only the direct part is checked
    options.textures = &s.loaded.textures;
    const engine::WorkLux grid = engine::work_lux_grid(scene, chart, options);

    double analytic_min = std::numeric_limits<double>::infinity();
    double analytic_max = 0.0;
    double direct_min = std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < 25; ++k) {
        const engine::WorkLuxPoint& p = grid.points[k];
        const double expected = analytic_direct_lux(s.world, p.point_m);
        INFO("point " << k << " at (" << p.face_m.x << ", " << p.face_m.y
                      << ") on the face: direct " << p.reading.direct_lux.value
                      << " lux, I cos / d^2 " << expected);
        CHECK(p.reading.direct_lux.value > 0.0);
        CHECK_THAT(p.reading.direct_lux.value, WithinRel(expected, issue_tolerance));
        // Inside the opening, not on its edge.
        CHECK(p.face_m.x > chart.visible.min_m.x);
        CHECK(p.face_m.x < chart.visible.max_m.x);
        CHECK(p.face_m.y > chart.visible.min_m.y);
        CHECK(p.face_m.y < chart.visible.max_m.y);
        analytic_min = std::min(analytic_min, expected);
        analytic_max = std::max(analytic_max, expected);
        direct_min = std::min(direct_min, p.reading.direct_lux.value);
    }
    CHECK(grid.direct_lux.minimum == direct_min);

    double corner_min = std::numeric_limits<double>::infinity();
    double corner_max = 0.0;
    for (const dvec2 corner : {chart.visible.min_m, chart.visible.max_m,
                               dvec2(chart.visible.min_m.x, chart.visible.max_m.y),
                               dvec2(chart.visible.max_m.x, chart.visible.min_m.y)}) {
        const double e = analytic_direct_lux(s.world, chart.face_point(corner));
        corner_min = std::min(corner_min, e);
        corner_max = std::max(corner_max, e);
    }
    INFO("I cos / d^2: sight opening's corners "
         << corner_min << " to " << corner_max << " lux, grid points " << analytic_min << " to "
         << analytic_max << "; grid minimum " << grid.direct_lux.minimum);
    // The premise of the range: the analytic field is darker at the opening's corners than at any
    // grid point.
    CHECK(corner_min > 0.0);
    CHECK(corner_min <= analytic_min);

    CHECK(grid.direct_lux.minimum > 0.0);
    CHECK(grid.direct_lux.minimum >= corner_min * (1.0 - issue_tolerance));
    CHECK(grid.direct_lux.minimum <= corner_max * (1.0 + issue_tolerance));
    CHECK(grid.direct_lux.minimum <= analytic_max);
    CHECK_THAT(grid.direct_lux.minimum, WithinRel(analytic_min, issue_tolerance));
}

// ---- The chart's texels in the indirect light --------------------------------------------------

TEST_CASE("lux_at on the compiled skeleton: the wall beside the chart reads less total light when "
          "the chart is black, and the same direct light",
          "[integration][accuracy]") {
    // The owner's comments of 2026-11-15 and 2026-11-16, through lux_at. Two points on wall A's
    // face (y = 0, facing +y into the room): 0.6 m to the side of the frame (whose outer edge is
    // 0.42 m from the chart's centre), outside the spot's beam; and 8.5 cm above the frame's top,
    // 10.4 degrees from the beam's axis, inside its field, so that it has direct light too.
    //
    // Why the difference is not noise: with the same seed, sample s of both readings draws the
    // same random numbers (lux.hpp: its own generator, hash_seed(seed, 0, 0, s, lux_dimension)),
    // so it follows the same path until the path meets the chart. A path that never meets it
    // adds the same light, bit for bit, to both readings; one that does adds nothing further when
    // the chart is black (albedo 0 ends it) and something when it shows its photograph. Every
    // sample therefore adds at least as much with the photograph as with black, and the sums,
    // taken in the same order, keep that order (rounding is monotonic). The total with the
    // photograph is strictly more once one path meets a lit, non-black part of the chart: about
    // 1% of paths reach the chart's 0.67 m^2 within two bounces, some 40 of the 4096 here, so
    // missing them all has a probability far below e^-20. A lux_at that ignored the textures
    // would read the 0.8 grey base colour in both and give equal totals.
    const Skeleton s = load_skeleton();
    REQUIRE(s.loaded.problems.empty());
    const engine::Scene scene(s.world);
    const engine::Textures black = black_chart(s.chart);

    struct Point {
        const char* name;
        dvec3 position_m;
        bool lit;
    };
    const Point point = GENERATE(Point{"0.6 m beside the frame", {1.98, 0.0, 1.5}, false},
                                 Point{"8.5 cm above the frame", {3.0, 0.0, 2.1}, true});
    INFO(point.name);
    const dvec3 normal(0.0, 1.0, 0.0);

    engine::LuxOptions options;
    options.samples = 4096;
    options.seed = 1;
    options.pool = &shared_pool();
    options.textures = &s.loaded.textures;
    const engine::LuxReading photograph = engine::lux_at(scene, point.position_m, normal, options);
    options.textures = &black;
    const engine::LuxReading blackened = engine::lux_at(scene, point.position_m, normal, options);

    INFO("photograph: direct " << photograph.direct_lux.value << ", total "
                               << photograph.total_lux.value << " +- "
                               << photograph.total_lux.standard_error
                               << " lux; black chart: direct " << blackened.direct_lux.value
                               << ", total " << blackened.total_lux.value << " +- "
                               << blackened.total_lux.standard_error << " lux");
    // Direct light does not depend on any texture.
    CHECK(same_bits(photograph.direct_lux.value, blackened.direct_lux.value));
    CHECK(photograph.direct_lux.standard_error == 0.0);
    CHECK(blackened.direct_lux.standard_error == 0.0);
    if (point.lit) {
        CHECK(photograph.direct_lux.value > 0.0);
    }
    // The indirect light does.
    CHECK(photograph.total_lux.value > blackened.total_lux.value);
    CHECK(blackened.total_lux.value >= blackened.direct_lux.value);
}

TEST_CASE("render of the compiled skeleton: the wall reads less light when the chart is black",
          "[integration]") {
    // The same check through the path tracer (owner's comment of 2026-11-16), from the scene's
    // viewpoint at a small size. Only pixels that see the room's surfaces, nowhere near the chart
    // or its frame, are compared, so that the difference comes from light the chart reflects
    // onto the room, read by the path tracer's own texel lookup at later bounces.
    //
    // As above, the same seed gives each pixel sample the same path in both renders until it
    // meets the chart (path_tracer.hpp: its own generator per pixel sample), so each sample adds
    // at least as much with the photograph as with black, and strictly more for any path that
    // reaches a lit, non-black part of the chart. The wall pixels' 16 samples each, several
    // thousand paths, include dozens that do. A path tracer that ignored the textures would give
    // equal images.
    const Skeleton s = load_skeleton();
    REQUIRE(s.loaded.problems.empty());
    const engine::Scene scene(s.world);
    const engine::Textures black = black_chart(s.chart);
    const world::Camera& camera = s.world.cameras.front();
    REQUIRE(camera.projection == world::Projection::perspective);

    engine::RenderOptions options;
    options.width = 32;
    options.height = 18;
    options.samples_per_pixel = 16;
    options.seed = 1;
    options.pool = &shared_pool();
    options.textures = &s.loaded.textures;
    const cs::Image photograph = engine::render(scene, camera, options);
    options.textures = &black;
    const cs::Image blackened = engine::render(scene, camera, options);

    // Which pixels see only the room: rays through a 3 x 3 grid spanning the pixel and half a
    // pixel round it all hit the room's own surfaces. The rays follow path_tracer.hpp's camera
    // (right = forward x up, row 0 at the top) with the usual pinhole: the vertical field of view
    // over the height, the same scale across. A pixel wrongly let in by a different convention
    // could only make the difference larger, never reverse it, by the argument above.
    const dvec3 forward = glm::normalize(camera.forward);
    const dvec3 up = glm::normalize(camera.up - glm::dot(camera.up, forward) * forward);
    const dvec3 right = glm::cross(forward, up);
    const double tan_half = std::tan(0.5 * camera.vertical_fov_rad);
    const double w = options.width;
    const double h = options.height;
    const auto sees_only_room = [&](int x, int y) {
        for (const double dy : {-0.5, 0.5, 1.5}) {
            for (const double dx : {-0.5, 0.5, 1.5}) {
                const double sx = (2.0 * (x + dx) / w - 1.0) * tan_half * (w / h);
                const double sy = (1.0 - 2.0 * (y + dy) / h) * tan_half;
                const dvec3 direction = glm::normalize(forward + sx * right + sy * up);
                const auto hit = scene.intersect(camera.position_m, direction);
                if (!hit || s.world.instances[hit->instance].part != world::Part::room) {
                    return false;
                }
            }
        }
        return true;
    };

    double wall_photograph = 0.0;
    double wall_black = 0.0;
    double all_photograph = 0.0;
    double all_black = 0.0;
    int wall_pixels = 0;
    for (int y = 0; y < options.height; ++y) {
        for (int x = 0; x < options.width; ++x) {
            const cs::Rgb& p = photograph.at(x, y);
            const cs::Rgb& b = blackened.at(x, y);
            const double lp = cs::luminance(dvec3(p.r, p.g, p.b));
            const double lb = cs::luminance(dvec3(b.r, b.g, b.b));
            all_photograph += lp;
            all_black += lb;
            if (sees_only_room(x, y)) {
                ++wall_pixels;
                wall_photograph += lp;
                wall_black += lb;
            }
        }
    }
    INFO(wall_pixels << " room pixels: mean " << wall_photograph / wall_pixels
                     << " nits with the photograph, " << wall_black / wall_pixels
                     << " with a black chart; whole image " << all_photograph / (w * h) << " and "
                     << all_black / (w * h));
    // The chart and frame cover about 6 x 7 of the 32 x 18 pixels; with the half-pixel margin the
    // room still fills most of the image.
    REQUIRE(wall_pixels >= 300);
    CHECK(wall_photograph > wall_black);
    CHECK(all_photograph > all_black);
}
