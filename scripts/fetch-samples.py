#!/usr/bin/env python3
"""Download the sample paintings' images, checking each against its manifest's SHA-256.

    python3 scripts/fetch-samples.py                 every image into build/samples/paintings/
    python3 scripts/fetch-samples.py --out DIR       into another folder

Each data/samples/paintings/<id>.json (M1-50) names a museum's open-access image by its URL and
its SHA-256. The image is saved as build/samples/paintings/<id>.<extension of the URL>, where the
app's Works > Add sample painting looks for it. Images are never committed, and the app never
loads them from museum servers (docs/adr/0004-catalogue.md); this script is how they arrive.

An image already there with the right hash is skipped; one with the wrong hash is downloaded
again. The script stops, with a message saying which sample and why, on a manifest it cannot
use, a URL that is not https (redirects included), a failed download, an image larger than
MAX_IMAGE_BYTES, or an image whose hash is not the manifest's; nothing is written for that image.
A download is written to a temporary file and renamed once its hash is checked. Standard library
only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
MANIFESTS = ROOT / "data" / "samples" / "paintings"
OUT = ROOT / "build" / "samples" / "paintings"

# Bounds on what is read: manifests come from the repository, images from museum servers.
MAX_MANIFEST_BYTES = 64 * 1024
MAX_SAMPLES = 64
# A JPEG of at most 4096 px on its long side is a few MB; this leaves room and stops a runaway.
MAX_IMAGE_BYTES = 64 * 1024 * 1024
TIMEOUT_S = 60
CHUNK_BYTES = 1024 * 1024
# A generic name: the request says nothing about who runs it.
USER_AGENT = "fetch-samples/1 (Python urllib)"

ID_PATTERN = re.compile(r"[a-z0-9][a-z0-9-]{0,63}")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
EXTENSIONS = (".jpg", ".jpeg", ".png")


class FetchError(Exception):
    """A sample that cannot be fetched; the message says which and why."""


@dataclass(frozen=True)
class Sample:
    id: str
    image_url: str
    image_sha256: str

    @property
    def image_name(self) -> str:
        """<id>.<extension of the URL's path>, as the app looks for it."""
        path = urllib.parse.urlsplit(self.image_url).path
        return self.id + path[path.rfind(".") :]


def https_url(url: str) -> bool:
    parts = urllib.parse.urlsplit(url)
    return parts.scheme == "https" and bool(parts.hostname)


def load_manifest(path: Path) -> Sample:
    """The sample a manifest describes; FetchError when it cannot be used."""
    name = path.name
    if path.stat().st_size > MAX_MANIFEST_BYTES:
        raise FetchError(f"{name}: larger than {MAX_MANIFEST_BYTES} bytes")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise FetchError(f"{name}: not JSON in UTF-8: {e}") from None
    if not isinstance(data, dict):
        raise FetchError(f"{name}: not a JSON object")
    sample_id, url, sha256 = data.get("id"), data.get("image_url"), data.get("image_sha256")
    if not isinstance(sample_id, str) or not ID_PATTERN.fullmatch(sample_id):
        raise FetchError(f"{name}: id must be 1 to 64 of a-z, 0-9 and '-', not {sample_id!r}")
    if path.stem != sample_id:
        raise FetchError(f"{name}: the file is named after its id, {sample_id!r}")
    if not isinstance(url, str) or not https_url(url):
        raise FetchError(f"{name}: the image URL is not https: {url!r}")
    if not urllib.parse.urlsplit(url).path.endswith(EXTENSIONS):
        raise FetchError(f"{name}: the image URL does not end in {', '.join(EXTENSIONS)}: {url}")
    if not isinstance(sha256, str) or not SHA256_PATTERN.fullmatch(sha256):
        raise FetchError(f"{name}: image_sha256 is not 64 lowercase hexadecimal digits")
    return Sample(sample_id, url, sha256)


def load_manifests(folder: Path) -> list[Sample]:
    paths = sorted(folder.glob("*.json"))
    if not paths:
        raise FetchError(f"no manifests (*.json) in {folder}")
    if len(paths) > MAX_SAMPLES:
        raise FetchError(f"{len(paths)} manifests in {folder}; at most {MAX_SAMPLES} are read")
    return [load_manifest(path) for path in paths]


class HttpsOnlyRedirects(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only to another https URL."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not https_url(newurl):
            raise urllib.error.HTTPError(
                newurl, code, f"redirected to a URL that is not https: {newurl}", headers, fp
            )
        return super().redirect_request(req, fp, code, msg, headers, newurl)


# Opens a request and returns a response: read(n), headers.get(name), geturl(), and a context
# manager. Tests pass their own, serving local files.
Opener = Callable[..., object]


def https_opener() -> Opener:
    # Only https: the default opener's http:, file:, ftp: and data: handlers are not installed, so
    # any other URL is refused as an unknown type.
    opener = urllib.request.OpenerDirector()
    for handler in (
        urllib.request.UnknownHandler(),
        urllib.request.HTTPSHandler(),
        HttpsOnlyRedirects(),
        urllib.request.HTTPErrorProcessor(),
        urllib.request.HTTPDefaultErrorHandler(),
    ):
        opener.add_handler(handler)
    return opener.open


def sha256_of(path: Path) -> str:
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def download(sample: Sample, destination: Path, opener: Opener, max_bytes: int) -> int:
    """Downloads the sample's image to destination; returns its size in bytes."""
    request = urllib.request.Request(sample.image_url, headers={"User-Agent": USER_AGENT})
    where = f"{sample.id}: {sample.image_url}"
    digest = hashlib.sha256()
    size = 0
    # Written beside the image and renamed once whole and checked. Made with the umask's
    # permissions, as any other file the user writes; never through a link left at its name.
    part = destination.with_name(f".{destination.name}.{os.getpid()}.part")
    part.unlink(missing_ok=True)
    try:
        try:
            out = part.open("xb")  # exclusive: fails on anything left at the name, links too
        except OSError as e:
            raise FetchError(f"{sample.id}: cannot write {part}: {e}") from None
        with out:
            try:
                with opener(request, timeout=TIMEOUT_S) as response:
                    if not https_url(response.geturl()):
                        raise FetchError(f"{where}: answered from a URL that is not https")
                    declared = response.headers.get("Content-Length")
                    if declared is not None and declared.isdigit() and int(declared) > max_bytes:
                        raise FetchError(
                            f"{where}: {declared} bytes, more than the {max_bytes} allowed"
                        )
                    while chunk := response.read(min(CHUNK_BYTES, max_bytes + 1 - size)):
                        size += len(chunk)
                        if size > max_bytes:
                            raise FetchError(f"{where}: more than the {max_bytes} bytes allowed")
                        digest.update(chunk)
                        out.write(chunk)
            except (urllib.error.URLError, OSError, ValueError) as e:
                raise FetchError(f"{where}: the download failed: {e}") from None
        actual = digest.hexdigest()
        if actual != sample.image_sha256:
            raise FetchError(
                f"{where}: the image's SHA-256 is {actual}, the manifest's is "
                f"{sample.image_sha256}. The museum may have changed the image; check it, and "
                f"update the manifest only on purpose"
            )
        try:
            os.replace(part, destination)
        except OSError as e:
            raise FetchError(f"{sample.id}: cannot write {destination}: {e}") from None
    finally:
        part.unlink(missing_ok=True)
    return size


def fetch(
    manifests: Path = MANIFESTS,
    out: Path = OUT,
    opener: Opener | None = None,
    max_bytes: int = MAX_IMAGE_BYTES,
    log: Callable[[str], None] = print,
) -> int:
    """Fetches every sample's image into out; returns how many were downloaded."""
    samples = load_manifests(manifests)  # every URL is checked before anything is downloaded
    opener = opener or https_opener()
    out.mkdir(parents=True, exist_ok=True)
    downloaded = 0
    for sample in samples:
        destination = out / sample.image_name
        if destination.is_file():
            if sha256_of(destination) == sample.image_sha256:
                log(f"{sample.id}: already there")
                continue
            log(f"{sample.id}: {destination.name} does not match its hash; downloading it again")
        size = download(sample, destination, opener, max_bytes)
        downloaded += 1
        log(f"{sample.id}: downloaded, {size} bytes")
    return downloaded


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--manifests", type=Path, default=MANIFESTS, help=argparse.SUPPRESS)
    parser.add_argument("--out", type=Path, default=OUT, help="where the images go")
    args = parser.parse_args(argv)
    try:
        count = fetch(args.manifests, args.out)
    except FetchError as e:
        print(f"fetch-samples: {e}", file=sys.stderr)
        return 1
    print(f"fetch-samples: {count} downloaded; the images are in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
