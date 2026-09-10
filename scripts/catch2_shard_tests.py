#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a Catch2 XML test listing into a filter file --input-file can replay.

Reads `helix-tests --list-tests --reporter xml` on stdin, writes one test name
per line on stdout, escaped so Catch2 parses each line as a single test spec.

Catch2 splits a spec on commas and reads [] as tags and * as a wildcard, so a
name carrying any of those has to arrive backslash-escaped; quoting the name
does not work. The human `--list-tests` output cannot be used for this at all:
it indents tags under each name and wraps long names across lines, so a line
count there is not a test count.
"""

import sys
import xml.etree.ElementTree as ET

# Backslash first, or it would escape the escapes added after it.
_SPECIAL = "\\,[]*"


def escape(name: str) -> str:
    out = []
    for ch in name:
        if ch in _SPECIAL:
            out.append("\\")
        out.append(ch)
    return "".join(out)


def main() -> int:
    raw = sys.stdin.read()
    if not raw.strip():
        return 0
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as exc:
        print(f"catch2_shard_tests: unparseable listing: {exc}", file=sys.stderr)
        return 1
    for name in root.iter("Name"):
        if name.text:
            print(escape(name.text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
