# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/test-chart/make_test_chart.py.

Run: python3 -m unittest discover -s tools/test-chart -v

The PNG is decoded here with the standard library (zlib.decompress), independently of the writer,
and every pixel is compared with the layout and the documented patch values.
"""

import contextlib
import hashlib
import io
import importlib.util
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SKELETON_ASSETS = ROOT / "tests" / "scenes" / "skeleton" / "assets"

spec = importlib.util.spec_from_file_location("make_test_chart", HERE / "make_test_chart.py")
chart = importlib.util.module_from_spec(spec)
sys.modules["make_test_chart"] = chart
spec.loader.exec_module(chart)

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def chunks(png: bytes) -> list[tuple[bytes, bytes]]:
    """The (type, data) of every chunk, checking each one's length and CRC."""
    assert png[:8] == PNG_SIGNATURE, "not a PNG signature"
    out, at = [], 8
    while at < len(png):
        (length,) = struct.unpack(">I", png[at:at + 4])
        kind, data = png[at + 4:at + 8], png[at + 8:at + 8 + length]
        (crc,) = struct.unpack(">I", png[at + 8 + length:at + 12 + length])
        assert len(data) == length, f"{kind!r} is truncated"
        assert crc == zlib.crc32(kind + data), f"{kind!r} has a wrong CRC"
        out.append((kind, data))
        at += 12 + length
    return out


def decode(png: bytes) -> tuple[int, int, list[list[tuple[int, int, int]]]]:
    """Width, height and rows of (r, g, b) pixels of an 8-bit RGB PNG whose rows all use filter 0,
    which is what the writer promises."""
    parts = chunks(png)
    kind, ihdr = parts[0]
    assert kind == b"IHDR"
    width, height, depth, colour_type, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", ihdr)
    assert (depth, colour_type, compression, filtering, interlace) == (8, 2, 0, 0, 0)
    raw = zlib.decompress(b"".join(data for kind, data in parts if kind == b"IDAT"))
    stride = 1 + 3 * width
    assert len(raw) == stride * height
    rows = []
    for y in range(height):
        line = raw[y * stride:(y + 1) * stride]
        assert line[0] == 0, f"row {y} uses filter {line[0]}"
        rows.append([tuple(line[1 + 3 * x:4 + 3 * x]) for x in range(width)])
    return width, height, rows


class TestChart(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.png = chart.chart_png()
        cls.width, cls.height, cls.rows = decode(cls.png)

    def test_is_a_png_of_at_most_50_kb(self):
        self.assertLessEqual(len(self.png), 50_000)
        kinds = [kind for kind, _ in chunks(self.png)]
        self.assertEqual(kinds[0], b"IHDR")
        self.assertEqual(kinds[-1], b"IEND")
        self.assertIn(b"sRGB", kinds)  # the values are sRGB, and the file says so

    def test_has_the_skeleton_work_proportions(self):
        # 0.73 m wide and 0.92 m high, so that the chart is not stretched on the work.
        self.assertEqual((self.width, self.height), (chart.WIDTH, chart.HEIGHT))
        self.assertEqual(self.width * 92, self.height * 73)

    def test_is_a_4_by_6_grid_of_the_documented_patches(self):
        self.assertEqual((chart.COLUMNS, chart.ROWS), (4, 6))
        self.assertEqual(len(chart.PATCHES), 24)
        self.assertEqual(len({patch.srgb for patch in chart.PATCHES}), 24, "patches must differ")
        # A border all round and gaps between patches, filling the image exactly.
        self.assertEqual(2 * chart.BORDER + 4 * chart.PATCH_WIDTH + 3 * chart.GAP, self.width)
        self.assertEqual(2 * chart.BORDER + 6 * chart.PATCH_HEIGHT + 5 * chart.GAP, self.height)
        expected = [[chart.BORDER_COLOUR] * self.width for _ in range(self.height)]
        for index, patch in enumerate(chart.PATCHES):
            # Row by row from the top, left to right.
            row, column = divmod(index, 4)
            x0 = chart.BORDER + column * (chart.PATCH_WIDTH + chart.GAP)
            y0 = chart.BORDER + row * (chart.PATCH_HEIGHT + chart.GAP)
            x1, y1 = x0 + chart.PATCH_WIDTH, y0 + chart.PATCH_HEIGHT
            self.assertEqual(chart.patch_rect(index), (x0, y0, x1, y1))
            for y in range(y0, y1):
                for x in range(x0, x1):
                    expected[y][x] = patch.srgb
        for y in range(2 * chart.BORDER):
            for x in range(2 * chart.BORDER - y):
                expected[y][x] = chart.MARK_COLOUR
        for y in range(self.height):
            self.assertEqual(self.rows[y], expected[y], f"row {y}")

    def test_has_an_orientation_mark_in_the_top_left_only(self):
        corners = {
            "top left": self.rows[0][0],
            "top right": self.rows[0][-1],
            "bottom left": self.rows[-1][0],
            "bottom right": self.rows[-1][-1],
        }
        self.assertEqual(corners.pop("top left"), chart.MARK_COLOUR)
        self.assertEqual(set(corners.values()), {chart.BORDER_COLOUR})
        self.assertNotEqual(chart.MARK_COLOUR, chart.BORDER_COLOUR)
        # The mark lies wholly in the border, so that no patch loses pixels to it.
        for index in range(len(chart.PATCHES)):
            x0, y0, _, _ = chart.patch_rect(index)
            self.assertGreaterEqual(x0 + y0, 2 * chart.BORDER)

    def test_patch_values_are_neither_black_nor_full_white(self):
        # Base albedos of 0 or 1 are not physical; keeping inside them keeps the render honest.
        for patch in chart.PATCHES:
            self.assertTrue(all(0 < channel < 255 for channel in patch.srgb), patch.name)

    def test_the_readme_documents_every_patch(self):
        readme = (HERE / "README.md").read_text(encoding="utf-8")
        self.assertIn(chart.markdown_table(), readme)

    def test_is_the_same_bytes_every_time(self):
        self.assertEqual(chart.chart_png(), self.png)


def run(*argv: str) -> int:
    """make_test_chart.py's exit code for these arguments, without its messages."""
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return chart.main(list(argv))


class TestCommandLine(unittest.TestCase):
    def test_writes_the_chart_named_by_its_sha256_and_check_accepts_it(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(run(folder), 0)
            written = list(Path(folder).iterdir())
            self.assertEqual(len(written), 1)
            data = written[0].read_bytes()
            self.assertEqual(data, chart.chart_png())
            self.assertEqual(written[0].name, hashlib.sha256(data).hexdigest() + ".png")
            self.assertEqual(run("--check", folder), 0)

    def test_check_rejects_a_changed_or_missing_chart(self):
        with tempfile.TemporaryDirectory() as folder:
            run(folder)
            path = next(Path(folder).iterdir())
            data = bytearray(path.read_bytes())
            data[len(data) // 2] ^= 0x01
            path.write_bytes(bytes(data))
            self.assertEqual(run("--check", folder), 1)
            path.unlink()
            self.assertEqual(run("--check", folder), 1)

    def test_the_committed_chart_of_the_skeleton_scene_is_current(self):
        self.assertEqual(run("--check", str(SKELETON_ASSETS)), 0)


if __name__ == "__main__":
    unittest.main()
