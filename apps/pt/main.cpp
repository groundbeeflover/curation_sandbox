// pt - the command-line path tracer.
//
// For now this only proves that the toolchain works: it writes a colour
// gradient, the first image in "Ray Tracing in One Weekend" (chapter 2).
// It applies the sRGB curve, so it looks a little brighter than the book's
// version; the book adds gamma correction a few chapters later.
//
// As you work through the book, keep this file thin: put vec3, ray, camera,
// materials and so on in the core library, and use them from here.

#include <cs/core/image.hpp>

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char* argv[]) {
    const std::filesystem::path output = argc > 1 ? argv[1] : "renders/first.ppm";

    constexpr int width = 256;
    constexpr int height = 256;
    cs::Image image(width, height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Red grows from left to right and green from top to bottom, as in the book.
            const float r = static_cast<float>(x) / (width - 1);
            const float g = static_cast<float>(y) / (height - 1);
            image.at(x, y) = cs::Rgb{r, g, 0.0f};
        }
    }

    try {
        if (output.has_parent_path()) {
            std::filesystem::create_directories(output.parent_path());
        }
        image.write_ppm(output);
    } catch (const std::exception& error) {
        std::cerr << "pt: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Wrote " << output.string() << " (" << width << " x " << height << ")\n";
    return 0;
}
