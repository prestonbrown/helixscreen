#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Wall: an LVGL subject is owned by helix::Subject (include/core/subject.h), never held by
# value and never set up through the subject API LVGL 9.6 deprecates. A subject held by
# value is torn down with lv_subject_deinit() and re-initialized in place; helix::Subject
# owns an lv_subject_create() node whose address survives reset() and every re-init.
# docs/devel/THREADING.md § "Subject ownership".
#
# Two shapes are counted, in code only (comments and string literals blanked):
#
#   call      lv_subject_init_{int,float,string,pointer,color,group}, lv_subject_deinit,
#             lv_subject_copy_string
#   storage   an lv_subject_t held by value: each declarator of a variable, member or
#             array declaration (`lv_subject_t a, b;` is two), lv_subject_t or lv_subject_t[]
#             as any template argument (array, vector, map, optional, pair, shared_ptr,
#             unique_ptr, make_unique, ...), new lv_subject_t. lv_subject_t* and
#             lv_subject_t& are not storage.
#
# Any site fails. A line carrying `SUBJECT_API_OK: <reason>`, or directly below a comment
# line carrying it, is skipped: for a test that pins LVGL's own subject API.
#
# Usage:
#   ./scripts/check_lvgl_subject_api.py
#   ./scripts/check_lvgl_subject_api.py --list
#   ./scripts/check_lvgl_subject_api.py --staged-only
#   ./scripts/check_lvgl_subject_api.py --root DIR

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from source_strip import blank_cxx  # noqa: E402
from staged_content import staged_files  # noqa: E402

DIRS = ("src", "include", "tests", "firmware")
SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")
SKIP_PARTS = ("/build/", "/managed_components/")
OPT_OUT = "SUBJECT_API_OK:"

CALL = re.compile(
    r"\b(lv_subject_init_(?:int|float|string|pointer|color|group)|lv_subject_deinit|"
    r"lv_subject_copy_string)\s*\(")
# lv_subject_t by value as a template argument: std::array/vector/deque/list/map/optional/
# pair/shared_ptr/unique_ptr/make_unique and the like, including the T[] forms.
TEMPLATE_ARG = re.compile(r"[<,]\s*(?:const\s+)?lv_subject_t\s*(?:\[\s*\])?\s*(?=[,>])")
NEW_SUBJECT = re.compile(r"\bnew\s+lv_subject_t\b")
# One declarator after the type; the declarator list continues with `, name` entries.
DECLARATOR = r"[A-Za-z_]\w*\s*(?:\[[^\]]*\]\s*)*(?:\{[^}]*\}\s*)?(?:=\s*\{[^}]*\}\s*)?"
DECL = re.compile(r"\blv_subject_t\s+(?!\*|&)(" + DECLARATOR + r"(?:,\s*(?![A-Za-z_]\w*\s+[A-Za-z_*&])" + DECLARATOR + r")*)(?=[;,=)])")


def storage_sites(code: str) -> int:
    """By-value lv_subject_t sites on one line: each declarator, template argument and new."""
    n = len(TEMPLATE_ARG.findall(code)) + len(NEW_SUBJECT.findall(code))
    for m in DECL.finditer(code):
        n += m.group(1).count(",") + 1 - _commas_inside(m.group(1))
    return n


def _commas_inside(declarators: str) -> int:
    """Commas inside [] or {} of a declarator list, which separate no declarators."""
    depth = 0
    inner = 0
    for c in declarators:
        if c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
        elif c == "," and depth > 0:
            inner += 1
    return inner


def count_file(rel: str, text: str) -> tuple[dict[str, int], list[str]]:
    counts: dict[str, int] = {}
    detail: list[str] = []
    raw_lines = text.splitlines()
    code_lines = blank_cxx(text).splitlines()
    prev_ok = False
    for i, raw in enumerate(raw_lines):
        code = code_lines[i] if i < len(code_lines) else ""
        opt_out = OPT_OUT in raw or prev_ok
        stripped = raw.strip()
        prev_ok = stripped.startswith(("//", "*", "/*")) and OPT_OUT in raw
        if opt_out:
            continue
        for shape, n in (("call", len(CALL.findall(code))), ("storage", storage_sites(code))):
            if n:
                counts[rel] = counts.get(rel, 0) + n
                detail.append(f"{rel}:{i + 1}: {shape}\n      {stripped[:110]}")
    return counts, detail


def scan(root: Path, staged: bool) -> tuple[dict[str, int], list[str]]:
    """Every site in the tree, or with STAGED only in the staged blobs.

    Any site fails, so a commit's staged files alone answer for the commit: a file it
    does not stage cannot gain a site through it.
    """
    texts: dict[str, str] = {}
    for rel_dir in DIRS if not staged else ():
        base = root / rel_dir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            rel = path.relative_to(root).as_posix()
            if any(part in f"/{rel}" for part in SKIP_PARTS):
                continue
            if path.is_file() and path.suffix in SUFFIXES:
                texts[rel] = path.read_text(encoding="utf-8", errors="replace")
    if staged:
        for path, text in staged_files(suffixes=SUFFIXES):
            rel = str(path)
            if rel.startswith(tuple(d + "/" for d in DIRS)):
                texts[rel] = text

    counts: dict[str, int] = {}
    detail: list[str] = []
    for rel, text in texts.items():
        if "lv_subject" not in text:  # every shape names it; skip the costly blanking
            continue
        c, d = count_file(rel, text)
        counts.update(c)
        detail.extend(d)
    return counts, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--staged-only", action="store_true",
                        help="Read staged blobs instead of the working tree (pre-commit hook)")
    parser.add_argument("--list", action="store_true", help="print every site found")
    args = parser.parse_args()

    counts, detail = scan(args.root, args.staged_only)

    if args.list:
        for line in detail:
            print(f"  {line}")
        print(f"\n{sum(counts.values())} site(s) in {len(counts)} file(s).")
        return 0

    if counts:
        print("FAIL: an LVGL subject set up through the deprecated API or held by value.\n"
              "      Declare it helix::Subject (include/core/subject.h) and call its\n"
              "      init_*()/reset() members.\n")
        for line in detail:
            print(f"    {line}")
        print(f"\n  A test pinning LVGL's own subject API takes `{OPT_OUT} <reason>` on or\n"
              f"  directly above the line.\n")
        return 1

    print("OK: no deprecated subject call or by-value lv_subject_t.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
