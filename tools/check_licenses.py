#!/usr/bin/env python3
"""Dependency licence check for Devil-DJ (task P0-07).

Collects every dependency the build uses and checks each one against the
"Manifest packages" allowlist in docs/LICENSES.md:

  * cmake/Dependencies.cmake  -> names passed to FetchContent_Declare(...)
  * vcpkg.json                -> "dependencies" entries
  * optionally, a vcpkg installed tree (--vcpkg-installed <dir>/<triplet>)
    -> every installed port, so transitive packages are caught too

Fails (exit code 1) when a dependency has no row in the allowlist, when a row's
licence is forbidden (GPL/AGPL/LGPL/non-commercial/...), or when a row's licence
is not in the allowed set. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ALLOWED_LICENCES = {
    "MIT",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "Apache-2.0",
    "BSL-1.0",
    "Zlib",
    "ISC",
    "Unlicense",
    "Public-Domain",
    "MPL-1.1",
    "MPL-2.0",
    "JUCE-EULA",  # approved explicitly in docs/LICENSES.md (decision D6)
}

# Checked before the allowlist so e.g. "LGPL-2.1" or "CC-BY-NC-4.0" can never slip through.
FORBIDDEN_PATTERN = re.compile(r"(^|[^A-Z])(A?GPL|LGPL|SSPL|BUSL|NC|NON-?COMMERCIAL|RESEARCH)([^A-Z]|$)", re.IGNORECASE)

TABLE_HEADING = "### Manifest packages"


@dataclass(frozen=True)
class Dependency:
    name: str
    source: str  # where we found it, for error messages


def normalise(name: str) -> str:
    return name.strip().strip("`").lower()


def parse_allowlist(licenses_md: str) -> dict[str, str]:
    """Returns {package id: licence} from the Manifest packages table."""
    start = licenses_md.find(TABLE_HEADING)
    if start < 0:
        raise ValueError(f"'{TABLE_HEADING}' section not found in LICENSES.md")

    rows: dict[str, str] = {}
    in_table = False
    for line in licenses_md[start + len(TABLE_HEADING):].splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            break  # next section
        if not stripped.startswith("|"):
            if in_table:
                break  # table ended
            continue
        cells = [c.strip() for c in stripped.strip("|").split("|")]
        if not in_table:
            in_table = True  # header row
            continue
        if all(set(c) <= set("-: ") for c in cells):
            continue  # separator row
        if len(cells) < 3:
            raise ValueError(f"malformed allowlist row: {line!r}")
        rows[normalise(cells[0])] = cells[2].strip("`")

    if not rows:
        raise ValueError("Manifest packages table is empty")
    return rows


def parse_fetchcontent(cmake_text: str) -> list[str]:
    # Drop comments so a commented-out declaration doesn't count.
    text = "\n".join(line.split("#", 1)[0] for line in cmake_text.splitlines())
    return [normalise(m) for m in re.findall(r"FetchContent_Declare\s*\(\s*([A-Za-z0-9_.+-]+)", text)]


def parse_vcpkg_manifest(vcpkg_json: str) -> list[str]:
    data = json.loads(vcpkg_json)
    names = []
    for dep in data.get("dependencies", []):
        names.append(normalise(dep if isinstance(dep, str) else dep["name"]))
    return names


def list_installed_ports(installed_triplet_dir: Path) -> list[str]:
    """Ports in a vcpkg installed tree (<vcpkg_installed>/<triplet>); each port has share/<name>/vcpkg.spdx.json."""
    share = installed_triplet_dir / "share"
    if not share.is_dir():
        raise ValueError(f"not a vcpkg installed tree: {installed_triplet_dir}")
    return sorted(normalise(p.parent.name) for p in share.glob("*/vcpkg.spdx.json"))


def check(dependencies: list[Dependency], allowlist: dict[str, str]) -> list[str]:
    errors = []
    for dep in dependencies:
        licence = allowlist.get(dep.name)
        if licence is None:
            errors.append(f"{dep.name} ({dep.source}): not listed in docs/LICENSES.md 'Manifest packages'")
        elif FORBIDDEN_PATTERN.search(licence):
            errors.append(f"{dep.name} ({dep.source}): forbidden licence '{licence}'")
        elif licence not in ALLOWED_LICENCES:
            errors.append(f"{dep.name} ({dep.source}): licence '{licence}' is not in the allowed set")
    return errors


def collect(repo: Path, installed: Path | None) -> list[Dependency]:
    deps: list[Dependency] = []
    deps += [Dependency(n, "cmake/Dependencies.cmake") for n in parse_fetchcontent(
        (repo / "cmake" / "Dependencies.cmake").read_text(encoding="utf-8"))]
    deps += [Dependency(n, "vcpkg.json") for n in parse_vcpkg_manifest(
        (repo / "vcpkg.json").read_text(encoding="utf-8"))]
    if installed is not None:
        deps += [Dependency(n, f"installed in {installed.name}") for n in list_installed_ports(installed)]

    seen: set[str] = set()
    unique = []
    for d in deps:
        if d.name not in seen:
            seen.add(d.name)
            unique.append(d)
    return unique


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: parent of tools/)")
    parser.add_argument("--vcpkg-installed", type=Path, default=None,
                        help="vcpkg installed tree for the target triplet, e.g. build/windows-x64/vcpkg_installed/x64-windows-static")
    args = parser.parse_args(argv)

    try:
        allowlist = parse_allowlist((args.repo / "docs" / "LICENSES.md").read_text(encoding="utf-8"))
        deps = collect(args.repo, args.vcpkg_installed)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as e:
        print(f"check_licenses: error: {e}", file=sys.stderr)
        return 2

    errors = check(deps, allowlist)
    for e in errors:
        print(f"check_licenses: FAIL {e}", file=sys.stderr)
    if errors:
        return 1

    print(f"check_licenses: OK ({len(deps)} dependencies: {', '.join(d.name for d in deps)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
