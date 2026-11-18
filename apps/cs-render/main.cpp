// SPDX-License-Identifier: Apache-2.0

// cs-render: a project folder to an image and a lux report, headless.
//
//   cs-render <project-folder> --tier path|standard --size 256x144 --spp 64 --seed 1 --out <prefix>
//
// writes <prefix>.exr (linear Rec. 709 radiance in nits), <prefix>.png (exposed at the
// viewpoint's EV100, or metered from the image when it has none, tone mapped with PBR Neutral,
// sRGB) and <prefix>.lux.json (per exhibit, the lux on its 5 x 5 grid, engine::work_lux_grid).
// The image is the first viewpoint's, path traced by the engine on the CPU (--tier path, the
// default) or drawn by the Standard tier on the GPU (--tier standard: one frame, offscreen, so
// --spp is the path tier's alone). The lux always comes from the engine, never from pixels.
// Everything is seeded: the same command gives the same files whatever the machine's thread
// count.
//
//   cs-render golden <test-folder> [--update]
//
// runs a golden-image test instead (golden.hpp, tests/golden/README.md), and
//
//   cs-render --device-report
//
// prints the report of the Vulkan device the Standard tier renders on, for bug reports and CI's
// logs, and the name of its golden references.

#include "common.hpp"
#include "golden.hpp"
#include "standard.hpp"

#include <cs/core/image.hpp>
#include <cs/core/image_io.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/doc/document.hpp>
#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/work_lux.hpp>
#include <cs/gpu/device.hpp>
#include <cs/world/world.hpp>

#include <nlohmann/json.hpp>

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

namespace {

namespace fs = std::filesystem;
using cs_render::UsageError;
using nlohmann::json;

constexpr std::string_view usage =
    "usage: cs-render <project-folder> --out <prefix> [--tier path|standard] [--size 256x144]\n"
    "                 [--spp 64] [--seed 1] [--direct-only] [--lux-samples 1024]\n"
    "                 [--threads <n>]\n"
    "       cs-render golden <test-folder> [--update] [--out <folder>] [--threads <n>]\n"
    "       cs-render --device-report\n"
    "\n"
    "Writes <prefix>.exr (linear nits), <prefix>.png (EV100 exposure, PBR Neutral, sRGB) and\n"
    "<prefix>.lux.json (the lux on every exhibit, from the engine).\n"
    "\n"
    "--tier path traces the image on the CPU with --spp samples per pixel; with --direct-only,\n"
    "only the light that reflects once, which the Standard tier evaluates. --tier standard\n"
    "draws it on the GPU, one frame. --device-report prints the Vulkan device the Standard\n"
    "tier uses (CS_GPU_DEVICE chooses one). `cs-render golden --help` explains the golden-image\n"
    "tests.\n";

// Bounds on what the command line may ask for.
constexpr std::uint32_t max_lux_samples = 1u << 24;

struct Arguments {
    fs::path project;
    fs::path out;
    cs_render::Tier tier = cs_render::Tier::path;
    cs_render::ImageSettings image;
    std::uint32_t lux_samples = 1024;
    unsigned threads = 0; // 0: the machine's
};

Arguments parse_arguments(int argc, char* argv[]) {
    using cs_render::parse_number;
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << usage;
            std::exit(0);
        }
        if (arg == "--device-report") {
            throw UsageError("--device-report takes nothing else: cs-render --device-report");
        }
        if (arg == "--direct-only") {
            arguments.image.direct_only = true; // the Standard tier's light is only direct anyway
            continue;
        }
        if (!arg.starts_with("--")) {
            if (!arguments.project.empty()) {
                throw UsageError("more than one project folder: \"" + std::string(arg) + "\"");
            }
            arguments.project = fs::path(arg);
            continue;
        }
        if (i + 1 >= argc) {
            throw UsageError(std::string(arg) + " needs a value");
        }
        const std::string_view value = argv[++i];
        if (arg == "--tier") {
            const std::optional<cs_render::Tier> tier = cs_render::parse_tier(value);
            if (!tier) {
                throw UsageError("--tier is path or standard, not \"" + std::string(value) + "\"");
            }
            arguments.tier = *tier;
        } else if (arg == "--size") {
            const std::size_t x = value.find('x');
            if (x == std::string_view::npos) {
                throw UsageError("--size is <width>x<height>, such as 256x144");
            }
            arguments.image.width =
                parse_number("--size", value.substr(0, x), 1, cs::max_image_side);
            arguments.image.height =
                parse_number("--size", value.substr(x + 1), 1, cs::max_image_side);
            if (std::int64_t{arguments.image.width} * arguments.image.height >
                cs::max_image_pixels) {
                throw UsageError("--size is larger than " + std::to_string(cs::max_image_pixels) +
                                 " pixels");
            }
        } else if (arg == "--spp") {
            arguments.image.samples_per_pixel =
                parse_number<std::uint32_t>("--spp", value, 1, cs_render::max_spp);
        } else if (arg == "--seed") {
            arguments.image.seed = parse_number<std::uint64_t>(
                "--seed", value, 0, std::numeric_limits<std::uint64_t>::max());
        } else if (arg == "--lux-samples") {
            arguments.lux_samples =
                parse_number<std::uint32_t>("--lux-samples", value, 2, max_lux_samples);
        } else if (arg == "--threads") {
            arguments.threads =
                parse_number<unsigned>("--threads", value, 1, cs_render::max_threads);
        } else if (arg == "--out") {
            arguments.out = fs::path(value);
        } else {
            throw UsageError("unknown option " + std::string(arg));
        }
    }
    if (arguments.project.empty()) {
        throw UsageError("no project folder");
    }
    if (!arguments.out.has_filename()) {
        throw UsageError("--out is a path prefix ending in a name, such as build/out/skeleton");
    }
    return arguments;
}

// <prefix><suffix>: the prefix is a path whose last part the suffix extends.
fs::path with_suffix(const fs::path& prefix, std::string_view suffix) {
    fs::path path = prefix;
    path += suffix;
    return path;
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
    const cs_render::Project project = cs_render::load_project(arguments.project);
    const cs::world::World& world = project.world;
    const unsigned threads = cs_render::thread_count(arguments.threads);
    cs::ThreadPool pool(threads);
    const cs::engine::Scene scene(world);
    const double load_s = seconds_since(start);

    // The Standard tier draws one frame; the device lives only while it does. A validation
    // message fails the command, as it fails a GPU test (the layer is on in debug builds).
    const bool standard = arguments.tier == cs_render::Tier::standard;
    const std::uint32_t samples_per_pixel = standard ? 1 : arguments.image.samples_per_pixel;
    std::optional<cs::gpu::DeviceReport> device_report;
    const auto render_start = std::chrono::steady_clock::now();
    const cs::Image image = [&] {
        if (!standard) {
            return cs_render::render_view(project, scene, arguments.image, pool);
        }
        const cs_render::ValidationCounter validation;
        cs::Image radiance = [&] {
            const cs::gpu::Device device(cs_render::device_options());
            device_report = device.report();
            return cs_render::render_standard(device, project, arguments.image.width,
                                              arguments.image.height);
        }();
        cs_render::throw_if_reported(validation);
        return radiance;
    }();
    const double render_s = seconds_since(render_start);

    const auto lux_start = std::chrono::steady_clock::now();
    cs::engine::LuxOptions lux_options;
    lux_options.samples = arguments.lux_samples;
    lux_options.seed = arguments.image.seed;
    lux_options.pool = &pool;
    lux_options.textures = &project.textures.textures;
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
             {"name", entity_name(project.document, exhibit.entity)},
             {"work", exhibit.work.to_string()},
             {"title", work_title(project.document, exhibit.work)},
             {"visible_m",
              {{"min", vec(exhibit.visible.min_m)}, {"max", vec(exhibit.visible.max_m)}}},
             {"direct", summary(lux.direct_lux)},
             {"total", summary(lux.total_lux)},
             {"points", std::move(points)}});
    }
    const double lux_s = seconds_since(lux_start);

    const bool automatic = !project.camera().ev100.has_value();
    const double ev100 = cs_render::display_ev100(project.camera(), image);

    json problems = json::array();
    for (const cs::world::Problem& problem : world.problems) {
        problems.push_back({{"entity", problem.entity.to_string()}, {"reason", problem.reason}});
    }
    for (const cs::engine::TextureProblem& problem : project.textures.problems) {
        problems.push_back({{"asset", problem.asset.sha256 + "." + problem.asset.extension},
                            {"reason", problem.reason}});
    }
    json report = {
        {"tier", cs_render::tier_name(arguments.tier)},
        {"seed", arguments.image.seed},
        {"luxSamples", arguments.lux_samples},
        {"luxGrid",
         {{"columns", cs::engine::work_lux_grid_side}, {"rows", cs::engine::work_lux_grid_side}}},
        {"image",
         {{"width", arguments.image.width},
          {"height", arguments.image.height},
          {"samplesPerPixel", samples_per_pixel},
          {"directOnly", standard || arguments.image.direct_only},
          {"ev100", ev100},
          {"automaticExposure", automatic}}},
        {"exhibits", std::move(exhibits)},
        {"problems", std::move(problems)}};
    if (device_report) {
        report["device"] = {{"name", device_report->name},
                            {"driver", device_report->driver},
                            {"driverInfo", device_report->driver_info},
                            {"validation", device_report->validation}};
    }

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
        // Paths in problem reasons may not be UTF-8; replace such bytes rather than fail.
        file << report.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        if (!file) {
            throw std::runtime_error("cannot write " + lux_json.string());
        }
    }

    std::cout << "cs-render: " << arguments.image.width << " x " << arguments.image.height;
    if (standard) {
        std::cout << ", Standard tier on " << device_report->name << ", lux on " << threads
                  << " threads";
    } else {
        std::cout << " at " << samples_per_pixel << " spp on " << threads << " threads";
    }
    std::cout << ": load " << load_s << " s, render " << render_s << " s, lux " << lux_s
              << " s, total " << seconds_since(start) << " s\n"
              << "  " << exr.string() << "\n  " << png.string() << " (EV100 " << ev100
              << (automatic ? ", automatic" : "") << ")\n  " << lux_json.string() << '\n';
    return 0;
}

// The device the Standard tier renders on, as cs::gpu::DeviceReport::to_string prints it, and the
// name of its golden references.
int print_device_report() {
    const cs_render::ValidationCounter validation;
    {
        const cs::gpu::Device device(cs_render::device_options());
        std::cout << device.report().to_string() << "\ngolden references: ref."
                  << cs_render::driver_label(device.report()) << ".exr\n";
    }
    cs_render::throw_if_reported(validation);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::string_view(argv[1]) == "golden") {
        return cs_render::golden_main(argc - 1, argv + 1);
    }
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--device-report") {
            return print_device_report();
        }
        return run(parse_arguments(argc, argv));
    } catch (const UsageError& error) {
        std::cerr << "cs-render: " << error.what() << "\n\n" << usage;
        return cs_render::exit_usage;
    } catch (const cs::gpu::NoDeviceError& error) {
        std::cerr << "cs-render: no Vulkan device: " << error.what() << '\n';
        return cs_render::exit_failure;
    } catch (const std::exception& error) {
        std::cerr << "cs-render: " << error.what() << '\n';
        return cs_render::exit_failure;
    }
}
