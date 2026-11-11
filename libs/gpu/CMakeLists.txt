# SPDX-License-Identifier: Apache-2.0
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# Shaders are compiled at build time, never at run time (ADR 0001), so only Slang's compiler is
# needed: the package sets SLANGC_EXECUTABLE for the shader build (M1-20). Its library is not
# linked.
find_package(slang CONFIG REQUIRED)

# Vulkan through volk, which loads every function at run time, so no loader is linked and no
# file may see the loader's prototypes: otherwise a vulkan.h included before volk.h (by
# VkBootstrap.h, say) declares functions that volk declares again as pointers. volk's target
# does not say so itself. VMA then fetches its functions at run time too.
add_library(cs_gpu_vulkan INTERFACE)
target_compile_definitions(cs_gpu_vulkan INTERFACE VK_NO_PROTOTYPES)
target_link_libraries(cs_gpu_vulkan INTERFACE Vulkan::Headers volk::volk)

# vk-bootstrap for the instance and device, VMA for memory.
cs_add_module(NAME gpu DEPS core world light material assets
    EXTERNAL cs_gpu_vulkan vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator)
