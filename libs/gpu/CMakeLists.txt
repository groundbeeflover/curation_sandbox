# SPDX-License-Identifier: Apache-2.0
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# Shaders are compiled at build time, never at run time (ADR 0001), so only Slang's compiler is
# needed: the package defines the imported executable slang::slangc for the shader build (M1-20).
# Its library is not linked. It is a host tool (vcpkg.json), installed for the host triplet.
find_package(slang CONFIG REQUIRED HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}")

# Vulkan through volk, which loads every function at run time, so no loader is linked and no
# file may see the loader's prototypes: otherwise a vulkan.h included before volk.h (by
# VkBootstrap.h, say) declares functions that volk declares again as pointers. volk's target
# does not say so itself. VMA then fetches its functions at run time too.
# volk is header-only here: exactly one gpu source defines VOLK_IMPLEMENTATION, with the
# VK_USE_PLATFORM_* defines its surfaces need, before including volk.h (M1-19). The prebuilt
# volk::volk is compiled without platform defines, so it never loads surface functions.
add_library(cs_gpu_vulkan INTERFACE)
target_compile_definitions(cs_gpu_vulkan INTERFACE VK_NO_PROTOTYPES)
target_link_libraries(cs_gpu_vulkan INTERFACE Vulkan::Headers volk::volk_headers)

# vk-bootstrap for the instance and device, VMA for memory.
cs_add_module(NAME gpu DEPS core world light material assets
    EXTERNAL cs_gpu_vulkan vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator)
