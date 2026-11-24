#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Join soft-wrapped markdown lines so published copies have no mid-sentence breaks.

CHANGELOG.md and tag annotations are hard-wrapped for people editing them in a
terminal. GitHub release bodies and Discord render a single newline inside a
paragraph or list item as a line break, so the wrapped source shows ragged
breaks there. Every publisher pipes the text through here first.

A continuation line (non-blank, not a list item, heading, table row,
blockquote, rule or fence) is joined onto the previous paragraph or list item
with one space. Fenced code blocks, tables, headings, blockquotes, blank lines
and lines ending in a hard break (two trailing spaces or a backslash) are left
alone.

Usage:
  scripts/unwrap-markdown.py [FILE]    # default: stdin; writes stdout
"""

import re
import sys

_FENCE = re.compile(r"^\s*(`{3,}|~{3,})")
_LIST_ITEM = re.compile(r"^\s*([-*+]|\d+[.)])(\s|$)")
_STANDALONE = re.compile(r"^\s*(#{1,6}(\s|$)|\||>|<|([-*_]\s*){3,}$|=+\s*$)")


def unwrap(text: str) -> str:
    out: list[str] = []
    fence: str | None = None  # opening fence marker while inside a code block
    joinable = False  # the last output line is paragraph or list text

    for line in text.split("\n"):
        m = _FENCE.match(line)
        if fence is not None:
            out.append(line)
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) \
                    and line.strip() == m.group(1):
                fence = None
            joinable = False
            continue
        if m:
            fence = m.group(1)
            out.append(line)
            joinable = False
            continue
        if not line.strip():
            out.append(line)
            joinable = False
            continue
        if _LIST_ITEM.match(line):
            out.append(line)
            joinable = not line.endswith(("  ", "\\"))
            continue
        if _STANDALONE.match(line):
            out.append(line)
            joinable = False
            continue
        if joinable:
            out[-1] = out[-1].rstrip() + " " + line.strip()
        else:
            out.append(line)
        joinable = not line.endswith(("  ", "\\"))
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) > 2:
        print(f"usage: {argv[0]} [FILE]", file=sys.stderr)
        return 2
    if len(argv) == 2:
        with open(argv[1], encoding="utf-8") as f:
            text = f.read()
    else:
        text = sys.stdin.read()
    sys.stdout.write(unwrap(text))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
