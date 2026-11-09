# Milestone 1: C++ and a CPU path tracer

**Goal:** a path tracer, running on the CPU, that renders a gallery room in physically based
light and passes two checks:

1. **Inverse-square check.** A 1,000 cd point light 2 m above a white floor gives 250 lux on
   the floor below it, within 1% of `cs::photometry::point_illuminance`.
2. **White furnace check.** A white object inside uniform light shows exactly that light: no
   energy gained or lost, within 1%.

**Time:** about 4 to 6 weeks at 10 to 15 hours a week. The milestone is done when both checks
pass and you have written up what you learned, not when the images are perfect.

**Why start here rather than with Vulkan:** no graphics API stands in the way, results are
visible within days, and the finished tracer becomes the ground truth that every later
renderer is tested against.

## 0. Setup (first evening)

- [ ] Build the project and run the tests (see the README).
- [ ] Run `pt` and open `renders/first.ppm`: a gradient from black (top left) through red
      and green to yellow (bottom right).
- [ ] Learn the debugger: set a breakpoint in `apps/pt/main.cpp`, step through the loop and
      watch `x`, `y` and `image`. Use `gdb`, or an editor that drives it (VS Code with the
      C/C++ or clangd extension, or CLion).
- [ ] Start your learning log in `docs/notes/`.

For C++ itself, work through [learncpp.com](https://www.learncpp.com/) until you are
comfortable with functions, references, classes, `std::vector`, and splitting code into
headers and source files. Keep it open as a reference; you do not need to finish it first.

## 1. Ray Tracing in One Weekend (weeks 1 to 2)

Follow [Ray Tracing in One Weekend](https://raytracing.github.io/books/RayTracingInOneWeekend.html)
from start to finish, typing every line yourself rather than copying it.

- [ ] Put the book's classes (`vec3`, `ray`, `hittable`, `sphere`, `camera`, `material`) in
      `core/`, inside `namespace cs`: headers in `core/include/cs/core/`, code in
      `core/src/`. Add new `.cpp` files to `core/CMakeLists.txt`. Keep `main.cpp` thin.
- [ ] Write into a `cs::Image` instead of printing PPM to `std::cout`.
- [ ] Add `tests/test_vec3.cpp` (dot and cross products, length, unit vectors) and list it
      in `tests/CMakeLists.txt`.
- [ ] Render the book's final scene, and time it with the `release` preset.
- [ ] Use every CPU core: split the image into rows or tiles and render them on several
      `std::thread`s. Note the speed-up in your log.

## 2. Triangles and a BVH (weeks 2 to 3)

- [ ] Read the BVH chapter of
      [Ray Tracing: The Next Week](https://raytracing.github.io/books/RayTracingTheNextWeek.html),
      then the matching chapter of [Physically Based Rendering](https://pbr-book.org/).
- [ ] Add quads and triangles (Moller-Trumbore intersection). A gallery is walls, floors and
      flat paintings; frames arrive later as triangle meshes.
- [ ] Build your own BVH and measure the speed-up on a scene with thousands of triangles.
- [ ] Optional: compare with Embree (`find_package(embree 4)`).

## 3. The gallery room (week 3)

- [ ] Describe the scene in `scenes/gallery-room.json`: a 6 x 4 x 3.5 m room, wall and floor
      colours, one painting at its true size with an image file, and one spotlight. A small
      JSON library such as [nlohmann/json](https://github.com/nlohmann/json) can be added
      with `FetchContent`, the same way as Catch2 in `tests/CMakeLists.txt`.
- [ ] Load the painting with `stb_image.h` from [stb](https://github.com/nothings/stb) and map
      it onto a quad at true size.
- [ ] Work in metres everywhere. Put the camera at eye height (1.6 m), looking at the painting.
- [ ] Use a CC0 painting (Rijksmuseum, The Met, Art Institute of Chicago) and save its credit
      line next to the image file.

## 4. Physically based materials (weeks 3 to 4)

Read [LearnOpenGL's PBR theory](https://learnopengl.com/PBR/Theory) first, then the
reflection-model chapters of Physically Based Rendering.

- [ ] Lambertian diffuse for walls and the paint layer.
- [ ] GGX microfacet reflection with Schlick's Fresnel, for varnish.
- [ ] A clear coat: varnish over paint. A simple two-layer blend is enough for now.
- [ ] Metal for gilding, starting from an RGB reflectance for gold.
- [ ] Importance-sample every material, so that rays go where the light is likely to be.

## 5. Lights in real units (week 4)

- [ ] Follow the convention in `core/include/cs/core/photometry.hpp`: luminance in nits, point
      lights and spotlights in candela, lumen ratings converted with `uniform_cone_intensity`.
- [ ] Next-event estimation: at every bounce, sample a light directly. Then add multiple
      importance sampling between light samples and material samples (both are explained in
      Physically Based Rendering).
- [ ] IES profiles: parse the LM-63 text format (vertical angles, horizontal angles, then a
      table of candela values) and use it as a spotlight's distribution. Lighting
      manufacturers publish free IES files for museum spotlights.

## 6. Clean output (week 5)

- [ ] Write linear HDR output (PFM is a few lines; EXR via tinyexr) next to a display PNG
      (`stb_image_write.h`).
- [ ] Tone map with [Khronos PBR Neutral](https://www.khronos.org/news/press/khronos-pbr-neutral-tone-mapper-released-for-true-to-life-color-rendering-of-3d-products)
      instead of clamping.
- [ ] Denoise with [Open Image Denoise](https://www.openimagedenoise.org/)
      (`find_package(OpenImageDenoise)`), passing albedo and normal images as guides.

## 7. A virtual lux meter and the two checks (weeks 5 to 6)

- [ ] A sensor at a point with a normal: trace many rays and add up the incoming luminance,
      weighted by the cosine of the angle to the normal. With cosine-weighted sampling, each
      sample contributes pi times the luminance it finds. The average is the illuminance in lux.
- [ ] **Inverse-square check, as a unit test.** Point light 2 m above a large white floor,
      sensor on the floor directly below, facing up: 250 lux within 1%. Random rays can never
      hit a point light, so this also proves your next-event estimation works.
- [ ] **White furnace check, as a unit test.** A white diffuse sphere (albedo 1) inside a
      uniform environment of 100 nits must render as 100 nits everywhere, within 1%. (GGX
      loses a little energy at high roughness; that is expected, and a topic for later.)
- [ ] Print the lux on the painting when rendering the gallery room.

## Done when

- [ ] The gallery room renders: painting at true size, spotlight in candela, a varnish
      highlight, denoised.
- [ ] Both checks pass as unit tests.
- [ ] `docs/notes/milestone-1.md` says what you built, what surprised you, and what you would
      do differently.

Then milestone 2 begins: a Vulkan renderer whose images must match these.
