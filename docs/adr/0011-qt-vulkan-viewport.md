# 0011. The Qt viewport spike: keep Qt Widgets around our own Vulkan swapchain

- Status: proposed; accepted when the owner's Windows result is in (section "Windows")
- Date: 2026-11-11
- Decides: milestone 1 task M1-04 (issue #39), which confirms or replaces the toolkit choice of
  [ADR 0006](0006-ui-and-interaction.md) before any UI is built on it

## Context

ADR 0006 chose Qt 6 Widgets for the application shell and a `QWindow` with a Vulkan surface,
embedded with `QWidget::createWindowContainer`, for the 3D viewport. The renderer creates its own
instance, device and swapchain; Qt only wraps the instance (`QVulkanInstance::setVkInstance`) to
make the surface. ADR 0006 named the risks and asked for them to be tested within one session:
Wayland and X11, window resize, high-DPI scaling, relative mouse mode for walking, Windows, and a
screenshot from an offscreen UI test in CI. Its fallbacks are Qt Quick with a shared device, then
Dear ImGui with its Test Engine.

## The prototype

Branch `spike/qt-vulkan` (commits `16d823c` to `da638cc`, never merged), directory
`spike/qt-vulkan/`, about 1,900 lines of C++ (750 of them the renderer):

- `vk_renderer.*`: a Vulkan 1.3 renderer with no Qt dependency. It opens the loader at run time
  (as volk will), creates the instance with every surface extension the loader offers, picks a
  device that can present to the given surface, and owns the swapchain, a pipeline with dynamic
  rendering and two frames in flight. It draws a rotating triangle (angle and pitch as push
  constants) and can copy the frame it just drew back to the host.
- `viewport.*`: `VulkanViewportWindow`, a `QWindow` of type `VulkanSurface` embedded with
  `createWindowContainer`. It owns a `QVulkanInstance` that wraps our `VkInstance`, takes the
  surface from `QVulkanInstance::surfaceForWindow`, sizes the swapchain as the window's logical
  size times `devicePixelRatio()`, and renders on `UpdateRequest`. `HeadlessViewportWidget` is
  the same renderer drawing into an offscreen image, painted by an ordinary widget, used on the
  `offscreen` platform.
- `wayland_pointer.*`: relative mouse mode on Wayland (see below).
- `main_window.*`: a `QMainWindow` with File and View menus, an "Inspector" dock that shows the
  live state (platform, device pixel ratio, swapchain extent, swapchains created, frames,
  validation messages, pointer mode, relative motion received), and a composite screenshot:
  `QWidget::grab` of the widgets with the viewport's last frame painted where its native window
  sits.
- `tests/tst_shell.cpp`: the QTest described under "CI" below.
- `.github/workflows/spike-qt-vulkan.yml`: the CI described below.

Every run can be scripted (`--resize-cycle`, `--walk`, `--shot`, `--quit-after`) and prints a
status line, so each result below comes from a command, not from watching the window.

## Results on the 780M (Linux)

Machine: the owner's laptop, Radeon 780M (RADV, Mesa 26.2.6), Arch Linux, Qt 6.12.0 from the
distribution, Vulkan loader and validation layer 1.4.363, the niri compositor (26.04) with the
built-in panel at a native scale of 1.25. Validation layer on in every run.

| Platform | Scale | Resize | Swapchain extent | Validation messages | Screenshot |
| --- | --- | --- | --- | --- | --- |
| Wayland (native) | 1.25 (compositor, fractional) | works, 6 swapchains in the cycle | 969 × 820 for 775 × 656 logical | 0 | [wayland-1.25](0011-qt-vulkan-viewport/wayland-1.25.png) |
| Wayland | 1.0 | works | 775 × 656 | 0 | [wayland-1.0](0011-qt-vulkan-viewport/wayland-1.0.png) |
| Wayland | 1.5 | works | 1163 × 984 | 0 | [wayland-1.5](0011-qt-vulkan-viewport/wayland-1.5.png) |
| Wayland | 2.0 | works | 1550 × 1312 | 0 | [wayland-2.0](0011-qt-vulkan-viewport/wayland-2.0.png) |
| X11 (Xwayland) | 1.0 | works, 7 swapchains in the cycle | 775 × 656 | 0 | [x11-1.0](0011-qt-vulkan-viewport/x11-1.0.png) |
| X11 (Xwayland) | 1.5 | works | 1163 × 984 | 0 | [x11-1.5](0011-qt-vulkan-viewport/x11-1.5.png) |
| X11 (Xwayland) | 2.0 | works | 1550 × 1312 | 0 | [x11-2.0](0011-qt-vulkan-viewport/x11-2.0.png) |

The screenshots are the compositor's own picture of the window (`niri msg action
screenshot-window`), so they show what reaches the screen, not what our code believes it drew.
In every one the Vulkan viewport sits exactly in its container, at full resolution, beside the
Qt widgets.

How each was run (from the repository root, on `spike/qt-vulkan`):

```sh
cmake -S spike/qt-vulkan -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/spike
QT_QPA_PLATFORM=wayland ./build/spike/qt-vulkan-spike --resize-cycle --niri-shot /abs/wayland-1.25.png --after-ms 1200
QT_QPA_PLATFORM=wayland QT_SCALE_FACTOR=1.2 ./build/spike/qt-vulkan-spike --resize-cycle --niri-shot /abs/wayland-1.5.png --after-ms 1000
QT_QPA_PLATFORM=xcb QT_SCALE_FACTOR=1.5 ./build/spike/qt-vulkan-spike --resize-cycle --niri-shot /abs/x11-1.5.png --after-ms 1000
```

What the table does not cover, and why:

- **Scale.** The panel's own scale is 1.25. Scales 1.0, 1.5 and 2.0 were reached with
  `QT_SCALE_FACTOR` (0.8, 1.2 and 1.6 on Wayland, multiplying the compositor's 1.25; 1.0, 1.5 and
  2.0 on X11, whose Xwayland server reports a scale of 1). This tests Qt's device pixel ratio
  and our swapchain sizing at each scale, and the native 1.25 run tests the Wayland
  fractional-scale path. It does not test the compositor switching to 1.5 or 2.0, or moving a
  window between two screens of different scale, which needs a second display. Changing the
  laptop's output scale during the spike would have reflowed every window the owner had open,
  so it is left to M1-26's acceptance test ("moving between displays of different scale").
- **Resize.** Driven by `QWidget::resize` through four sizes (700 × 450, 1400 × 850, 500 × 380,
  1100 × 700) while the triangle animates. Each step recreated the swapchain at the new extent
  with no validation message. Resizing by dragging a window edge was not scripted; under niri,
  a tiling compositor, windows are mostly sized by the layout anyway.
- **X11.** This laptop has no X11 session (its sessions are niri, Hyprland and Plasma, all
  Wayland). The X11 runs use Qt's `xcb` platform on niri's Xwayland (`xwayland-satellite`), which
  exercises Qt's X11 code and the `VK_KHR_xcb_surface` path. At a scale of 1 the compositor
  upscales the X11 window to the panel's 1.25, so edges are slightly soft in `x11-1.0`; that is
  Xwayland, not our code. A native Xorg session is untested.

### Relative mouse mode (walking)

| Platform | Mechanism | Result |
| --- | --- | --- |
| Wayland | `zwp_pointer_constraints_v1` lock + `zwp_relative_pointer_v1`, cursor hidden | **Works.** niri granted the lock (`locked` event) in every run; 266 unaccelerated relative-motion events arrived in a 3 s run while the mouse was in use; the compositor sent `unlocked` when focus moved away, and Esc leaves walk mode. How it feels over a longer walk is for the owner to judge. [wayland-walk](0011-qt-vulkan-viewport/wayland-walk.png) shows the inspector reporting "locked by compositor" |
| X11 on Xwayland | `QWindow::setMouseGrabEnabled`, hidden cursor, `QCursor::setPos` back to the centre after each move | **Does not work.** The X server reports the warp as done (`warp to 802,1636 -> cursor at 802,1636`), but the next motion events follow the real Wayland pointer, not the warped one (positions 293, 251, 210… against a centre of 302), so the deltas accumulate the pointer's distance from the centre instead of its motion |
| Native X11, Windows | grab and warp, as above | Untested here; warping works on native X11 and on Windows, but see "What M1-26 and M1-27 must do differently" for why raw input is better |

Qt has no public API for relative mouse motion or pointer locking. The Wayland path binds the
two protocols itself, generated with `qt6_generate_wayland_protocol_client_sources` and bound
through the public `QWaylandClientExtensionTemplate`. The `wl_pointer` comes from the public
`QNativeInterface::QWaylandApplication`. The viewport's `wl_surface` does not: it is available
only through `QPlatformNativeInterface::nativeResourceForWindow("surface", window)`, a Qt
private header (`Qt6::GuiPrivate`). Private headers carry no compatibility promise, so that call
must be checked again at every Qt upgrade.

## Windows (RTX 2060)

**Pending: the owner's result.** The exact commands are in [the comment on
#39](https://github.com/groundbeeflover/curation_sandbox/issues/39#issuecomment-6783358146). They
use the `qt-vulkan-spike-windows` build from CI (Qt 6.10.3, bundled with `windeployqt`), so nothing
has to be installed. This section will record, from the owner's reply: the status lines and
screenshots at the display's own scale and at 1.5 and 2.0; a resize by dragging a window edge;
moving between displays of different scale, if there are two; and walk mode (right-click the
viewport, move the mouse, Esc), where Windows uses the grab-and-warp path.

## CI: an offscreen QTest with a screenshot

`tests/tst_shell.cpp` runs on Qt's `offscreen` platform. It opens the main window, waits for
three frames, checks the title, the "inspector" dock and the two menus, saves a screenshot of
the whole window, checks that the viewport's centre pixel is the triangle's colour (the
triangle's centroid is the centre at every angle), and checks that the validation layer
reported nothing. With no Vulkan device it skips, unless `SPIKE_REQUIRE_VULKAN` is set (as in
CI), when it fails.

The offscreen platform cannot make Vulkan surfaces, and `QWidget::grab` cannot see a native child
window on any platform. So on `offscreen` the viewport is a plain widget that paints the
renderer's offscreen image, and the screenshot comes out complete, viewport included. Locally
the test passes on both RADV and lavapipe in about 0.2 s:

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build/spike -L ui -V
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json QT_QPA_PLATFORM=offscreen ctest --test-dir build/spike -L ui -V
```

In GitHub Actions, workflow `Spike Qt Vulkan` on the spike branch,
[run 42932204772](https://github.com/groundbeeflover/curation_sandbox/actions/runs/42932204772)
(commit `da638cc`), both jobs green:

- **Linux (`ubuntu-24.04`)**: apt installs `libvulkan-dev`, `mesa-vulkan-drivers` (lavapipe),
  `vulkan-validationlayers`, `wayland-protocols` and `libwayland-dev` (13 s). The build includes
  the Wayland pointer code. Then the test runs on the offscreen platform with lavapipe
  (`llvmpipe (LLVM 20.1.2, 256 bits)`): 3 passed, 0 skipped, 0 validation messages. The
  screenshot is uploaded as the `offscreen-screenshot` artifact:
  [offscreen-ci](0011-qt-vulkan-viewport/offscreen-ci.png).
- **Windows (`windows-2022`)**: builds with MSVC 2022 and bundles the program with `windeployqt`
  (artifact `qt-vulkan-spike-windows`, 23 MB, for the owner's run). The hosted runner has no
  Vulkan loader, so the test skips there.

**How Qt is installed**: [`jurplel/install-qt-action@v4`](https://github.com/jurplel/install-qt-action)
(v4.5.0, which runs aqtinstall 3.3) with `cache: true`. No Qt account is needed, and no modules
beyond the default set: Widgets, Test, the Wayland client and its scanner are all in it. On
Ubuntu, though, Qt's WaylandClient CMake package also needs `libwayland-dev`. Without it,
`find_package` quietly reports the module missing and the Wayland code is left out of the build.

| Job | Qt | First install | From the action's cache |
| --- | --- | --- | --- |
| Linux | 6.12.0, `linux_gcc_64` | 64 s | 40 to 48 s (three runs) |
| Windows | 6.10.3, `win64_msvc2022_64` | 101 s | 75 s |

The cached times are mostly pip installing aqtinstall and restoring the cache (325 MB on Linux,
349 MB on Windows), so caching saves less than expected. Building the prototype took a further
17 to 27 s on Linux and 24 s on Windows.

Four things went wrong on the way, and M1-26 and M1-31 should not repeat them:

- The first CI run "passed" with the test **skipped**: `VK_DRIVER_FILES` named
  `/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`, which does not exist on Ubuntu 24.04. The file
  is `lvp_icd.json` there, and on Arch too. The loader found no driver, and the test's
  no-device skip hid it. CI now sets `SPIKE_REQUIRE_VULKAN=1`, which turns that skip into a
  failure. The same wrong path is in the GPU-test command in `CLAUDE.md`, to be corrected by the
  task that sets up GPU CI (M1-29).
- aqtinstall cannot install Qt 6.11 or later on Windows (see point 1 below).
- The Wayland code was silently left out of the Linux build until `libwayland-dev` was
  installed; the CMake now says which Qt package was missing.
- MSVC: `<windows.h>` defines `min` and `max` macros, so `NOMINMAX` is needed wherever it is
  included.

## Conclusion: keep Qt Widgets

Everything ADR 0006 asked of the spike works on Linux: the dock and menus beside a viewport whose
instance, device and swapchain our code owns; resize; scales 1.0, 1.5 and 2.0 on Wayland and X11,
and the panel's fractional 1.25 on Wayland; walking with a locked pointer on Wayland; and a headless
UI test with a screenshot in CI. No validation message was reported in any run. The one failure,
warp-based mouse capture on Xwayland, is a choice of input mechanism, and the fallbacks would not
avoid it: Qt Quick has the same input layer, and Dear ImGui's backends (SDL or GLFW) solve it with
the same protocols and raw input that M1-27 can use here. Neither fallback is needed. ADR 0006's
toolkit stands, and its status can move to accepted once the Windows result agrees.

## What M1-26 and M1-27 must do differently

For **M1-26** (application shell):

1. **Pin one exact Qt version for CI and releases, and pick one that aqtinstall can install on
   both platforms.** On 2026-11-11, aqtinstall 3.3.0 (the newest, used by install-qt-action)
   installs 6.12.0 on Linux but cannot read Qt's Windows repositories for 6.11 and later, which
   are now split per compiler ("Failed to download checksum for the file 'Updates.xml'"). It
   does install 6.10.3 on Windows. So the pin in `docs/dependencies.md` should be **6.10.3** on both
   platforms until aqtinstall catches up; then move to the newest version both can install.
   Local Linux builds may use the distribution's Qt at that version or newer (Arch ships 6.12.0,
   on which the prototype was developed), so CMake asks for a minimum version, while CI and
   the release builds install the exact one. The Wayland surface lookup uses a Qt private header
   (`Qt6::GuiPrivate`), which is safe because the app is always built and shipped with the same
   Qt. Keep that call in one file of the viewport adapter, so an upgrade has one place to check.
2. **The adapter owns a `QVulkanInstance` that wraps the renderer's instance, and the order of
   destruction matters.** In the viewport window's destructor: `QWindow::destroy()` first (it
   sends `SurfaceAboutToBeDestroyed`, where the swapchain is released, and Qt destroys the
   surface), then `QVulkanInstance::destroy()`, then the renderer's device and instance. Members
   are destroyed before the `QWindow` base class, so leaving it to the compiler destroys the
   instance under a live surface. The renderer must enable the window-system surface extensions
   itself (`VK_KHR_surface` and whichever of xcb, xlib, wayland and win32 the loader offers),
   because Qt's surface creation calls them on our instance.
3. **Swapchain extent**: Wayland reports `currentExtent` as `0xFFFFFFFF`, so the size is the
   window's logical size times `devicePixelRatio()`, rounded (`std::lround`) and clamped to the
   surface limits; X11 and Windows report it. Recreate on the next frame after a resize, on
   `VK_ERROR_OUT_OF_DATE_KHR` and on `VK_SUBOPTIMAL_KHR`; skip frames at a size of zero.
   Also handle `SurfaceAboutToBeDestroyed` (reparenting can destroy the native window) by
   releasing the swapchain and taking a new surface at the next expose, on the same device.
4. **The offscreen platform has no Vulkan surface.** As M1-26's acceptance criteria already say,
   the viewport there must be a plain widget painting the offscreen target. The prototype
   confirms that this gives complete UI screenshots in CI.
5. **Screenshots of the real window are composites.** `QWidget::grab` shows the viewport's
   container as empty. Create the swapchain with `TRANSFER_SRC` usage when the surface allows
   it, copy the frame back on request, and paint it over the container's rectangle (the
   prototype's `MainWindow::grab_composite`). Use it for `docs/product/screens/`.
6. **Build**: enable the C language in CMake (the generated Wayland protocol code is C), find
   `Qt6::WaylandClient` and `Qt6::GuiPrivate`, and install `wayland-protocols` and
   `libwayland-dev` in CI. Make a missing Wayland module a configure error on Linux rather
   than a silent fallback. Set `QT_NO_PRIVATE_MODULE_WARNING` to silence the private-module
   warning once the pin is in place.
7. Under a tiling compositor (niri, Hyprland, Sway) `QWidget::resize` is only a request: UI tests
   must not assume the window has the size they asked for, which is one more reason they run on
   `offscreen`.

For **M1-27** (walking, `camera_input.cpp`):

8. **Relative mouse mode needs one backend per platform**, and never warp-to-centre on Wayland
   or Xwayland:
   - Wayland: the pointer-constraints and relative-pointer protocols, as in the prototype; use the
     unaccelerated deltas.
   - Windows: raw input (`RegisterRawInputDevices`, `WM_INPUT` through a
     `QAbstractNativeEventFilter`) with `ClipCursor` to the viewport and a hidden cursor; warping
     with `SetCursorPos` works but gives accelerated, rounded deltas.
   - X11: XInput2 raw motion (`XI_RawMotion`) with a pointer grab, through a native event filter,
     as SDL and GLFW do; untested here, and its behaviour on Xwayland is unknown. Warping is
     wrong on Xwayland, and on Linux Qt already prefers its Wayland platform when a Wayland
     session is running, so this backend matters mostly for Xorg users.
   - A last resort on every platform: drag-to-look (hold a button; deltas from absolute motion
     while the button is down), which needs no capture.
9. Leave walk mode on Esc, on focus loss and on the compositor's `unlocked` event, and say so
   in the viewport ("Esc to stop walking"). The prototype handles Esc and focus loss but only
   records `unlocked`, which leaves the cursor hidden and walking still on after an Alt-Tab on
   Wayland.

## Revisit

When the owner's Windows result is in (this record's status), when Qt is upgraded (the private
surface lookup), and if a later Qt adds a public relative-pointer API.

## Sources

- QVulkanInstance and `setVkInstance`: https://doc.qt.io/qt-6/qvulkaninstance.html
- `QWidget::createWindowContainer`: https://doc.qt.io/qt-6/qwidget.html#createWindowContainer
- Qt LGPL obligations: https://www.qt.io/development/open-source-lgpl-obligations
- Wayland pointer constraints:
  https://wayland.app/protocols/pointer-constraints-unstable-v1
- Wayland relative pointer: https://wayland.app/protocols/relative-pointer-unstable-v1
- Qt Wayland client extensions:
  https://doc.qt.io/qt-6/qwaylandclientextension.html and
  https://doc.qt.io/qt-6/qt-generate-wayland-protocol-client-sources.html
- install-qt-action: https://github.com/jurplel/install-qt-action
- Windows raw input: https://learn.microsoft.com/en-us/windows/win32/inputdev/raw-input
- XInput2 raw events: https://www.x.org/releases/current/doc/inputproto/XI2proto.txt
