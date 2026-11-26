"""Tests for scripts/check-dependencies.py. Run: python3 -m unittest discover -s scripts/tests"""

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "check-dependencies.py"
spec = importlib.util.spec_from_file_location("deps", SCRIPT)
deps = importlib.util.module_from_spec(spec)
sys.modules["deps"] = deps
spec.loader.exec_module(deps)

DOC = """# Dependencies

Some prose, and a table that is not one of the checked sections:

| A | B |
| --- | --- |
| `zlib` | `GPL-3.0-only` |

## Allowed licences

| SPDX | Licence |
| --- | --- |
| `MIT` | MIT License |
| `Apache-2.0` | Apache License 2.0 |
| `BSD-3-Clause` | BSD 3-Clause |

## Libraries from vcpkg

| Port | Version | Licence | Purpose | Module |
| --- | --- | --- | --- | --- |
| `glm` | 1.0.3 | `MIT` | maths | core |
| `embree` | 4.4.1 | `Apache-2.0` | [CPU ray tracing](https://www.embree.org/) | engine |
| `vulkan-headers` | 1.4.363.0 | `Apache-2.0 OR MIT` | Vulkan | gpu |

### Pulled in by other ports

| Port | Version | Licence | Needed by |
| --- | --- | --- | --- |
| `zlib` | 1.3.1 | `BSD-3-Clause` | tiff |

## Vendored

| Library | Version | Licence | Purpose | Module |
| --- | --- | --- | --- | --- |
| NVIDIA FLIP | 1.7 | `BSD-3-Clause` | image comparison | core |
"""

MANIFEST = {
    "name": "x",
    "dependencies": ["glm", {"name": "embree", "default-features": False}, "vulkan-headers"],
}


def problems(manifest=None, doc=DOC, installed=None):
    return deps.check(manifest if manifest is not None else MANIFEST, doc, installed)


def make_installed_dir(test, list_files):
    """A temporary vcpkg_installed directory whose vcpkg/info/ holds these (empty) files."""
    tmp = tempfile.TemporaryDirectory()
    test.addCleanup(tmp.cleanup)
    info = Path(tmp.name) / "vcpkg" / "info"
    info.mkdir(parents=True)
    for name in list_files:
        (info / name).write_text("")
    return Path(tmp.name)


class LicenceExpressionTests(unittest.TestCase):
    ALLOWED = {"MIT", "Apache-2.0", "BSD-3-Clause"}

    def allowed(self, expr):
        return deps.licence_allowed(expr, self.ALLOWED)

    def test_single(self):
        self.assertTrue(self.allowed("MIT"))
        self.assertFalse(self.allowed("GPL-3.0-only"))

    def test_or_needs_one_allowed_choice(self):
        self.assertTrue(self.allowed("Apache-2.0 OR MIT"))
        self.assertTrue(self.allowed("GPL-2.0-only OR MIT"))
        self.assertFalse(self.allowed("GPL-2.0-only OR LGPL-3.0-only"))

    def test_and_needs_every_part_allowed(self):
        self.assertTrue(self.allowed("MIT AND BSD-3-Clause"))
        self.assertFalse(self.allowed("MIT AND GPL-2.0-only"))

    def test_and_binds_tighter_than_or(self):
        # MIT OR (GPL AND Apache): the MIT choice is enough.
        self.assertTrue(self.allowed("MIT OR GPL-2.0-only AND Apache-2.0"))
        # (MIT OR GPL) AND CC-BY-NC: the second part is never allowed.
        self.assertFalse(self.allowed("(MIT OR GPL-2.0-only) AND CC-BY-NC-4.0"))

    def test_exception_keeps_the_base_licence(self):
        # An exception only adds permissions, so the base licence decides.
        self.assertTrue(self.allowed("Apache-2.0 WITH LLVM-exception"))
        self.assertFalse(self.allowed("GPL-2.0-only WITH Classpath-exception-2.0"))

    def test_unknown_or_malformed_is_refused(self):
        # SPDX operators are upper case, and "mit" is not the identifier "MIT".
        for expr in ["", "LicenseRef-proprietary", "MIT OR", "(MIT", "MIT)", "MIT AND AND MIT",
                     "WITH MIT", "MIT WITH", "MIT MIT", "mit", "MIT or Apache-2.0"]:
            with self.subTest(expr=expr):
                self.assertFalse(self.allowed(expr))


class CheckTests(unittest.TestCase):
    def test_consistent_files_pass(self):
        self.assertEqual(problems(), [])

    def test_manifest_dependency_missing_from_doc(self):
        manifest = dict(MANIFEST, dependencies=MANIFEST["dependencies"] + ["miniz"])
        found = problems(manifest)
        self.assertEqual(len(found), 1)
        self.assertIn("miniz", found[0])

    def test_dependency_listed_only_as_pulled_in_is_missing(self):
        # A direct dependency belongs in the vcpkg table, not among the ports others pull in.
        manifest = dict(MANIFEST, dependencies=MANIFEST["dependencies"] + ["zlib"])
        self.assertTrue(any("zlib" in p for p in problems(manifest)))

    def test_object_form_dependency(self):
        manifest = {"dependencies": [{"name": "glm"}, {"name": "embree", "features": ["a"]},
                                     "vulkan-headers"]}
        self.assertEqual(problems(manifest), [])

    def test_disallowed_licence(self):
        doc = DOC.replace("| `glm` | 1.0.3 | `MIT` |", "| `glm` | 1.0.3 | `GPL-3.0-only` |")
        found = problems(doc=doc)
        self.assertEqual(len(found), 1)
        self.assertIn("glm", found[0])
        self.assertIn("GPL-3.0-only", found[0])

    def test_licence_without_backticks_is_read(self):
        doc = DOC.replace("| `glm` | 1.0.3 | `MIT` |", "| `glm` | 1.0.3 | MIT |")
        self.assertEqual(problems(doc=doc), [])

    def test_licence_in_several_code_spans_is_read_whole(self):
        doc = DOC.replace("| `glm` | 1.0.3 | `MIT` |", "| `glm` | 1.0.3 | `MIT` AND `GPL-3.0-only` |")
        found = problems(doc=doc)
        self.assertEqual(len(found), 1)
        self.assertIn("MIT AND GPL-3.0-only", found[0])

    def test_escaped_pipe_stays_in_its_cell(self):
        doc = DOC.replace("| maths | core |", "| maths \\| more maths | core |")
        self.assertEqual(problems(doc=doc), [])
        doc = DOC.replace("| `glm` | 1.0.3 | `MIT` |", "| `glm` | 1.0.3 | `MIT` \\| x |")
        self.assertTrue(any("glm" in p for p in problems(doc=doc)))

    def test_row_for_a_port_not_in_the_manifest(self):
        manifest = dict(MANIFEST, dependencies=["glm", "embree"])
        found = problems(manifest)
        self.assertEqual(len(found), 1)
        self.assertIn("vulkan-headers", found[0])

    def test_duplicate_row(self):
        doc = DOC.replace("| `glm` | 1.0.3 | `MIT` | maths | core |",
                          "| `glm` | 1.0.3 | `MIT` | maths | core |\n| `glm` | 1.0.3 | `MIT` | x | y |")
        self.assertTrue(any("glm" in p for p in problems(doc=doc)))

    def test_licences_of_pulled_in_and_vendored_rows_are_checked(self):
        doc = DOC.replace("| `BSD-3-Clause` | BSD 3-Clause |\n", "")
        found = problems(doc=doc)
        self.assertEqual(len(found), 2)
        self.assertIn("zlib", found[0])
        self.assertIn("NVIDIA FLIP", found[1])

    def test_missing_sections(self):
        start = DOC.index("| Port | Version | Licence | Purpose | Module |")
        without_table = DOC[:start] + DOC[DOC.index("### Pulled in"):]
        cases = {heading: DOC.replace(heading, "## Something else")
                 for heading in ["## Allowed licences", "## Libraries from vcpkg"]}
        cases["vcpkg heading without a table"] = without_table
        for name, doc in cases.items():
            with self.subTest(name):
                self.assertTrue(problems(doc=doc))

    def test_comment_in_a_code_block_is_not_a_heading(self):
        # Between a heading and its table: read as a heading, it would take the table.
        for fence in ["```", "~~~"]:
            with self.subTest(fence=fence):
                doc = DOC.replace("## Libraries from vcpkg\n",
                                  f"## Libraries from vcpkg\n\n{fence}sh\n# install them\n{fence}\n")
                self.assertEqual(problems(doc=doc), [])


def on_one_triplet(versions, triplet="x64-linux"):
    """What read_installed returns when every port is installed for one triplet."""
    return {name: {triplet: version} for name, version in versions.items()}


# A target triplet other than the host's: vcpkg installs host tools (and what they need) for the
# host triplet and libraries for the target, so one port can be installed once for each.
HOST, TARGET = "x64-windows", "x64-windows-static"


class InstalledTests(unittest.TestCase):
    def test_read_installed(self):
        found = deps.read_installed(make_installed_dir(self, [
            "glm_1.0.3_x64-linux.list", "vulkan-headers_1.4.363.0_x64-linux.list",
            "shader-slang_2026.21_x64-windows.list", "status"]))
        self.assertEqual(found, {"glm": {"x64-linux": "1.0.3"},
                                 "vulkan-headers": {"x64-linux": "1.4.363.0"},
                                 "shader-slang": {"x64-windows": "2026.21"}})

    def test_read_installed_keeps_one_copy_per_triplet(self):
        found = deps.read_installed(make_installed_dir(self, [
            f"zlib_1.3.1_{HOST}.list", f"zlib_1.3.1_{TARGET}.list"]))
        self.assertEqual(found, {"zlib": {HOST: "1.3.1", TARGET: "1.3.1"}})

    def test_read_installed_needs_the_info_directory(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(FileNotFoundError):
            deps.read_installed(Path(tmp))

    def test_matching_versions_pass(self):
        installed = on_one_triplet({"glm": "1.0.3", "embree": "4.4.1",
                                    "vulkan-headers": "1.4.363.0", "zlib": "1.3.1",
                                    "vcpkg-cmake": "2024-04-23"})
        self.assertEqual(problems(installed=installed), [])

    def test_version_differs(self):
        installed = on_one_triplet({"glm": "1.0.4", "embree": "4.4.1",
                                    "vulkan-headers": "1.4.363.0", "zlib": "1.3.1"})
        found = problems(installed=installed)
        self.assertEqual(len(found), 1)
        self.assertIn("glm", found[0])
        self.assertIn("1.0.4", found[0])

    def test_every_copy_of_a_port_is_compared(self):
        # The wrong copy is found whichever triplet holds it, so the answer does not depend on
        # which list file is read last.
        others = [f"{p}_{TARGET}.list" for p in ["glm_1.0.3", "embree_4.4.1",
                                                 "vulkan-headers_1.4.363.0"]]
        for wrong, right in [(HOST, TARGET), (TARGET, HOST)]:
            with self.subTest(wrong=wrong):
                installed = deps.read_installed(make_installed_dir(self, others + [
                    f"zlib_1.3.0_{wrong}.list", f"zlib_1.3.1_{right}.list"]))
                found = problems(installed=installed)
                self.assertEqual(len(found), 1, found)
                self.assertIn("zlib", found[0])
                self.assertIn("1.3.0", found[0])
                self.assertIn(wrong, found[0])

    def test_installed_port_not_documented(self):
        installed = on_one_triplet({"glm": "1.0.3", "embree": "4.4.1",
                                    "vulkan-headers": "1.4.363.0", "zlib": "1.3.1",
                                    "tbb": "2022.2.0"})
        found = problems(installed=installed)
        self.assertEqual(len(found), 1)
        self.assertIn("tbb", found[0])

    def test_undocumented_port_installed_twice_is_reported_once(self):
        installed = on_one_triplet({"glm": "1.0.3", "embree": "4.4.1",
                                    "vulkan-headers": "1.4.363.0", "zlib": "1.3.1"})
        installed["tbb"] = {HOST: "2022.2.0", TARGET: "2022.2.0"}
        found = problems(installed=installed)
        self.assertEqual(len(found), 1, found)
        self.assertIn("tbb", found[0])

    def test_documented_port_not_installed(self):
        everything = {"glm": "1.0.3", "embree": "4.4.1", "vulkan-headers": "1.4.363.0",
                      "zlib": "1.3.1"}
        for missing in ["zlib", "embree"]:  # pulled in, and direct
            with self.subTest(missing=missing):
                installed = on_one_triplet({n: v for n, v in everything.items() if n != missing})
                found = problems(installed=installed)
                self.assertEqual(len(found), 1)
                self.assertIn(missing, found[0])


class CommandLineTests(unittest.TestCase):
    def run_script(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True)

    def write(self, manifest, doc):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        m, d = Path(tmp.name) / "vcpkg.json", Path(tmp.name) / "dependencies.md"
        m.write_text(json.dumps(manifest))
        d.write_text(doc)
        return ["--manifest", str(m), "--doc", str(d)]

    def test_repository_passes(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exit_0_when_consistent(self):
        result = self.run_script(*self.write(MANIFEST, DOC))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exit_1_when_a_dependency_is_missing(self):
        manifest = dict(MANIFEST, dependencies=MANIFEST["dependencies"] + ["miniz"])
        result = self.run_script(*self.write(manifest, DOC))
        self.assertEqual(result.returncode, 1)
        self.assertIn("miniz", result.stdout + result.stderr)

    def test_exit_1_when_a_licence_is_not_allowed(self):
        doc = DOC.replace("| `embree` | 4.4.1 | `Apache-2.0` |", "| `embree` | 4.4.1 | `AGPL-3.0-only` |")
        result = self.run_script(*self.write(MANIFEST, doc))
        self.assertEqual(result.returncode, 1)
        self.assertIn("embree", result.stdout + result.stderr)

    def test_exit_1_when_an_installed_version_differs(self):
        installed = make_installed_dir(self, [f"{name}_x64-linux.list" for name in [
            "glm_1.0.3", "embree_4.4.2", "vulkan-headers_1.4.363.0", "zlib_1.3.1"]])
        result = self.run_script(*self.write(MANIFEST, DOC), "--installed", str(installed))
        self.assertEqual(result.returncode, 1)
        self.assertIn("embree", result.stdout + result.stderr)

    def test_exit_1_when_one_triplets_copy_differs(self):
        installed = make_installed_dir(self, [f"{name}_{TARGET}.list" for name in [
            "glm_1.0.3", "embree_4.4.1", "vulkan-headers_1.4.363.0", "zlib_1.3.1"]] + [
            f"zlib_1.3.0_{HOST}.list"])
        result = self.run_script(*self.write(MANIFEST, DOC), "--installed", str(installed))
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"zlib: 1.3.0 is installed for {HOST}", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
