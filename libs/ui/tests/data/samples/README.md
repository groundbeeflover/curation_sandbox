# Test samples

What `libs/ui/tests/test_sample_paintings.cpp` gives Works > Add sample painting in place of
`data/samples/paintings/` and `build/samples/paintings/`: one folder that holds both the
manifests and the images.

| File | What it is |
| --- | --- |
| `test-sample.json` | A sample 400 x 300 mm, whose image is `test-sample.png` |
| `test-sample.png` | A 4 x 3 px RGB chart of twelve colours, made by the project (Apache-2.0); its SHA-256 is the manifest's `image_sha256` |
| `missing-sample.json` | A sample 300 x 500 mm whose image, `missing-sample.png`, is never committed: its item is disabled |

The URLs are under `example.org` and are never fetched. `test-sample.png` was written with
Python's standard library alone:

```python
import struct, zlib

pixels = [
    [(200, 40, 40), (40, 160, 60), (40, 70, 200), (240, 240, 240)],
    [(230, 200, 60), (128, 128, 128), (60, 200, 210), (190, 60, 170)],
    [(20, 20, 20), (90, 60, 40), (170, 130, 90), (250, 230, 200)],
]

def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 3, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
open("test-sample.png", "wb").write(png)
```
