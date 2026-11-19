// SPDX-License-Identifier: Apache-2.0

// The swapchain presenter (presenter.hpp), issue M1-23, with the owner's addition of 2026-11-18:
// the image shown is M1-44's display image, R8G8B8A8_SRGB, and the swapchain's format is _SRGB
// too (B8G8R8A8_SRGB first, then R8G8B8A8_SRGB, where the device can blit to it), so that the
// colours arrive encoded once.
//
// The window is a headless surface (VK_EXT_headless_surface), which lavapipe offers: it has no
// window system, so its size is whatever the presenter is told (its currentExtent is undefined,
// as on Wayland). The display image stands in for M1-44's pass: an R8G8B8A8_SRGB image cleared to
// a linear grey by commands of its own, submitted to the queue before every present, as the
// tone-mapping pass will be.
//
// Every test that needs a device starts with make_device() (gpu_test.hpp): without a Vulkan
// device it skips, and with CS_REQUIRE_GPU=1 it fails. The harness (main.cpp) fails the running
// test on any validation warning or error, synchronisation validation included, so every test is
// also a "validation clean" check. Vulkan objects are declared after the device and the surface,
// so they are destroyed before them, also when an assertion throws.

#include "gpu_test.hpp"

#include <cs/gpu/device.hpp>
#include <cs/gpu/presenter.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;
using cs::gpu::choose_surface_format;
using cs::gpu::Device;
using cs::gpu::DeviceOptions;
using cs::gpu::PresentedImage;
using cs::gpu::Presenter;
using cs::gpu::PresentSource;
using cs::gpu::test::gpu_required;
using cs::gpu::test::make_device;
using cs::gpu::test::test_options;

namespace {

// ---- A headless window ------------------------------------------------------------------------

DeviceOptions presenting_options() {
    DeviceOptions options = test_options();
    options.presentation = true;
    options.optional_instance_extensions = {"VK_EXT_headless_surface"};
    return options;
}

// A surface with no window behind it, destroyed with the object. Lavapipe always offers
// VK_EXT_headless_surface, so there its absence fails under CS_REQUIRE_GPU=1; another driver
// (NVIDIA's) may not, and the test skips.
class HeadlessSurface {
  public:
    explicit HeadlessSurface(const Device& device) : instance_(device.instance()) {
        if (!device.has_instance_extension("VK_EXT_headless_surface")) {
            if (gpu_required() && device.report().driver == "llvmpipe") {
                FAIL("lavapipe does not offer VK_EXT_headless_surface");
            } else {
                SKIP("the driver does not offer VK_EXT_headless_surface");
            }
        }
        VkHeadlessSurfaceCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        REQUIRE(vkCreateHeadlessSurfaceEXT(instance_, &info, nullptr, &surface_) == VK_SUCCESS);
    }
    ~HeadlessSurface() { vkDestroySurfaceKHR(instance_, surface_, nullptr); }
    HeadlessSurface(const HeadlessSurface&) = delete;
    HeadlessSurface& operator=(const HeadlessSurface&) = delete;

    [[nodiscard]] VkSurfaceKHR get() const { return surface_; }

  private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
};

// ---- A stand-in for M1-44's display image ------------------------------------------------------

// R8G8B8A8_SRGB, cleared to a uniform linear grey by draw(), with commands recorded once and
// submitted before every present with no fence of their own, as a render pass would be. A clear of
// an _SRGB image takes linear values and encodes them, as the tone-mapping pass's writes are
// encoded by its format.
//
// The two ways a producer can treat the image between frames test the two halves of the
// presenter's contract (PresentSource):
// - keeps_layout: the image goes to TRANSFER_DST_OPTIMAL once, and every later draw is a bare
//   clear with no barrier of its own. Only the presenter's barrier back, made visible to the
//   clear's stage, orders that clear after the blit's read: the strict case.
// - discards: every draw first changes the layout from UNDEFINED, as M1-44's pass does. Layout
//   changes on one queue are ordered among themselves, so this also covers draws with no present
//   between them (a skipped frame), which two bare clears would not.
class DisplayImage {
  public:
    static constexpr VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    enum class Producer { keeps_layout, discards };

    DisplayImage(const Device& device, VkExtent2D extent, float linear, Producer producer)
        : device_(device.device()), queue_(device.queue()), allocator_(device.allocator()),
          extent_(extent) {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {extent.width, extent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        REQUIRE(vmaCreateImage(allocator_, &image_info, &allocation_info, &image_, &allocation_,
                               nullptr) == VK_SUCCESS);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = device.queue_family();
        REQUIRE(vkCreateCommandPool(device_, &pool_info, nullptr, &pool_) == VK_SUCCESS);
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 2;
        std::array<VkCommandBuffer, 2> buffers{};
        REQUIRE(vkAllocateCommandBuffers(device_, &command_info, buffers.data()) == VK_SUCCESS);
        first_ = buffers[0];
        again_ = buffers[1];

        VkClearColorValue colour{};
        colour.float32[0] = linear;
        colour.float32[1] = linear;
        colour.float32[2] = linear;
        colour.float32[3] = 1.0f;
        record(first_, colour, true);
        record(again_, colour, producer == Producer::discards);
    }

    ~DisplayImage() {
        vkDeviceWaitIdle(device_); // its submissions have no fence
        vkDestroyCommandPool(device_, pool_, nullptr);
        if (image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, image_, allocation_);
        }
    }
    DisplayImage(const DisplayImage&) = delete;
    DisplayImage& operator=(const DisplayImage&) = delete;

    void draw() {
        VkCommandBufferSubmitInfo command{};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command.commandBuffer = drawn_ ? again_ : first_;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        REQUIRE(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
        drawn_ = true;
    }

    // As draw() leaves it: written last by the clear, in TRANSFER_DST_OPTIMAL.
    [[nodiscard]] PresentSource source() const {
        PresentSource source;
        source.image = image_;
        source.format = format;
        source.extent = extent_;
        source.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        source.stages = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        source.accesses = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        return source;
    }

  private:
    static constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer first_ = VK_NULL_HANDLE; // changes the layout, then clears
    VkCommandBuffer again_ = VK_NULL_HANDLE; // the same, or only clears (keeps_layout)
    bool drawn_ = false;

    // A clear, after a change of layout from UNDEFINED (discarding the image) when asked.
    void record(VkCommandBuffer commands, const VkClearColorValue& colour, bool change_layout) {
        // Submitted again while an earlier submission may still be pending.
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        REQUIRE(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
        if (change_layout) {
            VkImageMemoryBarrier2 discard{};
            discard.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            discard.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            discard.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            discard.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            discard.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            discard.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            discard.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            discard.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            discard.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            discard.image = image_;
            discard.subresourceRange = range;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &discard;
            vkCmdPipelineBarrier2(commands, &dependency);
        }
        vkCmdClearColorImage(commands, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1,
                             &range);
        REQUIRE(vkEndCommandBuffer(commands) == VK_SUCCESS);
    }
};

bool same(VkExtent2D a, VkExtent2D b) {
    return a.width == b.width && a.height == b.height;
}

// The sRGB encoding of a linear value in [0, 1], as 8 bits (IEC 61966-2-1).
int srgb_byte(double linear) {
    const double encoded =
        linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(encoded * 255.0));
}

constexpr VkSurfaceFormatKHR srgb(VkFormat format) {
    return {format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
}

bool any_format(VkFormat /*format*/) {
    return true;
}

} // namespace

// ---- The swapchain's format: sRGB, blittable, B8G8R8A8 first (owner's comment) -----------------

TEST_CASE("presenter: the swapchain format is B8G8R8A8_SRGB, else R8G8B8A8_SRGB, where the "
          "device can blit to it",
          "[gpu]") {
    const std::vector<VkSurfaceFormatKHR> both = {srgb(VK_FORMAT_B8G8R8A8_UNORM),
                                                  srgb(VK_FORMAT_R8G8B8A8_SRGB),
                                                  srgb(VK_FORMAT_B8G8R8A8_SRGB)};

    SECTION("B8G8R8A8_SRGB is preferred whatever the surface's order") {
        const auto chosen = choose_surface_format(both, any_format);
        REQUIRE(chosen);
        CHECK(chosen->format == VK_FORMAT_B8G8R8A8_SRGB);
        CHECK(chosen->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    }
    SECTION("R8G8B8A8_SRGB when the device cannot blit to B8G8R8A8_SRGB") {
        const auto chosen = choose_surface_format(
            both, [](VkFormat format) { return format != VK_FORMAT_B8G8R8A8_SRGB; });
        REQUIRE(chosen);
        CHECK(chosen->format == VK_FORMAT_R8G8B8A8_SRGB);
    }
    SECTION("R8G8B8A8_SRGB when the surface offers only it") {
        const std::vector<VkSurfaceFormatKHR> offered = {srgb(VK_FORMAT_R8G8B8A8_UNORM),
                                                         srgb(VK_FORMAT_R8G8B8A8_SRGB)};
        const auto chosen = choose_surface_format(offered, any_format);
        REQUIRE(chosen);
        CHECK(chosen->format == VK_FORMAT_R8G8B8A8_SRGB);
    }
    SECTION("an sRGB format in another colour space is not taken") {
        const std::vector<VkSurfaceFormatKHR> offered = {
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
            srgb(VK_FORMAT_R8G8B8A8_SRGB)};
        const auto chosen = choose_surface_format(offered, any_format);
        REQUIRE(chosen);
        CHECK(chosen->format == VK_FORMAT_R8G8B8A8_SRGB);
        CHECK(chosen->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    }
}

TEST_CASE("presenter: no format is chosen, rather than one that would encode twice or not at "
          "all, when the surface offers no sRGB format the device can blit to",
          "[gpu]") {
    // UNORM formats would show the display image's sRGB bytes as linear values: encoded twice.
    const std::vector<VkSurfaceFormatKHR> unorm = {srgb(VK_FORMAT_B8G8R8A8_UNORM),
                                                   srgb(VK_FORMAT_R8G8B8A8_UNORM),
                                                   srgb(VK_FORMAT_A2B10G10R10_UNORM_PACK32)};
    CHECK_FALSE(choose_surface_format(unorm, any_format));
    CHECK_FALSE(choose_surface_format({}, any_format));
    CHECK_FALSE(choose_surface_format(
        std::vector{srgb(VK_FORMAT_B8G8R8A8_SRGB), srgb(VK_FORMAT_R8G8B8A8_SRGB)},
        [](VkFormat) { return false; }));
    CHECK_FALSE(choose_surface_format(
        std::vector<VkSurfaceFormatKHR>{
            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT}},
        any_format));
}

// ---- Criterion 1: a swapchain on any surface, made again on resize, two frames in flight -------

TEST_CASE("presenter: a presenter makes an sRGB swapchain of the window's size on the surface",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device(presenting_options());
    const HeadlessSurface surface(*gpu);

    const Presenter presenter(*gpu, surface.get(), 320, 240);
    CHECK(Presenter::frames_in_flight == 2);
    CHECK((presenter.format() == VK_FORMAT_B8G8R8A8_SRGB ||
           presenter.format() == VK_FORMAT_R8G8B8A8_SRGB));
    CHECK(same(presenter.extent(), {320, 240}));
    CHECK(presenter.swapchains_created() == 1);
}

TEST_CASE("presenter: a device made without presentation, or a null surface, is refused", "[gpu]") {
    SECTION("a headless device") {
        const std::unique_ptr<Device> gpu = make_device();
        REQUIRE_FALSE(gpu->presentation());
        CHECK_THROWS_AS(Presenter(*gpu, VK_NULL_HANDLE, 64, 64), std::invalid_argument);
    }
    SECTION("a presenting device and no surface") {
        const std::unique_ptr<Device> gpu = make_device(presenting_options());
        CHECK_THROWS_AS(Presenter(*gpu, VK_NULL_HANDLE, 64, 64), std::invalid_argument);
    }
}

TEST_CASE("presenter: a source with no image, no area, an undefined layout or a format it cannot "
          "blit from is refused",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device(presenting_options());
    const HeadlessSurface surface(*gpu);
    DisplayImage display(*gpu, {64, 48}, 0.5f, DisplayImage::Producer::discards);
    Presenter presenter(*gpu, surface.get(), 64, 48);

    PresentSource no_image = display.source();
    no_image.image = VK_NULL_HANDLE;
    CHECK_THROWS_AS(presenter.present(no_image), std::invalid_argument);
    PresentSource no_area = display.source();
    no_area.extent = {0, 48};
    CHECK_THROWS_AS(presenter.present(no_area), std::invalid_argument);
    PresentSource huge = display.source(); // a blit's offsets are signed 32-bit
    huge.extent = {0x8000'0000u, 48};
    CHECK_THROWS_AS(presenter.present(huge), std::invalid_argument);
    PresentSource undefined = display.source();
    undefined.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK_THROWS_AS(presenter.present(undefined), std::invalid_argument);
    // With no stages the blit would wait for nothing, and the barrier back would order nothing.
    PresentSource no_stages = display.source();
    no_stages.stages = VK_PIPELINE_STAGE_2_NONE;
    CHECK_THROWS_AS(presenter.present(no_stages), std::invalid_argument);
    // No device may filter an integer format linearly, so none can stretch it in a blit.
    PresentSource integer = display.source();
    integer.format = VK_FORMAT_R8G8B8A8_UINT;
    CHECK_THROWS_MATCHES(presenter.present(integer), std::invalid_argument,
                         MessageMatches(ContainsSubstring("linear filter")));

    // None of them touched the swapchain: a good source still presents.
    display.draw();
    CHECK(presenter.present(display.source()));
}

// ---- Criterion 2: 100 frames, 3 resizes, validation clean, nothing left at shutdown ------------

TEST_CASE("presenter: 100 frames on a headless surface with 3 resizes between them are "
          "validation clean and leave no Vulkan object at shutdown",
          "[gpu]") {
    std::unique_ptr<Device> gpu = make_device(presenting_options());
    auto surface = std::make_unique<HeadlessSurface>(*gpu);
    auto display = std::make_unique<DisplayImage>(*gpu, VkExtent2D{320, 240}, 0.5f,
                                                  DisplayImage::Producer::keeps_layout);
    const std::uint64_t allocations_before = cs::gpu::memory_in_use(*gpu).allocations;
    auto presenter = std::make_unique<Presenter>(*gpu, surface->get(), 320, 240);

    // Each resize before its frame; the source keeps its size and is stretched to the window's.
    struct Resize {
        int frame;
        VkExtent2D extent;
    };
    constexpr std::array<Resize, 3> resizes{{{25, {640, 360}}, {50, {200, 500}}, {75, {97, 61}}}};
    VkExtent2D window{320, 240};
    int presented = 0;
    for (int frame = 0; frame < 100; ++frame) {
        for (const Resize& resize : resizes) {
            if (resize.frame == frame) {
                window = resize.extent;
                presenter->resize(window.width, window.height);
            }
        }
        display->draw();
        INFO("frame " << frame << " at " << window.width << "x" << window.height);
        bool shown = false;
        if (frame == 99 && presenter->can_read_back()) {
            // The last frame is read back, so that a read-back buffer is among what must be freed.
            const std::optional<PresentedImage> image =
                presenter->present_and_read_back(display->source());
            shown = image.has_value();
            if (image) {
                CHECK(image->width == window.width);
                CHECK(image->height == window.height);
            }
        } else {
            shown = presenter->present(display->source());
        }
        if (shown) {
            ++presented;
        }
        // A headless surface has no size of its own: the swapchain takes the window's.
        CHECK(same(presenter->extent(), window));
    }
    CHECK(presented == 100);
    CHECK(presenter->swapchains_created() == 4); // the first, and one for each resize

    // The presenter frees every allocation it made, the last frame's read-back buffer included...
    presenter.reset();
    CHECK(cs::gpu::memory_in_use(*gpu).allocations == allocations_before);
    // ...and every other object: the validation layer's object tracker reports any object still
    // alive when the device is destroyed, and any surface when the instance is, as an error,
    // which fails this test (main.cpp). The objects go here, inside the test, in their order.
    display.reset();
    surface.reset();
    const bool layer_checked = gpu->report().validation;
    gpu.reset();
    if (!layer_checked) {
        WARN("the validation layer is not loaded, so no object left alive could be reported");
    }
}

TEST_CASE("presenter: a window with no area skips frames, and the swapchain returns with the "
          "next size",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device(presenting_options());
    const HeadlessSurface surface(*gpu);
    // Discarding: two draws meet with no present between them when a frame is skipped.
    DisplayImage display(*gpu, {64, 48}, 0.5f, DisplayImage::Producer::discards);

    SECTION("minimised after a frame") {
        Presenter presenter(*gpu, surface.get(), 64, 48);
        display.draw();
        CHECK(presenter.present(display.source()));
        presenter.resize(0, 0);
        display.draw();
        CHECK_FALSE(presenter.present(display.source()));
        presenter.resize(128, 96);
        display.draw();
        CHECK(presenter.present(display.source()));
        CHECK(same(presenter.extent(), {128, 96}));
        CHECK(presenter.swapchains_created() == 2);
    }
    SECTION("made with no area") {
        Presenter presenter(*gpu, surface.get(), 0, 0);
        CHECK(same(presenter.extent(), {0, 0}));
        CHECK(presenter.swapchains_created() == 0);
        display.draw();
        CHECK_FALSE(presenter.present(display.source()));
        presenter.resize(64, 48);
        display.draw();
        CHECK(presenter.present(display.source()));
        CHECK(same(presenter.extent(), {64, 48}));
        CHECK(presenter.swapchains_created() == 1);
    }
}

// ---- Owner's comment: the colours arrive encoded once ------------------------------------------

TEST_CASE("presenter: a uniform display image of linear 0.5 reads back from the swapchain as "
          "188, encoded once",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device(presenting_options());
    const HeadlessSurface surface(*gpu);
    DisplayImage display(*gpu, {64, 48}, 0.5f, DisplayImage::Producer::keeps_layout);
    Presenter presenter(*gpu, surface.get(), 64, 48);
    REQUIRE(presenter.can_read_back()); // lavapipe's swapchain images allow TRANSFER_SRC

    // Encoded once: 0.5 is 187.5 in sRGB, so 188. Not encoded at all it would be 128; encoded
    // twice, 223. Within 1, as the GPU's sRGB conversion (decode in the blit, encode on the
    // write) may round either way of a value this close to a half.
    const int expected = srgb_byte(0.5);
    REQUIRE(expected == 188);
    REQUIRE(srgb_byte(srgb_byte(0.5) / 255.0) == 223);

    // A few frames first, so that the one read back has another in flight behind it.
    for (int frame = 0; frame < 3; ++frame) {
        display.draw();
        REQUIRE(presenter.present(display.source()));
    }
    display.draw();
    const std::optional<PresentedImage> image = presenter.present_and_read_back(display.source());
    REQUIRE(image);
    CHECK(image->width == 64);
    CHECK(image->height == 48);
    REQUIRE(image->rgba.size() == std::size_t{64} * 48 * 4);

    int worst = 0;
    std::size_t worst_at = 0;
    for (std::size_t i = 0; i < image->rgba.size(); ++i) {
        const int want = i % 4 == 3 ? 255 : expected; // alpha 1 stays 255
        const int error = std::abs(int{image->rgba[i]} - want);
        if (error > worst) {
            worst = error;
            worst_at = i;
        }
    }
    INFO("the worst byte, " << int{image->rgba[worst_at]} << ", is channel " << worst_at % 4
                            << " of pixel " << worst_at / 4);
    CHECK(worst <= 1);

    // The presenter keeps presenting after a read-back.
    display.draw();
    CHECK(presenter.present(display.source()));
}
