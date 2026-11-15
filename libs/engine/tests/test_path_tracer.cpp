// SPDX-License-Identifier: Apache-2.0

// The path tracer (path_tracer.hpp), from issue #77 (M1-16) and the owner's comment on it: images
// in absolute luminance (nits), checked against the white furnace (a closed albedo-1 surface in a
// uniform environment shows the environment's luminance, however the light interreflects), a
// Lambert floor under a point light (rho E / pi), and the integrating sphere of the lux meter's
// tests, whose walls must show rho E / pi with E the direct plus the inter-reflected illuminance.
// direct_only drops the indirect light; images must be the same, bit for bit, whatever the
// thread count, and a progressive render must end with the image one render of the same samples
// gives. Invalid sizes, environments and cameras throw.
//
// Every world is built here by hand from geom's meshes; the helpers are copied from
// test_lux.cpp, since test files share nothing. Every mesh is seen from its front (a sphere or box
// seen from outside faces out, a room or the integrating sphere seen from inside faces in), so
// that nothing here depends on how a surface seen from behind is shaded.

#include <cs/engine/lux.hpp>
#include <cs/engine/path_tracer.hpp>
#include <cs/engine/scene.hpp>

#include <cs/core/colour.hpp>
#include <cs/core/image.hpp>
#include <cs/core/image_io.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/geom/room.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::engine::ProgressiveRender;
using cs::engine::render;
using cs::engine::RenderOptions;
using cs::engine::Scene;
using glm::dvec2;
using glm::dvec3;

namespace geom = cs::geom;
namespace light = cs::light;
namespace world = cs::world;

namespace {

constexpr double pi = std::numbers::pi;
constexpr double degrees = pi / 180.0;
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

// The issue's tolerances: 1% for every image mean, 3% for every pixel of the white furnace.
constexpr double issue_tolerance = 0.01;
constexpr double furnace_pixel_tolerance = 0.03;

// ---- Lights (from test_lux.cpp) ----------------------------------------------------------------

// An ideal point source: the same intensity in every direction, so 4 pi times it in lumens.
class Isotropic final : public light::Distribution {
  public:
    explicit Isotropic(double intensity_cd) : intensity_cd_(intensity_cd) {}

    double intensity_cd(glm::dvec3 direction) const override {
        if (std::isnan(direction.x) || std::isnan(direction.y) || std::isnan(direction.z)) {
            return not_a_number;
        }
        return intensity_cd_;
    }
    double total_lumens() const override { return 4.0 * pi * intensity_cd_; }

  private:
    double intensity_cd_;
};

world::Light make_light(dvec3 position_m, dvec3 aim_m,
                        std::shared_ptr<const light::Distribution> distribution,
                        std::shared_ptr<const light::GenericSpot> spot) {
    return world::Light{.entity = world::Id{},
                        .luminaire = light::Luminaire(position_m, aim_m, std::move(distribution)),
                        .spot = std::move(spot),
                        .colour_temperature_k = 3000.0};
}

// An isotropic light. World::Light also carries the distribution as a generic spot for the
// shaders: a uniform cone over the whole sphere (beam = field = 360 degrees), whose intensity is
// lumens / (2 pi (1 - cos pi)) = 4 pi I / (4 pi) = I, the same light.
world::Light isotropic_light(dvec3 position_m, double intensity_cd) {
    return make_light(
        position_m, position_m + dvec3(0.0, 0.0, -1.0),
        std::make_shared<const Isotropic>(intensity_cd),
        std::make_shared<const light::GenericSpot>(4.0 * pi * intensity_cd, 2.0 * pi, 2.0 * pi));
}

world::Light spot_light(dvec3 position_m, dvec3 aim_m,
                        const std::shared_ptr<const light::GenericSpot>& spot) {
    return make_light(position_m, aim_m, spot, spot);
}

std::shared_ptr<const light::GenericSpot> skeleton_spot() {
    // The skeleton's fixture type: 800 lm, beam 15 degrees and field 25 degrees (full angles).
    return std::make_shared<const light::GenericSpot>(800.0, 15.0 * degrees, 25.0 * degrees);
}

// ---- Meshes (from test_lux.cpp) ----------------------------------------------------------------

// A rectangle centred on `centre`, spanning +-half_u and +-half_v, facing cross(half_u, half_v):
// four vertices, two triangles wound counter-clockwise seen from its front.
geom::Mesh rectangle(dvec3 centre, dvec3 half_u, dvec3 half_v) {
    const dvec3 normal = glm::normalize(glm::cross(half_u, half_v));
    geom::Mesh mesh;
    mesh.positions = {centre - half_u - half_v, centre + half_u - half_v, centre + half_u + half_v,
                      centre - half_u + half_v};
    mesh.normals.assign(4, normal);
    mesh.uvs = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// A floor at z = 0, 1000 m on a side, facing up.
geom::Mesh large_floor() {
    return rectangle(dvec3(0.0), dvec3(500.0, 0.0, 0.0), dvec3(0.0, 500.0, 0.0));
}

// A closed polyhedron inscribed in the sphere of radius_m about the origin: n_lat bands of
// latitude, n_lon segments of longitude, flat triangles each with its own vertices and its
// normal, facing in (towards the centre). Corners are computed once and repeated bit for bit,
// so it is watertight.
geom::Mesh faceted_sphere(double radius_m, int n_lon, int n_lat) {
    std::vector<dvec3> grid(static_cast<std::size_t>((n_lat + 1) * n_lon));
    auto at = [&](int i, int j) -> const dvec3& {
        return grid[static_cast<std::size_t>(i * n_lon + j % n_lon)];
    };
    for (int i = 0; i <= n_lat; ++i) {
        const double theta = pi * i / n_lat;
        for (int j = 0; j < n_lon; ++j) {
            const double phi = 2.0 * pi * j / n_lon;
            dvec3 p = radius_m * dvec3(std::sin(theta) * std::cos(phi),
                                       std::sin(theta) * std::sin(phi), std::cos(theta));
            if (i == 0) {
                p = dvec3(0.0, 0.0, radius_m);
            } else if (i == n_lat) {
                p = dvec3(0.0, 0.0, -radius_m);
            }
            grid[static_cast<std::size_t>(i * n_lon + j)] = p;
        }
    }

    geom::Mesh mesh;
    auto add = [&](dvec3 a, dvec3 b, dvec3 c) {
        dvec3 n = glm::cross(b - a, c - a);
        if (glm::dot(n, a + b + c) > 0.0) { // facing out: turn it to face the centre
            std::swap(b, c);
            n = -n;
        }
        n = glm::normalize(n);
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (const dvec3& p : {a, b, c}) {
            mesh.positions.push_back(p);
            mesh.normals.push_back(n);
            mesh.uvs.push_back(dvec2(0.0));
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
    };
    for (int i = 0; i < n_lat; ++i) {
        for (int j = 0; j < n_lon; ++j) {
            const dvec3& a = at(i, j);
            const dvec3& b = at(i, j + 1);
            const dvec3& c = at(i + 1, j + 1);
            const dvec3& d = at(i + 1, j);
            if (i != 0) {
                add(a, b, c); // at the north pole a and b are the pole
            }
            if (i != n_lat - 1) {
                add(a, c, d); // at the south pole c and d are
            }
        }
    }
    return mesh;
}

// The same mesh facing the other way: every triangle's winding reversed and its normals negated.
// A closed mesh stays closed (geom::is_closed matches its edges by position).
geom::Mesh turned_inside_out(geom::Mesh mesh) {
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
    }
    for (dvec3& n : mesh.normals) {
        n = -n;
    }
    return mesh;
}

geom::Mesh translated(geom::Mesh mesh, dvec3 offset_m) {
    for (dvec3& p : mesh.positions) {
        p += offset_m;
    }
    return mesh;
}

// A closed sphere of 96 x 48 facets about centre_m, facing out, for a camera outside it.
geom::Mesh outward_sphere(dvec3 centre_m, double radius_m) {
    return translated(turned_inside_out(faceted_sphere(radius_m, 96, 48)), centre_m);
}

// ---- Worlds ------------------------------------------------------------------------------------

// Adds a mesh in world coordinates (identity transform) with a Lambert material of its own.
void add_lambert(world::World& w, geom::Mesh mesh, double albedo) {
    world::Instance instance;
    instance.part = world::Part::room;
    instance.mesh = static_cast<std::uint32_t>(w.meshes.size());
    instance.material = static_cast<std::uint32_t>(w.materials.size());
    w.meshes.push_back(std::move(mesh));
    w.materials.emplace_back(world::LambertMaterial{dvec3(albedo)});
    w.instances.push_back(instance);
}

// The skeleton's room: 6 x 4 x 3.5 m, from (0, 0, 0) to (6, 4, 3.5), closed, facing in, white
// gallery paint.
world::World closed_room() {
    world::World w;
    add_lambert(w, geom::room_mesh(6.0, 4.0, 3.5), world::default_wall_albedo);
    return w;
}

// The closed room with the skeleton's spot in the ceiling's plane over the middle of the floor,
// aimed straight down.
world::World room_with_ceiling_spot() {
    world::World w = closed_room();
    w.lights.push_back(spot_light({3.0, 2.0, 3.5}, {3.0, 2.0, 0.0}, skeleton_spot()));
    return w;
}

// ---- Cameras -----------------------------------------------------------------------------------

world::Camera perspective(dvec3 position_m, dvec3 target_m, dvec3 up, double vertical_fov_rad) {
    world::Camera camera;
    camera.projection = world::Projection::perspective;
    camera.position_m = position_m;
    camera.forward = glm::normalize(target_m - position_m);
    camera.up = up;
    camera.vertical_fov_rad = vertical_fov_rad;
    return camera;
}

// A camera inside the closed room, looking down at the spot's pool of light on the floor and the
// walls beyond it.
world::Camera room_camera() {
    return perspective({1.0, 0.8, 1.6}, {3.5, 2.5, 0.5}, {0.0, 0.0, 1.0}, 70.0 * degrees);
}

// ---- Images ------------------------------------------------------------------------------------

std::size_t hardware_threads() {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

double luminance_of(const cs::Rgb& p) {
    return cs::luminance(dvec3(p.r, p.g, p.b));
}

struct Stats {
    double mean = 0.0;
    double standard_deviation = 0.0; // of one pixel's luminance about the mean
    double standard_error = 0.0;     // of the mean: the pixels are independent estimates
    double min = 0.0;
    double max = 0.0;
};

// The pixels' luminances, in nits. Each pixel draws its own random numbers (path_tracer.hpp), so
// pixels of a uniform view are independent samples of the same estimator and their spread over
// sqrt(count) is the standard error of the mean.
Stats luminance_stats(const cs::Image& image) {
    const double n = static_cast<double>(image.width()) * image.height();
    Stats s;
    s.min = inf;
    s.max = -inf;
    double sum = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double l = luminance_of(image.at(x, y));
            sum += l;
            s.min = std::min(s.min, l);
            s.max = std::max(s.max, l);
        }
    }
    s.mean = sum / n;
    double squares = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double d = luminance_of(image.at(x, y)) - s.mean;
            squares += d * d;
        }
    }
    s.standard_deviation = n > 1.0 ? std::sqrt(squares / (n - 1.0)) : 0.0;
    s.standard_error = s.standard_deviation / std::sqrt(n);
    return s;
}

// How many pixels' luminances lie further than `relative` from `expected`; the worst is reported.
int pixels_off(const cs::Image& image, double expected, double relative) {
    int off = 0;
    double worst = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double l = luminance_of(image.at(x, y));
            const double error = std::abs(l - expected);
            worst = std::max(worst, error / expected);
            if (!(error <= relative * expected)) { // NaN counts as off
                ++off;
            }
        }
    }
    UNSCOPED_INFO("worst pixel " << worst * 100.0 << "% from " << expected);
    return off;
}

// Whether two images are equal bit for bit, every channel of every pixel (NaN included).
bool same_bits(const cs::Image& a, const cs::Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) {
        UNSCOPED_INFO("sizes differ: " << a.width() << " x " << a.height() << " and " << b.width()
                                       << " x " << b.height());
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const cs::Rgb& p = a.at(x, y);
            const cs::Rgb& q = b.at(x, y);
            if (std::bit_cast<std::uint32_t>(p.r) != std::bit_cast<std::uint32_t>(q.r) ||
                std::bit_cast<std::uint32_t>(p.g) != std::bit_cast<std::uint32_t>(q.g) ||
                std::bit_cast<std::uint32_t>(p.b) != std::bit_cast<std::uint32_t>(q.b)) {
                UNSCOPED_INFO("first difference at (" << x << ", " << y << "): (" << p.r << ", "
                                                      << p.g << ", " << p.b << ") and (" << q.r
                                                      << ", " << q.g << ", " << q.b << ")");
                return false;
            }
        }
    }
    return true;
}

int differing_pixels(const cs::Image& a, const cs::Image& b) {
    int differing = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const cs::Rgb& p = a.at(x, y);
            const cs::Rgb& q = b.at(x, y);
            if (p.r != q.r || p.g != q.g || p.b != q.b) {
                ++differing;
            }
        }
    }
    return differing;
}

bool all_finite_and_non_negative(const cs::Image& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const cs::Rgb& p = image.at(x, y);
            for (const float c : {p.r, p.g, p.b}) {
                if (!std::isfinite(c) || c < 0.0f) {
                    UNSCOPED_INFO("pixel (" << x << ", " << y << ") has " << c);
                    return false;
                }
            }
        }
    }
    return true;
}

bool all_equal_to(const cs::Image& image, float value) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const cs::Rgb& p = image.at(x, y);
            if (p.r != value || p.g != value || p.b != value) {
                UNSCOPED_INFO("pixel (" << x << ", " << y << ") is (" << p.r << ", " << p.g << ", "
                                        << p.b << "), not " << value);
                return false;
            }
        }
    }
    return true;
}

RenderOptions options(int width, int height, std::uint32_t spp, std::uint64_t seed) {
    RenderOptions o;
    o.width = width;
    o.height = height;
    o.samples_per_pixel = spp;
    o.seed = seed;
    return o;
}

} // namespace

// ---- The white furnace -------------------------------------------------------------------------

namespace {

// The white furnace: a surface of albedo 1 reflects all the light it receives, and a uniform
// environment of L nits sends L from every direction; so wherever a path goes and however often
// it bounces before it leaves, it carries L back, and every pixel sees exactly L. A path tracer
// that loses or invents energy (a wrong pi, a wrong cosine weight, a biased Russian roulette)
// shows a different number. The issue: 100 nits, within 1% averaged over the image and 3% at every
// pixel, at 256 samples per pixel.
constexpr double furnace_nits = 100.0;
constexpr std::uint32_t furnace_spp = 256;

void check_furnace(const cs::Image& image) {
    const Stats s = luminance_stats(image);
    INFO("mean " << s.mean << " +- " << s.standard_error << ", pixels from " << s.min << " to "
                 << s.max);
    CHECK(all_finite_and_non_negative(image));
    CHECK_THAT(s.mean, WithinRel(furnace_nits, issue_tolerance));
    CHECK(pixels_off(image, furnace_nits, furnace_pixel_tolerance) == 0);
}

// One closed albedo-1 sphere of radius 1 m at the origin, facing out, and a camera 3 m away
// whose 45 degree view it fills most of (it subtends asin(1/3) = 19.5 degrees each side of the
// centre, the view 22.5): most pixels see the sphere, the corners the environment.
world::World furnace_sphere_world() {
    world::World w;
    const geom::Mesh sphere = outward_sphere(dvec3(0.0), 1.0);
    REQUIRE(geom::is_closed(sphere));
    add_lambert(w, sphere, 1.0);
    return w;
}

world::Camera furnace_sphere_camera() {
    return perspective({0.0, -3.0, 0.3}, dvec3(0.0), {0.0, 0.0, 1.0}, 45.0 * degrees);
}

} // namespace

TEST_CASE("render: white furnace, a closed albedo-1 sphere in a 100-nit environment renders "
          "100 nits, within 1% over the image and 3% at every pixel, at 256 spp",
          "[engine][path_tracer][accuracy]") {
    // The issue's criterion. The sphere is convex, so every path leaves after one bounce; the
    // pixels that miss it see the environment directly. Both are 100 nits exactly in expectation.
    cs::ThreadPool pool(hardware_threads());
    const Scene scene(furnace_sphere_world());
    RenderOptions o = options(32, 32, furnace_spp, 1);
    o.environment_nits = furnace_nits;
    o.pool = &pool;
    check_furnace(render(scene, furnace_sphere_camera(), o));
}

TEST_CASE("render: white furnace with interreflection, a closed albedo-1 sphere resting just "
          "above a closed albedo-1 box, renders 100 nits on average, without bias",
          "[engine][path_tracer][accuracy]") {
    // The furnace where light bounces between surfaces: a sphere of radius 1 m 2 cm above a
    // 3 x 3 x 0.5 m box, both closed and facing out. Paths that enter the crevice between them
    // bounce many times and meet Russian roulette from the fourth hit; with albedo 1 every bounce
    // keeps all the light, so the answer is still exactly 100 nits in expectation (energy
    // conservation), and only a biased roulette or a lost bounce changes it: the mean is held to
    // the issue's 1% and to 3 standard errors.
    //
    // The pixels cannot be held to the convex sphere's 3%. With albedo 1, the lux meter's
    // roulette (which the path tracer must follow, owner's comment on #77) keeps a path with
    // probability 0.95 and multiplies it by 1 / 0.95 at every bounce, so a crevice pixel averages
    // a few long paths of large weight: at 256 samples its worst pixel was 4.6% off with the mean
    // 100.015 +- 0.015, and at 1024 samples still 3.4%. The per-pixel bound is 10%, which
    // catches a region that loses light (a dark crevice), not noise.
    world::World w;
    const geom::Mesh box =
        turned_inside_out(translated(geom::room_mesh(3.0, 3.0, 0.5), {-1.5, -1.5, -0.5}));
    const geom::Mesh sphere = outward_sphere({0.0, 0.0, 1.02}, 1.0);
    REQUIRE(geom::is_closed(box));
    REQUIRE(geom::volume(box) > 0.0); // facing out
    REQUIRE(geom::is_closed(sphere));
    REQUIRE(geom::volume(sphere) > 0.0);
    add_lambert(w, box, 1.0);
    add_lambert(w, sphere, 1.0);
    const Scene scene(w);

    // From the front and a little above, so the view shows the box's top around the sphere's
    // base, where the light interreflects most.
    const world::Camera camera =
        perspective({0.0, -4.5, 1.6}, {0.0, 0.0, 0.5}, {0.0, 0.0, 1.0}, 40.0 * degrees);
    cs::ThreadPool pool(hardware_threads());
    RenderOptions o = options(32, 32, furnace_spp, 2);
    o.environment_nits = furnace_nits;
    o.pool = &pool;
    const cs::Image image = render(scene, camera, o);
    const Stats s = luminance_stats(image);
    INFO("mean " << s.mean << " +- " << s.standard_error << ", pixels from " << s.min << " to "
                 << s.max);
    CHECK(all_finite_and_non_negative(image));
    CHECK_THAT(s.mean, WithinRel(furnace_nits, issue_tolerance));
    CHECK(std::abs(s.mean - furnace_nits) <= 3.0 * s.standard_error);
    CHECK(pixels_off(image, furnace_nits, 0.10) == 0);
}

TEST_CASE("render: direct_only, the convex furnace sphere still renders 100 nits, the "
          "environment seen through one reflection",
          "[engine][path_tracer][accuracy]") {
    // path_tracer.hpp: direct_only keeps "the environment seen through one reflection; plus the
    // environment seen straight through the pixel". Off a convex sphere every reflection leaves
    // the scene at once, so that is all the light, and the image is still 100 nits.
    cs::ThreadPool pool(hardware_threads());
    const Scene scene(furnace_sphere_world());
    RenderOptions o = options(32, 32, furnace_spp, 3);
    o.environment_nits = furnace_nits;
    o.direct_only = true;
    o.pool = &pool;
    check_furnace(render(scene, furnace_sphere_camera(), o));
}

// ---- A Lambert floor under a point light -------------------------------------------------------

namespace {

// A floor of albedo 0.5 and a 1000 cd isotropic light 2 m above the origin: E = I h / r^3, 250
// lux below the light. A Lambert surface shows L = rho E / pi, 0.5 x 250 / pi = 39.79 nits there.
constexpr double floor_albedo = 0.5;
constexpr double floor_light_cd = 1000.0;
constexpr double floor_light_height_m = 2.0;

world::World lit_floor_world() {
    world::World w;
    add_lambert(w, large_floor(), floor_albedo);
    w.lights.push_back(isotropic_light({0.0, 0.0, floor_light_height_m}, floor_light_cd));
    return w;
}

// The floor's luminance at a horizontal distance r from the point below the light.
double floor_nits(double r_m) {
    const double h = floor_light_height_m;
    const double d = std::sqrt(r_m * r_m + h * h);
    return floor_albedo / pi * floor_light_cd * h / (d * d * d);
}

} // namespace

TEST_CASE("render: a Lambert floor of albedo 0.5 under 250 lux renders rho E / pi = 39.79 nits "
          "within 1%",
          "[engine][path_tracer][accuracy]") {
    // The issue's criterion. The camera hangs 1 m above the floor, between it and the light
    // (cameras are not geometry), looking straight down at the point below the light with a 2
    // degree view: it sees the floor out to tan(1 deg) x sqrt(2) = 2.5 cm, where E has fallen
    // from 250 lux by 1.5 r^2 / h^2 = 0.02%. Nothing but the floor reflects, so there is no
    // indirect light, and with or without direct_only the image is the direct light alone.
    const bool direct_only = GENERATE(false, true);
    INFO("direct_only " << direct_only);
    CHECK_THAT(floor_nits(0.0), WithinRel(39.79, 1e-4)); // the issue's figure, to its digits

    const Scene scene(lit_floor_world());
    const world::Camera camera =
        perspective({0.0, 0.0, 1.0}, dvec3(0.0), {0.0, 1.0, 0.0}, 2.0 * degrees);
    cs::ThreadPool pool(hardware_threads());
    RenderOptions o = options(32, 32, 16, 4);
    o.direct_only = direct_only;
    o.pool = &pool;
    const cs::Image image = render(scene, camera, o);
    REQUIRE(image.width() == 32);
    REQUIRE(image.height() == 32);

    const Stats s = luminance_stats(image);
    INFO("mean " << s.mean << ", pixels from " << s.min << " to " << s.max);
    CHECK(all_finite_and_non_negative(image));
    CHECK_THAT(s.mean, WithinRel(39.79, issue_tolerance));
    CHECK(pixels_off(image, floor_nits(0.0), issue_tolerance) == 0);

    // The light is 3000 K with luminance 1 (colour.hpp, as the lux meter's coloured sphere test
    // assumes), and the floor is grey: each channel is 39.79 times the light's colour.
    const dvec3 colour = cs::cct_to_linear_rgb(3000.0);
    const cs::Rgb& centre = image.at(16, 16);
    CHECK_THAT(centre.r, WithinRel(floor_nits(0.0) * colour.r, issue_tolerance));
    CHECK_THAT(centre.g, WithinRel(floor_nits(0.0) * colour.g, issue_tolerance));
    CHECK_THAT(centre.b, WithinRel(floor_nits(0.0) * colour.b, issue_tolerance));
}

TEST_CASE("render: a wide view of the lit floor follows rho / pi x I h / r^3 at every pixel",
          "[engine][path_tracer][accuracy]") {
    // A pinhole camera 1 m above the point below the light, looking straight down with a 60
    // degree view of a square image: pixel (x, y)'s centre looks at the floor point
    // (s_x, s_y) x tan(30 deg) x 1 m, with s = 2 (i + 0.5) / 32 - 1, give or take the image's
    // orientation, which the radially symmetric answer does not see. Over a pixel 3.6 cm wide,
    // the average of I h / r^3 differs from its value at the centre by at most
    // (w^2 / 24) |f'' / f| ~ 1e-4: the 1% tolerance is the issue's.
    const Scene scene(lit_floor_world());
    const world::Camera camera =
        perspective({0.0, 0.0, 1.0}, dvec3(0.0), {0.0, 1.0, 0.0}, 60.0 * degrees);
    cs::ThreadPool pool(hardware_threads());
    RenderOptions o = options(32, 32, 16, 5);
    o.pool = &pool;
    const cs::Image image = render(scene, camera, o);

    const double half = std::tan(30.0 * degrees);
    int off = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const double u = (2.0 * (x + 0.5) / 32.0 - 1.0) * half;
            const double v = (2.0 * (y + 0.5) / 32.0 - 1.0) * half;
            const double expected = floor_nits(std::sqrt(u * u + v * v));
            const double actual = luminance_of(image.at(x, y));
            if (!(std::abs(actual - expected) <= issue_tolerance * expected)) {
                ++off;
                UNSCOPED_INFO("pixel (" << x << ", " << y << "): " << actual << ", expected "
                                        << expected);
            }
        }
    }
    CHECK(off == 0);
}

TEST_CASE("render: an orthographic camera sees the floor point under each pixel",
          "[engine][path_tracer][accuracy]") {
    // An orthographic view 2 m high of a square image, from 1 m above the point below the light:
    // pixel (x, y)'s centre sees the floor at (s_x, s_y) x 1 m with s = 2 (i + 0.5) / 16 - 1, give
    // or take the orientation, which the radially symmetric answer does not see. Over a pixel
    // 12.5 cm wide, the average of I h / r^3 differs from its value at the centre by about
    // (w^2 / 24) |f'' / f| ~ 1e-3: well inside the issue's 1%. The samples' random positions in
    // the pixel add noise: where E falls by 0.6 per metre (1 m out), one sample's spread is about
    // 0.6 x 0.125 / sqrt(12) = 2.2%, so 256 samples give 0.14% and the worst of 256 pixels stays
    // near 0.5%. At 16 samples it reached 1.5%.
    const Scene scene(lit_floor_world());
    world::Camera camera;
    camera.projection = world::Projection::orthographic;
    camera.position_m = {0.0, 0.0, 1.0};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.vertical_extent_m = 2.0;
    RenderOptions o = options(16, 16, 256, 6);
    const cs::Image image = render(scene, camera, o);

    int off = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const double u = 2.0 * (x + 0.5) / 16.0 - 1.0;
            const double v = 2.0 * (y + 0.5) / 16.0 - 1.0;
            const double expected = floor_nits(std::sqrt(u * u + v * v));
            const double actual = luminance_of(image.at(x, y));
            if (!(std::abs(actual - expected) <= issue_tolerance * expected)) {
                ++off;
                UNSCOPED_INFO("pixel (" << x << ", " << y << "): " << actual << ", expected "
                                        << expected);
            }
        }
    }
    CHECK(off == 0);
}

TEST_CASE("render: row 0 is the top of the image and column 0 its left, as the camera sees it",
          "[engine][path_tracer]") {
    // path_tracer.hpp: the image's right is forward x up. A camera 1 m above the floor looking
    // horizontally along +y with up +z has its right along +x; a 60 degree view of a square
    // 32 x 32 image. The upper half of the image looks above the horizon at nothing: black,
    // exactly, with no environment. The light hangs 2 m above (2, 5, 0), whose floor point is
    // the brightest (Lambert luminance follows E, which peaks below the light). From the camera
    // it lies 2 m right, 1 m down and 5 m ahead: a pinhole puts it at column
    // (1 + (2/5) / tan 30) x 16 = 27.1 and row (1 + (1/5) / tan 30) x 16 = 21.5, right of and
    // below the centre. Allowing a pixel either way for the flat peak.
    world::World w;
    add_lambert(w, large_floor(), floor_albedo);
    w.lights.push_back(isotropic_light({2.0, 5.0, 2.0}, floor_light_cd));
    const Scene scene(w);
    const world::Camera camera =
        perspective({0.0, 0.0, 1.0}, {0.0, 1.0, 1.0}, {0.0, 0.0, 1.0}, 60.0 * degrees);
    cs::ThreadPool pool(hardware_threads());
    RenderOptions o = options(32, 32, 16, 7);
    o.pool = &pool;
    const cs::Image image = render(scene, camera, o);

    bool top_black = true;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 32; ++x) {
            const cs::Rgb& p = image.at(x, y);
            top_black = top_black && p.r == 0.0f && p.g == 0.0f && p.b == 0.0f;
        }
    }
    CHECK(top_black);

    int best_x = 0;
    int best_y = 0;
    double best = -1.0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const double l = luminance_of(image.at(x, y));
            if (l > best) {
                best = l;
                best_x = x;
                best_y = y;
            }
        }
    }
    INFO("brightest pixel (" << best_x << ", " << best_y << "): " << best << " nits");
    CHECK(best_x >= 26);
    CHECK(best_x <= 28);
    CHECK(best_y >= 20);
    CHECK(best_y <= 22);
    // And close to rho E / pi with E = 250 lux at the point below the light.
    CHECK(best <= floor_nits(0.0) * (1.0 + issue_tolerance));
    CHECK(best > 0.9 * floor_nits(0.0));
}

// ---- The owner's comment: the integrating sphere -----------------------------------------------

namespace {

// The integrating sphere of test_lux.cpp: the 96 x 48 faceted sphere of radius 1 m, facing in, of
// Lambert albedo rho, with a 1000 cd isotropic light at its centre. Every patch of wall receives
// the same inter-reflected illuminance, E_ind = rho Phi / (A (1 - rho)) with Phi = 4 pi I and A
// the mesh's area, on top of its direct E_d; its walls therefore show L = rho / pi (E_d + E_ind).
//
// The camera sits at the centre with the light (cameras are not geometry), aimed at the centroid
// of the facet nearest the equator at longitude 0 with a 1 degree view: the view reaches 0.71
// degrees from the centroid at its corners, inside the facet, whose sides are 0.88 degrees and
// more from its centroid (a right triangle with legs of 3.75 degrees). Across that view E_d
// changes by less than 0.1%.
//
// The facets make the inter-reflected light nearly but not exactly uniform: test_lux.cpp finds
// about 0.3% between facets for a source at the centre. Allowed here as in the lux meter's checks.
constexpr double sphere_intensity_cd = 1000.0;
constexpr double sphere_flux_lm = 4.0 * pi * sphere_intensity_cd;
constexpr double sphere_tolerance = 0.003;

struct SphereRender {
    double area_m2 = 0.0;
    double direct_lux = 0.0; // E_d where the centre ray meets the wall
    Stats stats;
    cs::Image image{1, 1};
};

SphereRender render_sphere(double rho, bool direct_only, int size, std::uint32_t spp,
                           std::uint64_t seed, cs::ThreadPool* pool) {
    const geom::Mesh sphere = faceted_sphere(1.0, 96, 48);
    REQUIRE(geom::is_closed(sphere));

    world::World w;
    add_lambert(w, sphere, rho);
    w.lights.push_back(isotropic_light(dvec3(0.0), sphere_intensity_cd));
    const Scene scene(w);

    const auto centroid = [&](std::size_t t) {
        return (sphere.positions[sphere.indices[3 * t]] +
                sphere.positions[sphere.indices[3 * t + 1]] +
                sphere.positions[sphere.indices[3 * t + 2]]) /
               3.0;
    };
    std::size_t best = 0;
    for (std::size_t t = 1; t < sphere.indices.size() / 3; ++t) {
        if (centroid(t).x > centroid(best).x) {
            best = t;
        }
    }
    const dvec3 forward = glm::normalize(centroid(best));
    const dvec3 normal = sphere.normals[sphere.indices[3 * best]]; // facing in, to the light

    const std::optional<cs::engine::Hit> hit = scene.intersect(dvec3(0.0), forward);
    REQUIRE(hit.has_value());
    CHECK(hit->triangle == best);

    world::Camera camera;
    camera.position_m = dvec3(0.0);
    camera.forward = forward;
    camera.up = {0.0, 0.0, 1.0};
    camera.vertical_fov_rad = 1.0 * degrees;

    RenderOptions o = options(size, size, spp, seed);
    o.direct_only = direct_only;
    o.pool = pool;
    SphereRender r;
    r.area_m2 = geom::area(sphere);
    r.direct_lux = cs::engine::direct_lux(scene, hit->position_m, normal);
    r.image = render(scene, camera, o);
    r.stats = luminance_stats(r.image);
    return r;
}

double sphere_indirect_lux(double rho, double area_m2) {
    return rho * sphere_flux_lm / (area_m2 * (1.0 - rho));
}

// The issue's 1%, and the facets' 0.3% plus 3 standard errors of the image's mean; the standard
// error must itself be small enough for that to mean something.
void check_sphere_wall(const SphereRender& r, double expected) {
    INFO("mean " << r.stats.mean << " +- " << r.stats.standard_error << " nits, expected "
                 << expected << "; pixels from " << r.stats.min << " to " << r.stats.max);
    CHECK(all_finite_and_non_negative(r.image));
    CHECK(r.stats.standard_error < 0.003 * expected);
    CHECK_THAT(r.stats.mean, WithinRel(expected, issue_tolerance));
    CHECK(std::abs(r.stats.mean - expected) <=
          sphere_tolerance * expected + 3.0 * r.stats.standard_error);
}

} // namespace

TEST_CASE("render: the integrating sphere's wall shows rho / pi (E_direct + rho Phi / (A (1 - "
          "rho))), within 1%, at albedo 0.8",
          "[engine][path_tracer][accuracy]") {
    // The owner's comment on #77. At rho = 0.8 the inter-reflected light is 4 times Phi / A,
    // about 4000 lux on top of the 1000 direct: L = 0.8 / pi x 5000 = 1270 nits. Paths survive
    // Russian roulette with probability 0.8 a bounce, about 8 hits on average; 16 x 16 pixels of
    // 512 samples (131,072 paths) bring the mean's standard error to about 0.15%.
    constexpr double rho = 0.8;
    cs::ThreadPool pool(hardware_threads());
    const SphereRender r = render_sphere(rho, false, 16, 512, 8, &pool);
    CHECK_THAT(r.direct_lux, WithinRel(1000.0, 0.01)); // I / R^2, the facet a little closer
    check_sphere_wall(r, rho / pi * (r.direct_lux + sphere_indirect_lux(rho, r.area_m2)));
}

TEST_CASE("render: the integrating sphere at albedo 0.95, where most light has bounced dozens of "
          "times, still shows the full luminance: the bounce cap does not under-read",
          "[engine][path_tracer][accuracy]") {
    // The owner's comment: M1-15's review found that a cap of 64 bounces under-read a room of
    // albedo 0.95 by 3.5% (0.95^65); the path tracer's cap is 1024. Here E_ind = 19 Phi / A,
    // about 19,000 lux against 1000 direct: L = 0.95 / pi x 20,000 = 6050 nits, 95% of it
    // inter-reflected. A path survives roulette with probability 0.95 a bounce, so its number of
    // hits is nearly geometric with mean 20 and a relative spread near 1; 24 x 24 pixels of 512
    // samples (294,912 paths) bring the mean's standard error to about 0.2%.
    static_assert(cs::engine::max_path_bounces >= 1024);
    constexpr double rho = 0.95;
    cs::ThreadPool pool(hardware_threads());
    const SphereRender r = render_sphere(rho, false, 24, 512, 9, &pool);
    check_sphere_wall(r, rho / pi * (r.direct_lux + sphere_indirect_lux(rho, r.area_m2)));
}

TEST_CASE("render: direct_only, the integrating sphere's wall shows rho E_direct / pi, less "
          "than the full render",
          "[engine][path_tracer][accuracy]") {
    // The issue's direct_only criterion: no indirect bounces. The wall then shows only the light
    // that reached it straight from the source, rho / pi x E_d, about 0.8 / pi x 1000 = 255 nits.
    // There is no Monte Carlo spread but the pixel's footprint, and E_d varies by less than 0.1%
    // over the 1 degree view, so every pixel, not only the mean, is held to the issue's 1%.
    constexpr double rho = 0.8;
    cs::ThreadPool pool(hardware_threads());
    const SphereRender direct = render_sphere(rho, true, 16, 16, 10, &pool);
    const double expected = rho / pi * direct.direct_lux;
    INFO("direct_only mean " << direct.stats.mean << ", expected " << expected);
    CHECK(all_finite_and_non_negative(direct.image));
    CHECK_THAT(direct.stats.mean, WithinRel(expected, issue_tolerance));
    CHECK(pixels_off(direct.image, expected, issue_tolerance) == 0);

    // The full render adds the inter-reflected 4000 lux: five times as bright. Fewer samples are
    // enough to tell.
    const SphereRender full = render_sphere(rho, false, 8, 64, 10, &pool);
    INFO("full mean " << full.stats.mean);
    CHECK(full.stats.mean > 2.0 * direct.stats.mean);
}

// ---- Seeds, samples and threads ----------------------------------------------------------------

TEST_CASE("render: images are bit-identical with 1 and 8 threads, and with none, for the same "
          "seed",
          "[engine][path_tracer]") {
    // The issue's criterion: every pixel is accumulated by one thread in sample order
    // (path_tracer.hpp), so the thread count cannot change a bit. A closed room lit by a spot,
    // where paths bounce and are cut by roulette, in a non-square image.
    const Scene scene(room_with_ceiling_spot());
    const world::Camera camera = room_camera();
    cs::ThreadPool one(1);
    cs::ThreadPool eight(8);

    const bool direct_only = GENERATE(false, true);
    INFO("direct_only " << direct_only);
    RenderOptions o = options(24, 16, 8, 42);
    o.direct_only = direct_only;
    o.pool = &one;
    const cs::Image a = render(scene, camera, o);
    o.pool = &eight;
    const cs::Image b = render(scene, camera, o);
    o.pool = nullptr; // all on the calling thread
    const cs::Image c = render(scene, camera, o);

    REQUIRE(a.width() == 24);
    REQUIRE(a.height() == 16);
    CHECK(all_finite_and_non_negative(a));
    CHECK(luminance_stats(a).max > 0.0); // the spot lights the room: not a trivially black image
    CHECK(same_bits(a, b));
    CHECK(same_bits(a, c));
}

TEST_CASE("render: the same seed gives the same image, another seed another",
          "[engine][path_tracer]") {
    const Scene scene(room_with_ceiling_spot());
    const world::Camera camera = room_camera();
    cs::ThreadPool pool(4);
    RenderOptions o = options(24, 16, 8, 1);
    o.pool = &pool;
    const cs::Image a = render(scene, camera, o);
    const cs::Image again = render(scene, camera, o);
    o.seed = 2;
    const cs::Image b = render(scene, camera, o);

    CHECK(same_bits(a, again));
    // Every pixel jitters its samples and bounces its paths at random: with another seed, pixels
    // agree to the last bit only by an astronomically unlikely chance. Most must differ.
    const int differing = differing_pixels(a, b);
    INFO(differing << " of 384 pixels differ");
    CHECK(differing > 384 / 2);
}

TEST_CASE("render: rendering a world is rendering the scene built from it",
          "[engine][path_tracer]") {
    // path_tracer.hpp: the World overload builds the scene first; same input, same image.
    const world::World w = room_with_ceiling_spot();
    const Scene scene(w);
    const RenderOptions o = options(24, 16, 4, 3);
    CHECK(same_bits(render(w, room_camera(), o), render(scene, room_camera(), o)));
}

TEST_CASE("ProgressiveRender: 3 then 5 samples give the image of one render of 8, bit for bit",
          "[engine][path_tracer]") {
    // path_tracer.hpp: sample s of every pixel draws from its own generator and is added in
    // sample order, so batches end where one render of the same total ends. The progressive
    // render runs on a pool, the reference on the calling thread.
    const Scene scene(room_with_ceiling_spot());
    const world::Camera camera = room_camera();
    cs::ThreadPool pool(8);

    RenderOptions o = options(24, 16, 1, 5);
    o.pool = &pool;
    ProgressiveRender progressive(scene, camera, o);
    CHECK(progressive.samples() == 0);

    progressive.add_samples(3);
    CHECK(progressive.samples() == 3);
    RenderOptions reference = options(24, 16, 3, 5);
    CHECK(same_bits(progressive.image(), render(scene, camera, reference)));

    progressive.add_samples(5);
    CHECK(progressive.samples() == 8);
    reference.samples_per_pixel = 8;
    const cs::Image eight = render(scene, camera, reference);
    CHECK(same_bits(progressive.image(), eight));

    // One sample at a time, eight times: the same again.
    ProgressiveRender one_by_one(scene, camera, o);
    for (int i = 0; i < 8; ++i) {
        one_by_one.add_samples(1);
    }
    CHECK(one_by_one.samples() == 8);
    CHECK(same_bits(one_by_one.image(), eight));

    // A moved render keeps its samples and goes on from them.
    ProgressiveRender moved = std::move(progressive);
    CHECK(moved.samples() == 8);
    CHECK(same_bits(moved.image(), eight));
    moved.add_samples(2);
    CHECK(moved.samples() == 10);
    reference.samples_per_pixel = 10;
    CHECK(same_bits(moved.image(), render(scene, camera, reference)));
}

TEST_CASE("ProgressiveRender: the image is black before the first sample, at the options' size",
          "[engine][path_tracer]") {
    // An empty world in a 50-nit environment: every ray sees 50 nits, but only once it is traced.
    const Scene scene(world::World{});
    const world::Camera camera = room_camera();
    RenderOptions o = options(7, 5, 0, 1); // samples_per_pixel is not used by ProgressiveRender
    o.environment_nits = 50.0;
    ProgressiveRender progressive(scene, camera, o);
    const cs::Image before = progressive.image();
    CHECK(before.width() == 7);
    CHECK(before.height() == 5);
    CHECK(all_equal_to(before, 0.0f));

    progressive.add_samples(1);
    CHECK(all_equal_to(progressive.image(), 50.0f));
}

// ---- Darkness and the environment --------------------------------------------------------------

TEST_CASE("render: a scene with no lights and no environment renders black",
          "[engine][path_tracer]") {
    const Scene scene(closed_room());
    const cs::Image image = render(scene, room_camera(), options(24, 16, 4, 1));
    CHECK(all_equal_to(image, 0.0f));

    // An open scene too: rays that leave it find the default environment, 0 nits.
    world::World open;
    add_lambert(open, large_floor(), 0.5);
    const Scene floor(open);
    const world::Camera horizontal =
        perspective({0.0, 0.0, 1.0}, {0.0, 1.0, 1.0}, {0.0, 0.0, 1.0}, 60.0 * degrees);
    CHECK(all_equal_to(render(floor, horizontal, options(16, 16, 4, 1)), 0.0f));
}

TEST_CASE("render: a camera ray that misses everything sees environment_nits exactly",
          "[engine][path_tracer]") {
    // 37.5 nits and 8 samples: the mean of eight 37.5s is 37.5 exactly in double and in float,
    // however it is summed, so the pixels must be exactly (37.5, 37.5, 37.5), white.
    constexpr double nits = 37.5;
    SECTION("an empty world") {
        const Scene scene(world::World{});
        RenderOptions o = options(16, 12, 8, 1);
        o.environment_nits = nits;
        CHECK(all_equal_to(render(scene, room_camera(), o), static_cast<float>(nits)));
        o.direct_only = true; // the environment seen straight through the pixel is kept
        CHECK(all_equal_to(render(scene, room_camera(), o), static_cast<float>(nits)));
    }
    SECTION("the sky above a floor, which reflects half of it below") {
        // Looking horizontally from 1 m above a floor of albedo 0.5 and no lights: the upper half
        // of the image looks at the sky. The lower half sees the floor, which reflects half the
        // environment: every bounce off it leaves the scene, so each path carries 0.5 x 37.5 =
        // 18.75 nits, up to the rounding of the BRDF's weight (1e-6). Row 8, just below the
        // horizon, is left out: its rays within atan(1 / 500) = 0.11 degrees of the horizontal
        // pass beyond the 1000 m floor's edge and see the sky. Row 9 starts 4.1 degrees down and
        // meets the floor within 14 m.
        world::World w;
        add_lambert(w, large_floor(), 0.5);
        const Scene scene(w);
        const world::Camera camera =
            perspective({0.0, 0.0, 1.0}, {0.0, 1.0, 1.0}, {0.0, 0.0, 1.0}, 60.0 * degrees);
        RenderOptions o = options(16, 16, 8, 1);
        o.environment_nits = nits;
        const cs::Image image = render(scene, camera, o);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const cs::Rgb& p = image.at(x, y);
                INFO("pixel (" << x << ", " << y << ")");
                if (y < 8) {
                    CHECK(p.r == static_cast<float>(nits));
                    CHECK(p.g == static_cast<float>(nits));
                    CHECK(p.b == static_cast<float>(nits));
                } else if (y == 8) {
                    CHECK(luminance_of(p) >= 0.5 * nits * (1.0 - 1e-6));
                    CHECK(luminance_of(p) <= nits * (1.0 + 1e-6));
                } else {
                    CHECK_THAT(p.r, WithinRel(0.5 * nits, 1e-6));
                    CHECK_THAT(p.g, WithinRel(0.5 * nits, 1e-6));
                    CHECK_THAT(p.b, WithinRel(0.5 * nits, 1e-6));
                }
            }
        }
    }
}

TEST_CASE("render: the environment does not leak into a closed room", "[engine][path_tracer]") {
    // A camera inside the closed room, a bright environment outside it and no lights: a
    // watertight room (geom/mesh.hpp) lets no path out, so the image is black but for leaks at
    // the room's edges and corners. One leaked path would add 1000 / (24 x 16 x 8) = 0.33 nits to
    // the image's mean; asking for less than 0.1 nits allows none.
    const Scene scene(closed_room());
    RenderOptions o = options(24, 16, 8, 1);
    o.environment_nits = 1000.0;
    const cs::Image image = render(scene, room_camera(), o);
    CHECK(all_finite_and_non_negative(image));
    CHECK(luminance_stats(image).mean < 0.1);
}

// ---- Input -------------------------------------------------------------------------------------

namespace {

// Both entry points must refuse the same arguments: render and ProgressiveRender's constructor.
void check_both_throw(const Scene& scene, const world::Camera& camera, const RenderOptions& o) {
    CHECK_THROWS_AS(render(scene, camera, o), std::invalid_argument);
    CHECK_THROWS_AS(ProgressiveRender(scene, camera, o), std::invalid_argument);
}

} // namespace

TEST_CASE("render: a size outside [1, max_image_side], or more than max_image_pixels, throws "
          "std::invalid_argument",
          "[engine][path_tracer]") {
    const Scene scene(world::World{});
    const world::Camera camera = room_camera();
    struct Size {
        int width;
        int height;
    };
    const Size size = GENERATE(
        Size{0, 16}, Size{16, 0}, Size{-1, 16}, Size{16, -5}, Size{cs::max_image_side + 1, 1},
        Size{1, cs::max_image_side + 1},
        // 2^28 pixels, both sides within the limit
        Size{cs::max_image_side, cs::max_image_side},
        // one row more than max_image_pixels allows at full width
        Size{cs::max_image_side, static_cast<int>(cs::max_image_pixels / cs::max_image_side) + 1},
        Size{std::numeric_limits<int>::max(), 1}, Size{std::numeric_limits<int>::min(), 1});
    INFO(size.width << " x " << size.height);
    check_both_throw(scene, camera, options(size.width, size.height, 1, 1));
}

TEST_CASE("render: the largest side is accepted", "[engine][path_tracer]") {
    // max_image_side x 1 and 1 x max_image_side: within both limits. An empty world, one sample.
    const Scene scene(world::World{});
    const world::Camera camera = room_camera();
    const cs::Image wide = render(scene, camera, options(cs::max_image_side, 1, 1, 1));
    CHECK(wide.width() == cs::max_image_side);
    CHECK(wide.height() == 1);
    CHECK_NOTHROW(ProgressiveRender(scene, camera, options(1, cs::max_image_side, 1, 1)));
}

TEST_CASE("render: 0 samples per pixel throws std::invalid_argument; ProgressiveRender ignores "
          "it",
          "[engine][path_tracer]") {
    const Scene scene(world::World{});
    const world::Camera camera = room_camera();
    CHECK_THROWS_AS(render(scene, camera, options(8, 8, 0, 1)), std::invalid_argument);
    CHECK_NOTHROW(ProgressiveRender(scene, camera, options(8, 8, 0, 1)));
}

TEST_CASE("render: an environment that is negative or not finite throws std::invalid_argument",
          "[engine][path_tracer]") {
    const Scene scene(world::World{});
    const world::Camera camera = room_camera();
    const double nits = GENERATE(-1.0, -1e-300, not_a_number, inf, -inf);
    INFO("environment_nits " << nits);
    RenderOptions o = options(8, 8, 1, 1);
    o.environment_nits = nits;
    check_both_throw(scene, camera, o);
}

TEST_CASE("render: a camera it cannot use throws std::invalid_argument", "[engine][path_tracer]") {
    const Scene scene(world::World{});
    const world::Camera good = room_camera();
    const RenderOptions o = options(8, 8, 1, 1);
    REQUIRE_NOTHROW(render(scene, good, o));

    struct Case {
        std::string what;
        world::Camera camera;
    };
    std::vector<Case> cases;
    const auto with = [&](std::string what, auto change) {
        world::Camera c = good;
        change(c);
        cases.push_back({std::move(what), c});
    };
    with("position NaN", [](world::Camera& c) { c.position_m.x = not_a_number; });
    with("position infinite", [](world::Camera& c) { c.position_m.z = inf; });
    with("forward zero", [](world::Camera& c) { c.forward = dvec3(0.0); });
    with("forward NaN", [](world::Camera& c) { c.forward.y = not_a_number; });
    with("forward infinite", [](world::Camera& c) { c.forward = {inf, 0.0, 0.0}; });
    with("up zero", [](world::Camera& c) { c.up = dvec3(0.0); });
    with("up NaN", [](world::Camera& c) { c.up.z = not_a_number; });
    with("up infinite", [](world::Camera& c) { c.up = {0.0, 0.0, -inf}; });
    with("forward parallel to up", [](world::Camera& c) {
        c.forward = {0.0, 0.0, 2.0};
        c.up = {0.0, 0.0, 1.0};
    });
    with("forward opposite to up", [](world::Camera& c) {
        c.forward = {0.0, 0.0, -1.0};
        c.up = {0.0, 0.0, 3.0};
    });
    with("fov 0", [](world::Camera& c) { c.vertical_fov_rad = 0.0; });
    with("fov pi", [](world::Camera& c) { c.vertical_fov_rad = pi; });
    with("fov above pi", [](world::Camera& c) { c.vertical_fov_rad = 4.0; });
    with("fov negative", [](world::Camera& c) { c.vertical_fov_rad = -0.5; });
    with("fov NaN", [](world::Camera& c) { c.vertical_fov_rad = not_a_number; });
    with("fov infinite", [](world::Camera& c) { c.vertical_fov_rad = inf; });
    for (const double extent : {0.0, -1.0, not_a_number, inf}) {
        with("orthographic extent " + std::to_string(extent), [extent](world::Camera& c) {
            c.projection = world::Projection::orthographic;
            c.vertical_extent_m = extent;
        });
    }

    for (const Case& c : cases) {
        INFO(c.what);
        check_both_throw(scene, c.camera, o);
    }
}

TEST_CASE("render: forward and up need not be unit or perpendicular", "[engine][path_tracer]") {
    // path_tracer.hpp: the image's up is up made perpendicular to forward. Looking straight down
    // with forward (0, 0, -2) and up (0, 1, 0.7) is looking down with up (0, 1, 0): the same image
    // but for rounding in the rays' directions (1e-5 allows for float rays). An orthographic
    // camera ignores vertical_fov_rad, so 0 is fine there.
    const Scene scene(lit_floor_world());
    const world::Camera unit =
        perspective({0.0, 0.0, 1.0}, dvec3(0.0), {0.0, 1.0, 0.0}, 60.0 * degrees);
    world::Camera loose = unit;
    loose.forward = {0.0, 0.0, -2.0};
    loose.up = {0.0, 1.0, 0.7};
    const RenderOptions o = options(16, 16, 4, 11);
    const cs::Image a = render(scene, unit, o);
    const cs::Image b = render(scene, loose, o);
    int off = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const double la = luminance_of(a.at(x, y));
            const double lb = luminance_of(b.at(x, y));
            if (!(std::abs(la - lb) <= 1e-5 * la)) {
                ++off;
                UNSCOPED_INFO("pixel (" << x << ", " << y << "): " << la << " and " << lb);
            }
        }
    }
    CHECK(off == 0);

    world::Camera orthographic = unit;
    orthographic.projection = world::Projection::orthographic;
    orthographic.vertical_fov_rad = 0.0;
    orthographic.vertical_extent_m = 1.0;
    CHECK_NOTHROW(render(scene, orthographic, o));
}
