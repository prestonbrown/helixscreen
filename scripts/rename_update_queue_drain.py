#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rewrite UpdateQueueTestAccess's drain calls onto UpdateQueue's public methods.

    UpdateQueueTestAccess::drain(q)         -> q.drain()
    UpdateQueueTestAccess::drain_all(q)     -> q.drain_until_empty()
    UpdateQueueTestAccess::drain_all(q, 6)  -> q.drain_until_empty(6)

A file left with no other use of the shim loses its `using` declaration, and its
include becomes core/update_queue.h. A file that still uses the shim without
including it (it arrived through a header this rewrite stops including from) gains
the include. Formatting is left to the commit hook, whose clang-format is pinned.
Idempotent: a branch that still spells the old calls runs this after rebasing:

    scripts/rename_update_queue_drain.py            # every tracked file under tests/
    scripts/rename_update_queue_drain.py PATH...    # just these
    scripts/rename_update_queue_drain.py --check    # exit 1 if anything would change

A reference that is not a call (a function pointer, say) has no rewrite; it is
reported and left alone, and the exit status is 1.
"""

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rename_observers import match_close, skip_literal  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SHIM = "tests/test_helpers/update_queue_test_access.h"
CODE_EXT = {".cpp", ".h"}

CALL_RE = re.compile(r"(?<![\w:])(?:\w+::)*UpdateQueueTestAccess::(drain_all|drain)\s*\(")
LEFTOVER_RE = re.compile(r"UpdateQueueTestAccess::drain(?:_all)?\b")
USING_RE = re.compile(r"^[ \t]*using\s+(?:\w+::)*UpdateQueueTestAccess\s*;[ \t]*\n", re.M)
SHIM_INCLUDE_RE = re.compile(r'^#include\s+"[^"\n]*update_queue_test_access\.h"[ \t]*\n', re.M)
QUEUE_INCLUDE_RE = re.compile(r'^#include\s+"core/update_queue\.h"', re.M)
LAST_INCLUDE_RE = re.compile(r'^#include\s+"[^"\n]*"[ \t]*\n', re.M)
# A receiver that can take `.method()` as written: names, calls, member chains.
POSTFIX_RE = re.compile(r"^[\w:]+(?:\(\))?(?:(?:\.|->)\w+(?:\(\))?)*$")


def split_args(args):
    """Split a call's argument text at its top-level commas."""
    parts, depth, start, i = [], 0, 0, 0
    while i < len(args):
        j = skip_literal(args, i)
        if j != i:
            i = j
            continue
        if args[i] in "([{<":
            depth += 1
        elif args[i] in ")]}>":
            depth -= 1
        elif args[i] == "," and depth == 0:
            parts.append(args[start:i])
            start = i + 1
        i += 1
    parts.append(args[start:])
    return [" ".join(p.split()) for p in parts if p.strip()]


def rewrite_calls(text):
    out, pos = [], 0
    for m in CALL_RE.finditer(text):
        if m.start() < pos:
            continue
        close = match_close(text, m.end() - 1, "(", ")")
        if close < 0:
            continue
        args = split_args(text[m.end() : close])
        if not args:
            continue
        receiver = args[0] if POSTFIX_RE.match(args[0]) else f"({args[0]})"
        method = "drain" if m.group(1) == "drain" else "drain_until_empty"
        out.append(text[pos : m.start()])
        out.append(f"{receiver}.{method}({', '.join(args[1:])})")
        pos = close + 1
    out.append(text[pos:])
    return "".join(out)


def code_only(text):
    """@p text with its comments and string literals removed."""
    out, i = [], 0
    while i < len(text):
        # A quote after a digit or letter is a digit separator or a literal's prefix.
        digit_separator = text[i] == "'" and i > 0 and text[i - 1].isalnum()
        j = i if digit_separator else skip_literal(text, i)
        if j != i:
            i = j
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def rewrite(text, rel):
    new = rewrite_calls(text)
    has_include = SHIM_INCLUDE_RE.search(new) is not None
    if "UpdateQueueTestAccess" in USING_RE.sub("", code_only(new)):
        if has_include:
            return new
        spelled = (
            "update_queue_test_access.h"
            if rel.startswith("tests/test_helpers/")
            else "test_helpers/update_queue_test_access.h"
        )
        last = list(LAST_INCLUDE_RE.finditer(new))
        at = last[-1].end() if last else 0
        return f'{new[:at]}#include "{spelled}"\n{new[at:]}'
    new = USING_RE.sub("", new)
    replacement = "" if QUEUE_INCLUDE_RE.search(new) else '#include "core/update_queue.h"\n'
    return SHIM_INCLUDE_RE.sub(lambda _: replacement, new)


def tracked_files():
    out = subprocess.run(
        ["git", "ls-files", "-z", "tests"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout
    return [f for f in out.split("\0") if f]


def main(argv):
    check = "--check" in argv
    rels = [a for a in argv if not a.startswith("--")] or tracked_files()
    changed, leftover = [], []
    for rel in rels:
        rel = str(Path(rel).resolve().relative_to(ROOT))
        p = ROOT / rel
        if rel == SHIM or p.suffix not in CODE_EXT or p.is_symlink() or not p.is_file():
            continue
        old = p.read_text()
        new = rewrite(old, rel)
        if LEFTOVER_RE.search(new):
            leftover.append(rel)
        if new != old:
            changed.append(rel)
            if not check:
                p.write_text(new)
    for rel in leftover:
        print(f"UpdateQueueTestAccess drain reference with no rewrite: {rel}", file=sys.stderr)
    if check:
        for rel in changed:
            print(f"old drain call: {rel}")
        return 1 if changed or leftover else 0
    print(f"rewrote {len(changed)} files")
    return 1 if leftover else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
