#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lint gate: DisplayManager must cache the display resolution only AFTER the
backend has settled rotation.

The DRM backend may take rotation over itself (a scanout plane advertising
90/270) and clear LVGL's rotation, which un-swaps the resolution. A cache read
taken before set_display_rotation() returns records a value the display no
longer has (prestonbrown/helixscreen#1275, #1587).

Why a lint and not a unit test: apply_rotation's body lives behind
`#ifndef HELIX_DISPLAY_SDL`, and the test binary compiles with
HELIX_DISPLAY_SDL, so the path cannot run headless. The dormant path goes
live the moment a plane owns rotation on i915/amdgpu (x86 targets).

The gate fails closed in both directions:

  * The CENSUS asserts that every function in GUARDED_FUNCTIONS is still
    defined and still performs the number of settle/cache pairs recorded
    there. A gate guarding nothing is a failure, not a pass - and "nothing"
    is what a rename, a deletion, a wrapper around the settle call, or a
    cache write extracted into a helper all leave behind.
  * The ORDER rules pair each cache write with the nearest unconsumed settle
    before it, one settle per cache, so a third wrongly ordered pair in a
    function that already holds two correct ones cannot borrow an earlier
    settle.

Comments in both spellings, string and character literals, and preprocessor
directives are blanked before matching. Prose naming the call therefore
neither satisfies an order nor defeats the exemption, and a brace inside a
format string cannot end a function body early. Bodies are brace-matched
rather than run function-start to function-start, so a helper defined after a
guarded method does not inherit that method's settle call.

A function that caches the resolution and calls no settle at all is the
external-resize catch-up (resize_timer_cb: an Android fold/unfold changes the
resolution out from under us) and has nothing to order. The census is what
keeps that exemption from being a way to launder the guarded sequence out of
the file.

Residual limits, stated rather than papered over: the settle call is matched
by name, so a wrapper hides it from the order rules and only the census
notices; and a pair split across two functions satisfies neither rule, again
leaving the census as the thing that fails.

Usage:
  ./scripts/check_rotation_cache_order.py
  ./scripts/check_rotation_cache_order.py --file /path/to/display_manager.cpp
  ./scripts/check_rotation_cache_order.py --file snippet.cpp --rules-only
"""

from __future__ import annotations

import argparse
import bisect
import re
import sys
from pathlib import Path

DEFAULT_FILE = "src/application/display_manager.cpp"

# Every function that must still perform the guarded sequence, and how many
# settle/cache pairs each one performs. The counts are the gate's proof that
# it is looking at something: a rename, a deletion, a wrapper around the
# settle call and an extracted cache helper each change a number here.
GUARDED_FUNCTIONS = {
    "DisplayManager::init": 1,
    "DisplayManager::apply_rotation": 1,
    "DisplayManager::run_rotation_probe": 2,
}

# The cache write under protection - either half of the pair. Matching runs
# over joined text, so clang-format breaking the line between `m_width =` and
# the getter does not hide the write.
CACHE_WRITE = re.compile(
    r"\bm_(?P<field>width|height)\s*=\s*lv_display_get_(?:horizontal|vertical)_resolution"
)

# The call that must precede it, matched on the literal callee. A wrapper
# renames the call out of the order rules' sight; the census is what then
# reports the pair as lost.
BACKEND_SETTLE = re.compile(r"\bset_display_rotation\s*\(")

# Opts a settle call out of the "no cache read after it" finding, for a
# rotation whose caller genuinely does not want the cache refreshed. An
# ordering violation is the bug this gate exists for and has no marker.
OPT_OUT = re.compile(r"ROTATION_CACHE_OK\s*:")
OPT_OUT_WINDOW = 3

# A block whose declarator names none of these is a function definition.
CONTROL_KEYWORDS = frozenset(
    {"if", "for", "while", "switch", "catch", "else", "do", "return", "try"}
)

# The trailing identifier of a declarator, `Class::method` included.
DECLARATOR_NAME = re.compile(r"([A-Za-z_~][\w~]*(?:::~?\w+)*)\s*$")

# The opening of a raw string, whose delimiter runs to the matching )delim".
RAW_STRING_OPEN = re.compile(r'R"([^()\\ \t]{0,16})\(')


def mask_code(text: str) -> str:
    """Blank comments, literals and preprocessor lines, preserving offsets.

    Every replaced byte becomes a space and every newline survives, so an
    offset into the result is an offset into the source and line numbers are
    unchanged.
    """
    out = list(text)
    n = len(text)

    def blank(start: int, stop: int) -> None:
        for k in range(start, min(stop, n)):
            if out[k] != "\n":
                out[k] = " "

    i = 0
    at_line_start = True
    while i < n:
        ch = text[i]

        if at_line_start and ch == "#":
            # A directive, continuations included: `#if defined(X)` would
            # otherwise read as a declarator, and a multi-line macro can carry
            # braces that do not belong to any function body.
            j = i
            while j < n:
                nl = text.find("\n", j)
                if nl < 0:
                    j = n
                    break
                if text[nl - 1 : nl] == "\\":
                    j = nl + 1
                    continue
                j = nl
                break
            blank(i, j)
            i = j
            continue

        if ch == "/" and text[i + 1 : i + 2] == "/":
            nl = text.find("\n", i)
            nl = n if nl < 0 else nl
            blank(i, nl)
            i = nl
        elif ch == "/" and text[i + 1 : i + 2] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            blank(i, end)
            i = end
        elif (
            ch == "R"
            and text[i + 1 : i + 2] == '"'
            and not (i and (text[i - 1].isalnum() or text[i - 1] == "_"))
            and RAW_STRING_OPEN.match(text, i)
        ):
            m = RAW_STRING_OPEN.match(text, i)
            closer = ")" + m.group(1) + '"'
            end = text.find(closer, m.end())
            end = n if end < 0 else end + len(closer)
            blank(i, end)
            i = end
        elif ch == '"' or (
            ch == "'" and not (i and (text[i - 1].isalnum() or text[i - 1] == "_"))
        ):
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == ch:
                    j += 1
                    break
                if text[j] == "\n":
                    break
                j += 1
            blank(i, j)
            i = min(j, n)
        else:
            i += 1
            at_line_start = ch == "\n" or (at_line_start and ch in " \t")
            continue

        at_line_start = i > 0 and text[i - 1 : i] == "\n"

    return "".join(out)


def declarator_name(decl: str) -> str | None:
    """Name of the function a `{` opens the body of, or None if it opens
    something else (a namespace, an aggregate initialiser, a plain block)."""
    head, paren, _ = decl.partition("(")
    if not paren:
        return None
    match = DECLARATOR_NAME.search(head)
    if not match:
        return None
    name = match.group(1)
    if name.split("::")[-1] in CONTROL_KEYWORDS:
        return None
    return name


def find_function_bodies(masked: str) -> list[tuple[str, int, int]]:
    """(name, body_open_offset, body_close_offset) for every function body.

    Brace-matched, so a body ends where it ends. Blocks opened inside a body
    already being tracked - a lambda, an if, a loop - belong to that body and
    do not start one of their own.
    """
    bodies: list[tuple[str, int, int]] = []
    depth = 0
    fn_depth: int | None = None
    fn_name = ""
    fn_open = 0
    boundary = 0  # just past the last ; { } - where the next declarator starts

    for i, ch in enumerate(masked):
        if ch == "{":
            if fn_depth is None:
                name = declarator_name(masked[boundary:i])
                if name is not None:
                    fn_depth, fn_name, fn_open = depth, name, i
            depth += 1
            boundary = i + 1
        elif ch == "}":
            depth -= 1
            if fn_depth is not None and depth <= fn_depth:
                bodies.append((fn_name, fn_open, i))
                fn_depth = None
            boundary = i + 1
        elif ch == ";":
            boundary = i + 1

    return bodies


def line_index(starts: list[int], offset: int) -> int:
    """1-based line number of a byte offset."""
    return bisect.bisect_right(starts, offset)


def opted_out(lines: list[str], lineno: int) -> bool:
    first = max(0, lineno - 1 - OPT_OUT_WINDOW)
    return any(OPT_OUT.search(line) for line in lines[first:lineno])


def check_source(text: str, source_name: str, census: bool = True) -> list[str]:
    masked = mask_code(text)
    lines = text.splitlines()
    starts = [0] + [m.end() for m in re.finditer(r"\n", masked)]

    failures: list[str] = []
    pairs_by_function: dict[str, int] = {}
    total_pairs = 0

    for name, open_at, close_at in find_function_bodies(masked):
        body = masked[open_at:close_at]
        pairs_by_function.setdefault(name, 0)

        events = sorted(
            [(m.start() + open_at, "settle", "") for m in BACKEND_SETTLE.finditer(body)]
            + [(m.start() + open_at, "cache", m.group("field"))
               for m in CACHE_WRITE.finditer(body)]
        )
        if not any(kind == "settle" for _, kind, _ in events):
            # A cache refresh with no settle in the function is the
            # external-resize catch-up; there is no rotation to order against.
            continue

        # m_width and m_height are one refresh consuming one settle call, so a
        # run of cache writes collapses - but only until a field repeats. The
        # second write of the same field starts a refresh of its own, which is
        # what makes a third, wrongly ordered pair visible in a function that
        # already holds two correct ones.
        collapsed: list[tuple[int, str]] = []
        refresh_fields: set[str] = set()
        for offset, kind, field in events:
            if kind == "settle":
                refresh_fields.clear()
            elif collapsed and collapsed[-1][1] == "cache" and field not in refresh_fields:
                refresh_fields.add(field)
                continue
            else:
                refresh_fields = {field}
            collapsed.append((offset, kind))

        pending: int | None = None
        pairs = 0
        for offset, kind in collapsed:
            if kind == "settle":
                if pending is not None:
                    report = unpaired_settle(lines, starts, source_name, name, pending)
                    if report:
                        failures.append(report)
                pending = offset
            elif pending is None:
                lineno = line_index(starts, offset)
                failures.append(
                    f"{source_name}:{lineno}: resolution cached before "
                    f"set_display_rotation() in {name}()"
                )
            else:
                pairs += 1
                pending = None
        if pending is not None:
            report = unpaired_settle(lines, starts, source_name, name, pending)
            if report:
                failures.append(report)

        pairs_by_function[name] += pairs
        total_pairs += pairs

    if census:
        failures.extend(run_census(source_name, pairs_by_function))

    return failures


def unpaired_settle(
    lines: list[str], starts: list[int], source_name: str, name: str, offset: int
) -> str | None:
    lineno = line_index(starts, offset)
    if opted_out(lines, lineno):
        return None
    return (
        f"{source_name}:{lineno}: set_display_rotation() in {name}() is followed by "
        f"no resolution cache read, so m_width/m_height keep their pre-rotation "
        f"values. Read the resolution after it, or mark the call "
        f"// ROTATION_CACHE_OK: <reason>."
    )


def run_census(source_name: str, pairs_by_function: dict[str, int]) -> list[str]:
    failures: list[str] = []
    for name, expected in GUARDED_FUNCTIONS.items():
        if name not in pairs_by_function:
            failures.append(
                f"{source_name}: {name}() is not defined here, so this gate guards "
                f"nothing. Update GUARDED_FUNCTIONS if the function was renamed or "
                f"the rotation path moved."
            )
        elif pairs_by_function[name] != expected:
            failures.append(
                f"{source_name}: {name}() performs {pairs_by_function[name]} "
                f"set_display_rotation()/cache pair(s), expected {expected}. A pair "
                f"lifted into a helper or routed through a wrapper leaves the order "
                f"unguarded; keep the sequence here, or update GUARDED_FUNCTIONS."
            )
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", default=DEFAULT_FILE,
                    help="display_manager.cpp to check (default: %(default)s)")
    ap.add_argument("--rules-only", action="store_true",
                    help="check ordering only, skipping the census of guarded "
                         "functions (for this gate's own meta-tests)")
    args = ap.parse_args()

    path = Path(args.file)
    if not path.is_file():
        print(f"{args.file}: not found", file=sys.stderr)
        return 2

    failures = check_source(path.read_text(), str(path), census=not args.rules_only)
    if failures:
        print("FAIL: DisplayManager must cache the resolution only after "
              "set_display_rotation() settles.\n")
        for failure in failures:
            print(f"  {failure}")
        print()
        print("The backend may change LVGL's rotation (a plane owning 90/270 "
              "un-swaps the resolution); cache m_width/m_height only after "
              "set_display_rotation() returns.")
        return 1

    if args.rules_only:
        print(f"OK: {path} orders every cache read behind its settle call.")
    else:
        guarded = sum(GUARDED_FUNCTIONS.values())
        print(f"OK: {guarded} guarded set_display_rotation()/cache pair(s) across "
              f"{len(GUARDED_FUNCTIONS)} DisplayManager function(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
