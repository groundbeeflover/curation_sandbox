// SPDX-License-Identifier: Apache-2.0

#pragma once

// The lux meter
// -------------
// lux_at measures the illuminance on a small sensor at a point, facing along a normal, from the
// scene's own lights: what a curator's lux meter would read held there. It never looks at a
// rendered image (CLAUDE.md).
//
// The reading has two parts:
//
// - Direct: the light that comes straight from each luminaire. The lights are points, so this
//   is a sum, not an estimate: for every luminaire the sensor faces and can see, I cos(theta) /
//   d^2, with I the luminaire's intensity towards the sensor in candela, theta the angle between
//   the sensor's normal and the direction to the light, and d the distance. Its standard error
//   is 0 for as long as every light is a point.
//
// - Indirect: light that has bounced off the room, works and frames first. It is estimated by
//   Monte Carlo: `samples` directions over the hemisphere above the sensor, drawn with density
//   cos(theta) / pi, so that each one contributes pi times the luminance it finds. Each path
//   follows the surfaces' BRDFs, adding at every bounce the direct light the surface receives
//   (next-event estimation, with the shadow rays below), and ends by Russian roulette or after
//   max_bounces bounces.
//
// Total = direct + indirect; its standard error is the indirect estimate's.
//
// Shadow rays stop short of the light. A luminaire often sits exactly in a surface: a spot on a
// track at the ceiling's height lies in the ceiling's plane. A shadow ray that ran all the way
// to it would end on the ceiling and could find it in the way, reading 0 lux. So a shadow ray
// over distance d stops shadow_gap_m(d) before the light: anything that close to the light, or
// a surface the light lies in, never shadows it. Shadow rays start ray_offset_m(point) off the
// surface or sensor they leave (scene.hpp). The path tracer (M1-16) uses light_visible for its
// own next-event rays, so that the two agree.
//
// Randomness: sample s uses its own generator, cs::Rng(cs::hash_seed(seed, 0, 0, s,
// lux_dimension)), and samples run in chunks of lux_chunk_samples whose sums are added in chunk
// order. The reading is therefore the same, bit for bit, for the same seed, whatever the pool's
// thread count. Two lux_at calls with the same seed at different points share their random
// numbers; give them different seeds when their errors should be independent.

#include <cs/engine/scene.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>

namespace cs {
class ThreadPool;
} // namespace cs

namespace cs::engine {

// The gap a shadow ray leaves before a light d metres away: 1e-4 of d plus 1e-5 m, so 0.35 mm
// over the 3.5 m from the skeleton's ceiling spot to the floor. The relative part covers
// Embree's single-precision hit distances, which are good to about 1e-6 of d; the absolute part
// covers points near the origin, where a relative gap would shrink below a float's resolution at
// the light's coordinates. A blocker this close to a point light could only be its own housing.
constexpr double shadow_gap_m(double distance_m) {
    return 1e-4 * distance_m + 1e-5;
}

// Whether a point light at light_m is visible from from_m: no surface of the scene lies on the
// segment between them, except within shadow_gap_m of the light. The ray starts
// ray_offset_m(from_m) along `side`, a unit normal on the side of the surface (or sensor) that
// the light is on, so that the surface the point lies on does not shadow it. A light that is
// not on that side of the ray's start is not visible; one closer to it than the gap is.
bool light_visible(const Scene& scene, glm::dvec3 from_m, glm::dvec3 side, glm::dvec3 light_m);

// The direct illuminance at a point, in lux, facing along normal (unit length), from every light
// of the scene: the Direct part above.
double direct_lux(const Scene& scene, glm::dvec3 point_m, glm::dvec3 normal);

// Keeps the lux meter's random numbers apart from those of other uses of the same seed (the
// path tracer's camera samples, M1-16), as hash_seed's `dimension`.
inline constexpr std::uint32_t lux_dimension = 0x6c75'7801; // "lux" and 1

// Samples run in chunks of this many, each on one thread.
inline constexpr std::size_t lux_chunk_samples = 64;

struct LuxOptions {
    std::uint32_t samples = 1024; // indirect paths; at least 1
    std::uint64_t seed = 1;
    // Bounces after the first hit at most; with Russian roulette from the fourth, a path rarely
    // gets near it. 64 bounces in a white room of albedo 0.8 leave 0.8^64, about 1e-6, of the
    // indirect light uncounted.
    std::uint32_t max_bounces = 64;
    ThreadPool* pool = nullptr; // runs the samples; none: all on the calling thread
};

struct Estimate {
    double value = 0.0;
    double standard_error = 0.0; // of value; 0 when value is exact
};

struct LuxReading {
    Estimate direct_lux;
    Estimate total_lux; // direct plus indirect
};

// The lux meter. The normal must be finite and not zero; it is normalised. Throws
// std::invalid_argument if it is not, or if the point is not finite or samples is 0. With one
// sample, total_lux's standard error is infinite.
LuxReading lux_at(const Scene& scene, glm::dvec3 point_m, glm::dvec3 normal,
                  const LuxOptions& options = {});

} // namespace cs::engine
