# 0006. The editor: Qt 6 Widgets around a Vulkan viewport, and how things are placed

- Status: proposed; the toolkit is confirmed or replaced by the Qt viewport spike in milestone 1
- Date: 2026-11-10
- Decides: question 6 of the planning brief, the UI toolkit and the placement conventions (the
  project file, undo and exports are in [0007](0007-project-files-undo-exports.md))

## Context

The first users are institutional curators: not technical, used to Office, Outlook and their
collection system. The app must look and behave like a finished desktop product (menus, dialogs,
tables, a property inspector, file dialogs, proper text input, high-DPI displays, later
accessibility and translation), not like a developer tool. The UI must be defined in code,
reviewable as text, and testable headless in CI with scripted interaction and screenshots. The
renderer is our own Vulkan code (ADR 0001).

## Toolkit options

| Option | Product look | Accessibility | Licence | Viewport integration | Headless tests | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| **Qt 6 Widgets** | Desktop-native with Fusion and a palette; docks, tables, trees, inspectors | Strong | LGPL-3.0, fine when dynamically linked | A `QWindow` hosted with `createWindowContainer`; our renderer owns its device and swapchain | QTest on the `offscreen` platform, `QWidget::grab` for screenshots | **Chosen** |
| Qt 6 Quick (QML) | Strong, modern | Strong | LGPL-3.0 | Qt renders on our `VkDevice`; our image imported as a texture; synchronisation shared with Qt's scene graph | Qt Quick Test, offscreen | Runner-up: more interop to get right, and this app is mostly panels and tables |
| Dear ImGui, themed | Looks like a tool | None | MIT; Test Engine free for open source | Trivial | Excellent (Test Engine) | Fallback, and used for debug overlays only |
| Slint | Strong | Improving | GPLv3, or free with attribution | Unstable for custom GPU content | Software renderer only | No |
| RmlUi | Adequate | None | MIT | Trivial | Weak | No |
| Webview (Tauri, CEF) | Strong | Strong | Permissive | Poor on Linux (WebKitGTK compositing) | UI only | No |

Qt also brings what the app needs around the UI: `QPdfWriter` and `QSvgGenerator` for the
exported drawings, `QFileDialog`, settings, network access for catalogue downloads, translation
tools for later, and accessibility on Windows and Linux.

## Decision

- **Qt 6 Widgets** for the application shell: main window, menus, docks, the catalogue browser,
  the property inspector, the checklist and hanging-list tables, dialogs. The Fusion style with
  the product's own palette and an icon set; no Qt Quick in the MVP.
- **The 3D viewport** is a `QWindow` with a Vulkan surface, embedded with
  `QWidget::createWindowContainer`. The renderer creates its own instance, device and swapchain
  (Qt only wraps the instance with `QVulkanInstance::setVkInstance` to make the surface). The
  renderer has no Qt dependency; a thin adapter connects them.
- **Overlays inside the viewport** (snapping guides, dimension lines, the lux readout, gizmos)
  are drawn by the renderer as part of the frame, not by Qt. Dear ImGui is used only for
  developer overlays (frame timings, debug views), compiled out of release builds.
- **No logic in widgets.** Everything a widget does goes through the toolkit-free `editor`
  library: commands, selection, snapping, measurement, camera control. Journeys are tested
  against that library without any window; UI tests stay thin and check wiring and layout.
- Qt is dynamically linked, its version pinned, its licence and a source offer shipped with the
  app (LGPL-3.0 obligations).

**The Qt viewport spike** (milestone 1) builds the shell around the walking skeleton's viewport on Linux (X11 and
Wayland) and Windows: resize, high-DPI scaling, mouse capture for walking, and a screenshot
from an offscreen UI test in CI. If any of those cannot be made to work within the time box,
the fallback is Qt Quick with a shared device, then a themed Dear ImGui with its Test Engine.

## Interaction conventions

**Views.** Orbit (default); walk at an eye height of 1.6 m and 1.4 m/s (both settings); plan
view, orthographic from above; wall elevation, orthographic and square to one wall face, which
is the main view for hanging.

**Building.** Rooms from width, depth and height; walls drawn point to point with typed lengths
(a measurements box, as in SketchUp), with right-angle and endpoint snapping and a 1 cm grid;
openings placed on a wall by offset and width; walls lettered A, B, C automatically and
renamable.

**Hanging.** A work is placed on a wall face by its distance from the face's left end and its
centre height. Snap targets, shown as labelled guide lines, only for the nearest candidates:

- the centre line (project default 150 cm, adjustable per project and per wall from 140 to
  160 cm), with "align tops" and "align bottoms" as alternatives for a group;
- equal gaps to the left and right neighbours ("= 42 cm = 42 cm");
- the centre, top or bottom of any work on the same wall;
- the wall's centre; margins from corners, openings and other works.

A modifier key turns snapping off. Numbers can be typed while dragging. Group commands:
distribute evenly between two works, centre a group on the wall, align centres to the centre
line. Overlaps are flagged.

**Lighting.** A track is placed at the Canadian Conservation Institute's distance from the wall
(ceiling height − 1.6 m) × tan 30°, editable. "Aim at work" snaps a fixture's aim to the
work's centre and slides it along the track until the aim is 30° from vertical; a band from 25°
to 35° is shown in green. The beam and field cones are drawn on request.

**Measuring.** A tape measure snapping to edges, corners and centres; persistent dimension
objects in plan and elevation; a lux probe that reads illuminance at any clicked point, and
persistent lux sensors.

## Consequences

- One mature toolkit for everything around the viewport; the viewport stays a pure Vulkan
  surface, so the renderer is identical in the app and in headless tests.
- Qt adds a large dependency, installed from Qt's binaries rather than vcpkg (ADR 0001), and
  LGPL obligations in the installers.
- Native child windows cannot have Qt widgets drawn over them; that is why overlays are drawn by
  the renderer.

## Revisit

After the Qt viewport spike, and when accessibility and translation are planned for 1.0.

## Sources

- Qt LGPL obligations: https://www.qt.io/development/open-source-lgpl-obligations
- Qt modules and licences: https://doc.qt.io/qt-6/licensing.html
- Qt Quick on a supplied Vulkan device: https://doc.qt.io/qt-6/qquickrendercontrol.html and
  https://doc.qt.io/qt-6/qtquick-scenegraph-vulkantextureimport-example.html
- Dear ImGui Test Engine licence:
  https://raw.githubusercontent.com/ocornut/imgui_test_engine/main/imgui_test_engine/LICENSE.txt
- Canadian Conservation Institute, track lighting:
  https://www.canada.ca/en/conservation-institute/services/conservation-preservation-publications/canadian-conservation-institute-notes/track-lighting.html
- Ortelia Curator (closest existing product):
  https://ortelia.com/products/ortelia-curator-exhibition-design-software
