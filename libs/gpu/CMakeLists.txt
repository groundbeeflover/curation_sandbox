# SPDX-License-Identifier: Apache-2.0
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# Shaders are compiled at build time, never at run time (ADR 0001), so only Slang's compiler is
# needed: the package defines the imported executable slang::slangc, which cs_add_shaders runs
# (cmake/CsShaders.cmake). Its library is not linked. It is a host tool (vcpkg.json), installed for the host triplet.
find_package(slang CONFIG REQUIRED HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}")

# Vulkan through volk, which loads every function at run time, so no loader is linked and no
# file may see the loader's prototypes: otherwise a vulkan.h included before volk.h (by
# VkBootstrap.h, say) declares functions that volk declares again as pointers. volk's target
# does not say so itself. VMA then fetches its functions at run time too.
# volk is header-only here: exactly one gpu source (device.cpp) defines VOLK_IMPLEMENTATION before
# including volk.h. No VK_USE_PLATFORM_* define is needed: the device enables the window
# systems' surface extensions by name, and whoever owns the window creates the surface.
add_library(cs_gpu_vulkan INTERFACE)
target_compile_definitions(cs_gpu_vulkan INTERFACE VK_NO_PROTOTYPES)
target_link_libraries(cs_gpu_vulkan INTERFACE Vulkan::Headers volk::volk_headers)

# vk-bootstrap for the instance and device, VMA for memory.
# core, world and assets are exported, as their types are in gpu's public headers:
# OffscreenTarget::render returns a cs::Image, a Scene is made from a world::World with an
# assets::Texture loader, and StandardTier::record takes a world::Camera.
cs_add_module(NAME gpu DEPS core world light material assets EXPORTS core world assets
    EXTERNAL cs_gpu_vulkan vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator)

# Every shaders/*.slang, compiled to SPIR-V and embedded (cs/gpu/pipeline.hpp).
cs_add_shaders(cs_gpu "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

if(CS_BUILD_TESTS)
    # A shader that does not compile fails the build. A target outside `all` embeds one shader
    # with a syntax error, written here so that no broken file sits in the source tree; the test
    # builds it and passes when slangc reports the error at the broken line (the build then fails,
    # as a failing custom command always does).
    set(broken_dir "${CMAKE_CURRENT_BINARY_DIR}/broken_shader")
    file(CONFIGURE OUTPUT "${broken_dir}/broken.slang" CONTENT [=[
[shader("fragment")]
float4 fragment_main() : SV_Target { return float4(1, 0, 0, 1) }
]=])
    add_library(cs_gpu_broken_shader OBJECT EXCLUDE_FROM_ALL)
    target_link_libraries(cs_gpu_broken_shader PRIVATE cs::gpu)
    cs_add_shaders(cs_gpu_broken_shader "${broken_dir}")
    add_test(NAME "gpu: a shader with a syntax error fails the build"
        COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --config $<CONFIG>
            --target cs_gpu_broken_shader)
    set_tests_properties("gpu: a shader with a syntax error fails the build" PROPERTIES
        PASS_REGULAR_EXPRESSION "error\\[E[0-9]+\\][^\n]*\n[^\n]*broken\\.slang:2:"
        # It runs the build tool in the build tree, so never beside another test under -j.
        RUN_SERIAL TRUE)
endif()
