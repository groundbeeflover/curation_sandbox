# Test data for libs/core

## `flip_reference.exr` and `flip_test.exr`

A pair of 48 x 32 half-float EXR images (ZIP) for the FLIP test in `test_image_compare.cpp`: a
wall in nits lit by a spotlight pool, a striped painting and a bright lamp at the top; the test
image has multiplicative noise of ±20 % (like an unconverged path tracer) and the painting moved
one pixel to the right. They were written by the hidden test case `[.generate-flip-pair]`:

```sh
./build/debug/libs/core/cs_core_tests "[.generate-flip-pair]"
```

which overwrites both files here, so run it only on purpose.

### The expected mean FLIP

The test's expected value, **0.145033047**, comes from NVIDIA's reference implementation
(`third_party/flip/FLIP.h`, byte-identical to upstream at the commit in its README), called the
way its FLIP tool treats EXR input: HDR-FLIP with the default parameters (67.02 pixels per
degree, the ACES tone mapper, exposures found from the reference: -11.147449 to -4.998166 in 7
steps). The program below uses only tinyexr's `LoadEXR` and `FLIP::evaluate`, no code of ours, so
the test checks our reader and our use of FLIP against the reference rather than against itself.

```sh
g++ -std=c++20 -O2 -w -I build/debug/vcpkg_installed/x64-linux/include -I third_party/flip \
    flip_reference_value.cpp -L build/debug/vcpkg_installed/x64-linux/lib -ltinyexr -lminiz \
    -o flip_reference_value
./flip_reference_value libs/core/tests/data/flip_reference.exr libs/core/tests/data/flip_test.exr
# 48x32 ppd 67.0206 exposures -11.147449..-4.998166 (7) mean FLIP 0.145033047
```

```cpp
// Mean FLIP of two EXR files, computed only with tinyexr's LoadEXR and FLIP.h's own
// evaluate() from interleaved floats, as NVIDIA's FLIP tool does for HDR input: HDR-FLIP,
// default parameters (67.02 PPD, ACES, exposures found from the reference). No cs code.
#include <FLIP.h>
#include <tinyexr.h>
#include <cstdio>
#include <vector>

static std::vector<float> load_rgb(const char* path, int& w, int& h) {
    float* rgba = nullptr;
    const char* err = nullptr;
    if (LoadEXR(&rgba, &w, &h, path, &err) != 0) { std::fprintf(stderr, "%s\n", err); std::exit(1); }
    std::vector<float> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = rgba[i * 4 + c];
    free(rgba);
    return rgb;
}

int main(int argc, char** argv) {
    int w = 0, h = 0, tw = 0, th = 0;
    std::vector<float> ref = load_rgb(argv[1], w, h);
    std::vector<float> test = load_rgb(argv[2], tw, th);
    FLIP::Parameters parameters;
    float mean = -1.0f;
    float* map = nullptr;
    FLIP::evaluate(ref.data(), test.data(), w, h, true, parameters, false, true, mean, &map);
    delete[] map;
    std::printf("%dx%d ppd %.4f exposures %.6f..%.6f (%d) mean FLIP %.9f\n", w, h, parameters.PPD,
                parameters.startExposure, parameters.stopExposure, parameters.numExposures, mean);
}
```
