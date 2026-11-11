#!/usr/bin/env python3
"""Check that docs/dependencies.md matches vcpkg.json and allows every licence it lists.

    python3 scripts/check-dependencies.py
    python3 scripts/check-dependencies.py --installed build/debug/vcpkg_installed

Exits 1 and names each problem when:
- a vcpkg.json dependency has no row in the "Libraries from vcpkg" table, or a row there names
  a port that vcpkg.json does not declare;
- a library in that table, in "Pulled in by other ports" or in "Vendored" has a licence (an
  SPDX expression) that the "Allowed licences" table does not allow;
- with --installed (the vcpkg_installed directory of a configured build): an installed port's
  version differs from the documented one, or a port is installed but not documented, or a
  documented pulled-in port is no longer installed. Ports named vcpkg-* only build other ports
  and are not listed.

The test suite runs it on the repository, so CI checks it with the other script tests.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALLOWED = "Allowed licences"
VCPKG = "Libraries from vcpkg"
PULLED_IN = "Pulled in by other ports"
VENDORED = "Vendored"


# ---- SPDX licence expressions -------------------------------------------------
# expr := term ("OR" term)* ; term := atom ("AND" atom)* ; atom := "(" expr ")" | id ["WITH" id]

def licence_allowed(expr: str, allowed: set[str]) -> bool:
    """True when some choice the expression offers uses only allowed licences. An exception
    (WITH) only adds permissions, so the licence it modifies decides. Malformed is False."""
    tokens = re.findall(r"[()]|[^\s()]+", expr)
    pos = 0

    def peek():
        return tokens[pos] if pos < len(tokens) else None

    def take(expected=None):
        nonlocal pos
        token = peek()
        if token is None or (expected and token != expected):
            raise ValueError(expr)
        pos += 1
        return token

    def atom():
        if peek() == "(":
            take("(")
            ok = disjunction()
            take(")")
            return ok
        name = take()
        if name in ("AND", "OR", "WITH", ")"):
            raise ValueError(expr)
        if peek() == "WITH":
            take("WITH")
            if take() in ("AND", "OR", "WITH", "(", ")"):
                raise ValueError(expr)
        return name in allowed

    def conjunction():
        ok = atom()
        while peek() == "AND":
            take("AND")
            ok = atom() and ok
        return ok

    def disjunction():
        ok = conjunction()
        while peek() == "OR":
            take("OR")
            ok = conjunction() or ok
        return ok

    try:
        ok = disjunction()
        return ok and pos == len(tokens)
    except ValueError:
        return False


# ---- Markdown tables ----------------------------------------------------------

def cell_text(cell: str) -> str:
    """The text in backticks if there is any, else the cell without link markup."""
    code = re.findall(r"`([^`]*)`", cell)
    if code:
        return " ".join(code).strip()
    return re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", cell).strip()


def section_tables(doc: str) -> dict[str, list[dict[str, str]]]:
    """The first table under each heading, as rows keyed by column name."""
    tables: dict[str, list[dict[str, str]]] = {}
    heading = None
    in_code = False  # a "# comment" in a fenced code block is not a heading
    lines = doc.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("```"):
            in_code = not in_code
        elif in_code:
            pass
        elif line.startswith("#"):
            heading = line.lstrip("#").strip()
        elif line.startswith("|") and heading is not None and heading not in tables:
            header = [c.strip() for c in line.strip("|").split("|")]
            rows = []
            i += 2  # the separator row
            while i < len(lines) and lines[i].strip().startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                rows.append(dict(zip(header, cells)))
                i += 1
            tables[heading] = rows
            continue
        i += 1
    return tables


# ---- The check ----------------------------------------------------------------

def manifest_dependencies(manifest: dict) -> list[str]:
    return [d if isinstance(d, str) else d["name"] for d in manifest.get("dependencies", [])]


def read_installed(installed_dir: Path) -> dict[str, str]:
    """Port name to version, from vcpkg's <port>_<version>_<triplet>.list files."""
    info = installed_dir / "vcpkg" / "info"
    if not info.is_dir():
        raise FileNotFoundError(f"{info} not found: is this a vcpkg_installed directory?")
    ports = {}
    for path in info.glob("*.list"):
        name, _, rest = path.stem.partition("_")
        version = rest.rpartition("_")[0]
        if name and version:
            ports[name] = version
    return ports


def check(manifest: dict, doc: str, installed: dict[str, str] | None = None) -> list[str]:
    tables = section_tables(doc)
    for heading in (ALLOWED, VCPKG):
        if not tables.get(heading):
            return [f'docs/dependencies.md has no table under the heading "{heading}"']
    allowed = {cell_text(row.get("SPDX", "")) for row in tables[ALLOWED]}
    problems = []

    documented: dict[str, dict[str, str]] = {}  # port name to its row, both port tables
    for heading, key in ((VCPKG, "Port"), (PULLED_IN, "Port"), (VENDORED, "Library")):
        for row in tables.get(heading, []):
            name = cell_text(row.get(key, ""))
            licence = cell_text(row.get("Licence", ""))
            if not licence_allowed(licence, allowed):
                problems.append(f'{name} ("{heading}"): licence "{licence}" is not allowed by '
                                f'the "{ALLOWED}" table')
            if heading == VENDORED:
                continue
            if name in documented:
                problems.append(f"{name} is listed twice")
            documented[name] = dict(row, section=heading)

    declared = manifest_dependencies(manifest)
    direct = {n for n, row in documented.items() if row["section"] == VCPKG}
    for name in declared:
        if name not in direct:
            problems.append(f'{name} is in vcpkg.json but has no row in "{VCPKG}"')
    for name in sorted(direct - set(declared)):
        problems.append(f'{name} has a row in "{VCPKG}" but is not in vcpkg.json')

    if installed is not None:
        ports = {n: v for n, v in installed.items() if not n.startswith("vcpkg-")}
        for name, version in sorted(ports.items()):
            row = documented.get(name)
            if row is None:
                problems.append(f'{name} {version} is installed but has no row in "{VCPKG}" '
                                f'or "{PULLED_IN}"')
            elif cell_text(row.get("Version", "")) != version:
                problems.append(f'{name}: {version} is installed, the table says '
                                f'{cell_text(row.get("Version", ""))}')
        for name, row in sorted(documented.items()):
            if row["section"] == PULLED_IN and name not in ports:
                problems.append(f'{name} is listed in "{PULLED_IN}" but is not installed')
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--manifest", type=Path, default=ROOT / "vcpkg.json")
    parser.add_argument("--doc", type=Path, default=ROOT / "docs" / "dependencies.md")
    parser.add_argument("--installed", type=Path,
                        help="a vcpkg_installed directory, to compare installed versions too")
    args = parser.parse_args(argv)

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    doc = args.doc.read_text(encoding="utf-8")
    installed = read_installed(args.installed) if args.installed else None
    problems = check(manifest, doc, installed)
    for problem in problems:
        print(f"error: {problem}")
    if problems:
        print(f"{len(problems)} problem(s): update docs/dependencies.md (or vcpkg.json) to match")
        return 1
    print(f"ok: {len(manifest_dependencies(manifest))} vcpkg dependencies documented, "
          "every licence allowed" + (", installed versions match" if installed else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
