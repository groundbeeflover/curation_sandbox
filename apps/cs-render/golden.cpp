// SPDX-License-Identifier: Apache-2.0

#include "golden.hpp"

#include "common.hpp"

#include <cs/core/colour.hpp>
#include <cs/core/image.hpp>
#include <cs/core/image_compare.hpp>
#include <cs/core/image_io.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/engine/scene.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifndef CS_GOLDEN_OUT_DIR
#error "CS_GOLDEN_OUT_DIR, build/<preset>/golden-out, comes from apps/cs-render/CMakeLists.txt"
#endif

namespace cs_render {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view usage =
    "usage: cs-render golden <test-folder> [--update] [--out <folder>] [--threads <n>]\n"
    "\n"
    "Renders the scene <test-folder>/test.json describes and compares it with the reference for\n"
    "the driver that renders it, <test-folder>/ref.<driver>.exr (cpu for the path tier). Exits\n"
    "with 0 when the mean FLIP and the mean absolute error are within test.json's tolerances,\n"
    "1 when they are not or on an error, and 77 (skipped) when there is no reference.\n"
    "\n"
    "On a failure it writes render.exr, render.png, reference.png, difference.exr,\n"
    "difference.png and flip.png (the FLIP heat map) to --out, by default\n" CS_GOLDEN_OUT_DIR
    "/<name of the test folder>/.\n"
    "\n"
    "--update writes the render as the new reference, a half-float EXR, and nothing else; the\n"
    "pull request that commits it says why. Without --update, nothing in the test folder is\n"
    "written. tests/golden/README.md has the rest.\n";

// The driver that renders the path tier: the engine, on the CPU.
constexpr std::string_view engine_driver = "cpu";

// What a failed comparison writes, and a passed one removes when an earlier failure left it.
constexpr std::array<std::string_view, 6> failure_files = {
    "render.exr", "render.png", "reference.png", "difference.exr", "difference.png", "flip.png"};

// test.json is the repository's own file, but it is read as carefully as any input: bounded in
// size, and every key known, so that a misspelt tolerance is an error rather than ignored.
constexpr std::uintmax_t max_test_json_bytes = 64 * 1024;

// A reference larger than this gets a warning on --update: the blueprint keeps references at tens
// of kilobytes (section 9), and tests/golden/README.md says how to stay there.
constexpr std::uintmax_t large_reference_bytes = 60 * 1000;

struct Arguments {
    fs::path folder;
    fs::path out; // empty: CS_GOLDEN_OUT_DIR/<name>
    bool update = false;
    unsigned threads = 0; // 0: the machine's
};

struct Tolerances {
    double mean_flip = 0.0;
    double mean_absolute_error_nits = 0.0;
};

struct GoldenTest {
    fs::path scene; // a project folder
    std::string tier;
    ImageSettings image;
    Tolerances tolerances;
};

Arguments parse_arguments(int argc, char* argv[]) {
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << usage;
            std::exit(0);
        }
        if (arg == "--update") {
            arguments.update = true;
            continue;
        }
        if (!arg.starts_with("--")) {
            if (!arguments.folder.empty()) {
                throw UsageError("more than one test folder: \"" + std::string(arg) + "\"");
            }
            arguments.folder = fs::path(arg);
            continue;
        }
        if (i + 1 >= argc) {
            throw UsageError(std::string(arg) + " needs a value");
        }
        const std::string_view value = argv[++i];
        if (arg == "--out") {
            arguments.out = fs::path(value);
        } else if (arg == "--threads") {
            arguments.threads = parse_number<unsigned>("--threads", value, 1, max_threads);
        } else {
            throw UsageError("unknown option " + std::string(arg));
        }
    }
    if (arguments.folder.empty()) {
        throw UsageError("no test folder");
    }
    return arguments;
}

// Paths in messages and in test.json are UTF-8, whatever the system's narrow encoding.
std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

fs::path from_utf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// The test's name: its folder's, whichever way the folder was written (a trailing slash, ".").
std::string test_name(const fs::path& folder) {
    fs::path path = fs::absolute(folder).lexically_normal();
    if (!path.has_filename()) {
        path = path.parent_path();
    }
    return utf8(path.filename());
}

// Whether path is folder or lies inside it, after resolving "..", "." and symbolic links.
bool is_within(const fs::path& path, const fs::path& folder) {
    std::error_code error;
    const fs::path inner = fs::weakly_canonical(path, error);
    const fs::path outer = fs::weakly_canonical(folder, error);
    if (error) {
        return false;
    }
    const auto [outer_end, inner_end] =
        std::mismatch(outer.begin(), outer.end(), inner.begin(), inner.end());
    return outer_end == outer.end() ||
           (std::next(outer_end) == outer.end() && outer_end->empty()); // a trailing slash
}

std::string read_text(const fs::path& path, std::uintmax_t max_bytes) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        throw std::runtime_error("cannot read " + utf8(path) + ": " + error.message());
    }
    if (size > max_bytes) {
        throw std::runtime_error(utf8(path) + " is " + std::to_string(size) +
                                 " bytes, more than the " + std::to_string(max_bytes) +
                                 " a test.json may have");
    }
    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    file.read(text.data(), static_cast<std::streamsize>(size));
    if (!file) {
        throw std::runtime_error("cannot read " + utf8(path));
    }
    return text;
}

// Throws unless every key of the object is one of `known`.
void check_keys(const json& object, const std::string& where,
                std::initializer_list<std::string_view> known) {
    for (const auto& item : object.items()) {
        if (std::find(known.begin(), known.end(), item.key()) == known.end()) {
            throw std::runtime_error(where + ": unknown key \"" + item.key() + "\"");
        }
    }
}

const json& member(const json& object, const std::string& where, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        throw std::runtime_error(where + ": no \"" + key + "\"");
    }
    return *it;
}

std::uint64_t whole_number(const json& object, const std::string& where, const char* key,
                           std::uint64_t low, std::uint64_t high) {
    const json& value = member(object, where, key);
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() < low ||
        value.get<std::uint64_t>() > high) {
        throw std::runtime_error(where + ": \"" + key + "\" is a whole number from " +
                                 std::to_string(low) + " to " + std::to_string(high));
    }
    return value.get<std::uint64_t>();
}

double tolerance(const json& object, const std::string& where, const char* key, double high) {
    const json& value = member(object, where, key);
    if (!value.is_number() || !(value.get<double>() >= 0.0 && value.get<double>() <= high)) {
        throw std::runtime_error(where + ": \"" + key + "\" is a number from 0 to " +
                                 std::to_string(high));
    }
    return value.get<double>();
}

GoldenTest read_test(const fs::path& folder) {
    const fs::path path = folder / "test.json";
    const std::string where = utf8(path);
    json root;
    try {
        root = json::parse(read_text(path, max_test_json_bytes));
    } catch (const json::exception& error) {
        throw std::runtime_error(where + ": " + error.what());
    }
    if (!root.is_object()) {
        throw std::runtime_error(where + ": not a JSON object");
    }
    check_keys(root, where,
               {"scene", "tier", "width", "height", "samplesPerPixel", "seed", "tolerances"});

    GoldenTest test;
    const json& scene = member(root, where, "scene");
    if (!scene.is_string() || scene.get<std::string>().empty()) {
        throw std::runtime_error(where + ": \"scene\" is a project folder, relative to the test's");
    }
    test.scene = folder / from_utf8(scene.get<std::string>());

    const json& tier = member(root, where, "tier");
    if (!tier.is_string()) {
        throw std::runtime_error(where + ": \"tier\" is \"path\" or \"standard\"");
    }
    test.tier = tier.get<std::string>();
    if (test.tier == "standard") {
        throw std::runtime_error(where + ": the Standard tier's golden tests arrive with M1-29; "
                                         "cs-render golden runs the path tier only");
    }
    if (test.tier != "path") {
        throw std::runtime_error(where + ": \"tier\" is \"path\" or \"standard\", not \"" +
                                 test.tier + "\"");
    }

    test.image.width = static_cast<int>(whole_number(root, where, "width", 1, cs::max_image_side));
    test.image.height =
        static_cast<int>(whole_number(root, where, "height", 1, cs::max_image_side));
    if (std::int64_t{test.image.width} * test.image.height > cs::max_image_pixels) {
        throw std::runtime_error(where + ": the image is larger than " +
                                 std::to_string(cs::max_image_pixels) + " pixels");
    }
    test.image.samples_per_pixel =
        static_cast<std::uint32_t>(whole_number(root, where, "samplesPerPixel", 1, max_spp));
    test.image.seed =
        whole_number(root, where, "seed", 0, std::numeric_limits<std::uint64_t>::max());

    const json& tolerances = member(root, where, "tolerances");
    const std::string tolerances_where = where + ": tolerances";
    if (!tolerances.is_object()) {
        throw std::runtime_error(tolerances_where + " is an object");
    }
    check_keys(tolerances, tolerances_where, {"meanFlip", "meanAbsoluteError_nits"});
    test.tolerances.mean_flip = tolerance(tolerances, tolerances_where, "meanFlip", 1.0);
    test.tolerances.mean_absolute_error_nits = tolerance(
        tolerances, tolerances_where, "meanAbsoluteError_nits", std::numeric_limits<double>::max());
    return test;
}

// |render - reference| in each channel, in nits; NaN where either is NaN.
cs::Image absolute_difference(const cs::Image& reference, const cs::Image& render) {
    cs::Image difference(reference.width(), reference.height());
    for (int y = 0; y < reference.height(); ++y) {
        for (int x = 0; x < reference.width(); ++x) {
            const cs::Rgb& a = reference.at(x, y);
            const cs::Rgb& b = render.at(x, y);
            difference.at(x, y) =
                cs::Rgb{std::abs(b.r - a.r), std::abs(b.g - a.g), std::abs(b.b - a.b)};
        }
    }
    return difference;
}

// The render, the reference and the render at the same exposure, so that a person can flip
// between them, the difference (as data, and as a picture exposed for its own mean), and the
// FLIP heat map.
void write_failure_images(const fs::path& out, double ev100, const cs::Image& reference,
                          const cs::Image& render) {
    fs::create_directories(out);
    cs::write_exr(render, out / "render.exr", cs::ExrChannels::float32);
    cs::write_png(render, out / "render.png", ev100);
    cs::write_png(reference, out / "reference.png", ev100);
    const cs::Image difference = absolute_difference(reference, render);
    cs::write_exr(difference, out / "difference.exr", cs::ExrChannels::float32);
    cs::write_png(difference, out / "difference.png",
                  cs::ev100_from_average_luminance(mean_luminance(difference)));
    cs::write_display_png(cs::flip_heat_map(reference, render), out / "flip.png");
}

// Images an earlier failure left in out: once the test passes they only mislead.
void remove_failure_images(const fs::path& out) {
    for (const std::string_view name : failure_files) {
        std::error_code error;
        fs::remove(out / name, error);
        if (error) {
            std::cerr << "cs-render: warning: cannot remove " << utf8(out / name) << ": "
                      << error.message() << '\n';
        }
    }
}

void print_comparison(const cs::ImageComparison& comparison, const Tolerances* tolerances) {
    std::cout << "  mean FLIP " << comparison.mean_flip;
    if (tolerances != nullptr) {
        std::cout << " (tolerance " << tolerances->mean_flip << ")";
    }
    std::cout << "\n  mean absolute error " << comparison.mean_absolute_error << " nits";
    if (tolerances != nullptr) {
        std::cout << " (tolerance " << tolerances->mean_absolute_error_nits << ")";
    }
    std::cout << "\n  largest error " << comparison.max_error << " nits\n";
}

std::string size_text(const cs::Image& image) {
    return std::to_string(image.width()) + " x " + std::to_string(image.height());
}

// Writes the render as the new reference, by way of a temporary file, so that a failed write
// cannot leave a broken reference behind. Says how far it moved from the old one, for the pull
// request.
int update_reference(const fs::path& reference_path, const cs::Image& render) {
    if (fs::exists(reference_path)) {
        try {
            const cs::Image old = cs::read_exr(reference_path);
            if (old.width() == render.width() && old.height() == render.height()) {
                std::cout << "  the new reference against the old one:\n";
                print_comparison(cs::compare(old, render), nullptr);
            } else {
                std::cout << "  the old reference was " << size_text(old) << '\n';
            }
        } catch (const std::exception& error) {
            std::cout << "  the old reference cannot be read: " << error.what() << '\n';
        }
    }
    fs::path temporary = reference_path;
    temporary += ".new";
    cs::write_exr(render, temporary, cs::ExrChannels::half);
    fs::rename(temporary, reference_path);

    const std::uintmax_t bytes = fs::file_size(reference_path);
    std::cout << "  wrote " << utf8(reference_path) << " (" << bytes
              << " bytes); its pull request says why it changed\n";
    if (bytes > large_reference_bytes) {
        std::cerr << "cs-render: warning: the reference is larger than " << large_reference_bytes
                  << " bytes; tests/golden/README.md says how to keep it small\n";
    }
    return 0;
}

int run(const Arguments& arguments) {
    const auto start = std::chrono::steady_clock::now();
    const fs::path& folder = arguments.folder;
    const std::string name = test_name(folder);
    const fs::path out =
        arguments.out.empty() ? fs::path(CS_GOLDEN_OUT_DIR) / from_utf8(name) : arguments.out;
    if (is_within(out, folder)) {
        throw UsageError("--out is inside the test folder, which only --update writes to");
    }
    const GoldenTest test = read_test(folder);
    const std::string driver(engine_driver);
    const fs::path reference_path = folder / ("ref." + driver + ".exr");

    std::cout << "golden " << name << ": " << test.tier << " tier on " << driver << ", "
              << test.image.width << " x " << test.image.height << " at "
              << test.image.samples_per_pixel << " spp, seed " << test.image.seed << '\n';
    if (!arguments.update && !fs::exists(reference_path)) {
        std::cout << "  skipped: no reference for " << driver << ", " << utf8(reference_path)
                  << "\n  `cs-render golden " << utf8(folder)
                  << " --update` makes one (tests/golden/README.md)\n";
        return exit_skip;
    }

    const Project project = load_project(test.scene);
    const unsigned threads = thread_count(arguments.threads);
    cs::ThreadPool pool(threads);
    const cs::engine::Scene scene(project.world);
    const cs::Image render = render_view(project, scene, test.image, pool);
    std::cout << "  loaded and rendered on " << threads << " threads in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << " s\n";

    if (arguments.update) {
        return update_reference(reference_path, render);
    }

    const cs::Image reference = cs::read_exr(reference_path);
    const double ev100 = display_ev100(project.camera(), reference);
    if (reference.width() != render.width() || reference.height() != render.height()) {
        remove_failure_images(out); // An earlier failure's difference would mislead.
        fs::create_directories(out);
        cs::write_exr(render, out / "render.exr", cs::ExrChannels::float32);
        cs::write_png(render, out / "render.png", ev100);
        std::cout << "  differs: the reference is " << size_text(reference)
                  << ", but test.json asks for " << size_text(render)
                  << "\n  wrote render.exr and render.png to " << utf8(out) << '\n';
        return exit_failure;
    }

    const cs::ImageComparison comparison = cs::compare(reference, render);
    print_comparison(comparison, &test.tolerances);
    // Written so that NaN fails.
    const bool flip_within = comparison.mean_flip <= test.tolerances.mean_flip;
    const bool error_within =
        comparison.mean_absolute_error <= test.tolerances.mean_absolute_error_nits;
    if (flip_within && error_within) {
        remove_failure_images(out);
        std::cout << "  matches\n";
        return 0;
    }

    write_failure_images(out, ev100, reference, render);
    std::cout << "  differs: "
              << (!flip_within && !error_within ? "mean FLIP and mean absolute error are"
                  : !flip_within                ? "mean FLIP is"
                                                : "mean absolute error is")
              << " above the tolerance\n  wrote";
    for (const std::string_view file : failure_files) {
        std::cout << ' ' << file;
    }
    std::cout << " to " << utf8(out) << '\n';
    return exit_failure;
}

} // namespace

int golden_main(int argc, char* argv[]) {
    try {
        return run(parse_arguments(argc, argv));
    } catch (const UsageError& error) {
        std::cerr << "cs-render golden: " << error.what() << "\n\n" << usage;
        return exit_usage;
    } catch (const std::exception& error) {
        std::cerr << "cs-render golden: " << error.what() << '\n';
        return exit_failure;
    }
}

} // namespace cs_render
