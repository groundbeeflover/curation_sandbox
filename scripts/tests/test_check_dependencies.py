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
        for expr in ["", "LicenseRef-proprietary", "MIT OR", "(MIT", "MIT)", "MIT AND AND MIT",
                     "WITH MIT", "MIT WITH", "MIT MIT"]:
            with self.subTest(expr=expr):
                self.assertFalse(self.allowed(expr))

    def test_case_matters(self):
        # SPDX operators are upper case; "mit" is not the identifier "MIT".
        self.assertFalse(self.allowed("mit"))
        self.assertFalse(self.allowed("MIT or Apache-2.0"))


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

    def test_disallowed_licence_of_pulled_in_port(self):
        doc = DOC.replace("| `zlib` | 1.3.1 | `BSD-3-Clause` |", "| `zlib` | 1.3.1 | `GPL-2.0-only` |")
        self.assertTrue(any("zlib" in p for p in problems(doc=doc)))

    def test_disallowed_licence_of_vendored_library(self):
        doc = DOC.replace("| NVIDIA FLIP | 1.7 | `BSD-3-Clause` |", "| NVIDIA FLIP | 1.7 | `CC-BY-NC-4.0` |")
        self.assertTrue(any("FLIP" in p for p in problems(doc=doc)))

    def test_row_for_a_port_not_in_the_manifest(self):
        manifest = dict(MANIFEST, dependencies=["glm", "embree"])
        found = problems(manifest)
        self.assertEqual(len(found), 1)
        self.assertIn("vulkan-headers", found[0])

    def test_duplicate_row(self):
        doc = DOC.replace("| `glm` | 1.0.3 | `MIT` | maths | core |",
                          "| `glm` | 1.0.3 | `MIT` | maths | core |\n| `glm` | 1.0.3 | `MIT` | x | y |")
        self.assertTrue(any("glm" in p for p in problems(doc=doc)))

    def test_a_licence_the_policy_does_not_list(self):
        doc = DOC.replace("| `BSD-3-Clause` | BSD 3-Clause |\n", "")
        found = problems(doc=doc)
        self.assertEqual(len(found), 2)  # the pulled-in zlib row and the vendored FLIP row

    def test_missing_sections(self):
        for heading in ["## Allowed licences", "## Libraries from vcpkg"]:
            with self.subTest(heading=heading):
                doc = DOC.replace(heading, "## Something else")
                self.assertTrue(problems(doc=doc))

    def test_section_without_a_table(self):
        start = DOC.index("| Port | Version | Licence | Purpose | Module |")
        end = DOC.index("### Pulled in")
        self.assertTrue(problems(doc=DOC[:start] + DOC[end:]))


class InstalledTests(unittest.TestCase):
    def installed(self, names):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        info = Path(tmp.name) / "vcpkg" / "info"
        info.mkdir(parents=True)
        for name in names:
            (info / name).write_text("include/x.h\n")
        return deps.read_installed(Path(tmp.name))

    def test_read_installed(self):
        found = self.installed(["glm_1.0.3_x64-linux.list", "vulkan-headers_1.4.363.0_x64-linux.list",
                                "shader-slang_2026.21_x64-windows.list", "status"])
        self.assertEqual(found, {"glm": "1.0.3", "vulkan-headers": "1.4.363.0",
                                 "shader-slang": "2026.21"})

    def test_read_installed_needs_the_info_directory(self):
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(FileNotFoundError):
            deps.read_installed(Path(tmp))

    def test_matching_versions_pass(self):
        installed = {"glm": "1.0.3", "embree": "4.4.1", "vulkan-headers": "1.4.363.0",
                     "zlib": "1.3.1", "vcpkg-cmake": "2024-04-23"}
        self.assertEqual(problems(installed=installed), [])

    def test_version_differs(self):
        installed = {"glm": "1.0.4", "embree": "4.4.1", "vulkan-headers": "1.4.363.0", "zlib": "1.3.1"}
        found = problems(installed=installed)
        self.assertEqual(len(found), 1)
        self.assertIn("glm", found[0])
        self.assertIn("1.0.4", found[0])

    def test_installed_port_not_documented(self):
        installed = {"glm": "1.0.3", "embree": "4.4.1", "vulkan-headers": "1.4.363.0",
                     "zlib": "1.3.1", "tbb": "2022.2.0"}
        found = problems(installed=installed)
        self.assertEqual(len(found), 1)
        self.assertIn("tbb", found[0])

    def test_documented_pulled_in_port_no_longer_installed(self):
        installed = {"glm": "1.0.3", "embree": "4.4.1", "vulkan-headers": "1.4.363.0"}
        found = problems(installed=installed)
        self.assertEqual(len(found), 1)
        self.assertIn("zlib", found[0])


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
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        info = Path(tmp.name) / "vcpkg" / "info"
        info.mkdir(parents=True)
        for name in ["glm_1.0.3", "embree_4.4.2", "vulkan-headers_1.4.363.0", "zlib_1.3.1"]:
            (info / f"{name}_x64-linux.list").write_text("")
        result = self.run_script(*self.write(MANIFEST, DOC), "--installed", tmp.name)
        self.assertEqual(result.returncode, 1)
        self.assertIn("embree", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
