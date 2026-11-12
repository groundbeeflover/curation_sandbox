#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the test chart that the skeleton scene's work shows, or check a committed copy of it.

    python3 tools/test-chart/make_test_chart.py DIR            write DIR/<sha256>.png
    python3 tools/test-chart/make_test_chart.py --check DIR    exit 1 unless DIR/<sha256>.png
                                                               holds exactly these bytes
    python3 tools/test-chart/make_test_chart.py --table        print README.md's patch table

The chart is a 4 x 6 grid of flat patches with documented sRGB values (PATCHES, and README.md)
on a dark border, with an orientation mark in the top-left corner. It is 292 x 368 pixels: 4 px
per cm on the skeleton's 0.73 x 0.92 m work. A project names its assets by their SHA-256 (ADR
0007), so any change to the chart changes its file name; after one, update the work's image hash
in tests/scenes/skeleton/project.json.

The bytes must be the same on every machine, and zlib's compressor does not promise that:
zlib-ng, which some systems and Python builds use in its place, writes different (equally valid)
streams. So this file writes its own DEFLATE stream, with the fixed Huffman codes and matches
only against the previous pixel or the pixel above, which is all a chart of flat patches needs.
zlib is used for checksums and to read the stream back. Standard library only.
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import struct
import sys
import zlib
from pathlib import Path
from typing import NamedTuple


class Patch(NamedTuple):
    name: str
    srgb: tuple[int, int, int]  # 8-bit sRGB


# Read row by row from the top, left to right. No channel is 0 or 255: a base albedo of 0 or 1 is
# not physical.
PATCHES = (
    # Rows 1 and 2: a neutral ramp, each patch one stop below the one before (half the
    # reflectance), from 0.90: the sRGB encoding of 0.9 x 2^-k, rounded.
    Patch("white", (243, 243, 243)),
    Patch("grey 1", (179, 179, 179)),
    Patch("grey 2", (130, 130, 130)),
    Patch("grey 3", (94, 94, 94)),
    Patch("grey 4", (67, 67, 67)),
    Patch("grey 5", (47, 47, 47)),
    Patch("grey 6", (31, 31, 31)),
    Patch("black", (20, 20, 20)),
    # Rows 3 and 4: primaries and secondaries, which show swapped or dropped channels.
    Patch("red", (190, 40, 40)),
    Patch("green", (40, 150, 70)),
    Patch("blue", (40, 70, 180)),
    Patch("yellow", (230, 200, 40)),
    Patch("cyan", (40, 170, 190)),
    Patch("magenta", (190, 50, 150)),
    Patch("orange", (230, 130, 40)),
    Patch("violet", (110, 70, 160)),
    # Rows 5 and 6: colours of things.
    Patch("light skin", (220, 170, 140)),
    Patch("dark skin", (120, 80, 60)),
    Patch("sky", (100, 140, 200)),
    Patch("foliage", (90, 120, 50)),
    Patch("sand", (210, 190, 140)),
    Patch("brick", (160, 70, 50)),
    Patch("teal", (30, 120, 120)),
    Patch("slate", (80, 90, 130)),
)

WIDTH, HEIGHT = 292, 368
COLUMNS, ROWS = 4, 6
BORDER = 20  # pixels around the grid
GAP = 8  # pixels between patches
PATCH_WIDTH = (WIDTH - 2 * BORDER - (COLUMNS - 1) * GAP) // COLUMNS  # 57
PATCH_HEIGHT = (HEIGHT - 2 * BORDER - (ROWS - 1) * GAP) // ROWS  # 48
BORDER_COLOUR = (12, 12, 12)
# A right triangle in the top-left corner, legs 2 x BORDER: the pixels with x + y < 2 x BORDER,
# which all lie in the border.
MARK_COLOUR = (243, 243, 243)
MAX_BYTES = 50_000

if (len(PATCHES) != COLUMNS * ROWS
        or 2 * BORDER + COLUMNS * PATCH_WIDTH + (COLUMNS - 1) * GAP != WIDTH
        or 2 * BORDER + ROWS * PATCH_HEIGHT + (ROWS - 1) * GAP != HEIGHT):
    raise SystemExit("the patches do not fill the chart exactly")


def patch_rect(index: int) -> tuple[int, int, int, int]:
    """The pixels of patch `index`: x0, y0 (inclusive), x1, y1 (exclusive)."""
    row, column = divmod(index, COLUMNS)
    x0 = BORDER + column * (PATCH_WIDTH + GAP)
    y0 = BORDER + row * (PATCH_HEIGHT + GAP)
    return x0, y0, x0 + PATCH_WIDTH, y0 + PATCH_HEIGHT


def chart_rows() -> list[bytes]:
    """The chart's rows of 8-bit RGB pixels, from the top."""
    rows = [bytearray(bytes(BORDER_COLOUR) * WIDTH) for _ in range(HEIGHT)]
    for index, patch in enumerate(PATCHES):
        x0, y0, x1, y1 = patch_rect(index)
        for y in range(y0, y1):
            rows[y][3 * x0:3 * x1] = bytes(patch.srgb) * (x1 - x0)
    for y in range(2 * BORDER):
        rows[y][:3 * (2 * BORDER - y)] = bytes(MARK_COLOUR) * (2 * BORDER - y)
    return [bytes(row) for row in rows]


# ---- DEFLATE (RFC 1951) and zlib (RFC 1950) ---------------------------------------------------

# Section 3.2.5: the smallest length of each length code 257 + i and the smallest distance of
# each distance code i, with the number of extra bits that follow them.
LENGTH_BASES = (3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                99, 115, 131, 163, 195, 227, 258)
LENGTH_EXTRA_BITS = (0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,
                     5, 5, 0)
DISTANCE_BASES = (1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577)
DISTANCE_EXTRA_BITS = (0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                       11, 11, 12, 12, 13, 13)


class BitWriter:
    """Bits packed from the least significant bit of each byte up, as DEFLATE stores them."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.pending = 0
        self.count = 0

    def bits(self, value: int, count: int) -> None:
        self.pending |= value << self.count
        self.count += count
        while self.count >= 8:
            self.data.append(self.pending & 0xFF)
            self.pending >>= 8
            self.count -= 8

    def code(self, code: int, length: int) -> None:
        """A Huffman code, which DEFLATE stores from its most significant bit."""
        self.bits(int(format(code, f"0{length}b")[::-1], 2), length)

    def symbol(self, symbol: int) -> None:
        """A literal or length symbol, in the fixed Huffman code of section 3.2.6."""
        if symbol < 144:
            self.code(0x30 + symbol, 8)
        elif symbol < 256:
            self.code(0x190 + symbol - 144, 9)
        elif symbol < 280:
            self.code(symbol - 256, 7)
        else:
            self.code(0xC0 + symbol - 280, 8)

    def finish(self) -> bytes:
        if self.count:
            self.data.append(self.pending)
        return bytes(self.data)


def deflate(data: bytes, distances: tuple[int, ...]) -> bytes:
    """One fixed-Huffman block. At each byte, the longest match at one of `distances`, if at least
    3 bytes long; otherwise the byte itself."""
    out = BitWriter()
    out.bits(1, 1)  # BFINAL: the last block
    out.bits(1, 2)  # BTYPE 01: fixed Huffman codes
    at = 0
    while at < len(data):
        limit = min(258, len(data) - at)
        length, distance = 0, 0
        for candidate in distances:
            n = 0
            if candidate <= at:
                while n < limit and data[at + n] == data[at + n - candidate]:
                    n += 1
            if n > length:
                length, distance = n, candidate
        if length >= 3:
            i = bisect.bisect_right(LENGTH_BASES, length) - 1
            out.symbol(257 + i)
            out.bits(length - LENGTH_BASES[i], LENGTH_EXTRA_BITS[i])
            j = bisect.bisect_right(DISTANCE_BASES, distance) - 1
            out.code(j, 5)
            out.bits(distance - DISTANCE_BASES[j], DISTANCE_EXTRA_BITS[j])
            at += length
        else:
            out.symbol(data[at])
            at += 1
    out.symbol(256)  # end of block
    return out.finish()


def zlib_stream(data: bytes, distances: tuple[int, ...]) -> bytes:
    # CMF 0x78: DEFLATE with a 32 KiB window. FLG 0x01: no dictionary, and the check bits that
    # make 0x7801 a multiple of 31.
    return b"\x78\x01" + deflate(data, distances) + struct.pack(">I", zlib.adler32(data))


# ---- PNG ---------------------------------------------------------------------------------------

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def chart_png() -> bytes:
    """The chart as a PNG: 8-bit RGB, every row with filter 0, an sRGB chunk, and the same bytes
    on every machine."""
    raw = b"".join(b"\x00" + row for row in chart_rows())
    stride = 1 + 3 * WIDTH
    stream = zlib_stream(raw, distances=(3, stride))  # the previous pixel, the pixel above
    if zlib.decompress(stream) != raw:
        raise RuntimeError("the DEFLATE stream does not read back")
    png = (PNG_SIGNATURE
           + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
           + chunk(b"sRGB", bytes([0]))  # perceptual rendering intent
           + chunk(b"gAMA", struct.pack(">I", 45455))  # for decoders without sRGB support
           + chunk(b"IDAT", stream)
           + chunk(b"IEND", b""))
    if len(png) > MAX_BYTES:
        raise RuntimeError(f"the chart is {len(png)} bytes, over {MAX_BYTES}")
    return png


def file_name(png: bytes) -> str:
    return hashlib.sha256(png).hexdigest() + ".png"


# ---- The patch table ---------------------------------------------------------------------------

def srgb_to_linear(value: int) -> float:
    """The sRGB transfer function (IEC 61966-2-1), for an 8-bit value."""
    c = value / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def markdown_table() -> str:
    def row(number: str, where: str, name: str, srgb: tuple[int, int, int]) -> str:
        linear = ", ".join(f"{srgb_to_linear(c):.4f}" for c in srgb)
        return f"| {number} | {where} | {name} | {', '.join(map(str, srgb))} | {linear} |"

    lines = ["| # | Row, column | Name | sRGB (8-bit) | Linear |", "| --- | --- | --- | --- | --- |"]
    for index, patch in enumerate(PATCHES):
        r, c = divmod(index, COLUMNS)
        lines.append(row(str(index + 1), f"{r + 1}, {c + 1}", patch.name, patch.srgb))
    lines.append(row("", "border and gaps", "border", BORDER_COLOUR))
    lines.append(row("", "top-left corner", "orientation mark", MARK_COLOUR))
    return "\n".join(lines)


# ---- Command line ------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Write the skeleton scene's test chart as DIR/<sha256>.png, or check it.")
    parser.add_argument("folder", nargs="?", type=Path, help="the assets folder")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 unless the folder holds the chart, byte for byte")
    parser.add_argument("--table", action="store_true", help="print the patch table")
    args = parser.parse_args(argv)

    if args.table:
        print(markdown_table())
        return 0
    if args.folder is None:
        parser.error("a folder is required")

    png = chart_png()
    path = args.folder / file_name(png)
    if args.check:
        if path.is_file() and path.read_bytes() == png:
            print(f"{path}: the test chart, byte for byte")
            return 0
        found = sorted(p.name for p in args.folder.glob("*.png")) if args.folder.is_dir() else []
        print(f"{path}: {'differs from' if path.exists() else 'missing; it would hold'} the chart "
              f"this script writes (PNGs in the folder: {', '.join(found) or 'none'}). If the "
              "chart changed on purpose, write it with this script, delete the old file, and put "
              "the new hash in the project's image reference.", file=sys.stderr)
        return 1

    args.folder.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)
    print(f"{path} ({len(png)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
