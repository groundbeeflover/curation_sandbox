// SPDX-License-Identifier: Apache-2.0

#include <cs/engine/lux.hpp>

#include <cs/core/colour.hpp>
#include <cs/core/math.hpp>
#include <cs/core/photometry.hpp>
#include <cs/core/random.hpp>
#include <cs/core/sampling.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/material/brdf.hpp>
#include <cs/material/painting_material.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

namespace cs::engine {

namespace {

bool finite(glm::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Russian roulette starts at this bounce (counting the first hit as 0) and keeps a path with
// the probability of its largest throughput channel, at most this.
constexpr std::uint32_t roulette_from_bounce = 3;
constexpr double roulette_max_survival = 0.95;

// A world light as the paths see it.
struct Lamp {
    const light::Luminaire* luminaire;
    glm::dvec3 colour; // linear RGB of luminance 1: radiance per nit
};

std::vector<Lamp> lamps_of(const world::World& world) {
    std::vector<Lamp> lamps;
    lamps.reserve(world.lights.size());
    for (const world::Light& light : world.lights) {
        const glm::dvec3 rgb = cct_to_linear_rgb(light.colour_temperature_k);
        const double y = luminance(rgb);
        // cct_to_linear_rgb gives luminance 1; dividing by it makes that exact, so that the
        // indirect light off a grey surface is exactly its albedo times the direct.
        lamps.push_back({&light.luminaire, y > 0.0 ? rgb / y : glm::dvec3(1.0)});
    }
    return lamps;
}

// Albedos outside [0, 1] would create energy, or a negative one; NaN becomes 0.
glm::dvec3 clamp_albedo(glm::dvec3 albedo) {
    const auto unit = [](double x) { return std::isnan(x) ? 0.0 : std::clamp(x, 0.0, 1.0); };
    return {unit(albedo.r), unit(albedo.g), unit(albedo.b)};
}

// One BRDF per world material.
std::vector<std::unique_ptr<material::Brdf>> brdfs_of(const world::World& world) {
    std::vector<std::unique_ptr<material::Brdf>> brdfs;
    brdfs.reserve(world.materials.size());
    for (const world::Material& material : world.materials) {
        brdfs.push_back(std::visit(
            [](const auto& m) -> std::unique_ptr<material::Brdf> {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, world::LambertMaterial>) {
                    return std::make_unique<material::LambertianBrdf>(clamp_albedo(m.albedo));
                } else {
                    // The photograph's texels are not read yet (M1-16): its base colour stands in.
                    material::PaintingMaterial parameters = m.parameters;
                    parameters.base.color = clamp_albedo(parameters.base.color);
                    return std::make_unique<material::PaintingBrdf>(parameters);
                }
            },
            material));
    }
    return brdfs;
}

// Calls add(lamp, wi, illuminance_lux) for every lamp that lights the point: in front of
// `normal`, emitting towards the point, and visible from it. wi is the unit direction to the
// lamp; `side` is the geometric normal on the side the light must come from (light_visible).
template <typename Add>
void for_each_lamp_lighting(const Scene& scene, const std::vector<Lamp>& lamps, glm::dvec3 point,
                            glm::dvec3 normal, glm::dvec3 side, Add&& add) {
    for (const Lamp& lamp : lamps) {
        const glm::dvec3 to_light = lamp.luminaire->position_m() - point;
        const double d = glm::length(to_light);
        if (!(d > 0.0)) {
            continue;
        }
        const glm::dvec3 wi = to_light / d;
        const double cos_theta = glm::dot(normal, wi);
        if (!(cos_theta > 0.0)) {
            continue;
        }
        const double intensity_cd = lamp.luminaire->intensity_cd(-wi);
        if (!(intensity_cd > 0.0)) {
            continue;
        }
        if (!light_visible(scene, point, side, lamp.luminaire->position_m())) {
            continue;
        }
        add(lamp, wi, photometry::point_illuminance(intensity_cd, d, cos_theta));
    }
}

// The direct illuminance on a sensor, in lux.
double sensor_direct_lux(const Scene& scene, const std::vector<Lamp>& lamps, glm::dvec3 point,
                         glm::dvec3 normal) {
    double sum = 0.0;
    for_each_lamp_lighting(scene, lamps, point, normal, normal,
                           [&](const Lamp&, glm::dvec3, double lux) { sum += lux; });
    return sum;
}

// What the indirect paths need of the world, made once per reading.
struct Lighting {
    std::vector<Lamp> lamps;
    std::vector<std::unique_ptr<material::Brdf>> brdfs; // one per world material
};

// One indirect path from the sensor: pi times the luminance found in a cosine-distributed
// direction, the light that has bounced at least once.
double indirect_sample(const Scene& scene, const Lighting& lighting, glm::dvec3 point,
                       const Onb<double>& sensor, std::uint32_t max_bounces, Rng& rng) {
    const double u0 = rng.next_double();
    const double u1 = rng.next_double();
    glm::dvec3 direction =
        glm::normalize(sensor.to_world(sample_cosine_hemisphere({u0, u1}).direction));
    glm::dvec3 origin = point + ray_offset_m(point) * sensor.normal;

    glm::dvec3 throughput(1.0);
    glm::dvec3 radiance(0.0);
    for (std::uint32_t bounce = 0;; ++bounce) {
        const std::optional<Hit> hit = scene.intersect(origin, direction);
        if (!hit) {
            break; // out into the dark: no environment light yet
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

        // Next-event estimation: the lamps' direct light, reflected towards wo.
        for_each_lamp_lighting(scene, lighting.lamps, hit->position_m, shading, side,
                               [&](const Lamp& lamp, glm::dvec3 wi, double lux) {
                                   radiance += throughput *
                                               brdf.eval(wo_local, frame.to_local(wi)) *
                                               lamp.colour * lux;
                               });

        if (bounce >= max_bounces) {
            break;
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
    return std::numbers::pi * luminance(radiance);
}

// The mean and the sum of squared deviations of a run of samples (Welford), combinable in a
// fixed order (Chan, Golub and LeVeque).
struct Moments {
    double count = 0.0;
    double mean = 0.0;
    double m2 = 0.0;

    void add(double x) {
        count += 1.0;
        const double delta = x - mean;
        mean += delta / count;
        m2 += delta * (x - mean);
    }

    void merge(const Moments& other) {
        if (other.count == 0.0) {
            return;
        }
        const double total = count + other.count;
        const double delta = other.mean - mean;
        mean += delta * other.count / total;
        m2 += other.m2 + delta * delta * count * other.count / total;
        count = total;
    }
};

glm::dvec3 checked_point(glm::dvec3 point_m) {
    if (!finite(point_m)) {
        throw std::invalid_argument("lux meter: the point is not finite");
    }
    return point_m;
}

// The sensor's normal, of unit length.
glm::dvec3 checked_normal(glm::dvec3 normal) {
    const double length = glm::length(normal);
    if (!finite(normal) || !(length > 0.0) || !std::isfinite(length)) {
        throw std::invalid_argument("lux meter: the normal is not finite or is zero");
    }
    return normal / length;
}

} // namespace

bool light_visible(const Scene& scene, glm::dvec3 from_m, glm::dvec3 side, glm::dvec3 light_m) {
    const glm::dvec3 origin = from_m + ray_offset_m(from_m) * side;
    const glm::dvec3 to_light = light_m - origin;
    const double distance = glm::length(to_light);
    if (!(distance > 0.0)) {
        return false;
    }
    const glm::dvec3 direction = to_light / distance;
    if (!(glm::dot(direction, side) > 0.0)) {
        return false;
    }
    const double t_max = distance - shadow_gap_m(distance);
    if (!(t_max > 0.0)) {
        return true; // nothing fits between
    }
    return !scene.occluded(origin, direction, 0.0, t_max);
}

double direct_lux(const Scene& scene, glm::dvec3 point_m, glm::dvec3 normal) {
    return sensor_direct_lux(scene, lamps_of(scene.world()), checked_point(point_m),
                             checked_normal(normal));
}

LuxReading lux_at(const Scene& scene, glm::dvec3 point_m, glm::dvec3 normal,
                  const LuxOptions& options) {
    checked_point(point_m);
    const glm::dvec3 n = checked_normal(normal);
    if (options.samples == 0) {
        throw std::invalid_argument("lux_at: samples must be at least 1");
    }
    const Onb<double> sensor = onb_from_normal(n);
    const Lighting lighting{lamps_of(scene.world()), brdfs_of(scene.world())};

    LuxReading reading;
    reading.direct_lux.value = sensor_direct_lux(scene, lighting.lamps, point_m, n);

    const std::size_t samples = options.samples;
    const std::size_t chunks = (samples + lux_chunk_samples - 1) / lux_chunk_samples;
    std::vector<Moments> partial(chunks);
    const auto run = [&](std::size_t begin, std::size_t end) {
        Moments& moments = partial[begin / lux_chunk_samples];
        for (std::size_t s = begin; s < end; ++s) {
            Rng rng(hash_seed(options.seed, 0, 0, static_cast<std::uint32_t>(s), lux_dimension));
            moments.add(
                indirect_sample(scene, lighting, point_m, sensor, options.max_bounces, rng));
        }
    };
    if (options.pool != nullptr) {
        options.pool->parallel_for(samples, lux_chunk_samples, run);
    } else {
        ThreadPool(1).parallel_for(samples, lux_chunk_samples, run); // on this thread, in order
    }

    Moments indirect;
    for (const Moments& m : partial) {
        indirect.merge(m);
    }
    reading.total_lux.value = reading.direct_lux.value + indirect.mean;
    reading.total_lux.standard_error =
        samples > 1 ? std::sqrt(indirect.m2 / (indirect.count - 1.0) / indirect.count)
                    : std::numeric_limits<double>::infinity();
    return reading;
}

} // namespace cs::engine
