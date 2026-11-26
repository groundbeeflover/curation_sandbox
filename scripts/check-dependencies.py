#!/usr/bin/env python3
"""Check that docs/dependencies.md matches vcpkg.json and allows every licence it lists.

    python3 scripts/check-dependencies.py
    python3 scripts/check-dependencies.py --installed build/debug/vcpkg_installed

Exits 1 and names each problem when:
- a vcpkg.json dependency has no row in the "Libraries from vcpkg" table, or a row there names
  a port that vcpkg.json does not declare;
- a library in that table, in "Pulled in by other ports" or in "Vendored" has a licence (an
  SPDX expression) that the "Allowed licences" table does not allow;
- with --installed (the vcpkg_installed directory of a configured build): the version of an
  installed port differs from the documented one (every copy is compared, one per triplet it
  is installed for), or a port is installed but not documented, or a documented port is not
  installed. Ports named vcpkg-* only build other ports and are not listed.

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
    """The cell without backticks, so `MIT` AND `GPL-3.0-only` is read whole."""
    return cell.replace("`", "").strip()


def table_cells(line: str) -> list[str]:
    """A table row's cells; an escaped pipe (\\|) stays inside its cell."""
    return [c.strip() for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]


def section_tables(doc: str) -> dict[str, list[dict[str, str]]]:
    """The first table under each heading, as rows keyed by column name."""
    tables: dict[str, list[dict[str, str]]] = {}
    heading = None
    fence = None  # inside a fenced code block, a "# comment" is not a heading
    lines = doc.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if fence is None and line.startswith(("```", "~~~")):
            fence = line[:3]
        elif fence is not None:
            if line.startswith(fence):
                fence = None
        elif line.startswith("#"):
            heading = line.lstrip("#").strip()
        elif line.startswith("|") and heading is not None and heading not in tables:
            header = table_cells(line)
            rows = []
            i += 2  # the separator row
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(dict(zip(header, table_cells(lines[i]))))
                i += 1
            tables[heading] = rows
            continue
        i += 1
    return tables


# ---- The check ----------------------------------------------------------------

def manifest_dependencies(manifest: dict) -> list[str]:
    return [d if isinstance(d, str) else d["name"] for d in manifest.get("dependencies", [])]


def read_installed(installed_dir: Path) -> dict[str, dict[str, str]]:
    """Port name to {triplet: version}, from vcpkg's <port>_<version>_<triplet>.list files. A
    port is installed once for each triplet that needs it: for the host when a host tool needs
    it, for the target when the project does."""
    info = installed_dir / "vcpkg" / "info"
    if not info.is_dir():
        raise FileNotFoundError(f"{info} not found: is this a vcpkg_installed directory?")
    ports: dict[str, dict[str, str]] = {}
    for path in info.glob("*.list"):
        name, _, rest = path.stem.partition("_")
        version, _, triplet = rest.rpartition("_")
        if name and version and triplet:
            ports.setdefault(name, {})[triplet] = version
    return ports


def check(manifest: dict, doc: str,
          installed: dict[str, dict[str, str]] | None = None) -> list[str]:
    tables = section_tables(doc)
    for heading in (ALLOWED, VCPKG):
        if not tables.get(heading):
            return [f'docs/dependencies.md has no table under the heading "{heading}"']
    allowed = {cell_text(row.get("SPDX", "")) for row in tables[ALLOWED]}
    problems = []

    for heading, key in ((VCPKG, "Port"), (PULLED_IN, "Port"), (VENDORED, "Library")):
        for row in tables.get(heading, []):
            licence = cell_text(row.get("Licence", ""))
            if not licence_allowed(licence, allowed):
                problems.append(f'{cell_text(row.get(key, ""))} ("{heading}"): licence '
                                f'"{licence}" is not allowed by the "{ALLOWED}" table')

    versions: dict[str, str] = {}  # every documented port, direct or pulled in
    for heading in (VCPKG, PULLED_IN):
        for row in tables.get(heading, []):
            name = cell_text(row.get("Port", ""))
            if name in versions:
                problems.append(f"{name} is listed twice")
            versions[name] = cell_text(row.get("Version", ""))
    direct = {cell_text(row.get("Port", "")) for row in tables[VCPKG]}

    declared = manifest_dependencies(manifest)
    for name in declared:
        if name not in direct:
            problems.append(f'{name} is in vcpkg.json but has no row in "{VCPKG}"')
    for name in sorted(direct - set(declared)):
        problems.append(f'{name} has a row in "{VCPKG}" but is not in vcpkg.json')

    if installed is not None:
        ports = {n: c for n, c in installed.items() if not n.startswith("vcpkg-")}
        for name, copies in sorted(ports.items()):
            if name not in versions:
                found = ", ".join(f"{v} for {t}" for t, v in sorted(copies.items()))
                problems.append(f'{name} {found} is installed but has no row in "{VCPKG}" '
                                f'or "{PULLED_IN}"')
                continue
            for triplet, version in sorted(copies.items()):
                if version != versions[name]:
                    problems.append(f"{name}: {version} is installed for {triplet}, the table "
                                    f"says {versions[name]}")
        for name in sorted(versions.keys() - ports.keys()):
            problems.append(f"{name} is documented but not installed")
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
