#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that responsive design tokens are declared at the top level of ui_xml/.

Token auto-discovery is top-level-only. It lists files with
theme_manager_find_xml_files() (src/ui/theme/theme_token_scan.cpp) and recursive=false,
which skips every subdirectory, so only ui_xml/*.xml is ever parsed for `<px name="foo_small">` /
`<string name="foo_small">`. A responsive token declared in ui_xml/components/,
ui_xml/portrait/, ui_xml/micro/ — anywhere below the top level — is never
registered, and every `#foo` that references it silently resolves to nothing.
Neither side warns (prestonbrown/helixscreen#1211).

Recursing was the obvious fix and is the wrong one. Discovery is alphabetical
last-wins, so a portrait-only `nav_width_small` would shadow the base token
globally instead of only while the portrait variant is active. Discovery stays
top-level-only; this gate makes the constraint loud instead of silent.

WHAT IS FLAGGED
  A `<px>` or `<string>` declaration whose name ends in a breakpoint suffix
  (_micro, _tiny, _small, _medium, _large, _xlarge, _xxlarge) in any ui_xml file
  that is not directly in the top level of ui_xml/.

NOT FLAGGED
  - Component-local `<consts>` with no breakpoint suffix. ui_xml/components/
    files declare grid_gap, pin_dot_size, tray_default_height and similar; those
    resolve through the component's own const scope, not through discovery.
  - References. `height="#row_height_small"` from a variant file is exactly what
    variants are for — only the DECLARATION has to sit at the top level.
  - `name=` on anything else: a widget handle or an <api> <prop> is a different
    namespace and never reaches token discovery.

Exit 0 when every responsive token is top-level, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

SCAN_DIRS = ("ui_xml",)

# android/app/src/main/assets/ui_xml/ is a gradle copy of ui_xml/, not a source
# of truth; build/ and .worktrees/ are likewise copies.
SKIP_PARTS = ("android", "build", ".worktrees")

# Mirrors the breakpoint suffixes theme_manager resolves. _small/_medium/_large
# are the core tiers; the rest are optional and fall back.
RESPONSIVE_SUFFIXES = ("_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge")

# <px name="..."> / <string name="..."> — the two element types
# theme_manager_parse_all_xml_for_suffix() reads. Attributes may wrap onto the
# next line, so match across newlines.
DECL_RE = re.compile(r'<(px|string)\b[^>]*?\bname\s*=\s*"([^"]*)"', re.DOTALL)

COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)


def strip_comments(text: str) -> str:
    """Blank out comment bodies, keeping newlines so line numbers stay right."""
    return COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def is_in_subdirectory(path: Path) -> bool:
    """True when the file sits below the top level of a ui_xml/ tree.

    Location is decided by the last `ui_xml` path component, so an explicitly
    passed absolute path (a test fixture, a file from another checkout) is
    judged the same way as `ui_xml/components/foo.xml` from the repo root. A
    path with no ui_xml component at all cannot be placed and is skipped.
    """
    parts = path.parts
    for i in range(len(parts) - 1, -1, -1):
        if parts[i] == "ui_xml":
            return len(parts) - i > 2
    return False


def scan_file(path: Path) -> list[tuple[str, int, str]]:
    """Return (path, line, token) for each responsive token declared here."""
    if not is_in_subdirectory(path):
        return []

    try:
        text = strip_comments(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError):
        return []

    out: list[tuple[str, int, str]] = []
    for m in DECL_RE.finditer(text):
        name = m.group(2)
        if name.endswith(RESPONSIVE_SUFFIXES):
            out.append((str(path), line_of(text, m.start()), f"<{m.group(1)} name=\"{name}\">"))
    return out


def collect_files(args: argparse.Namespace) -> list[Path]:
    if args.files:
        return [Path(f) for f in args.files]

    files: list[Path] = []
    for d in SCAN_DIRS:
        for p in Path(d).rglob("*.xml"):
            if not any(part in SKIP_PARTS for part in p.parts):
                files.append(p)
    return sorted(files)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("files", nargs="*", help=f"XML files to check (default: scan {SCAN_DIRS[0]}/)")
    args = ap.parse_args()

    if not args.files:
        root = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=False
        ).stdout.strip()
        if root:
            import os

            os.chdir(root)

    paths = collect_files(args)

    findings: list[tuple[str, int, str]] = []
    for path in paths:
        findings += scan_file(path)

    if not findings:
        if not args.files:
            print(
                f"✓ responsive tokens: {len(paths)} ui_xml file(s), "
                "none declared in a subdirectory"
            )
        return 0

    print("Responsive design tokens must be declared at the top level of ui_xml/ (#1211).\n")
    for path, line, decl in sorted(findings):
        print(f"  {path}:{line}: {decl} is never registered. Move it to ui_xml/globals.xml.")
    print(
        f"\n{len(findings)} violation(s). theme_manager_find_xml_files() skips subdirectories, so\n"
        "token auto-discovery only ever parses top-level ui_xml/*.xml — a token declared\n"
        "below that is silently missing and every `#token` reading it resolves to nothing.\n"
        "\n"
        "It is top-level-only on purpose: discovery is alphabetical last-wins, so letting a\n"
        "variant directory declare tokens would let a variant token shadow its base token\n"
        "globally rather than only while that variant is active.\n"
        "\n"
        "Declare the whole responsive triplet in ui_xml/globals.xml and reference it with\n"
        "#token from the variant file."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
