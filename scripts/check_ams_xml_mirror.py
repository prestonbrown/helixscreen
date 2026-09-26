#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lint gate: AmsState::register_xml_subject_names() must publish exactly the
XML subject names AmsState::init_subjects() publishes (prestonbrown/helixscreen#1439).

init_subjects() registers each name on first init; register_xml_subject_names()
re-publishes the same subjects when a later init_subjects(true) re-enters an
already-initialized singleton (#1374). The two lists are hand-written copies in
src/printer/ams_state.cpp. A name added to the first and forgotten in the second
stays unpublished after a register_xml=false first init, and nothing else fires.

Both function bodies are brace-matched with comments blanked, then every
registration is reduced to a (name, subject) pair:

  * INIT_SUBJECT_*(name, ..., register_xml) - the macro publishes #name for
    &name_ (include/state/subject_macros.h). A literal `false` last argument
    publishes nothing and is skipped.
  * helix::xml::register_subject_in_current_scope("name", &subject)
  * lv_xml_register_subject(nullptr, "name" | CONSTANT, &subject). A named
    constant is compared by its spelling, which both lists share.
  * lv_xml_register_subject(nullptr, name_buf, &subject[i]) - the name is the
    format of the nearest snprintf(name_buf, ...) before it, keyed with the
    bound of the enclosing `for (int i = 0; i < BOUND; ...)` loop, so
    "ams_slot_%d_color" under MAX_SLOTS and under MAX_UNITS differ.

The gate fails closed: a registration call it cannot reduce, a missing
function, or a function yielding no pairs is a failure, never a pass.

Usage:
  ./scripts/check_ams_xml_mirror.py
  ./scripts/check_ams_xml_mirror.py --file /path/to/ams_state.cpp
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFAULT_FILE = "src/printer/ams_state.cpp"
FIRST_INIT = "AmsState::init_subjects"
MIRROR = "AmsState::register_xml_subject_names"

# Anything that can publish a name. Each hit must be reduced by one of the
# shapes below, or the gate fails.
CALL_RE = re.compile(
    r"\b(INIT_SUBJECT_\w+|register_subject_in_current_scope|lv_xml_register_subject)\s*\("
)
SNPRINTF_RE = re.compile(r'snprintf\s*\(\s*name_buf\s*,[^,]*,\s*"([^"]+)"')
FOR_RE = re.compile(r"\bfor\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*(\w+)\s*;")


def blank_comments(src: str) -> str:
    """Replace comments with spaces, keeping string literals and offsets."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i : j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_close(src: str, open_pos: int, open_ch: str = "{", close_ch: str = "}") -> int:
    """Index of the bracket closing the one at open_pos, skipping string literals."""
    depth = 0
    i = open_pos
    while i < len(src):
        c = src[i]
        if c == '"':
            i += 1
            while i < len(src) and src[i] != '"':
                i += 2 if src[i] == "\\" else 1
        elif c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def function_body(src: str, qualified: str) -> tuple[int, int] | None:
    m = re.search(r"\bvoid\s+" + re.escape(qualified) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        return None
    open_pos = m.end() - 1
    close = match_close(src, open_pos)
    return None if close < 0 else (open_pos + 1, close)


def split_args(text: str) -> list[str]:
    args, depth, cur = [], 0, []
    for c in text:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
    args.append("".join(cur).strip())
    return args


def enclosing_loop_bound(body: str, pos: int) -> str | None:
    bound = None
    for m in FOR_RE.finditer(body, 0, pos):
        brace = body.find("{", m.end())
        if brace < 0:
            continue
        close = match_close(body, brace)
        if brace < pos < close:
            bound = m.group(1)  # later matches are more deeply nested
    return bound


def extract(body: str) -> tuple[set[tuple[str, str]], list[str]]:
    """(name, subject) pairs the body publishes, and calls it could not reduce."""
    pairs: set[tuple[str, str]] = set()
    unrecognized: list[str] = []
    for m in CALL_RE.finditer(body):
        close = match_close(body, m.end() - 1, "(", ")")
        call = body[m.start() : close + 1]
        args = split_args(body[m.end() : close])
        what = " ".join(call.split())
        kind = m.group(1)

        if kind.startswith("INIT_SUBJECT_"):
            if len(args) < 3 or not re.fullmatch(r"\w+", args[0]):
                unrecognized.append(what)
            elif args[-1] != "false":
                pairs.add((args[0], f"&{args[0]}_"))
            continue

        if kind == "register_subject_in_current_scope":
            name_arg, subj = (args + ["", ""])[:2]
        else:
            if len(args) != 3 or args[0] != "nullptr":
                unrecognized.append(what)
                continue
            name_arg, subj = args[1], args[2]

        subj = "".join(subj.split())
        if not subj.startswith("&"):
            unrecognized.append(what)
        elif re.fullmatch(r'"[^"]+"', name_arg):
            pairs.add((name_arg.strip('"'), subj))
        elif name_arg == "name_buf":
            fmts = list(SNPRINTF_RE.finditer(body, 0, m.start()))
            bound = enclosing_loop_bound(body, m.start())
            if not fmts or bound is None:
                unrecognized.append(what)
            else:
                pairs.add((f"{fmts[-1].group(1)}[{bound}]", subj))
        elif re.fullmatch(r"[A-Z][A-Z0-9_]*", name_arg):
            pairs.add((name_arg, subj))
        else:
            unrecognized.append(what)
    return pairs, unrecognized


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--file", default=DEFAULT_FILE)
    args = parser.parse_args()

    path = Path(args.file)
    if not path.is_file():
        print(f"❌ {path}: no such file", file=sys.stderr)
        return 2
    src = blank_comments(path.read_text())

    failures: list[str] = []
    found: dict[str, set[tuple[str, str]]] = {}
    for fn in (FIRST_INIT, MIRROR):
        span = function_body(src, fn)
        if span is None:
            failures.append(f"{fn}() not found - nothing to compare")
            continue
        pairs, unrecognized = extract(src[span[0] : span[1]])
        for call in unrecognized:
            failures.append(f"{fn}(): cannot reduce registration `{call}`")
        if not pairs:
            failures.append(f"{fn}(): no XML subject registrations found")
        found[fn] = pairs

    if len(found) == 2 and found[FIRST_INIT] and found[MIRROR]:
        for name, subj in sorted(found[FIRST_INIT] - found[MIRROR]):
            failures.append(f"{name} ({subj}) is published by {FIRST_INIT}() but not {MIRROR}()")
        for name, subj in sorted(found[MIRROR] - found[FIRST_INIT]):
            failures.append(f"{name} ({subj}) is published by {MIRROR}() but not {FIRST_INIT}()")

    if failures:
        print(f"❌ AmsState XML-name mirror ({path}):")
        for f in failures:
            print(f"   {f}")
        return 1
    print(f"✅ AmsState XML-name mirror: {len(found[FIRST_INIT])} registrations match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
