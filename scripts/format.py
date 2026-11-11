#!/usr/bin/env python3
"""Format the C++ sources with clang-format, or check that they are formatted.

    python3 scripts/format.py              format every tracked C++ file in place
    python3 scripts/format.py --check      exit 1 if any file needs formatting (CI)
    python3 scripts/format.py FILE...      format only these files
    python3 scripts/format.py --hook       read a Claude Code PostToolUse event on stdin and
                                           format the edited file if it is C++

clang-format's output changes between major versions, so CI and developers use the same one:
REQUIRED_MAJOR below, installed in CI with `pip install clang-format==<version>` (see
docs/dependencies.md). Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REQUIRED_MAJOR = 23
EXTENSIONS = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".inl"}
EXCLUDED = ("third_party/", "build/", "out/")


def clang_format() -> str:
    exe = shutil.which("clang-format")
    if not exe:
        sys.exit("clang-format not found; install it with "
                 f"`pipx install clang-format=={REQUIRED_MAJOR}.1.4`")
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
    m = re.search(r"version (\d+)\.", out)
    if not m or int(m.group(1)) != REQUIRED_MAJOR:
        sys.exit(f"clang-format {REQUIRED_MAJOR}.x is required, found: {out.strip()}")
    return exe


def is_source(path: str) -> bool:
    rel = path.replace("\\", "/")
    return Path(rel).suffix in EXTENSIONS and not rel.startswith(EXCLUDED)


def tracked_sources() -> list[str]:
    out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"],
                         cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return sorted(p for p in out.splitlines() if is_source(p) and (ROOT / p).exists())


def run(files: list[str], check: bool) -> int:
    if not files:
        return 0
    exe = clang_format()
    args = [exe, "--dry-run", "--Werror"] if check else [exe, "-i"]
    status = 0
    for i in range(0, len(files), 100):  # keep command lines short on Windows
        proc = subprocess.run(args + files[i:i + 100], cwd=ROOT)
        status = status or proc.returncode
    if check and status:
        print("Some files need formatting: run python3 scripts/format.py", file=sys.stderr)
    return 1 if status else 0


def hook() -> int:
    """PostToolUse hook: format the file an Edit or Write just changed. Never blocks the edit."""
    try:
        event = json.load(sys.stdin)
        path = event.get("tool_input", {}).get("file_path", "")
    except (json.JSONDecodeError, AttributeError):
        return 0
    if not path:
        return 0
    try:
        rel = str(Path(path).resolve().relative_to(ROOT))
    except ValueError:
        return 0  # outside the repository
    if not is_source(rel) or not (ROOT / rel).exists():
        return 0
    try:
        run([rel], check=False)
    except SystemExit as err:  # clang-format missing or the wrong version: say so, do not block
        print(f"format hook: {err}", file=sys.stderr)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("files", nargs="*")
    p.add_argument("--check", action="store_true")
    p.add_argument("--hook", action="store_true")
    args = p.parse_args()
    if args.hook:
        return hook()
    files = [f for f in args.files if is_source(f)] if args.files else tracked_sources()
    return run(files, args.check)


if __name__ == "__main__":
    sys.exit(main())
