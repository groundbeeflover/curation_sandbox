# Throwaway program for spike M1-03 (issue #38); lives only on the spike/lavapipe branch.
cmake_minimum_required(VERSION 3.25)
project(lvpbench CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Vulkan REQUIRED)
add_executable(lvpbench main.cpp)
target_include_directories(lvpbench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lvpbench PRIVATE Vulkan::Vulkan)
