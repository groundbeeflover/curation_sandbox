# Test chart

The picture that the skeleton scene's work shows (`tests/scenes/skeleton/`). In any render you
can check from it that the image is the right way up, not mirrored, with its channels in the right
order, and that its colours and greys come through.

```sh
python3 tools/test-chart/make_test_chart.py tests/scenes/skeleton/assets/           # write it
python3 tools/test-chart/make_test_chart.py --check tests/scenes/skeleton/assets/   # check it
python3 tools/test-chart/make_test_chart.py --table                                 # the table below
python3 -m unittest discover -s tools/test-chart -v                                  # its tests
```

The file is named by its SHA-256, as every asset of a project is (ADR 0007). After a change on
purpose to the chart: write it, delete the old file, and put the new hash in the work's `image`
in `tests/scenes/skeleton/project.json`. Golden images that show the chart change with it.

## Layout

- 292 x 368 pixels, 8-bit sRGB, about 3.5 KB. That is 4 px per cm on the skeleton's work,
  0.73 m wide and 0.92 m high, so the chart has the work's proportions and is not stretched.
- A grid of 4 columns and 6 rows of flat patches, each 57 x 48 px, read row by row from the top,
  left to right. The grid sits on a near-black border 20 px wide, with gaps of 8 px.
- The orientation mark is a right triangle in the top-left corner, legs 40 px: the pixels with
  x + y < 40. It lies wholly in the border, so every patch is whole. Turned over, the mark moves
  to the top right (mirrored left to right) or the bottom left (upside down).
- The patches:
  - rows 1 and 2: a neutral ramp from white, each one stop (half the reflectance) below the
    one before, from 0.90 down to 0.007;
  - rows 3 and 4: primaries and secondaries, which show swapped or dropped channels;
  - rows 5 and 6: colours of things: skin, sky, foliage, sand, brick.
- No channel is 0 or 255, because a base albedo of 0 or 1 is not physical. The 24 patches all
  differ, and the white patch is the brightest, top left; the darkest grey ends row 2.

The linear values are the sRGB transfer function (IEC 61966-2-1) of the 8-bit values. They are
the base albedos the engine sees once the image is imported (M1-14).

| # | Row, column | Name | sRGB (8-bit) | Linear |
| --- | --- | --- | --- | --- |
| 1 | 1, 1 | white | 243, 243, 243 | 0.8963, 0.8963, 0.8963 |
| 2 | 1, 2 | grey 1 | 179, 179, 179 | 0.4508, 0.4508, 0.4508 |
| 3 | 1, 3 | grey 2 | 130, 130, 130 | 0.2232, 0.2232, 0.2232 |
| 4 | 1, 4 | grey 3 | 94, 94, 94 | 0.1119, 0.1119, 0.1119 |
| 5 | 2, 1 | grey 4 | 67, 67, 67 | 0.0561, 0.0561, 0.0561 |
| 6 | 2, 2 | grey 5 | 47, 47, 47 | 0.0284, 0.0284, 0.0284 |
| 7 | 2, 3 | grey 6 | 31, 31, 31 | 0.0137, 0.0137, 0.0137 |
| 8 | 2, 4 | black | 20, 20, 20 | 0.0070, 0.0070, 0.0070 |
| 9 | 3, 1 | red | 190, 40, 40 | 0.5149, 0.0212, 0.0212 |
| 10 | 3, 2 | green | 40, 150, 70 | 0.0212, 0.3050, 0.0612 |
| 11 | 3, 3 | blue | 40, 70, 180 | 0.0212, 0.0612, 0.4564 |
| 12 | 3, 4 | yellow | 230, 200, 40 | 0.7913, 0.5776, 0.0212 |
| 13 | 4, 1 | cyan | 40, 170, 190 | 0.0212, 0.4020, 0.5149 |
| 14 | 4, 2 | magenta | 190, 50, 150 | 0.5149, 0.0319, 0.3050 |
| 15 | 4, 3 | orange | 230, 130, 40 | 0.7913, 0.2232, 0.0212 |
| 16 | 4, 4 | violet | 110, 70, 160 | 0.1559, 0.0612, 0.3515 |
| 17 | 5, 1 | light skin | 220, 170, 140 | 0.7157, 0.4020, 0.2623 |
| 18 | 5, 2 | dark skin | 120, 80, 60 | 0.1878, 0.0802, 0.0452 |
| 19 | 5, 3 | sky | 100, 140, 200 | 0.1274, 0.2623, 0.5776 |
| 20 | 5, 4 | foliage | 90, 120, 50 | 0.1022, 0.1878, 0.0319 |
| 21 | 6, 1 | sand | 210, 190, 140 | 0.6445, 0.5149, 0.2623 |
| 22 | 6, 2 | brick | 160, 70, 50 | 0.3515, 0.0612, 0.0319 |
| 23 | 6, 3 | teal | 30, 120, 120 | 0.0130, 0.1878, 0.1878 |
| 24 | 6, 4 | slate | 80, 90, 130 | 0.0802, 0.1022, 0.2232 |
|  | border and gaps | border | 12, 12, 12 | 0.0037, 0.0037, 0.0037 |
|  | top-left corner | orientation mark | 243, 243, 243 | 0.8963, 0.8963, 0.8963 |

The tests check this table against the script, so the two cannot drift apart.

## Why the script writes its own compressed stream

`--check` compares bytes, and the file's name is its hash, so the PNG must come out the same on
every machine. zlib's compressor does not promise that: zlib-ng, which some Linux distributions
and some Python builds use in its place, writes different streams that decode to the same pixels.
So the script writes its own DEFLATE stream: one block with the fixed Huffman codes of RFC 1951,
and at each byte the longer of two matches, against the previous pixel or the pixel above.
That is all a chart of flat patches needs. zlib is used only for the checksums, and to read the
stream back before the file is written.
