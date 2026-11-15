// SPDX-License-Identifier: Apache-2.0

#include <cs/engine/path_tracer.hpp>

#include <cs/core/colour.hpp>
#include <cs/core/image_io.hpp>
#include <cs/core/math.hpp>
#include <cs/core/photometry.hpp>
#include <cs/core/random.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/engine/lux.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/material/brdf.hpp>
#include <cs/material/painting_material.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace cs::engine {

namespace {

bool finite(glm::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Russian roulette as the lux meter's (lux.cpp): from the fourth hit (bounce 3, counting the
// first hit as 0), a path survives with the probability of its largest throughput channel, at
// most this.
constexpr std::uint32_t roulette_from_bounce = 3;
constexpr double roulette_max_survival = 0.95;

// A world light as the paths see it, as in the lux meter.
struct Lamp {
    const light::Luminaire* luminaire;
    glm::dvec3 colour; // linear RGB of luminance 1: radiance per nit
};

// Albedos outside [0, 1] would create energy, or a negative one; NaN becomes 0.
glm::dvec3 clamp_albedo(glm::dvec3 albedo) {
    const auto unit = [](double x) { return std::isnan(x) ? 0.0 : std::clamp(x, 0.0, 1.0); };
    return {unit(albedo.r), unit(albedo.g), unit(albedo.b)};
}

// What the paths need of the world: its lamps and one BRDF per material. The lux meter makes
// the same from the same world (lux.cpp); M2-28 is to cache both on the scene for the two.
struct Lighting {
    std::vector<Lamp> lamps;
    std::vector<std::unique_ptr<material::Brdf>> brdfs;

    explicit Lighting(const world::World& world) {
        lamps.reserve(world.lights.size());
        for (const world::Light& light : world.lights) {
            const glm::dvec3 rgb = cct_to_linear_rgb(light.colour_temperature_k);
            const double y = luminance(rgb);
            lamps.push_back({&light.luminaire, y > 0.0 ? rgb / y : glm::dvec3(1.0)});
        }
        brdfs.reserve(world.materials.size());
        for (const world::Material& material : world.materials) {
            brdfs.push_back(std::visit(
                [](const auto& m) -> std::unique_ptr<material::Brdf> {
                    using M = std::decay_t<decltype(m)>;
                    if constexpr (std::is_same_v<M, world::LambertMaterial>) {
                        return std::make_unique<material::LambertianBrdf>(clamp_albedo(m.albedo));
                    } else {
                        // The photograph's texels are not read yet (M1-17): its base colour
                        // stands in.
                        material::PaintingMaterial parameters = m.parameters;
                        parameters.base.color = clamp_albedo(parameters.base.color);
                        return std::make_unique<material::PaintingBrdf>(parameters);
                    }
                },
                material));
        }
    }
};

// The camera's rays, from a world::Camera made usable: unit, perpendicular axes.
class CameraRays {
  public:
    CameraRays(const world::Camera& camera, int width, int height)
        : projection_(camera.projection), position_(camera.position_m), width_(width),
          height_(height) {
        if (!finite(camera.position_m)) {
            throw std::invalid_argument("path tracer: the camera's position is not finite");
        }
        const double forward_length = glm::length(camera.forward);
        const double up_length = glm::length(camera.up);
        if (!finite(camera.forward) || !finite(camera.up) || !(forward_length > 0.0) ||
            !(up_length > 0.0) || !std::isfinite(forward_length) || !std::isfinite(up_length)) {
            throw std::invalid_argument(
                "path tracer: the camera's forward or up is not finite or is zero");
        }
        forward_ = camera.forward / forward_length;
        const glm::dvec3 right = glm::cross(forward_, camera.up / up_length);
        const double right_length = glm::length(right);
        if (!(right_length > 1e-9)) {
            throw std::invalid_argument("path tracer: the camera's forward and up are parallel");
        }
        right_ = right / right_length;
        up_ = glm::cross(right_, forward_);

        if (projection_ == world::Projection::perspective) {
            if (!(camera.vertical_fov_rad > 0.0 && camera.vertical_fov_rad < std::numbers::pi)) {
                throw std::invalid_argument(
                    "path tracer: the camera's vertical field of view is not in (0, pi)");
            }
            half_height_ = std::tan(0.5 * camera.vertical_fov_rad);
        } else {
            if (!(camera.vertical_extent_m > 0.0) || !std::isfinite(camera.vertical_extent_m)) {
                throw std::invalid_argument(
                    "path tracer: the camera's vertical extent is not finite and positive");
            }
            half_height_ = 0.5 * camera.vertical_extent_m;
        }
        half_width_ = half_height_ * static_cast<double>(width) / static_cast<double>(height);
    }

    struct Ray {
        glm::dvec3 origin;
        glm::dvec3 direction; // unit
    };

    // The ray through the point (x + u.x, y + u.y) of the image, in pixels from its top-left
    // corner: on the image plane at distance 1 (perspective) or on the camera's plane
    // (orthographic).
    Ray ray(int x, int y, glm::dvec2 u) const {
        const double sx = (2.0 * (x + u.x) / width_ - 1.0) * half_width_;
        const double sy = (1.0 - 2.0 * (y + u.y) / height_) * half_height_;
        if (projection_ == world::Projection::perspective) {
            return {position_, glm::normalize(forward_ + sx * right_ + sy * up_)};
        }
        return {position_ + sx * right_ + sy * up_, forward_};
    }

  private:
    world::Projection projection_;
    glm::dvec3 position_;
    glm::dvec3 forward_{1.0, 0.0, 0.0};
    glm::dvec3 right_{0.0, -1.0, 0.0};
    glm::dvec3 up_{0.0, 0.0, 1.0};
    double half_width_ = 0.0;  // of the image plane at distance 1, or of the view, in metres
    double half_height_ = 0.0; // likewise
    int width_;
    int height_;
};

// The radiance, in nits, that reaches the camera along one ray: path_tracer.hpp says which light
// it counts.
glm::dvec3 trace(const Scene& scene, const Lighting& lighting, CameraRays::Ray ray,
                 bool direct_only, double environment_nits, Rng& rng) {
    // Light reflected at most last_bounce + 1 times is counted: next-event estimation at hits
    // 0 to last_bounce, and the environment along rays leaving them.
    const std::uint32_t last_bounce = direct_only ? 0 : max_path_bounces;
    glm::dvec3 origin = ray.origin;
    glm::dvec3 direction = ray.direction;
    glm::dvec3 throughput(1.0);
    glm::dvec3 radiance(0.0);
    for (std::uint32_t bounce = 0;; ++bounce) {
        const std::optional<Hit> hit = scene.intersect(origin, direction);
        if (!hit) {
            radiance += throughput * environment_nits; // white: luminance 1 per nit
            break;
        }
        if (bounce > last_bounce) {
            break;
        }
        const glm::dvec3 wo = -direction;
        // Both sides of a triangle reflect: turn the normals to the side the ray came from.
        const glm::dvec3 side = glm::dot(hit->geometric_normal, wo) >= 0.0 ? hit->geometric_normal
                                                                           : -hit->geometric_normal;
        glm::dvec3 shading = glm::dot(hit->normal, side) >= 0.0 ? hit->normal : -hit->normal;
        if (!(glm::dot(shading, wo) > 0.0)) {
            shading = side;
        }
        const Onb<double> frame = onb_from_normal(shading);
        const glm::dvec3 wo_local = frame.to_local(wo);
        const material::Brdf& brdf =
            *lighting.brdfs[scene.world().instances[hit->instance].material];

        // Next-event estimation, with the lux meter's shadow rays.
        for (const Lamp& lamp : lighting.lamps) {
            const glm::dvec3 light_m = lamp.luminaire->position_m();
            const glm::dvec3 to_light = light_m - hit->position_m;
            const double d = glm::length(to_light);
            if (!(d > 0.0)) {
                continue;
            }
            const glm::dvec3 wi = to_light / d;
            const double cos_theta = glm::dot(shading, wi);
            if (!(cos_theta > 0.0)) {
                continue;
            }
            const double intensity_cd = lamp.luminaire->intensity_cd(-wi);
            if (!(intensity_cd > 0.0) || !light_visible(scene, hit->position_m, side, light_m)) {
                continue;
            }
            radiance += throughput * brdf.eval(wo_local, frame.to_local(wi)) * lamp.colour *
                        photometry::point_illuminance(intensity_cd, d, cos_theta);
        }

        if (bounce == last_bounce && !(environment_nits > 0.0)) {
            break; // nothing more could be counted
        }
        const double v0 = rng.next_double();
        const double v1 = rng.next_double();
        const material::BrdfSample next = brdf.sample(wo_local, {v0, v1});
        if (!(next.pdf > 0.0)) {
            break;
        }
        throughput *= next.weight;
        if (bounce >= roulette_from_bounce) {
            const double survival = std::min(std::max({throughput.r, throughput.g, throughput.b}),
                                             roulette_max_survival);
            if (!(rng.next_double() < survival)) {
                break;
            }
            throughput /= survival;
        }
        direction = glm::normalize(frame.to_world(next.wi));
        if (!(glm::dot(direction, side) > 0.0)) {
            break; // rounding put it below the surface
        }
        origin = hit->position_m + ray_offset_m(hit->position_m) * side;
    }
    return radiance;
}

void check_size(const RenderOptions& options) {
    if (options.width < 1 || options.height < 1 || options.width > max_image_side ||
        options.height > max_image_side ||
        std::int64_t{options.width} * options.height > max_image_pixels) {
        throw std::invalid_argument("path tracer: the image size is outside the limits");
    }
    if (!(options.environment_nits >= 0.0) || !std::isfinite(options.environment_nits)) {
        throw std::invalid_argument("path tracer: environment_nits is negative or not finite");
    }
}

} // namespace

struct ProgressiveRender::Impl {
    Impl(const Scene& scene_, const world::Camera& camera, const RenderOptions& options_)
        : scene(&scene_), options((check_size(options_), options_)),
          camera_rays(camera, options.width, options.height), lighting(scene_.world()),
          sums(static_cast<std::size_t>(options.width) * static_cast<std::size_t>(options.height),
               glm::dvec3(0.0)) {}

    const Scene* scene;
    RenderOptions options;
    CameraRays camera_rays;
    Lighting lighting;
    std::vector<glm::dvec3> sums; // of every sample's radiance, row by row
    std::uint32_t samples = 0;
};

ProgressiveRender::ProgressiveRender(const Scene& scene, const world::Camera& camera,
                                     const RenderOptions& options)
    : impl_(std::make_unique<Impl>(scene, camera, options)) {}

ProgressiveRender::~ProgressiveRender() = default;
ProgressiveRender::ProgressiveRender(ProgressiveRender&&) noexcept = default;
ProgressiveRender& ProgressiveRender::operator=(ProgressiveRender&&) noexcept = default;

void ProgressiveRender::add_samples(std::uint32_t count) {
    Impl& r = *impl_;
    if (count > std::numeric_limits<std::uint32_t>::max() - r.samples) {
        throw std::invalid_argument("path tracer: more than 2^32 - 1 samples per pixel");
    }
    const std::uint32_t first = r.samples;
    const auto width = static_cast<std::size_t>(r.options.width);
    const auto rows = [&](std::size_t begin, std::size_t end) {
        for (std::size_t y = begin; y < end; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                glm::dvec3& sum = r.sums[y * width + x];
                for (std::uint32_t s = first; s < first + count; ++s) {
                    Rng rng(hash_seed(r.options.seed, static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y), s, path_dimension));
                    const double u0 = rng.next_double();
                    const double u1 = rng.next_double();
                    const CameraRays::Ray ray =
                        r.camera_rays.ray(static_cast<int>(x), static_cast<int>(y), {u0, u1});
                    sum += trace(*r.scene, r.lighting, ray, r.options.direct_only,
                                 r.options.environment_nits, rng);
                }
            }
        }
    };
    const auto height = static_cast<std::size_t>(r.options.height);
    if (r.options.pool != nullptr) {
        r.options.pool->parallel_for(height, 1, rows);
    } else {
        rows(0, height);
    }
    r.samples += count;
}

std::uint32_t ProgressiveRender::samples() const {
    return impl_->samples;
}

Image ProgressiveRender::image() const {
    const Impl& r = *impl_;
    Image image(r.options.width, r.options.height);
    if (r.samples == 0) {
        return image;
    }
    const double n = r.samples;
    for (int y = 0; y < r.options.height; ++y) {
        for (int x = 0; x < r.options.width; ++x) {
            const glm::dvec3 mean =
                r.sums[static_cast<std::size_t>(y) * static_cast<std::size_t>(r.options.width) +
                       static_cast<std::size_t>(x)] /
                n;
            image.at(x, y) = {static_cast<float>(mean.r), static_cast<float>(mean.g),
                              static_cast<float>(mean.b)};
        }
    }
    return image;
}

Image render(const Scene& scene, const world::Camera& camera, const RenderOptions& options) {
    if (options.samples_per_pixel == 0) {
        throw std::invalid_argument("path tracer: samples_per_pixel must be at least 1");
    }
    ProgressiveRender progressive(scene, camera, options);
    progressive.add_samples(options.samples_per_pixel);
    return progressive.image();
}

Image render(const world::World& world, const world::Camera& camera, const RenderOptions& options) {
    const Scene scene(world);
    return render(scene, camera, options);
}

} // namespace cs::engine
