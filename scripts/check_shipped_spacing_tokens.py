#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that every spacing token read from C++ survives release packaging.

`theme_manager_get_spacing("foo")` resolves `foo` from a const that
theme_manager registered by scanning the top level of ui_xml/ for the
`foo_small` / `foo_medium` / `foo_large` triplet. A missing const is not an
error: lv_xml_get_const_silent() returns nullptr, get_spacing returns 0, and
every caller silently takes its hardcoded fallback.

Release packaging is where a token can go missing. `release-copy-xml-config`
in mk/cross.mk stages ui_xml/ and then deletes the DEV_PANEL_XML files, so a
token declared only in a dev-only panel exists in the development tree and on
no shipped device. Nothing warns on either side, and every affected widget
renders at its fallback size on every breakpoint — which on the MEDIUM tier
usually matches the token exactly, so the desktop and the 800x480 panels look
right while the small and large ones do not.

WHAT IS FLAGGED
  A token passed to theme_manager_get_spacing() whose `<px>` declaration —
  the bare name or any of the _small/_medium/_large variants — is declared
  ONLY in files that release packaging deletes. That is a token shipping code
  depends on and shipped XML does not carry.

NOT FLAGGED
  - Tokens with no `<px>` declaration anywhere in ui_xml/. border_radius and
    button_height are registered from C++ tables per breakpoint, never from
    XML; packaging cannot take away what XML never provided.
  - Tokens whose declarations live in shipped files, complete or not. An
    incomplete responsive set is theme_manager_validate_constant_sets()'s job
    and it reports one at startup.
  - Declarations in ui_xml subdirectories: token discovery never reads them,
    which is check_responsive_token_scope.py's rule.

The DEV_PANEL_XML list is read from mk/cross.mk so this gate and packaging
cannot disagree about which files ship.

Exit 0 when every referenced token survives packaging, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

UI_XML_DIR = Path("ui_xml")
CROSS_MK = Path("mk/cross.mk")
SCAN_DIRS = ("src", "include")
SOURCE_SUFFIXES = (".cpp", ".cc", ".h", ".hpp")

# The three tiers theme_manager requires for a complete responsive set. The
# optional tiers (_micro/_tiny/_xlarge/_xxlarge) fall back to these, so losing
# one of them alone cannot strand a token.
REQUIRED_SUFFIXES = ("_small", "_medium", "_large")

CALL_RE = re.compile(r'theme_manager_get_spacing\(\s*"([^"]*)"')

PX_DECL_RE = re.compile(r'<px\b[^>]*?\bname\s*=\s*"([^"]*)"', re.DOTALL)

XML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)

# String literals first so a `//` or `/*` inside one is not read as a comment.
CXX_COMMENT_RE = re.compile(
    r'"(?:\\.|[^"\\])*"' r"|'(?:\\.|[^'\\])*'" r"|/\*.*?\*/" r"|//[^\n]*", re.DOTALL
)

DEV_PANEL_RE = re.compile(r"^DEV_PANEL_XML\s*:?=\s*(.*)$")


def blank_out(match: re.Match) -> str:
    """Replace a comment with its newlines so line numbers stay right."""
    return "\n" * match.group(0).count("\n")


def strip_xml_comments(text: str) -> str:
    return XML_COMMENT_RE.sub(blank_out, text)


def strip_cxx_comments(text: str) -> str:
    def repl(m: re.Match) -> str:
        return blank_out(m) if m.group(0).startswith("/") else m.group(0)

    return CXX_COMMENT_RE.sub(repl, text)


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def read_dev_panel_xml(path: Path) -> set[str]:
    """The XML basenames release packaging deletes, per mk/cross.mk."""
    names: list[str] = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        m = DEV_PANEL_RE.match(line)
        if not m:
            continue
        value = m.group(1)
        # Honour make's backslash line continuations.
        while value.endswith("\\") and i + 1 < len(lines):
            i += 1
            value = value[:-1] + " " + lines[i].strip()
        names += value.split()
        break
    return set(names)


def declaring_files(ui_xml: Path) -> dict[str, set[str]]:
    """px token name -> basenames of the top-level ui_xml files declaring it."""
    out: dict[str, set[str]] = {}
    for path in sorted(ui_xml.glob("*.xml")):
        if not path.is_file():
            continue
        try:
            text = strip_xml_comments(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError):
            continue
        for m in PX_DECL_RE.finditer(text):
            out.setdefault(m.group(1), set()).add(path.name)
    return out


def collect_references(dirs: tuple[str, ...]) -> dict[str, list[tuple[str, int]]]:
    """token -> [(file, line)] for every theme_manager_get_spacing() call."""
    refs: dict[str, list[tuple[str, int]]] = {}
    for d in dirs:
        for path in sorted(Path(d).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            try:
                text = strip_cxx_comments(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError):
                continue
            for m in CALL_RE.finditer(text):
                refs.setdefault(m.group(1), []).append((str(path), line_of(text, m.start())))
    return refs


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--ui-xml", default=None, help=f"directory to scan for declarations (default: {UI_XML_DIR})"
    )
    ap.add_argument(
        "--src",
        action="append",
        default=None,
        help=f"directory to scan for get_spacing() calls, repeatable (default: {', '.join(SCAN_DIRS)})",
    )
    ap.add_argument(
        "--cross-mk", default=None, help=f"makefile holding DEV_PANEL_XML (default: {CROSS_MK})"
    )
    args = ap.parse_args()

    root = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=False
    ).stdout.strip()
    if root:
        import os

        os.chdir(root)

    ui_xml = Path(args.ui_xml) if args.ui_xml else UI_XML_DIR
    cross_mk = Path(args.cross_mk) if args.cross_mk else CROSS_MK
    src_dirs = tuple(args.src) if args.src else SCAN_DIRS

    if not ui_xml.is_dir():
        print(f"error: {ui_xml} is not a directory")
        return 1
    if not cross_mk.is_file():
        print(f"error: {cross_mk} not found — cannot read DEV_PANEL_XML")
        return 1

    dev_panels = read_dev_panel_xml(cross_mk)
    if not dev_panels:
        print(f"error: no DEV_PANEL_XML assignment found in {cross_mk}")
        return 1

    declared = declaring_files(ui_xml)
    refs = collect_references(src_dirs)

    findings: list[tuple[str, str, set[str], list[tuple[str, int]]]] = []
    for token in sorted(refs):
        for name in (token,) + tuple(token + s for s in REQUIRED_SUFFIXES):
            files = declared.get(name)
            if files and files <= dev_panels:
                findings.append((token, name, files, refs[token]))

    if not findings:
        print(
            f"✓ shipped spacing tokens: {len(refs)} token(s) read from C++, "
            "none declared only in dev-panel XML"
        )
        return 0

    print(
        "Spacing tokens read from C++ must be declared in XML that release packaging ships.\n"
    )
    for token, name, files, callers in findings:
        where = ", ".join(sorted(files))
        print(f'  <px name="{name}"> is declared only in {where}, which release packaging deletes.')
        for path, line in callers:
            print(f'    read as theme_manager_get_spacing("{token}") at {path}:{line}')
    print(
        f"\n{len(findings)} token declaration(s) missing from a packaged release.\n"
        "release-copy-xml-config (mk/cross.mk) deletes DEV_PANEL_XML from the staged\n"
        "ui_xml/, so on every shipped device these consts do not exist,\n"
        "theme_manager_get_spacing() returns 0 and each caller falls back to its\n"
        "hardcoded default at every breakpoint.\n"
        "\n"
        "Move the declaration to a shipped top-level ui_xml file — ui_xml/globals.xml\n"
        "for a token a widget reads, or the feature's own *_tokens.xml."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
