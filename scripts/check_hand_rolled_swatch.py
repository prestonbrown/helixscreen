#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ratchet: a colour swatch is drawn by the one swatch painter (include/ui_swatch.h) or the
# components built on it (color_swatch, color_dot, swatch_paint), never by hand. A
# hand-rolled circle filled with a filament, LED or tool colour skips the contrast edge,
# the multi-colour split and the finish every other swatch shows.
# See docs/devel/COLOR_SWATCHES.md.
#
# Three shapes are counted, per file:
#
#   xml-bound-circle  an XML element with style_radius="9999" whose bg_color comes from
#                     data: a <bind_style_prop prop="bg_color"> child or a
#                     style_bg_color="$prop".
#   xml-named-swatch  an <lv_obj> with style_radius="9999" named like a swatch or dot
#                     (swatch, color_dot, colour_dot, _dot).
#   cpp-circle-fill   in src/ or include/, a radius of LV_RADIUS_CIRCLE (a draw
#                     descriptor's .radius or lv_obj_set_style_radius) within a few lines of
#                     a bg_color set from a filament, spool, slot, LED, tool or swatch colour.
#
# The painter and the swatch components are exempt. A line carrying `SWATCH_OK: <reason>`,
# or directly preceded by a comment line carrying it, is skipped (in XML, inside a
# comment on the element's opening line or the line above).
#
# Ratchet, not a wall: per-file counts may fall and never rise, and a file absent from the
# baseline starts at zero. Lower a number by moving the site onto the painter, then re-run
# with --write-baseline.
#
# Usage:
#   ./scripts/check_hand_rolled_swatch.py
#   ./scripts/check_hand_rolled_swatch.py --list
#   ./scripts/check_hand_rolled_swatch.py --write-baseline
#   ./scripts/check_hand_rolled_swatch.py --staged-only
#   ./scripts/check_hand_rolled_swatch.py --root DIR [--baseline FILE]

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from staged_content import staged_files  # noqa: E402

CPP_DIRS = ("src", "include")
CPP_SUFFIXES = (".cpp", ".h")
XML_DIRS = ("ui_xml",)
XML_SUFFIXES = (".xml",)

EXEMPT = (
    r"^src/ui/components/ui_swatch\.cpp$",
    r"^include/ui_swatch\.h$",
    r"^ui_xml/components/color_swatch\.xml$",
    r"^ui_xml/components/color_dot\.xml$",
)

SHAPES = ("xml-bound-circle", "xml-named-swatch", "cpp-circle-fill")

CIRCLE_ATTR = re.compile(r'\bstyle_radius="9999"')
OPEN_TAG = re.compile(r"<([A-Za-z_][\w-]*)\b([^<>]*?)(/?)>", re.S)
CLOSE_TAG = re.compile(r"</([A-Za-z_][\w-]*)\s*>")
NAME_ATTR = re.compile(r'\bname="([^"]*)"')
SWATCH_NAME = re.compile(r"swatch|colou?r_dot|_dot(?:_|\b|\$)", re.I)
BOUND_BG_ATTR = re.compile(r'\bstyle_bg_color="\$')
BIND_BG_CHILD = re.compile(r'<bind_style_prop\b[^>]*\bprop="bg_color"')

CPP_RADIUS = re.compile(
    r"\.radius\s*=\s*LV_RADIUS_CIRCLE|lv_obj_set_style_radius\s*\([^;]*LV_RADIUS_CIRCLE")
CPP_FILL = re.compile(
    r"(?:\.bg_color\s*=|lv_obj_set_style_bg_color\s*\()[^;]*"
    r"(?:color_rgb|filament|spool|slot|\bled|led_|tool_color|swatch|preset)", re.I)
CPP_WINDOW = 8
LINE_COMMENT = re.compile(r"//.*$")
XML_COMMENT = re.compile(r"<!--.*?-->", re.S)


def exempt(rel: str) -> bool:
    return any(re.search(p, rel) for p in EXEMPT)


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def xml_opted_out(lines: list[str], lineno: int) -> bool:
    here = lines[lineno - 1] if 0 < lineno <= len(lines) else ""
    above = lines[lineno - 2] if lineno >= 2 else ""
    return "SWATCH_OK:" in here or "SWATCH_OK:" in above


def count_xml(rel: str, text: str) -> tuple[dict[str, int], list[str]]:
    counts: dict[str, int] = {}
    detail: list[str] = []
    lines = text.splitlines()
    # Blank out comments (keeping offsets) so commented-out markup is not counted.
    scan = XML_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)

    # Walk the tags, tracking each open element's bound-bg child.
    stack: list[dict] = []

    def close(elem: dict) -> None:
        if not elem["circle"] or elem["opt_out"]:
            return
        shape = None
        if elem["bound"]:
            shape = "xml-bound-circle"
        elif elem["tag"] == "lv_obj" and SWATCH_NAME.search(elem["name"]):
            shape = "xml-named-swatch"
        if shape:
            key = f"{rel}:{shape}"
            counts[key] = counts.get(key, 0) + 1
            detail.append(f"{rel}:{elem['line']}: {shape}\n      <{elem['tag']} name=\"{elem['name']}\">")

    pos = 0
    while True:
        m_open = OPEN_TAG.search(scan, pos)
        m_close = CLOSE_TAG.search(scan, pos)
        if not m_open and not m_close:
            break
        if m_close and (not m_open or m_close.start() < m_open.start()):
            if stack:
                close(stack.pop())
            pos = m_close.end()
            continue
        tag, attrs, self_closing = m_open.group(1), m_open.group(2), m_open.group(3)
        pos = m_open.end()
        if tag.startswith("?") or tag.startswith("!"):
            continue
        whole = m_open.group(0)
        if stack and BIND_BG_CHILD.search(whole):
            stack[-1]["bound"] = True
        lineno = line_of(scan, m_open.start())
        name = NAME_ATTR.search(attrs)
        elem = {
            "tag": tag,
            "name": name.group(1) if name else "",
            "line": lineno,
            "circle": bool(CIRCLE_ATTR.search(attrs)),
            "bound": bool(BOUND_BG_ATTR.search(attrs)),
            "opt_out": xml_opted_out(lines, lineno),
        }
        if self_closing:
            close(elem)
        else:
            stack.append(elem)
    while stack:
        close(stack.pop())
    return counts, detail


def count_cpp(rel: str, text: str) -> tuple[dict[str, int], list[str]]:
    counts: dict[str, int] = {}
    detail: list[str] = []
    raw_lines = text.splitlines()
    code = [LINE_COMMENT.sub("", ln) for ln in raw_lines]
    fills = [bool(CPP_FILL.search(c)) for c in code]
    prev_ok = False
    for i, raw in enumerate(raw_lines):
        opt_out = "SWATCH_OK:" in raw or prev_ok
        stripped = raw.strip()
        prev_ok = stripped.startswith(("//", "*", "/*")) and "SWATCH_OK:" in raw
        if opt_out or not CPP_RADIUS.search(code[i]):
            continue
        lo, hi = max(0, i - CPP_WINDOW), min(len(code), i + CPP_WINDOW + 1)
        if any(fills[lo:hi]):
            key = f"{rel}:cpp-circle-fill"
            counts[key] = counts.get(key, 0) + 1
            detail.append(f"{rel}:{i + 1}: cpp-circle-fill\n      {stripped[:110]}")
    return counts, detail


def scan(root: Path, staged: bool) -> tuple[dict[str, int], list[str]]:
    texts: dict[str, str] = {}
    for dirs, suffixes in ((CPP_DIRS, CPP_SUFFIXES), (XML_DIRS, XML_SUFFIXES)):
        for rel_dir in dirs:
            base = root / rel_dir
            if not base.is_dir():
                continue
            for path in sorted(base.rglob("*")):
                if path.is_file() and path.suffix in suffixes:
                    texts[path.relative_to(root).as_posix()] = path.read_text(
                        encoding="utf-8", errors="replace")
    if staged:
        # The commit will contain the index blob, not whatever is on disk now.
        for path, text in staged_files(suffixes=CPP_SUFFIXES + XML_SUFFIXES):
            rel = str(path)
            if rel.startswith(CPP_DIRS + XML_DIRS):
                texts[rel] = text

    counts: dict[str, int] = {}
    detail: list[str] = []
    for rel, text in texts.items():
        if exempt(rel):
            continue
        if rel.endswith(XML_SUFFIXES):
            c, d = count_xml(rel, text)
        else:
            c, d = count_cpp(rel, text)
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
        "# Hand-rolled colour swatches, per file and shape. Ratchet: may fall, never rise.\n"
        "# Lower one by moving the site onto the swatch painter, then re-run with\n"
        "# --write-baseline. docs/devel/COLOR_SWATCHES.md.\n" + body,
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
    baseline_path = args.baseline or (root / "scripts" / "hand_rolled_swatch_baseline.txt")
    counts, detail = scan(root, args.staged_only)

    if args.list:
        for line in detail:
            print(f"  {line}")
        print(f"\n{sum(counts.values())} site(s) in {len(counts)} file/shape pair(s).")
        return 0

    if args.write_baseline:
        write_baseline(baseline_path, counts)
        print(f"Baseline written: {sum(counts.values())} site(s) in {len(counts)} pair(s).")
        return 0

    baseline = read_baseline(baseline_path)
    risen = {k: (n, baseline.get(k, 0)) for k, n in counts.items() if n > baseline.get(k, 0)}

    if risen:
        print("FAIL: a colour swatch drawn by hand. Use <color_swatch>, <color_dot> or\n"
              "      <swatch_paint> in XML, apply_swatch_color() or draw_swatch() in C++.\n")
        for k, (now, was) in sorted(risen.items()):
            print(f"  {k}: {now} site(s), baseline {was}")
        print("\n  Offending sites:")
        for line in detail:
            head = line.split("\n", 1)[0]
            rel, _, rest = head.partition(":")
            shape = head.rsplit(": ", 1)[-1]
            if f"{rel}:{shape}" in risen:
                print(f"    {line}")
        print("\n  See docs/devel/COLOR_SWATCHES.md. A circle that is not a colour swatch\n"
              "  takes `SWATCH_OK: <reason>` on or directly above the line.\n")
        return 1

    fallen = {k: n for k, n in baseline.items() if counts.get(k, 0) < n}
    total = sum(counts.values())
    if fallen:
        print(f"OK: {total} hand-rolled swatch site(s), {len(fallen)} pair(s) can be lowered.\n"
              f"    Ratchet down with --write-baseline.")
    else:
        print(f"OK: {total} hand-rolled swatch site(s), none above baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
