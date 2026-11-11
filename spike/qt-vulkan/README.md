# Qt Widgets around a Vulkan viewport: M1-04 prototype

Throwaway code for issue #39. It is never merged; the decision is in
`docs/adr/0011-qt-vulkan-viewport.md` on `main`.

A `QMainWindow` with a menu bar, an "Inspector" dock and a Vulkan viewport that draws a rotating
triangle. The renderer (`src/vk_renderer.*`) has no Qt dependency. It loads the Vulkan loader at
run time and owns the `VkInstance`, `VkDevice` and swapchain. Qt only wraps our instance
(`QVulkanInstance::setVkInstance`) to create the window surface (`src/viewport.*`). On the
`offscreen` platform the same renderer draws into an offscreen image shown by a plain widget, so
`QWidget::grab` sees it (`tests/tst_shell.cpp`).

## Build and run (Linux)

```sh
cmake -S spike/qt-vulkan -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/spike
./build/spike/qt-vulkan-spike                         # right-click the viewport to walk, Esc leaves
QT_QPA_PLATFORM=xcb ./build/spike/qt-vulkan-spike     # X11 (or Xwayland)
QT_SCALE_FACTOR=1.5 ./build/spike/qt-vulkan-spike     # Qt's own scale on top of the screen's
QT_QPA_PLATFORM=offscreen ctest --test-dir build/spike -L ui -V
```

Scripted runs: `--resize-cycle`, `--walk`, `--shot out.png` (widgets plus the viewport's frame),
`--niri-shot out.png` (the compositor's view, niri only), `--quit-after ms`, `--headless`,
`--no-validation`. Every run prints a status line with the platform, device pixel ratio,
swapchain extent, swapchains created, frames, validation messages and pointer mode.

## Windows

See the issue comment on #39 for the exact commands. The `Spike Qt Vulkan` workflow also uploads
a bundled build (`qt-vulkan-spike-windows`).

Regenerate `src/triangle_spv.hpp` after editing a shader: `python3 spike/qt-vulkan/shaders/embed_spv.py`.
