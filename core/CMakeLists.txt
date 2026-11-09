# cs_core is the GPU-free heart of the project: images, photometry and, soon,
# the CPU path tracer. It must never depend on Vulkan or window code, so it
# builds and tests on any machine, and later renderers can be checked against it.
add_library(cs_core
    src/image.cpp
)
add_library(cs::core ALIAS cs_core)

target_include_directories(cs_core PUBLIC include)
target_link_libraries(cs_core PRIVATE cs_options)
