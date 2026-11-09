#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ratchet: generic code must not branch on which vendor's board or filament system it is
# running on. A vendor name belongs to ONE module per capability; everything else asks
# that module a capability question. See .claude/rules/vendor-abstraction.md.
#
# Five shapes are counted, per file, in src/ and include/:
#
#   platform-macro   a preprocessor conditional naming HELIX_PLATFORM_<AD5M|AD5X|MIPS|K1|K2|
#                    CC1|SNAPMAKER_U1>. Build shapes (_ESP32, _ANDROID, _X86, _PI32) are not
#                    vendors and are not matched.
#   amstype-branch   ==, != or `case` on an AmsType value other than NONE.
#   board-probe      a call to flashforge_machine_id(, ad5x_mod_layout_present(,
#                    flashforge_mod_tree( or a `name()` compare either way round.
#   vendor-name      an identifier containing a vendor, firmware or mod name. Comments and
#                    string literals are blanked first, so text a user reads never counts.
#   vendor-string    a string literal naming one where the program acts on it: compared with
#                    == or !=, or passed to has_macro(, has_object( or a gcode send.
#
# A line carrying `VENDOR_OK: <reason>`, or directly preceded by a comment line carrying
# it, is skipped. The module that owns a capability is exempt for that shape.
#
# Ratchet, not a wall: per-file counts may fall and never rise, and a file absent from the
# baseline starts at zero. Lower a number by removing the branch, then re-run with
# --write-baseline.
#
# Usage:
#   ./scripts/check_vendor_branches.py
#   ./scripts/check_vendor_branches.py --list
#   ./scripts/check_vendor_branches.py --write-baseline
#   ./scripts/check_vendor_branches.py --staged-only
#   ./scripts/check_vendor_branches.py --root DIR [--baseline FILE]

from __future__ import annotations

import argparse
import bisect
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from source_strip import blank_cxx  # noqa: E402
from staged_content import staged_files  # noqa: E402

SCAN_DIRS = ("src", "include")
SCAN_SUFFIXES = (".cpp", ".h")

PLATFORM_MACRO = re.compile(
    r"^\s*#\s*(?:if|ifdef|ifndef|elif|elifdef|elifndef)\b.*"
    r"\bHELIX_PLATFORM_(?:AD5M|AD5X|MIPS|K1|K2|CC1|SNAPMAKER_U1)\b"
)
AMSTYPE_BRANCH = re.compile(
    r"(?:(?:==|!=)\s*(?:[A-Za-z_]\w*::)*AmsType::(?!NONE\b)\w+"
    r"|(?:[A-Za-z_]\w*::)*AmsType::(?!NONE\b)\w+\s*(?:==|!=)"
    r"|\bcase\s+(?:[A-Za-z_]\w*::)*AmsType::(?!NONE\b)\w+)"
)
BOARD_PROBE = re.compile(
    r"\b(?:flashforge_machine_id|ad5x_mod_layout_present|flashforge_mod_tree)\s*\("
    r"|(?:->|\.)\s*name\s*\(\s*\)\s*\)?\s*(?:==|!=)"
    r"|(?:==|!=)\s*\(?\s*[A-Za-z_][\w:]*(?:\s*(?:->|\.)\s*\w+)*\s*(?:->|\.)\s*name\s*\(\s*\)"
)
LINE_COMMENT = re.compile(r"//.*$")

_VENDOR_WORDS = (
    r"zmod|forge_?x|creality|flashforge|snapmaker|qidi|bambu|elegoo|anycubic|sovol|voron|prusa"
    r"|happy_?hare|ad5m|ad5x|kalico|rinkhals|guppy|paxx|ercf|tradrack|centauri"
)
_VENDOR_TEXT = _VENDOR_WORDS + r"|z-mod|forge-x|happy[ -]hare"
# Lowercase patterns matched against lowered text: re.I over the whole tree costs seconds.
VENDOR_FILTER = re.compile(_VENDOR_TEXT)
VENDOR_IDENT = re.compile(_VENDOR_WORDS)
_VENDOR_LITERAL = rf"\"[^\"\n]*(?:{_VENDOR_TEXT})[^\"\n]*\""
VENDOR_STRING = re.compile(
    rf"(?:==|!=)\s*{_VENDOR_LITERAL}"
    rf"|{_VENDOR_LITERAL}\s*(?:==|!=)"
    rf"|\b(?:has_macro|has_object|execute_gcode|send_gcode|gcode_script)\s*\(\s*{_VENDOR_LITERAL}"
)

SHAPES = ("platform-macro", "amstype-branch", "board-probe", "vendor-name", "vendor-string")

# The modules .claude/rules/vendor-abstraction.md names as owning a capability, plus the code
# that exists to imitate or describe vendors. A file named for a vendor is exempt separately.
VENDOR_OWNERS = (
    r"^src/mock/",
    r"_mock",
    r"^include/mock_",
    r"^src/generated/",
    r"^include/toolhead_style\.h$",
    r"^src/rendering/toolhead_art_gen\.cpp$",
    r"^(?:src/printer|include)/ams_backend",
    r"^include/ams_types?\.h$",
    r"^(?:src/system|include)/board_identity\.(?:cpp|h)$",
    r"^(?:src/system|include)/platform_(?:info|table)\.(?:cpp|h)$",
    r"^include/helix_features\.h$",
    r"^(?:src/printer|include)/z_offset_persistence\.(?:cpp|h)$",
    r"^(?:src/printer|include)/filament_macro_profiles\.(?:cpp|h)$",
    r"^(?:src/printer|include)/plr_(?:backend|profile)\.(?:cpp|h)$",
    r"^(?:src/printer|include)/pause_descriptor\.(?:cpp|h)$",
    r"^(?:src/\w+|include)/fault_path_position\.(?:cpp|h)$",
    r"^(?:src/printer|include)/detection_source_table\.(?:cpp|h)$",
    r"^(?:src/\w+|include)/klipper_error_table\.(?:cpp|h)$",
    r"^(?:src/printer|include)/printer_detector\.(?:cpp|h)$",
    r"helix_cache_dir(?:_internal)?\.(?:cpp|h)$",
    r"^src/system/(?:log_collector|diagnostics)\.cpp$",
)

# Owners per shape: the module whose job is the vendor question.
EXEMPT = {
    "platform-macro": (
        r"src/system/platform_table\.cpp$",
        r"src/system/platform_info\.cpp$",
        r"src/system/board_identity\.cpp$",
    ),
    "amstype-branch": (
        r"^src/printer/ams_",
        r"^include/ams_",
        r"_mock",
    ),
    "board-probe": (
        r"^(?:src|include)/(?:system/)?platform_info\.(?:cpp|h)$",
        r"^(?:src/system|include)/board_identity\.(?:cpp|h)$",
        r"src/system/log_collector\.cpp$",
        r"src/system/diagnostics\.cpp$",
        r"src/system/helix_cache_dir\.cpp$",
    ),
    "vendor-name": VENDOR_OWNERS,
    "vendor-string": VENDOR_OWNERS,
}


def exempt(shape: str, rel: str) -> bool:
    if shape in ("vendor-name", "vendor-string") and VENDOR_FILTER.search(
            rel.rsplit("/", 1)[-1].lower()):
        return True
    return any(re.search(p, rel) for p in EXEMPT[shape])


def vendor_hits(rel: str, text: str) -> dict[int, list[str]]:
    """Vendor-mention shapes by 0-based line index, one entry per mention."""
    hits: dict[int, list[str]] = {}
    if exempt("vendor-name", rel) or not VENDOR_FILTER.search(text.lower()):
        return hits
    scans = (("vendor-name", VENDOR_IDENT, blank_cxx(text)),
             ("vendor-string", VENDOR_STRING, blank_cxx(text, strings="keep")))
    for shape, pattern, original in scans:
        source = original.lower()
        newlines = [m.start() for m in re.finditer("\n", source)]
        for match in pattern.finditer(source):
            hits.setdefault(bisect.bisect_left(newlines, match.start()), []).append(shape)
    return hits


def count_text(rel: str, text: str) -> tuple[dict[str, int], list[str]]:
    counts: dict[str, int] = {}
    detail: list[str] = []
    prev_ok = False
    in_pp_continuation = False
    pp_buf = ""
    pp_start = 0
    pp_opt = False
    vendor = vendor_hits(rel, text)
    for lineno, raw in enumerate(text.split("\n"), start=1):
        opt_out = "VENDOR_OK:" in raw or prev_ok
        stripped = raw.strip()
        prev_ok = stripped.startswith(("//", "*", "/*")) and "VENDOR_OK:" in raw

        if not (opt_out or (in_pp_continuation and pp_opt)):
            for shape in vendor.get(lineno - 1, ()):
                key = f"{rel}:{shape}"
                counts[key] = counts.get(key, 0) + 1
                detail.append(f"{rel}:{lineno}: {shape}\n      {raw.strip()[:110]}")

        # A conditional continued over backslash lines is one logical line.
        if in_pp_continuation:
            pp_buf += " " + raw
            if not raw.rstrip().endswith("\\"):
                in_pp_continuation = False
                logical, start = pp_buf, pp_start
                if (not pp_opt and not exempt("platform-macro", rel)
                        and PLATFORM_MACRO.search(logical)):
                    key = f"{rel}:platform-macro"
                    counts[key] = counts.get(key, 0) + 1
                    detail.append(f"{rel}:{start}: platform-macro\n      {logical.strip()[:110]}")
            continue
        if stripped.startswith("#") and raw.rstrip().endswith("\\"):
            in_pp_continuation, pp_buf, pp_start, pp_opt = True, raw, lineno, opt_out
            continue

        hits = []
        if "HELIX_PLATFORM_" in raw and PLATFORM_MACRO.search(raw):
            hits.append("platform-macro")
        # Substring guards: these patterns run over every line of the tree at commit time.
        if "AmsType::" in raw or "name" in raw or "flashforge_" in raw or "ad5x_mod" in raw:
            code = LINE_COMMENT.sub("", raw)
            if AMSTYPE_BRANCH.search(code):
                hits.append("amstype-branch")
            if BOARD_PROBE.search(code):
                hits.append("board-probe")
        for shape in hits:
            if opt_out or exempt(shape, rel):
                continue
            key = f"{rel}:{shape}"
            counts[key] = counts.get(key, 0) + 1
            detail.append(f"{rel}:{lineno}: {shape}\n      {raw.strip()[:110]}")
    return counts, detail


def scan(root: Path, staged: bool) -> tuple[dict[str, int], list[str]]:
    texts: dict[str, str] = {}
    for rel_dir in SCAN_DIRS:
        base = root / rel_dir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SCAN_SUFFIXES:
                texts[path.relative_to(root).as_posix()] = path.read_text(
                    encoding="utf-8", errors="replace")
    if staged:
        # The commit will contain the index blob, not whatever is on disk now.
        for path, text in staged_files(suffixes=SCAN_SUFFIXES):
            rel = str(path)
            if rel.startswith(SCAN_DIRS):
                texts[rel] = text

    counts: dict[str, int] = {}
    detail: list[str] = []
    for rel, text in texts.items():
        c, d = count_text(rel, text)
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
        "# Vendor branches in generic code, per file and shape. Ratchet: may fall, never rise.\n"
        "# Lower one by asking the owning module instead, then re-run with --write-baseline.\n"
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
    parser.add_argument("--list", action="store_true", help="print every branch found")
    args = parser.parse_args()

    root = args.root
    baseline_path = args.baseline or (root / "scripts" / "vendor_branch_baseline.txt")
    counts, detail = scan(root, args.staged_only)

    if args.list:
        for line in detail:
            print(f"  {line}")
        print(f"\n{sum(counts.values())} branch(es) in {len(counts)} file/shape pair(s).")
        return 0

    if args.write_baseline:
        write_baseline(baseline_path, counts)
        print(f"Baseline written: {sum(counts.values())} branch(es) in {len(counts)} pair(s).")
        return 0

    baseline = read_baseline(baseline_path)
    risen = {k: (n, baseline.get(k, 0)) for k, n in counts.items() if n > baseline.get(k, 0)}

    if risen:
        print("FAIL: generic code branches on a vendor. A vendor name belongs to one module\n"
              "      per capability; ask that module a capability question instead.\n")
        for k, (now, was) in sorted(risen.items()):
            print(f"  {k}: {now} branch(es), baseline {was}")
        print("\n  Offending lines:")
        for line in detail:
            head = line.split("\n", 1)[0]
            rel, _, rest = head.partition(":")
            shape = head.rsplit(": ", 1)[-1]
            if f"{rel}:{shape}" in risen:
                print(f"    {line}")
        print("\n  See .claude/rules/vendor-abstraction.md. An unavoidable branch takes\n"
              "  `// VENDOR_OK: <reason>` on or directly above the line.\n")
        return 1

    fallen = {k: n for k, n in baseline.items() if counts.get(k, 0) < n}
    total = sum(counts.values())
    if fallen:
        print(f"OK: {total} vendor branch(es), {len(fallen)} pair(s) can be lowered.\n"
              f"    Ratchet down with --write-baseline.")
    else:
        print(f"OK: {total} vendor branch(es) in generic code, none above baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
