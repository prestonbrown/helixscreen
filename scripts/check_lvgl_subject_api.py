#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ratchet: an LVGL subject is owned by helix::Subject (include/core/subject.h), never held
# by value and never set up through the subject API LVGL 9.6 deprecates. A subject held by
# value is torn down with lv_subject_deinit() and re-initialized in place; helix::Subject
# owns an lv_subject_create() node whose address survives reset() and every re-init.
# Plan: docs/devel/plans/2026-11-15-lvgl96-subject-storage.md.
#
# Two shapes are counted, per file, in code only (comments and string literals blanked):
#
#   call      lv_subject_init_{int,float,string,pointer,color,group}, lv_subject_deinit,
#             lv_subject_copy_string
#   storage   an lv_subject_t held by value: each declarator of a variable, member or
#             array declaration (`lv_subject_t a, b;` is two), lv_subject_t or lv_subject_t[]
#             as any template argument (array, vector, map, optional, pair, shared_ptr,
#             unique_ptr, make_unique, ...), new lv_subject_t. lv_subject_t* and
#             lv_subject_t& are not storage.
#
# src/core/subject.cpp is exempt: it holds the transitional overloads. A line carrying
# `SUBJECT_API_OK: <reason>`, or directly below a comment line carrying it, is skipped.
#
# Ratchet, not a wall: per-file counts may fall and never rise, and a file absent from the
# baseline starts at zero. Lower a number by moving the site onto helix::Subject, then
# re-run with --write-baseline.
#
# Usage:
#   ./scripts/check_lvgl_subject_api.py
#   ./scripts/check_lvgl_subject_api.py --list
#   ./scripts/check_lvgl_subject_api.py --write-baseline
#   ./scripts/check_lvgl_subject_api.py --staged-only
#   ./scripts/check_lvgl_subject_api.py --root DIR [--baseline FILE]

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ratchet  # noqa: E402
from source_strip import blank_cxx  # noqa: E402
from staged_content import staged_files  # noqa: E402

DIRS = ("src", "include", "tests", "firmware")
SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")
EXEMPT = (r"^src/core/subject\.cpp$",)
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


def exempt(rel: str) -> bool:
    return any(re.search(p, rel) for p in EXEMPT)


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
    texts: dict[str, str] = {}
    for rel_dir in DIRS:
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
        if exempt(rel):
            continue
        c, d = count_file(rel, text)
        counts.update(c)
        detail.extend(d)
    return counts, detail


def read_baseline(path: Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    out: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        count, _, name = line.partition(" ")
        out[name] = int(count)
    return out


def write_baseline(path: Path, counts: dict[str, int]) -> None:
    body = "".join(f"{counts[k]} {k}\n" for k in sorted(counts))
    path.write_text(
        "# Deprecated LVGL subject calls and by-value lv_subject_t storage, per file.\n"
        "# Ratchet: may fall, never rise. Lower one by moving the site onto helix::Subject\n"
        "# (include/core/subject.h), then re-run scripts/check_lvgl_subject_api.py --write-baseline.\n"
        + body,
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--baseline", type=Path, default=None)
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--staged-only", action="store_true",
                        help="Read staged blobs instead of the working tree (pre-commit hook)")
    parser.add_argument("--list", action="store_true", help="print every site found")
    args = parser.parse_args()

    root = args.root
    baseline_path = args.baseline or (root / "scripts" / "lvgl_subject_api_baseline.txt")
    counts, detail = scan(root, args.staged_only)

    if args.list:
        for line in detail:
            print(f"  {line}")
        print(f"\n{sum(counts.values())} site(s) in {len(counts)} file(s).")
        return 0

    if args.write_baseline:
        write_baseline(baseline_path, counts)
        print(f"Baseline written: {sum(counts.values())} site(s) in {len(counts)} file(s).")
        return 0

    baseline = read_baseline(baseline_path)
    risen = {k: (n, baseline.get(k, 0)) for k, n in counts.items() if n > baseline.get(k, 0)}

    if risen:
        print("FAIL: an LVGL subject set up through the deprecated API or held by value.\n"
              "      Declare it helix::Subject (include/core/subject.h) and call its\n"
              "      init_*()/reset() members.\n")
        for k, (now, was) in sorted(risen.items()):
            print(f"  {k}: {now} site(s), baseline {was}")
        print("\n  Offending sites:")
        for line in detail:
            rel = line.split(":", 1)[0]
            if rel in risen:
                print(f"    {line}")
        print(f"\n  A site that must stay takes `{OPT_OUT} <reason>` on or directly above the line.\n")
        return 1

    fallen = {k: n for k, n in baseline.items() if counts.get(k, 0) < n}
    total = sum(counts.values())
    if fallen:
        return ratchet.under_baseline(
            "LVGL subject API",
            f"{ratchet.shown(baseline_path, root)} (python3 scripts/check_lvgl_subject_api.py --write-baseline)",
            [ratchet.lowered(k, n, counts.get(k, 0)) for k, n in sorted(fallen.items())],
            f"OK: {total} deprecated subject site(s), {len(fallen)} file(s) can be lowered.\n"
            f"    Ratchet down with --write-baseline.")
    print(f"OK: {total} deprecated subject site(s) in {len(counts)} file(s), none new.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
