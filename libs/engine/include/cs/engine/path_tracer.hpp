// SPDX-License-Identifier: Apache-2.0

#pragma once

// The path tracer
// ---------------
// render draws what a camera sees of a scene in absolute luminance: every pixel of the image is
// linear Rec. 709 RGB radiance in nits (cd/m^2), whose luminance Y is the nits a luminance meter
// would read looking through that pixel. It is the engine's reference image, the one every
// display tier is compared with (docs/adr/0002-rendering-tiers.md); lux readings still come from
// the lux meter (lux.hpp), never from these pixels.
//
// Each pixel averages samples_per_pixel camera rays, spread uniformly over the pixel (a box
// filter). Each ray starts a path (Physically Based Rendering, 4th edition, chapter 13):
//
// - Where the path meets a surface, next-event estimation adds the light each luminaire sends
//   there directly, reflected towards the path by the surface's BRDF. Its shadow rays are the
//   lux meter's: engine::light_visible, with its shadow_gap_m, so that a light lying in the
//   ceiling's plane lights a surface in the picture exactly as in the lux reading.
// - The path then continues in a direction drawn from the BRDF (cosine-distributed for Lambert
//   surfaces), carrying the BRDF's weight.
// - A ray that leaves the scene sees the environment: a uniform white emitter all around, of
//   environment_nits, for tests such as the white furnace; 0 (dark) by default. Lights are
//   points, so a ray never hits one; they reach the image only by next-event estimation.
// - Russian roulette and the bounce limit are the lux meter's: from the fourth hit on, a path
//   survives with the probability of its largest throughput channel, at most 0.95; and after
//   max_path_bounces bounces it ends whatever its throughput. In a closed room of albedo rho the
//   limit leaves rho^1025 of the light uncounted, 1e-23 at 0.95 (lux.hpp, LuxOptions).
//
// direct_only keeps only light that reflects once on its way to the camera: the luminaires'
// light at the first surface, and the environment seen through one reflection; plus the
// environment seen straight through the pixel. It is the light the Standard tier evaluates
// exactly, for comparisons with it.
//
// Materials: Lambert surfaces reflect with their albedo. A painting reflects with its
// PaintingBrdf; its photograph's texels are not read yet (M1-17), its base colour stands in, as in
// the lux meter. Albedos are clamped to [0, 1]. Both sides of a surface reflect (scene.hpp).
// Surfaces reflect about their interpolated normal; a path whose next direction, drawn about
// that normal, falls below the triangle itself ends there, as in PBRT, so smooth normals on a
// coarse mesh can only darken it a little (0.01% on the furnace's 96 x 48 sphere, about 1% on
// an 8 x 4 one).
//
// Randomness: sample s of pixel (x, y) uses its own generator,
// cs::Rng(cs::hash_seed(seed, x, y, s, path_dimension)), for its position in the pixel and its
// whole path. Each pixel is accumulated by one thread, in sample order, in double precision. The
// image is therefore the same, bit for bit, for the same seed whatever the pool's thread count,
// and a ProgressiveRender that adds its samples in several batches ends with the image that one
// batch of the same total gives.

#include <cs/core/image.hpp>
#include <cs/engine/scene.hpp>
#include <cs/world/world.hpp>

#include <cstdint>
#include <memory>

namespace cs {
class ThreadPool;
} // namespace cs

namespace cs::engine {

// Keeps the path tracer's random numbers apart from the lux meter's (lux_dimension), as
// hash_seed's `dimension`.
inline constexpr std::uint32_t path_dimension = 0x7061'7401; // "pat" and 1

// Bounces after the first hit at most, as LuxOptions::max_bounces by default.
inline constexpr std::uint32_t max_path_bounces = 1024;

struct RenderOptions {
    int width = 256;  // pixels, in [1, max_image_side] (image_io.hpp)
    int height = 144; // pixels, likewise; width * height at most max_image_pixels
    std::uint32_t samples_per_pixel = 64; // at least 1; render only
    std::uint64_t seed = 1;
    bool direct_only = false;      // see above
    double environment_nits = 0.0; // uniform white light all around; finite and at least 0
    ThreadPool* pool = nullptr;    // runs the rows; none: all on the calling thread
};

// A render that improves as samples are added: add_samples runs more samples on every pixel, and
// image() is the mean of all samples so far. It refers to the scene and to options.pool, which
// must outlive it, and copies the camera and the other options. A render moved from may only be
// destroyed or assigned to.
class ProgressiveRender {
  public:
    // Throws std::invalid_argument for a size outside the limits above, an environment_nits that
    // is negative or not finite, or a camera it cannot use: a position that is not finite, a
    // forward or up that is not finite, is zero or is parallel to the other, a perspective
    // vertical_fov_rad outside (0, pi) or an orthographic vertical_extent_m that is not finite
    // and positive. forward and up need not be unit or perpendicular: the image's up is up made
    // perpendicular to forward, its right is forward x up. samples_per_pixel is not used.
    ProgressiveRender(const Scene& scene, const world::Camera& camera,
                      const RenderOptions& options);
    ~ProgressiveRender();

    ProgressiveRender(ProgressiveRender&&) noexcept;
    ProgressiveRender& operator=(ProgressiveRender&&) noexcept;

    // Runs `count` more samples on every pixel, on options.pool if there is one. Throws
    // std::invalid_argument if that would make more than 2^32 - 1 samples per pixel. If a sample
    // throws, the exception is passed on and the render is broken, its pixels holding different
    // numbers of samples: from then on add_samples and image() throw std::logic_error.
    void add_samples(std::uint32_t count);

    // Samples per pixel so far.
    std::uint32_t samples() const;

    // The mean radiance of every pixel so far, in nits; black before the first sample. Row 0 is
    // the top of the image, column 0 its left, as the camera sees it. Throws std::logic_error for
    // a broken render (add_samples).
    Image image() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The image of options.samples_per_pixel samples per pixel. Throws std::invalid_argument as
// ProgressiveRender does, or when samples_per_pixel is 0.
Image render(const Scene& scene, const world::Camera& camera, const RenderOptions& options = {});

// The same, building the scene from the world first (Scene's exceptions apply too).
Image render(const world::World& world, const world::Camera& camera,
             const RenderOptions& options = {});

} // namespace cs::engine
