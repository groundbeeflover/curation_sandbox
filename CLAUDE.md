# Curation Sandbox

A desktop app in which museum curators build a room, hang artworks at their true size in
frames, light them with real fixtures, and read the lux on every work. C++20, CMake and vcpkg,
Vulkan 1.3 with Slang shaders, Embree for the CPU calculation engine, Qt 6 Widgets for the app.
Apache-2.0.

Depth lives elsewhere; read it when a task needs it, do not copy it here:
[blueprint](docs/product/blueprint.md) (product, architecture, budgets) ·
[roadmap](docs/roadmap.md) (milestones, lanes) · [decisions](docs/adr/) ·
[dependencies](docs/dependencies.md) (versions, licences, what may be added).

## Commands

Some of these arrive with milestone 1 tasks; the roadmap names the task that creates each.

```sh
cmake --preset debug && cmake --build --preset debug    # debug with ASan and UBSan: write code here
cmake --preset release && cmake --build --preset release
cmake --preset app && cmake --build --preset app       # debug build with the Qt app (needs Qt 6, see dependencies.md)
ctest --preset debug                                    # all tests; filter with -L <label>
ctest --preset debug -L engine                          # one module (labels = module names)
ctest --preset release -L accuracy                      # physical accuracy tests
CS_REQUIRE_GPU=1 VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --preset debug -L gpu   # lavapipe
# some distributions name the file lvp_icd.x86_64.json; `ls /usr/share/vulkan/icd.d/` shows which
QT_QPA_PLATFORM=offscreen ctest --preset app -L ui      # UI tests without a display
ctest --preset release -L golden                        # golden images; failures write build/<preset>/golden-out/
./build/release/apps/cs-render/cs-render <project> --tier path|standard --size 256x144 --spp 64 --seed 1 --out build/out/x
./build/release/apps/cs-render/cs-render golden tests/golden/<name> --update   # only on purpose, see Testing
./build/release/apps/cs-bench/cs-bench <scene> --tier standard --size 1920x1080 --frames 600 --out build/bench/x.json
python3 scripts/perf-compare.py build/bench/x.json --budgets bench/budgets.json --baseline bench/baselines/<machine>/<scene>-<tier>.json --milestone <M>
cmake --preset fuzz && cmake --build --preset fuzz      # libFuzzer targets (Clang, Linux)
python3 scripts/format.py --check                       # clang-format; without --check it fixes
python3 scripts/check-dependencies.py                   # vcpkg.json against docs/dependencies.md
python3 scripts/github-bootstrap.py --check             # validate docs/roadmap/tasks.toml
```

## Architecture rules

- Modules live in `libs/<m>/{include/cs/<m>/,src/,tests/,fuzz/}`, namespace `cs::<m>`. A module
  may use only the modules its `CMakeLists.txt` declares; the allowed graph is in blueprint
  section 7. Never add a dependency edge to make something compile; say so in the issue.
- Only `gpu` (and the viewport adapter in `ui`) includes Vulkan. Only `ui` and `apps/curation`
  include Qt. Only `doc` knows the file format.
- The document changes **only** through `doc::Transaction`. No command writes undo code.
- The engine and the GPU tiers read `world::World`, never the document.
- Lux is calculated by the engine, never read from rendered pixels.
- Never edit a shared list to add something: sources and tests are globbed; `cmake/Cs*.cmake`
  is included automatically; every `apps/*/` is added; UI features in `libs/ui/src/features/`
  register themselves; migrations are one file each; built-in fixtures, frames, finishes and
  presets are one file each under `data/`; golden tests are directories under `tests/golden/`.
- **Units are SI** everywhere in code and files: metres, radians, candela, lumens, lux, nits
  (cd/m²), kelvin. The UI converts to cm, inches and degrees. Put the unit in a name when it is
  not obvious: `height_m`, `beam_angle_rad`, `intensity_cd`.
- **Colour is linear Rec. 709 RGB**, with luminance Y in nits. sRGB appears only when writing a
  display image. Images from outside are converted to linear on import.
- Names: `snake_case` functions, variables and files; `PascalCase` types; `.hpp` and `.cpp`.
  Every source file starts with `// SPDX-License-Identifier: Apache-2.0`.
- **Input from outside is hostile** (project files, images, IES, CSV, manifests): bound sizes,
  counts and string lengths; never trust a path; every parser has a libFuzzer target in
  `libs/<m>/fuzz/`.
- A new third-party library needs a row in `docs/dependencies.md` with an allowed licence, and
  its own task. Never vendor or download code a task did not ask for.

## Testing rules

- Acceptance criteria become tests before the implementation. On M and L tasks the
  `test-author` subagent writes them from the issue and the public headers.
- Every physical feature has a numeric test tagged `[accuracy]`, against an analytic answer or
  the engine, with its tolerance and the reason for it written in the test.
- Randomness is seeded: `cs::hash_seed` and `cs::Rng` only; never `std::random_device`, time or
  addresses. Renders must be identical across thread counts.
- Golden images and benchmark baselines change only on purpose: `--update` in a pull request
  that says why. Never loosen a tolerance to pass a test without saying so in the pull request.
- GPU tests are tagged `[gpu]`; any validation message fails the test; with no device they skip,
  unless `CS_REQUIRE_GPU=1` is set (CI and /ship set it), when they fail.
- A test that compares two modules (the GPU tier against the engine, an editor camera rendered
  on the GPU) goes in `tests/integration/`, never into a module's own tests.
- Tag every test with its module (Catch2 tags become CTest labels).

## Workflow

- One task per session: `/task <issue>`. The issue is the specification. Branch
  `task/<issue>-<slug>`.
- Read the issue and every blocker first. If a blocker is still open, stop.
- Stay inside the issue's **Touches** and **Out of scope**. If the task turns out to need more,
  stop, comment on the issue with what and why, and leave the pull request at its size.
- Commits: an imperative summary line, a blank line, a body of bullets, and the Co-Authored-By
  trailer. Pull requests are squash-merged.
- `docs/notes/` is the owner's personal learning log. Never edit it.

### Definition of done

All of these, then `/ship` opens the pull request:

1. Formatted (`python3 scripts/format.py --check`).
2. Builds in debug (sanitizers) and release; also the `app` preset if `libs/ui` or
   `apps/curation` changed.
3. All tests pass in both, including `-L gpu` on lavapipe when `libs/gpu` changed.
4. For work touching rendering, scene loading or memory: `/perf-check`, within budget.
5. Every acceptance criterion ticked, each with the command that shows it.
6. Visual work: renders or screenshots, before and after.
7. M and L tasks: `/simplify` run.
8. `/code-review` run: `high` for rendering, lighting, materials and sampling; `medium`
   elsewhere. Findings fixed or answered in the pull request.
9. A new or changed parser or importer: `/security-review` run and answered.
10. The pull request template filled from what was actually run, with the **In plain words**
    and **Concepts** sections; CI green.

## Subagents and skills

Project skills (`.claude/skills/`): `/task` (the whole loop for one issue), `/ship` (definition of
done and pull request), `/dispatch` (start ready tasks in parallel), `/look` (render or screenshot
a scene, report lux, compare goldens), `/perf-check` (benchmarks against budgets), `/groom`
(weekly backlog care).

Project subagents (`.claude/agents/`):

- `physics-reviewer`: before opening any pull request that changes materials, lights, sampling
  or lighting in any tier. It reads and runs tests but does not edit.
- `test-author`: at the start of M and L tasks, to write tests from the acceptance criteria
  without seeing the implementation.
- `build-doctor`: when a compiler, sanitizer or CI log is long; it returns the cause and the
  smallest fix.
- `gpu-reviewer` (from M1-19): every pull request that touches `libs/gpu`.
- `ux-reviewer` (from M1-27): every pull request that changes `libs/ui`, with its screenshots.

Built-in agents: **Explore** to find things across the codebase; **Plan** for L tasks and
spikes; **general-purpose** to read an external specification or paper (IES LM-63, a Vulkan
specification chapter, Embree's or OIDN's API) and return only the details that matter.

Built-in skills: `/code-review` and `/simplify` as above; `/security-review` for parsers;
`/run` to launch the app and look at the result.
