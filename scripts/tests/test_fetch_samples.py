"""Tests for scripts/fetch-samples.py and the manifests of data/samples/paintings/ (M1-50).

Run: python3 -m pytest -q scripts. No test uses the network: downloads are served from local
files by a stand-in opener.
"""

import datetime
import hashlib
import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "fetch-samples.py"
spec = importlib.util.spec_from_file_location("fetch_samples", SCRIPT)
fetch_samples = importlib.util.module_from_spec(spec)
sys.modules["fetch_samples"] = fetch_samples
spec.loader.exec_module(fetch_samples)

FetchError = fetch_samples.FetchError
PAINTINGS = ROOT / "data" / "samples" / "paintings"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Response(io.BytesIO):
    """What an opener returns: the body, its headers and the URL it came from."""

    def __init__(self, body: bytes, url: str, headers: dict | None = None):
        super().__init__(body)
        self.url = url
        self.headers = headers or {}

    def geturl(self) -> str:
        return self.url


class LocalOpener:
    """Serves each URL from a local file, and records what was asked for."""

    def __init__(self, files: dict[str, Path], headers: dict | None = None):
        self.files = files
        self.headers = headers
        self.requests: list[str] = []

    def __call__(self, request, timeout=None):
        url = request.full_url
        self.requests.append(url)
        if url not in self.files:
            raise urllib.error.URLError("no such file in the test")
        return Response(self.files[url].read_bytes(), url, self.headers)


def never_opened(request, timeout=None):
    raise AssertionError(f"downloaded {request.full_url}, which should have been skipped")


class Folders(unittest.TestCase):
    """A manifests folder, an output folder and a folder of local "museum" files."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)
        self.manifests = tmp / "manifests"
        self.out = tmp / "out"
        self.museum = tmp / "museum"
        for folder in (self.manifests, self.museum):
            folder.mkdir()
        self.log: list[str] = []

    def tearDown(self):
        self._tmp.cleanup()

    def museum_file(self, name: str, data: bytes) -> Path:
        path = self.museum / name
        path.write_bytes(data)
        return path

    def manifest(self, sample_id: str, url: str, image_sha256: str, **extra) -> Path:
        path = self.manifests / f"{sample_id}.json"
        record = {"id": sample_id, "image_url": url, "image_sha256": image_sha256, **extra}
        path.write_text(json.dumps(record), encoding="utf-8")
        return path

    def fetch(self, opener, **kwargs) -> int:
        return fetch_samples.fetch(self.manifests, self.out, opener, log=self.log.append, **kwargs)

    def out_names(self) -> list[str]:
        return sorted(p.name for p in self.out.iterdir()) if self.out.exists() else []


class FetchTest(Folders):
    def test_downloads_each_image_under_its_id_with_the_urls_extension(self):
        jpeg = b"\xff\xd8 a JPEG's bytes"
        png = b"\x89PNG a PNG's bytes"
        jpeg_url = "https://museum.example/iiif/abc/full/!4096,4096/0/default.jpg"
        png_url = "https://museum.example/b.png"
        self.manifest("a-sample", jpeg_url, sha256(jpeg))
        self.manifest("b-sample", png_url, sha256(png))
        opener = LocalOpener(
            {
                jpeg_url: self.museum_file("a.jpg", jpeg),
                png_url: self.museum_file("b.png", png),
            }
        )
        self.assertEqual(self.fetch(opener), 2)
        self.assertEqual(self.out_names(), ["a-sample.jpg", "b-sample.png"])
        self.assertEqual((self.out / "a-sample.jpg").read_bytes(), jpeg)
        self.assertEqual((self.out / "b-sample.png").read_bytes(), png)

    def test_skips_an_image_already_there_with_the_right_hash(self):
        data = b"the image"
        self.manifest("a-sample", "https://museum.example/a.jpg", sha256(data))
        self.out.mkdir()
        (self.out / "a-sample.jpg").write_bytes(data)
        self.assertEqual(self.fetch(never_opened), 0)
        self.assertEqual((self.out / "a-sample.jpg").read_bytes(), data)
        self.assertIn("a-sample: already there", self.log)

    def test_downloads_again_an_image_whose_hash_is_wrong(self):
        data = b"the image"
        url = "https://museum.example/a.jpg"
        self.manifest("a-sample", url, sha256(data))
        self.out.mkdir()
        (self.out / "a-sample.jpg").write_bytes(b"a partial or changed copy")
        opener = LocalOpener({url: self.museum_file("a.jpg", data)})
        self.assertEqual(self.fetch(opener), 1)
        self.assertEqual(opener.requests, [url])
        self.assertEqual((self.out / "a-sample.jpg").read_bytes(), data)

    def test_stops_on_a_hash_mismatch_and_writes_nothing(self):
        url = "https://museum.example/a.jpg"
        expected = sha256(b"the image the manifest names")
        self.manifest("a-sample", url, expected)
        opener = LocalOpener({url: self.museum_file("a.jpg", b"another image")})
        with self.assertRaises(FetchError) as caught:
            self.fetch(opener)
        message = str(caught.exception)
        self.assertIn("a-sample", message)
        self.assertIn(expected, message)
        self.assertIn(sha256(b"another image"), message)
        self.assertEqual(self.out_names(), [])  # no image, no temporary file

    def test_stops_at_the_first_failure(self):
        good = b"good"
        url_a = "https://museum.example/a.jpg"
        url_b = "https://museum.example/b.jpg"
        url_c = "https://museum.example/c.jpg"
        self.manifest("a-sample", url_a, sha256(good))
        self.manifest("b-sample", url_b, sha256(b"expected"))
        self.manifest("c-sample", url_c, sha256(good))
        opener = LocalOpener(
            {
                url_a: self.museum_file("a.jpg", good),
                url_b: self.museum_file("b.jpg", b"wrong"),
                url_c: self.museum_file("c.jpg", good),
            }
        )
        with self.assertRaises(FetchError):
            self.fetch(opener)
        self.assertEqual(opener.requests, [url_a, url_b])
        self.assertEqual(self.out_names(), ["a-sample.jpg"])

    def test_a_failed_download_stops_with_a_message(self):
        url = "https://museum.example/missing.jpg"
        self.manifest("a-sample", url, sha256(b"x"))
        with self.assertRaises(FetchError) as caught:
            self.fetch(LocalOpener({}))
        self.assertIn("a-sample", str(caught.exception))
        self.assertIn("download failed", str(caught.exception))
        self.assertEqual(self.out_names(), [])

    def test_an_image_larger_than_the_bound_is_refused(self):
        url = "https://museum.example/a.jpg"
        data = bytes(1001)
        self.manifest("a-sample", url, sha256(data))
        opener = LocalOpener({url: self.museum_file("a.jpg", data)})
        with self.assertRaises(FetchError) as caught:
            self.fetch(opener, max_bytes=1000)
        self.assertIn("1000 bytes", str(caught.exception))
        self.assertEqual(self.out_names(), [])

    def test_an_image_at_the_bound_is_fetched(self):
        url = "https://museum.example/a.jpg"
        data = bytes(1000)
        self.manifest("a-sample", url, sha256(data))
        opener = LocalOpener({url: self.museum_file("a.jpg", data)})
        self.assertEqual(self.fetch(opener, max_bytes=1000), 1)

    def test_a_declared_length_beyond_the_bound_is_refused_before_reading(self):
        url = "https://museum.example/a.jpg"
        data = b"small"
        self.manifest("a-sample", url, sha256(data))
        opener = LocalOpener(
            {url: self.museum_file("a.jpg", data)}, headers={"Content-Length": "5000"}
        )
        with self.assertRaises(FetchError) as caught:
            self.fetch(opener, max_bytes=1000)
        self.assertIn("5000 bytes", str(caught.exception))
        self.assertEqual(self.out_names(), [])

    def test_urls_that_are_not_https_are_refused_before_any_download(self):
        for url in (
            "http://museum.example/a.jpg",
            "file:///etc/passwd.jpg",
            "ftp://museum.example/a.jpg",
            "https:///a.jpg",
        ):
            with self.subTest(url=url):
                # Another sample first, valid: nothing is downloaded at all.
                self.manifest("a-sample", "https://museum.example/a.jpg", sha256(b"a"))
                self.manifest("b-sample", url, sha256(b"b"))
                with self.assertRaises(FetchError) as caught:
                    self.fetch(never_opened)
                self.assertIn("not https", str(caught.exception))
                self.assertEqual(self.out_names(), [])

    def test_an_answer_from_a_url_that_is_not_https_is_refused(self):
        url = "https://museum.example/a.jpg"
        data = b"x"
        self.manifest("a-sample", url, sha256(data))

        def redirected(request, timeout=None):
            return Response(data, "http://elsewhere.example/a.jpg")

        with self.assertRaises(FetchError) as caught:
            self.fetch(redirected)
        self.assertIn("not https", str(caught.exception))
        self.assertEqual(self.out_names(), [])

    def test_the_real_opener_follows_redirects_only_to_https(self):
        handler = fetch_samples.HttpsOnlyRedirects()
        request = fetch_samples.urllib.request.Request("https://museum.example/a.jpg")
        with self.assertRaises(urllib.error.HTTPError):
            handler.redirect_request(request, None, 302, "Found", {}, "http://x.example/a.jpg")
        followed = handler.redirect_request(
            request, None, 302, "Found", {}, "https://cdn.example/a.jpg"
        )
        self.assertEqual(followed.full_url, "https://cdn.example/a.jpg")

    def test_main_reports_a_mismatch_and_exits_1(self):
        url = "https://museum.example/a.jpg"
        self.manifest("a-sample", url, sha256(b"expected"))
        stderr = io.StringIO()
        original = fetch_samples.https_opener
        fetch_samples.https_opener = lambda: LocalOpener(
            {url: self.museum_file("a.jpg", b"other")}
        )
        try:
            old_stderr, sys.stderr = sys.stderr, stderr
            try:
                code = fetch_samples.main(
                    ["--manifests", str(self.manifests), "--out", str(self.out)]
                )
            finally:
                sys.stderr = old_stderr
        finally:
            fetch_samples.https_opener = original
        self.assertEqual(code, 1)
        self.assertIn("a-sample", stderr.getvalue())
        self.assertIn("SHA-256", stderr.getvalue())


class ManifestTest(Folders):
    def refused(self, path: Path) -> str:
        with self.assertRaises(FetchError) as caught:
            fetch_samples.load_manifest(path)
        return str(caught.exception)

    def test_the_id_must_be_the_file_name(self):
        path = self.manifest("a-sample", "https://m.example/a.jpg", sha256(b""))
        path.rename(self.manifests / "other.json")
        self.assertIn("named after its id", self.refused(self.manifests / "other.json"))

    def test_an_id_that_could_climb_out_of_the_folder_is_refused(self):
        path = self.manifests / "x.json"
        for bad in ("../x", "A-Sample", "", "-x", "a" * 65, 7):
            with self.subTest(id=bad):
                record = {"id": bad, "image_url": "https://m.example/a.jpg"}
                record["image_sha256"] = "0" * 64
                path.write_text(json.dumps(record), encoding="utf-8")
                self.assertIn("id must be", self.refused(path))

    def test_the_hash_must_be_64_lowercase_hex_digits(self):
        for bad in ("0" * 63, "A" * 64, None):
            with self.subTest(sha256=bad):
                path = self.manifest("a-sample", "https://m.example/a.jpg", bad)
                self.assertIn("image_sha256", self.refused(path))

    def test_the_url_must_end_in_an_image_extension(self):
        path = self.manifest("a-sample", "https://m.example/a.tif", "0" * 64)
        self.assertIn("does not end in", self.refused(path))

    def test_a_manifest_that_is_too_large_or_not_an_object_is_refused(self):
        path = self.manifests / "a-sample.json"
        path.write_text("[]", encoding="utf-8")
        self.assertIn("not a JSON object", self.refused(path))
        path.write_text("{", encoding="utf-8")
        self.assertIn("not JSON", self.refused(path))
        path.write_text(" " * (fetch_samples.MAX_MANIFEST_BYTES + 1), encoding="utf-8")
        self.assertIn("larger than", self.refused(path))


# ---- The committed samples ---------------------------------------------------------------------

CC0 = "https://creativecommons.org/publicdomain/zero/1.0/"
PDM = "https://creativecommons.org/publicdomain/mark/1.0/"
# ADR 0004's rule 1: the source's own record marks the image CC0 or Public Domain; the raw field
# is stored. These are the raw fields of the sources used, and the licence each one means.
RIGHTS = {
    # Cleveland: share_license_status.
    "share_license_status: CC0": CC0,
    # NGA: published_images.csv's openaccess, "open access image with no usage restrictions under
    # the NGA's open access policy", which releases such images under CC0.
    "openaccess: 1": CC0,
    # SMK: public_domain and rights.
    "public_domain: true; rights: https://creativecommons.org/publicdomain/mark/1.0/": PDM,
}
STRINGS = (
    "id title date institution accession_number credit_line object_url image_url image_sha256 "
    "licence rights retrieved surface dimensions medium"
).split()


def committed_samples() -> list[dict]:
    return [
        json.loads(path.read_text(encoding="utf-8")) for path in sorted(PAINTINGS.glob("*.json"))
    ]


def latest_year(date: str) -> int:
    """The latest year a catalogue date names: "c. 1474/1478" gives 1478."""
    years = [int(y) for y in re.findall(r"\d{4}", date)]
    if not years:
        raise ValueError(f"no year in {date!r}")
    return max(years)


class CommittedSamplesTest(unittest.TestCase):
    """The acceptance criteria on data/samples/paintings/, checked without the network: the hash
    binds each manifest to the image whose pixel size it records."""

    def setUp(self):
        self.samples = committed_samples()

    def test_six_to_eight_manifests_that_the_script_reads(self):
        self.assertGreaterEqual(len(self.samples), 6)
        self.assertLessEqual(len(self.samples), 8)
        loaded = fetch_samples.load_manifests(PAINTINGS)
        self.assertEqual([s.id for s in loaded], [s["id"] for s in self.samples])

    def test_every_field_is_there(self):
        for s in self.samples:
            with self.subTest(sample=s.get("id")):
                for key in STRINGS:
                    self.assertIsInstance(s.get(key), str, key)
                for key in ("id", "title", "date", "institution", "accession_number",
                            "object_url", "dimensions", "medium"):
                    self.assertTrue(s[key].strip(), f"{key} is empty")
                artist = s["artist"]
                self.assertTrue(artist["name"].strip())
                self.assertIsInstance(artist["birth_year"], int)
                self.assertIsInstance(artist["death_year"], int)
                self.assertLess(artist["birth_year"], artist["death_year"])
                for key in ("width_mm", "height_mm", "image_width_px", "image_height_px"):
                    self.assertIsInstance(s[key], (int, float), key)
                    self.assertGreater(s[key], 0, key)
                self.assertTrue(s["object_url"].startswith("https://"))
                datetime.date.fromisoformat(s["retrieved"])

    def test_every_sample_passes_the_rights_rule(self):
        year = datetime.date.today().year
        for s in self.samples:
            with self.subTest(sample=s["id"]):
                # 1. The source's own record marks the image CC0 or Public Domain.
                self.assertIn(s["rights"], RIGHTS)
                self.assertEqual(s["licence"], RIGHTS[s["rights"]])
                # 2. The artist died at least 81 full years ago.
                self.assertLessEqual(s["artist"]["death_year"], year - 81)
                # 3. The work was made at least 96 years ago.
                self.assertLessEqual(latest_year(s["date"]), year - 96)

    def test_at_least_three_museums(self):
        self.assertGreaterEqual(len({s["institution"] for s in self.samples}), 3)

    def test_sizes_from_under_40_cm_to_over_1_5_m(self):
        long_sides_mm = [max(s["width_mm"], s["height_mm"]) for s in self.samples]
        self.assertLess(min(long_sides_mm), 400)
        self.assertGreater(max(long_sides_mm), 1500)

    def test_a_portrait_a_landscape_and_a_near_square_work(self):
        # Near square: the sides within 5% of each other.
        ratios = [s["width_mm"] / s["height_mm"] for s in self.samples]
        self.assertTrue(any(r < 0.95 for r in ratios), "no portrait")
        self.assertTrue(any(r > 1.05 for r in ratios), "no landscape")
        self.assertTrue(any(1 / 1.05 <= r <= 1.05 for r in ratios), "no near-square work")

    def test_each_image_is_at_most_4096_px_and_as_the_work_is_proportioned(self):
        for s in self.samples:
            with self.subTest(sample=s["id"]):
                self.assertLessEqual(max(s["image_width_px"], s["image_height_px"]), 4096)
                image = s["image_width_px"] / s["image_height_px"]
                work = s["width_mm"] / s["height_mm"]
                # ADR 0004's check of the dimensions against the image: within 2%.
                self.assertLessEqual(abs(image / work - 1.0), 0.02)


if __name__ == "__main__":
    unittest.main()
