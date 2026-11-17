// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Standard tier: exact direct light, rasterised
// -------------------------------------------------
// The default display tier (ADR 0002). Its first pass draws every instance of a Scene and lights
// each pixel's surface exactly from every luminaire of the world, as points, without shadows:
//
//     E = sum over lights of I(direction) * cos(theta) / d^2          (lux)
//     L = albedo / pi * sum over lights of colour * I * cos(theta) / d^2   (nits)
//
// with I the light's intensity in candela towards the surface, theta the angle between the
// surface's normal and the direction to the light, d the distance and colour the light's linear
// Rec. 709 RGB of luminance 1 (cs::cct_to_linear_rgb of its colour temperature, as in the
// engine). A light behind the surface (cos theta <= 0) adds nothing. Surfaces are Lambertian:
// their albedo is the material's base colour, or for a painting with a texture the texture's
// texel, sampled trilinearly; it is clamped to [0, 1], NaN to 0, as the engine clamps it. The
// engine gives paintings their PaintingBrdf (a specular lobe and a coat) instead, so their
// radiance differs from the engine's until the Standard tier's materials arrive (milestone 2);
// their illuminance does not. Both sides of a surface reflect: the interpolated normal is turned
// towards the camera. The engine decides the side from the triangle's own normal; the two agree
// on flat-shaded meshes, which is all libs/geom makes, and would differ at the silhouettes of
// smooth-shaded ones.
//
// The result is one HDR image of hdr_format: RGB is the radiance L in nits (linear, not exposed
// or tone mapped: that is M1-44's pass), A is the illuminance E in lux on the surface seen
// through the pixel, facing the normal above. Where no surface is seen, all four are 0. Lux shown
// to curators still comes from the engine (CLAUDE.md); E is here to prove that the light is right.
//
// Cameras are world::Camera, read as the engine's path tracer reads them: pixel (x, y) of a
// width x height image sees along the ray through ((2 (x + 0.5) / width - 1) * half_width,
// (1 - 2 (y + 0.5) / height) * half_height) on the image plane, row 0 at the top, so that a
// pixel of this tier and the same pixel of the engine's image see the same point. A perspective
// camera sees from 0.01 m in front of it to infinity; an orthographic one from its own plane to
// 1 km ahead, telling surfaces apart down to about 0.1 mm (a float's resolution near 1, times
// 1 km).

#include <cs/core/image.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/world/world.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cs::gpu {

// ---- Lights as the shader reads them -----------------------------------------------------------
//
// A spot's intensity is symmetric about its axis, so it is a function of theta, the angle from
// the axis, alone. The shader reads it from a table of relative intensity (a fraction of the
// peak) built on the CPU from the light's light::GenericSpot, rather than evaluating the
// spot's formula, so that light::GenericSpot stays the one definition and the shader computes
// no pow (undefined at 0, which is the beam axis) and no atan (which Vulkan allows to be off by
// 4096 ulp).
//
// The table is indexed by w = sin(theta / 2), which the shader computes from the direction in
// fixture space (x, y, z) without cancellation and without transcendental functions:
// 1 - cos theta = (x^2 + y^2) / (1 + z) for z > 0, and 1 - z otherwise; w = sqrt((1 - cos theta)
// / 2). It has two parts, each read with linear interpolation:
//
// - the core, profile_core_entries entries from the axis (w = 0) to w = core_end, spaced as
//   w = core_end * s^2 for s evenly from 0 to 1, denser towards the axis, where a profile with
//   p < 1 comes to a point;
// - the taper, profile_taper_entries entries evenly from w = core_end to w = lit_end, which
//   resolve the fade between half the field angle and the cut-off however narrow it is.
//
// Beyond lit_end the shader returns exactly 0. lit_end is the largest angle at which the spot
// emits anything (just short of its cut-off; half its beam angle for a uniform cone; 180 degrees
// when it lights the whole sphere), and core_end is half its field angle, or lit_end if that is
// smaller.
//
// Accuracy: within 1e-4 of the peak at every angle for spots whose exponent p (distribution.hpp)
// is from 0.5 to 25, a field / beam ratio from about 1.05 to 11, with a field angle up to 340
// degrees but not within 0.1 degree of 180; uniform cones and isotropic lights are exact. Outside
// those bounds the error stays where the profile is sharpest: within micro-degrees of the axis
// for p < 0.5 (1e-3 of the peak at 5 / 120 degrees), at the beam's edge for p > 25 (1e-2 at
// 24.9 / 25), in the last 12 degrees behind a fixture whose field is 360 degrees (4e-3 at
// 180 / 360), and in the fade of a field within 0.1 degree of 180, which narrows to nothing there
// (distribution.hpp) while the shader's angle is good to about 1e-7 rad (1e-3 at 179.999).

inline constexpr std::uint32_t profile_core_entries = 1024;
inline constexpr std::uint32_t profile_taper_entries = 256;
inline constexpr std::uint32_t profile_entries = profile_core_entries + profile_taper_entries;

// The most lights a Standard tier takes: every pixel loops over all of them. 25 times the 40 of
// the benchmark gallery (ADR 0002).
inline constexpr std::size_t max_standard_lights = 1024;

// One light, as the shader reads it: 80 bytes, its vec3 + float pairs laid out alike in C++ and
// in Slang's natural layout. The axes are the luminaire's, in the world: unit and right-handed,
// z along the beam (light::Luminaire).
struct LightData {
    glm::vec3 position_m{0.0f};
    float peak_cd = 0.0f; // the intensity on the axis
    glm::vec3 axis_x{1.0f, 0.0f, 0.0f};
    float core_end = 0.0f; // sin(theta / 2) where the core of the table ends
    glm::vec3 axis_y{0.0f, 1.0f, 0.0f};
    float lit_end = 0.0f; // sin(theta / 2) beyond which nothing is emitted
    glm::vec3 axis_z{0.0f, 0.0f, 1.0f};
    std::uint32_t profile = 0; // its table's first entry in LightTable::profiles
    glm::vec3 colour{1.0f};    // linear Rec. 709 RGB of luminance 1
    std::uint32_t padding = 0;
};
static_assert(sizeof(LightData) == 80);
static_assert(offsetof(LightData, peak_cd) == 12 && offsetof(LightData, core_end) == 28 &&
              offsetof(LightData, lit_end) == 44 && offsetof(LightData, profile) == 60 &&
              offsetof(LightData, colour) == 64); // luminaire.slang's offsets

// The lights of a world as the GPU holds them, in one buffer: every LightData, then every
// profile (profile_entries floats each, the core then the taper). A spot's shape relative to its
// peak depends on its beam and field angles alone, so there is one profile per pair of them, in
// the order the lights first use them: lights of one fixture type share theirs.
struct LightTable {
    std::vector<LightData> lights;
    std::vector<float> profiles;
};

// The table of the world's lights, in their order. Throws std::invalid_argument for a light
// without a generic spot (world::Light::spot) or whose peak intensity overflows a float (a spot
// of a few attoradians), or for more than max_standard_lights lights.
LightTable make_light_table(std::span<const world::Light> lights);

// ---- The tier ----------------------------------------------------------------------------------

// What render() reads back.
struct StandardImages {
    Image radiance;                     // RGB in nits, row 0 at the top
    std::vector<float> illuminance_lux; // width * height, row by row from the top: [y * width + x]
};

// Owns its pipeline, the lights' buffer, its descriptors and its images: the HDR image and a
// depth image. It refers to the scene, which must outlive it; a changed world needs a new tier
// (its lights are uploaded once). Destroy it before its Device, and only once every submission
// of what record() recorded has finished: it frees everything at once, without waiting.
class StandardTier {
  public:
    // RGB radiance in nits, A illuminance in lux (above). 32-bit floats: half floats would
    // overflow at 65504 nits or lux, which a spot reaches on a surface 0.4 m away.
    static constexpr VkFormat hdr_format = VK_FORMAT_R32G32B32A32_SFLOAT;

    // Throws std::invalid_argument when a size is 0, larger than the device's
    // maxImageDimension2D or than it can allocate as one image, and as make_light_table does;
    // std::runtime_error on a Vulkan failure.
    StandardTier(const Device& device, const Scene& scene, std::span<const world::Light> lights,
                 std::uint32_t width, std::uint32_t height);
    ~StandardTier();

    StandardTier(const StandardTier&) = delete;
    StandardTier& operator=(const StandardTier&) = delete;
    StandardTier(StandardTier&&) = delete;
    StandardTier& operator=(StandardTier&&) = delete;

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    [[nodiscard]] std::size_t light_count() const { return light_count_; }

    [[nodiscard]] VkImage hdr_image() const { return hdr_image_; }
    [[nodiscard]] VkImageView hdr_view() const { return hdr_view_; }

    // Records one frame into a command buffer that is recording, outside any rendering. Before
    // it, the frame waits for fragment and compute shader reads and transfer reads of the HDR
    // image by earlier commands on the queue, so frames may follow one another without the
    // caller's barriers; it discards what the image held. After it, the HDR image is in
    // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, its writes visible to fragment and compute shader
    // reads of later commands. Throws std::invalid_argument, before recording anything, for a
    // camera the engine's path tracer would refuse: a position that is not finite, a forward or
    // up that is not finite, is zero or is parallel to the other, a perspective
    // vertical_fov_rad outside (0, pi) or an orthographic vertical_extent_m that is not finite
    // and positive.
    void record(VkCommandBuffer commands, const world::Camera& camera) const;

    // Records a frame as above, submits it to the device's queue, waits up to a minute and reads
    // the HDR image back, leaving it as record() does. Throws as record() does, or
    // std::runtime_error on a Vulkan failure or the timeout. It submits to the device's queue, so
    // not while another thread uses that queue.
    [[nodiscard]] StandardImages render(const world::Camera& camera) const;

  private:
    const Device* device_ = nullptr;
    const Scene* scene_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t light_count_ = 0;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> texture_sets_;  // one per scene texture
    VkDescriptorSet white_set_ = VK_NULL_HANDLE; // for materials without a texture

    VkBuffer lights_ = VK_NULL_HANDLE; // LightTable: the lights, then the profiles
    VmaAllocation lights_allocation_ = VK_NULL_HANDLE;
    VkDeviceAddress lights_address_ = 0;

    VkImage white_image_ = VK_NULL_HANDLE; // 1 x 1, bound where no texture is read
    VmaAllocation white_allocation_ = VK_NULL_HANDLE;
    VkImageView white_view_ = VK_NULL_HANDLE;
    VkImage hdr_image_ = VK_NULL_HANDLE;
    VmaAllocation hdr_allocation_ = VK_NULL_HANDLE;
    VkImageView hdr_view_ = VK_NULL_HANDLE;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VmaAllocation depth_allocation_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;

    void create_images();
    void create_pipeline();
    void create_descriptors();
    void upload_lights(const LightTable& table);
    void destroy() noexcept;
};

} // namespace cs::gpu
