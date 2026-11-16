// SPDX-License-Identifier: Apache-2.0

// cs-render: a project folder to a path-traced image and a lux report, headless.
//
//   cs-render <project-folder> --tier path --size 256x144 --spp 64 --seed 1 --out <prefix>
//
// writes <prefix>.exr (linear Rec. 709 radiance in nits), <prefix>.png (exposed at the
// viewpoint's EV100, or metered from the image when it has none, tone mapped with PBR Neutral,
// sRGB) and <prefix>.lux.json (per exhibit, the lux on its 5 x 5 grid, engine::work_lux_grid).
// The image is the first viewpoint's. Everything is seeded: the same command gives the same
// files whatever the machine's thread count.

#include <cs/core/colour.hpp>
#include <cs/core/image.hpp>
#include <cs/core/image_io.hpp>
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

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view usage =
    "usage: cs-render <project-folder> --out <prefix> [--tier path] [--size 256x144]\n"
    "                 [--spp 64] [--seed 1] [--lux-samples 1024] [--threads <n>]\n"
    "\n"
    "Writes <prefix>.exr (linear nits), <prefix>.png (EV100 exposure, PBR Neutral, sRGB) and\n"
    "<prefix>.lux.json (the lux on every exhibit). The Standard tier arrives with M1-29.\n";

// Bounds on what the command line may ask for.
constexpr std::uint32_t max_spp = 1u << 20;
constexpr std::uint32_t max_lux_samples = 1u << 24;
constexpr unsigned max_threads = 1024;

// A usage error: exit code 2 and the usage text.
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Arguments {
    fs::path project;
    fs::path out;
    int width = 256;
    int height = 144;
    std::uint32_t spp = 64;
    std::uint64_t seed = 1;
    std::uint32_t lux_samples = 1024;
    unsigned threads = 0; // 0: the machine's
};

template <typename T> T parse_number(std::string_view flag, std::string_view text, T low, T high) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || value < low || value > high) {
        throw UsageError(std::string(flag) + " takes a whole number from " + std::to_string(low) +
                         " to " + std::to_string(high) + ", not \"" + std::string(text) + "\"");
    }
    return value;
}

Arguments parse_arguments(int argc, char* argv[]) {
    Arguments arguments;
    bool have_project = false;
    bool have_out = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << usage;
            std::exit(0);
        }
        if (!arg.starts_with("--")) {
            if (have_project) {
                throw UsageError("more than one project folder: \"" + std::string(arg) + "\"");
            }
            arguments.project = fs::path(arg);
            have_project = true;
            continue;
        }
        if (i + 1 >= argc) {
            throw UsageError(std::string(arg) + " needs a value");
        }
        const std::string_view value = argv[++i];
        if (arg == "--tier") {
            if (value == "standard") {
                throw UsageError(
                    "the Standard tier is not in cs-render yet (M1-29); use --tier path");
            }
            if (value != "path") {
                throw UsageError("--tier is path or standard, not \"" + std::string(value) + "\"");
            }
        } else if (arg == "--size") {
            const std::size_t x = value.find('x');
            if (x == std::string_view::npos) {
                throw UsageError("--size is <width>x<height>, such as 256x144");
            }
            arguments.width = parse_number("--size", value.substr(0, x), 1, cs::max_image_side);
            arguments.height = parse_number("--size", value.substr(x + 1), 1, cs::max_image_side);
            if (std::int64_t{arguments.width} * arguments.height > cs::max_image_pixels) {
                throw UsageError("--size is larger than " + std::to_string(cs::max_image_pixels) +
                                 " pixels");
            }
        } else if (arg == "--spp") {
            arguments.spp = parse_number<std::uint32_t>("--spp", value, 1, max_spp);
        } else if (arg == "--seed") {
            arguments.seed = parse_number<std::uint64_t>("--seed", value, 0,
                                                         std::numeric_limits<std::uint64_t>::max());
        } else if (arg == "--lux-samples") {
            arguments.lux_samples =
                parse_number<std::uint32_t>("--lux-samples", value, 1, max_lux_samples);
        } else if (arg == "--threads") {
            arguments.threads = parse_number<unsigned>("--threads", value, 1, max_threads);
        } else if (arg == "--out") {
            arguments.out = fs::path(value);
            have_out = !arguments.out.empty();
        } else {
            throw UsageError("unknown option " + std::string(arg));
        }
    }
    if (!have_project) {
        throw UsageError("no project folder");
    }
    if (!have_out) {
        throw UsageError("no --out prefix");
    }
    return arguments;
}

// <prefix><suffix>: the prefix is a path whose last part the suffix extends.
fs::path with_suffix(const fs::path& prefix, std::string_view suffix) {
    fs::path path = prefix;
    path += suffix;
    return path;
}

double mean_luminance(const cs::Image& image) {
    double sum = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const cs::Rgb& p = image.at(x, y);
            sum += cs::luminance(glm::dvec3(p.r, p.g, p.b));
        }
    }
    return sum / (static_cast<double>(image.width()) * image.height());
}

json vec(glm::dvec2 v) {
    return json::array({v.x, v.y});
}

json vec(glm::dvec3 v) {
    return json::array({v.x, v.y, v.z});
}

json summary(const cs::engine::LuxSummary& s) {
    return {{"mean_lx", s.mean},
            {"minimum_lx", s.minimum},
            {"maximum_lx", s.maximum},
            {"standardError_lx", s.standard_error}};
}

std::string entity_name(const cs::doc::Document& document, const cs::world::Id& id) {
    const cs::doc::Entity* entity = document.find_entity(cs::doc::Uuid(id.bytes()));
    return entity != nullptr ? entity->name : std::string();
}

std::string work_title(const cs::doc::Document& document, const cs::world::Id& id) {
    const cs::doc::Work* work = document.find_work(cs::doc::Uuid(id.bytes()));
    return work != nullptr ? work->title : std::string();
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

int run(const Arguments& arguments) {
    const auto start = std::chrono::steady_clock::now();
    const cs::doc::Document document = cs::doc::load_project_folder(arguments.project);
    const cs::world::World world = cs::world::compile(document);
    for (const cs::world::Problem& problem : world.problems) {
        std::cerr << "cs-render: warning: " << problem.entity.to_string() << ": " << problem.reason
                  << '\n';
    }
    if (world.cameras.empty()) {
        throw std::runtime_error("the project has no viewpoint to render from");
    }
    const cs::world::Camera& camera = world.cameras.front();

    const cs::engine::LoadedTextures loaded =
        cs::engine::load_textures(world, arguments.project / "assets");
    for (const cs::engine::TextureProblem& problem : loaded.problems) {
        std::cerr << "cs-render: warning: image " << problem.asset.sha256 << "."
                  << problem.asset.extension << " is shown as grey: " << problem.reason << '\n';
    }

    const unsigned threads = arguments.threads != 0
                                 ? arguments.threads
                                 : std::max(1u, std::thread::hardware_concurrency());
    cs::ThreadPool pool(threads);
    const cs::engine::Scene scene(world);
    const double load_s = seconds_since(start);

    const auto render_start = std::chrono::steady_clock::now();
    cs::engine::RenderOptions render_options;
    render_options.width = arguments.width;
    render_options.height = arguments.height;
    render_options.samples_per_pixel = arguments.spp;
    render_options.seed = arguments.seed;
    render_options.pool = &pool;
    render_options.textures = &loaded.textures;
    const cs::Image image = cs::engine::render(scene, camera, render_options);
    const double render_s = seconds_since(render_start);

    const auto lux_start = std::chrono::steady_clock::now();
    cs::engine::LuxOptions lux_options;
    lux_options.samples = arguments.lux_samples;
    lux_options.seed = arguments.seed;
    lux_options.pool = &pool;
    lux_options.textures = &loaded.textures;
    json exhibits = json::array();
    for (const cs::world::Exhibit& exhibit : world.exhibits) {
        const cs::engine::WorkLux lux = cs::engine::work_lux_grid(scene, exhibit, lux_options);
        json points = json::array();
        for (const cs::engine::WorkLuxPoint& point : lux.points) {
            points.push_back({{"face_m", vec(point.face_m)},
                              {"position_m", vec(point.point_m)},
                              {"direct_lx", point.reading.direct_lux.value},
                              {"total_lx", point.reading.total_lux.value},
                              {"totalStandardError_lx", point.reading.total_lux.standard_error}});
        }
        exhibits.push_back(
            {{"entity", exhibit.entity.to_string()},
             {"name", entity_name(document, exhibit.entity)},
             {"work", exhibit.work.to_string()},
             {"title", work_title(document, exhibit.work)},
             {"visible_m",
              {{"min", vec(exhibit.visible.min_m)}, {"max", vec(exhibit.visible.max_m)}}},
             {"direct", summary(lux.direct_lux)},
             {"total", summary(lux.total_lux)},
             {"points", std::move(points)}});
    }
    const double lux_s = seconds_since(lux_start);

    const bool automatic = !camera.ev100.has_value();
    const double ev100 =
        automatic ? cs::ev100_from_average_luminance(mean_luminance(image)) : *camera.ev100;

    json problems = json::array();
    for (const cs::world::Problem& problem : world.problems) {
        problems.push_back({{"entity", problem.entity.to_string()}, {"reason", problem.reason}});
    }
    for (const cs::engine::TextureProblem& problem : loaded.problems) {
        problems.push_back({{"asset", problem.asset.sha256 + "." + problem.asset.extension},
                            {"reason", problem.reason}});
    }
    const json report = {
        {"tier", "path"},
        {"seed", arguments.seed},
        {"luxSamples", arguments.lux_samples},
        {"luxGrid",
         {{"columns", cs::engine::work_lux_grid_side}, {"rows", cs::engine::work_lux_grid_side}}},
        {"image",
         {{"width", arguments.width},
          {"height", arguments.height},
          {"samplesPerPixel", arguments.spp},
          {"ev100", ev100},
          {"automaticExposure", automatic}}},
        {"exhibits", std::move(exhibits)},
        {"problems", std::move(problems)}};

    if (arguments.out.has_parent_path()) {
        fs::create_directories(arguments.out.parent_path());
    }
    const fs::path exr = with_suffix(arguments.out, ".exr");
    const fs::path png = with_suffix(arguments.out, ".png");
    const fs::path lux_json = with_suffix(arguments.out, ".lux.json");
    cs::write_exr(image, exr, cs::ExrChannels::float32);
    cs::write_png(image, png, ev100);
    {
        std::ofstream file(lux_json, std::ios::binary | std::ios::trunc);
        file << report.dump(2) << '\n';
        if (!file) {
            throw std::runtime_error("cannot write " + lux_json.string());
        }
    }

    std::cout << "cs-render: " << arguments.width << " x " << arguments.height << " at "
              << arguments.spp << " spp on " << threads << " threads: load " << load_s
              << " s, render " << render_s << " s, lux " << lux_s << " s, total "
              << seconds_since(start) << " s\n"
              << "  " << exr.string() << "\n  " << png.string() << " (EV100 " << ev100
              << (automatic ? ", automatic" : "") << ")\n  " << lux_json.string() << '\n';
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parse_arguments(argc, argv));
    } catch (const UsageError& error) {
        std::cerr << "cs-render: " << error.what() << "\n\n" << usage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "cs-render: " << error.what() << '\n';
        return 1;
    }
}
