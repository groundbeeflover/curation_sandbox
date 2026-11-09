include(FetchContent)

# Catch2 is the unit-test framework. CMake uses an installed copy if it finds
# one, and otherwise downloads this exact version.
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.1
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS 3
)
FetchContent_MakeAvailable(Catch2)

# Add each new test file to this list.
add_executable(cs_tests
    test_image.cpp
    test_photometry.cpp
)
target_link_libraries(cs_tests PRIVATE cs::core cs_options Catch2::Catch2WithMain)

# Registers every TEST_CASE with CTest, so `ctest` lists and runs them one by one.
if(catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
include(Catch)
catch_discover_tests(cs_tests)
