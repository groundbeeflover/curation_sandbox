# pt: the command-line path tracer. It renders a scene to an image file.
add_executable(pt main.cpp)
target_link_libraries(pt PRIVATE cs::core cs_options)
