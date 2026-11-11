// SPDX-License-Identifier: Apache-2.0
// Throwaway program for spike M1-03 (issue #38): times a headless Vulkan render and readback on
// lavapipe and other drivers. Not product code; it lives only on the spike/lavapipe branch.
//
//   lvpbench --mode raster|rq --size 256 --spp 64 [--bounces 2] [--lights 8] [--validation]
//            [--sync] [--out image.ppm]
//
// Prints one JSON line with the timings and exits non-zero if the validation layer reported
// anything.
#include <vulkan/vulkan.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "raster_frag_spv.h"
#include "raster_vert_spv.h"
#include "rq_comp_spv.h"

#define VK_CHECK(x)                                                                       \
    do {                                                                                  \
        VkResult r_ = (x);                                                                \
        if (r_ != VK_SUCCESS) {                                                           \
            std::fprintf(stderr, "%s failed: %d (line %d)\n", #x, (int)r_, __LINE__);   \
            std::exit(2);                                                                 \
        }                                                                                 \
    } while (0)

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalize(V3 a) { return a * (1.0f / std::sqrt(dot(a, a))); }

struct Vertex { V3 pos, nrm, alb; };

struct Scene {
    std::vector<Vertex> verts;
    std::vector<float> tri;  // per triangle: normal xyz0, albedo xyz0
    void quad(V3 a, V3 b, V3 c, V3 d, V3 alb) {
        V3 n = normalize(cross(b - a, c - a));
        for (V3 p : {a, b, c, a, c, d}) verts.push_back({p, n, alb});
        for (int i = 0; i < 2; ++i) {
            tri.insert(tri.end(), {n.x, n.y, n.z, 0.0f, alb.x, alb.y, alb.z, 0.0f});
        }
    }
    void box(V3 lo, V3 hi, V3 alb, bool inward = false) {
        auto quad = [&](V3 a, V3 b, V3 c, V3 d, V3 al) { inward ? this->quad(d, c, b, a, al) : this->quad(a, b, c, d, al); };
        V3 p[8];
        for (int i = 0; i < 8; ++i) p[i] = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
        quad(p[0], p[2], p[3], p[1], alb);  // -z
        quad(p[4], p[5], p[7], p[6], alb);  // +z
        quad(p[0], p[4], p[6], p[2], alb);  // -x
        quad(p[1], p[3], p[7], p[5], alb);  // +x
        quad(p[0], p[1], p[5], p[4], alb);  // -y
        quad(p[2], p[6], p[7], p[3], alb);  // +y
    }
};

// A room 8 x 3.2 x 6 m with a painting in a frame on the back wall and a bench.
Scene make_scene() {
    Scene s;
    s.box({-4, 0, -3}, {4, 3.2f, 3}, {0.6f, 0.6f, 0.6f}, true);     // room, normals inward
    s.box({-0.7f, 1.05f, -3.0f}, {0.7f, 2.15f, -2.94f}, {0.25f, 0.18f, 0.1f});  // frame
    s.quad({-0.6f, 1.15f, -2.93f}, {0.6f, 1.15f, -2.93f}, {0.6f, 2.05f, -2.93f}, {-0.6f, 2.05f, -2.93f},
           {0.7f, 0.3f, 0.2f});                                      // painting
    s.box({-1.0f, 0.0f, -0.5f}, {1.0f, 0.45f, 0.0f}, {0.4f, 0.35f, 0.3f});      // bench
    return s;
}

struct Args {
    std::string mode = "raster";
    int size = 256;
    int spp = 16;
    int bounces = 2;
    int lights = 8;
    bool validation = false;
    bool sync = false;
    std::string out;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (k == "--mode") a.mode = next();
        else if (k == "--size") a.size = std::atoi(next());
        else if (k == "--spp") a.spp = std::atoi(next());
        else if (k == "--bounces") a.bounces = std::atoi(next());
        else if (k == "--lights") a.lights = std::atoi(next());
        else if (k == "--validation") a.validation = true;
        else if (k == "--sync") a.sync = true;
        else if (k == "--out") a.out = next();
        else { std::fprintf(stderr, "unknown argument %s\n", k.c_str()); std::exit(2); }
    }
    if (a.size < 8 || a.size > 4096 || a.spp < 1 || a.spp > 65536) { std::fprintf(stderr, "bad size or spp\n"); std::exit(2); }
    return a;
}

int g_validation_messages = 0;
VKAPI_ATTR VkBool32 VKAPI_CALL on_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++g_validation_messages;
        std::fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

struct Ctx {
    VkInstance instance{};
    VkPhysicalDevice phys{};
    VkDevice dev{};
    VkQueue queue{};
    uint32_t family = 0;
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkPhysicalDeviceMemoryProperties mem{};
};

uint32_t find_memory(const Ctx& c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c.mem.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (c.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    std::fprintf(stderr, "no memory type\n");
    std::exit(2);
}

struct Buffer { VkBuffer buf{}; VkDeviceMemory mem{}; void* map = nullptr; VkDeviceAddress addr = 0; };

Buffer make_buffer(const Ctx& c, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VK_CHECK(vkCreateBuffer(c.dev, &bi, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.dev, b.buf, &req);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(c.dev, &ai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(c.dev, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(c.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = b.buf;
    b.addr = vkGetBufferDeviceAddress(c.dev, &addr);
    return b;
}

struct Image { VkImage img{}; VkDeviceMemory mem{}; VkImageView view{}; };

Image make_image(const Ctx& c, int size, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image im;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {(uint32_t)size, (uint32_t)size, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VK_CHECK(vkCreateImage(c.dev, &ci, nullptr, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.dev, im.img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(c, req.memoryTypeBits, 0);
    VK_CHECK(vkAllocateMemory(c.dev, &ai, nullptr, &im.mem));
    VK_CHECK(vkBindImageMemory(c.dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(c.dev, &vi, nullptr, &im.view));
    return im;
}

void begin(const Ctx& c) {
    VK_CHECK(vkResetCommandBuffer(c.cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(c.cmd, &bi));
}

void submit_wait(const Ctx& c) {
    VK_CHECK(vkEndCommandBuffer(c.cmd));
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = c.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    VK_CHECK(vkQueueSubmit2(c.queue, 1, &si, c.fence));
    VK_CHECK(vkWaitForFences(c.dev, 1, &c.fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(c.dev, 1, &c.fence));
}

void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

VkShaderModule make_module(const Ctx& c, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(c.dev, &ci, nullptr, &m));
    return m;
}

bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

struct Camera { V3 pos, fwd, right, up; float tan_half; };

Camera make_camera() {
    Camera cam;
    cam.pos = {0.0f, 1.6f, 2.5f};
    cam.fwd = normalize(V3{0.0f, 1.5f, -3.0f} - cam.pos);
    cam.right = normalize(cross(cam.fwd, {0, 1, 0}));
    cam.up = cross(cam.right, cam.fwd);
    cam.tan_half = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    return cam;
}

// Column-major view-projection for Vulkan clip space (y down, depth 0..1), with a sub-pixel jitter.
void view_proj(const Camera& cam, float jx, float jy, float out[16]) {
    float v[16] = {cam.right.x, cam.up.x, -cam.fwd.x, 0, cam.right.y, cam.up.y, -cam.fwd.y, 0,
                   cam.right.z, cam.up.z, -cam.fwd.z, 0, -dot(cam.right, cam.pos), -dot(cam.up, cam.pos),
                   dot(cam.fwd, cam.pos), 1};
    const float n = 0.05f, f = 50.0f, fy = 1.0f / cam.tan_half;
    float p[16] = {fy, 0, 0, 0, 0, -fy, 0, 0, -jx, -jy, f / (n - f), -1, 0, 0, n * f / (n - f), 0};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += p[k * 4 + row] * v[col * 4 + k];
            out[col * 4 + row] = s;
        }
    }
}

float halton(int i, int base) {
    float f = 1, r = 0;
    while (i > 0) { f /= base; r += f * (i % base); i /= base; }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);
    const bool rq = args.mode == "rq";
    if (!rq && args.mode != "raster") { std::fprintf(stderr, "mode must be raster or rq\n"); return 2; }
    const auto t_start = Clock::now();
    Ctx c;

    // Instance
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> inst_exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, inst_exts.data());
    const bool headless = has_extension(inst_exts, "VK_EXT_headless_surface");
    std::vector<const char*> exts, layers;
    if (args.validation) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "lvpbench";
    app.apiVersion = VK_API_VERSION_1_3;
    VkValidationFeatureEnableEXT sync_enable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = &sync_enable;
    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = on_debug;
    if (args.sync) dbg.pNext = &features;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = args.validation ? &dbg : nullptr;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &c.instance));
    VkDebugUtilsMessengerEXT messenger{};
    if (args.validation) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(c.instance, "vkCreateDebugUtilsMessengerEXT");
        dbg.pNext = nullptr;
        VK_CHECK(create(c.instance, &dbg, nullptr, &messenger));
    }

    // Device
    vkEnumeratePhysicalDevices(c.instance, &n, nullptr);
    if (n == 0) { std::fprintf(stderr, "no Vulkan device\n"); return 3; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(c.instance, &n, devs.data());
    c.phys = devs[0];
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(c.phys, &props);
    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem);
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dev_exts(n);
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &n, dev_exts.data());
    const bool has_rq = has_extension(dev_exts, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                        has_extension(dev_exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    if (rq && !has_rq) { std::fprintf(stderr, "device has no ray query\n"); return 3; }
    if (props.properties.apiVersion < VK_API_VERSION_1_3) { std::fprintf(stderr, "device is not Vulkan 1.3\n"); return 3; }

    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &n, qf.data());
    for (uint32_t i = 0; i < n; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { c.family = i; break; }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = c.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceRayQueryFeaturesKHR rq_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq_feat.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq_feat};
    as_feat.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, rq ? (void*)&as_feat : nullptr};
    f13.synchronization2 = VK_TRUE;
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.bufferDeviceAddress = VK_TRUE;
    std::vector<const char*> dexts;
    if (rq) {
        dexts = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                 VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    VK_CHECK(vkCreateDevice(c.phys, &dci, nullptr, &c.dev));
    vkGetDeviceQueue(c.dev, c.family, 0, &c.queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c.family;
    VK_CHECK(vkCreateCommandPool(c.dev, &pci, nullptr, &c.pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(c.dev, &cai, &c.cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(c.dev, &fci, nullptr, &c.fence));
    const double ms_device = ms_since(t_start);

    // Scene
    auto t = Clock::now();
    Scene scene = make_scene();
    const uint32_t vcount = (uint32_t)scene.verts.size();
    Buffer vb = make_buffer(c, sizeof(Vertex) * vcount,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                (rq ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0));
    std::memcpy(vb.map, scene.verts.data(), sizeof(Vertex) * vcount);
    Buffer tb = make_buffer(c, sizeof(float) * scene.tri.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(tb.map, scene.tri.data(), sizeof(float) * scene.tri.size());
    const int sz = args.size;
    const VkFormat color_fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(c.phys, color_fmt, &fp);
    if (!rq && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
        std::fprintf(stderr, "no blending on RGBA32F\n");
        return 3;
    }
    Image color = make_image(c, sz, color_fmt,
                             (rq ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth{};
    if (!rq) depth = make_image(c, sz, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    Buffer readback = make_buffer(c, (VkDeviceSize)sz * sz * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Acceleration structures (rq only)
    VkAccelerationStructureKHR blas{}, tlas{};
    std::vector<Buffer> keep;
    if (rq) {
        auto get_sizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(c.dev, "vkGetAccelerationStructureBuildSizesKHR");
        auto create_as = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(c.dev, "vkCreateAccelerationStructureKHR");
        auto build_as = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(c.dev, "vkCmdBuildAccelerationStructuresKHR");
        auto as_addr = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(c.dev, "vkGetAccelerationStructureDeviceAddressKHR");
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asp};
        vkGetPhysicalDeviceProperties2(c.phys, &p2);
        const VkDeviceSize align = asp.minAccelerationStructureScratchOffsetAlignment;

        auto build = [&](VkAccelerationStructureGeometryKHR& geom, uint32_t prims, VkAccelerationStructureTypeKHR type,
                         VkAccelerationStructureKHR& as) {
            VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            info.type = type;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geom;
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            get_sizes(c.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &prims, &sizes);
            Buffer storage = make_buffer(c, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
            Buffer scratch = make_buffer(c, sizes.buildScratchSize + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            keep.push_back(storage);
            keep.push_back(scratch);
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = storage.buf;
            ci.size = sizes.accelerationStructureSize;
            ci.type = type;
            VK_CHECK(create_as(c.dev, &ci, nullptr, &as));
            info.dstAccelerationStructure = as;
            info.scratchData.deviceAddress = (scratch.addr + align - 1) / align * align;
            VkAccelerationStructureBuildRangeInfoKHR range{prims, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
            begin(c);
            build_as(c.cmd, 1, &info, &ranges);
            memory_barrier(c.cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                           VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                           VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            submit_wait(c);
        };

        VkAccelerationStructureGeometryKHR tri{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tri.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        tri.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        tri.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.geometry.triangles.vertexData.deviceAddress = vb.addr;
        tri.geometry.triangles.vertexStride = sizeof(Vertex);
        tri.geometry.triangles.maxVertex = vcount - 1;
        tri.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        build(tri, vcount / 3, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blas);

        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = blas;
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = as_addr(c.dev, &ai);
        Buffer ib = make_buffer(c, sizeof(inst), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        std::memcpy(ib.map, &inst, sizeof(inst));
        keep.push_back(ib);
        VkAccelerationStructureGeometryKHR ig{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        ig.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        ig.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        ig.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        ig.geometry.instances.data.deviceAddress = ib.addr;
        build(ig, 1, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlas);
    }
    const double ms_scene = ms_since(t);

    // Pipeline
    t = Clock::now();
    VkPipelineLayout layout{};
    VkPipeline pipe{};
    VkDescriptorSetLayout dsl{};
    VkDescriptorSet set{};
    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, 112};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    if (rq) {
        VkDescriptorSetLayoutBinding b[3] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo dci2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dci2.bindingCount = 3;
        dci2.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(c.dev, &dci2, nullptr, &dsl));
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 3;
        dpi.pPoolSizes = ps;
        VkDescriptorPool dp;
        VK_CHECK(vkCreateDescriptorPool(c.dev, &dpi, nullptr, &dp));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dp;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl;
        VK_CHECK(vkAllocateDescriptorSets(c.dev, &dai, &set));
        VkWriteDescriptorSetAccelerationStructureKHR was{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        was.accelerationStructureCount = 1;
        was.pAccelerationStructures = &tlas;
        VkDescriptorImageInfo dii{VK_NULL_HANDLE, color.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo dbi{tb.buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3] = {};
        for (int i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = (uint32_t)i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = b[i].descriptorType;
        }
        w[0].pNext = &was;
        w[1].pImageInfo = &dii;
        w[2].pBufferInfo = &dbi;
        vkUpdateDescriptorSets(c.dev, 3, w, 0, nullptr);
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &dsl;
        VK_CHECK(vkCreatePipelineLayout(c.dev, &lci, nullptr, &layout));
        VkShaderModule m = make_module(c, rq_comp_spv, sizeof(rq_comp_spv));
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        cpi.layout = layout;
        VK_CHECK(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
    } else {
        VK_CHECK(vkCreatePipelineLayout(c.dev, &lci, nullptr, &layout));
        VkShaderModule vs = make_module(c, raster_vert_spv, sizeof(raster_vert_spv));
        VkShaderModule fs = make_module(c, raster_frag_spv, sizeof(raster_frag_spv));
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
        VkVertexInputBindingDescription vbd{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription vad[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                    {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vbd;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = vad;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.srcAlphaBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT;
        VkPipelineRenderingCreateInfo pri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        pri.colorAttachmentCount = 1;
        pri.pColorAttachmentFormats = &color_fmt;
        pri.depthAttachmentFormat = depth_fmt;
        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri};
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vi;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds;
        gpi.pColorBlendState = &cb;
        gpi.pDynamicState = &dy;
        gpi.layout = layout;
        VK_CHECK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
    }
    const double ms_pipeline = ms_since(t);

    const Camera cam = make_camera();
    const float aspect = 1.0f;
    const V3 light_pos{0.0f, 3.1f, -1.2f};
    const V3 light_dir = normalize(V3{0.0f, 1.6f, -2.93f} - light_pos);
    const float intensity_cd = 2000.0f, cos_outer = std::cos(0.35f), cos_inner = std::cos(0.25f);

    // One render of `spp` samples: clear, accumulate, leave the image ready for a transfer read.
    auto render = [&](int spp) {
        begin(c);
        const VkImageLayout work_layout = rq ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_barrier(c.cmd, color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c.cmd, color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        const VkPipelineStageFlags2 work_stage =
            rq ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkAccessFlags2 work_access =
            rq ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
               : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier(c.cmd, color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, work_layout,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, work_stage, work_access);
        if (!rq) {
            image_barrier(c.cmd, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
        vkCmdBindPipeline(c.cmd, rq ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        float pc[28] = {};
        for (int s = 0; s < spp; ++s) {
            const float weight = 1.0f / (float)spp;
            if (rq) {
                vkCmdBindDescriptorSets(c.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                const V3 r = cam.right * (cam.tan_half * aspect), u = cam.up * cam.tan_half;
                const float v[28] = {cam.pos.x, cam.pos.y, cam.pos.z, 0, r.x, r.y, r.z, 0, u.x, u.y, u.z, 0,
                                     cam.fwd.x, cam.fwd.y, cam.fwd.z, 0, light_pos.x, light_pos.y, light_pos.z, intensity_cd,
                                     light_dir.x, light_dir.y, light_dir.z, cos_outer, cos_inner, (float)s, weight,
                                     (float)args.bounces};
                std::memcpy(pc, v, sizeof(pc));
                vkCmdPushConstants(c.cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), pc);
                vkCmdDispatch(c.cmd, (uint32_t)(sz + 7) / 8, (uint32_t)(sz + 7) / 8, 1);
                memory_barrier(c.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            } else {
                const float jx = (halton(s + 1, 2) - 0.5f) * 2.0f / (float)sz, jy = (halton(s + 1, 3) - 0.5f) * 2.0f / (float)sz;
                view_proj(cam, jx, jy, pc);
                const float v[12] = {light_pos.x, light_pos.y, light_pos.z, intensity_cd, light_dir.x, light_dir.y, light_dir.z,
                                     cos_outer, cos_inner, (float)args.lights, weight, 0};
                std::memcpy(pc + 16, v, sizeof(v));
                VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                ca.imageView = color.view;
                ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                da.imageView = depth.view;
                da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                da.clearValue.depthStencil = {1.0f, 0};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {(uint32_t)sz, (uint32_t)sz}};
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &ca;
                ri.pDepthAttachment = &da;
                vkCmdBeginRendering(c.cmd, &ri);
                VkViewport viewport{0, 0, (float)sz, (float)sz, 0, 1};
                VkRect2D scissor{{0, 0}, {(uint32_t)sz, (uint32_t)sz}};
                vkCmdSetViewport(c.cmd, 0, 1, &viewport);
                vkCmdSetScissor(c.cmd, 0, 1, &scissor);
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(c.cmd, 0, 1, &vb.buf, &off);
                vkCmdPushConstants(c.cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), pc);
                vkCmdDraw(c.cmd, vcount, 1, 0, 0);
                vkCmdEndRendering(c.cmd);
                memory_barrier(c.cmd,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            }
        }
        image_barrier(c.cmd, color.img, VK_IMAGE_ASPECT_COLOR_BIT, work_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, work_stage,
                      rq ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        submit_wait(c);
    };

    std::vector<float> pixels((size_t)sz * sz * 4);
    auto read_back = [&]() {
        begin(c);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {(uint32_t)sz, (uint32_t)sz, 1};
        vkCmdCopyImageToBuffer(c.cmd, color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &region);
        memory_barrier(c.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                       VK_ACCESS_2_HOST_READ_BIT);
        submit_wait(c);
        std::memcpy(pixels.data(), readback.map, pixels.size() * sizeof(float));
    };

    t = Clock::now();
    render(1);
    const double ms_first_frame = ms_since(t);
    t = Clock::now();
    render(args.spp);
    const double ms_render = ms_since(t);
    t = Clock::now();
    read_back();
    const double ms_readback = ms_since(t);

    double mean = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) mean += pixels[i] + pixels[i + 1] + pixels[i + 2];
    mean /= (double)sz * sz * 3;
    if (!args.out.empty()) {
        FILE* f = std::fopen(args.out.c_str(), "wb");
        if (f) {
            std::fprintf(f, "P6\n%d %d\n255\n", sz, sz);
            for (size_t i = 0; i < pixels.size(); i += 4) {
                for (int k = 0; k < 3; ++k) {
                    const float v = 1.0f - std::exp(-pixels[i + k] * 0.03f);
                    std::fputc((int)(std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f), f);
                }
            }
            std::fclose(f);
        }
    }

    vkDeviceWaitIdle(c.dev);
    const double ms_total = ms_since(t_start);
    std::printf(
        "{\"mode\":\"%s\",\"size\":%d,\"spp\":%d,\"bounces\":%d,\"lights\":%d,\"validation\":%s,\"sync\":%s,"
        "\"device\":\"%s\",\"driver\":\"%s %s\",\"api\":\"%u.%u.%u\",\"ray_query\":%s,\"headless_surface\":%s,"
        "\"ms_device\":%.1f,\"ms_scene\":%.1f,\"ms_pipeline\":%.1f,\"ms_first_frame\":%.1f,\"ms_render\":%.1f,"
        "\"ms_per_sample\":%.2f,\"ms_readback\":%.2f,\"ms_total\":%.1f,\"mean\":%.4f,\"validation_messages\":%d}\n",
        args.mode.c_str(), sz, args.spp, args.bounces, args.lights, args.validation ? "true" : "false",
        args.sync ? "true" : "false", props.properties.deviceName, driver.driverName, driver.driverInfo,
        VK_API_VERSION_MAJOR(props.properties.apiVersion), VK_API_VERSION_MINOR(props.properties.apiVersion),
        VK_API_VERSION_PATCH(props.properties.apiVersion), has_rq ? "true" : "false", headless ? "true" : "false",
        ms_device, ms_scene, ms_pipeline, ms_first_frame, ms_render, ms_render / args.spp, ms_readback, ms_total, mean,
        g_validation_messages);
    // Resources are left to process exit: this is a throwaway timing program.
    return g_validation_messages == 0 ? 0 : 1;
}
