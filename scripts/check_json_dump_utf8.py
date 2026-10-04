#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a JSON save path must not serialize with a bare `.dump()`.
#
# nlohmann's default error handler is error_handler_t::strict, so dump() raises
# json::type_error.316 for any string holding bytes UTF-8 cannot decode. The
# strings that reach a save path are the ones nothing validates — SSIDs,
# printer and tool names, file names, macro text, gcode responses — so under
# strict one stray byte costs the entire document. Unguarded, the throw unwinds
# through LVGL's C frames and terminates the process; behind a catch, the write
# is silently skipped and the user's change is gone at next boot.
#
# THE CORRECT FORM
#
#   #include "json_utils.h"
#   ofs << helix::json_util::safe_dump(doc, 2);
#
# safe_dump() passes error_handler_t::replace, which substitutes U+FFFD for the
# offending bytes and writes everything else. A try/catch around the save is
# still worth keeping for what serialization can otherwise throw (bad_alloc on
# a RAM-constrained target); it is not a substitute for this.
#
# WHAT COUNTS AS A VIOLATION
#   Two shapes, both of which move a document out of the process:
#     1. Stream insertion — `ofs << doc.dump(2)`, the file-write idiom.
#     2. An HTTP body — `req->body = batch.dump()`, `std::string body = p.dump()`.
#
# NOT FLAGGED
#   - `.dump()` as a log or error-message argument. spdlog formats it into a
#     line that is thrown away; a save path is where the loss is permanent.
#     Those sites are worth converting too, but a gate that fired on all of
#     them would be noise on every commit.
#   - `safe_dump(...)`, which has no `.dump(` to match.
#   - Tests, which build fixture files from literals they control.
#
# Per-site opt-out — on the line, or on a `//` comment line just above:
#   ofs << flag.dump();  // JSON_DUMP_OK: <reason>
#
# Usage:
#   python3 scripts/check_json_dump_utf8.py              # scan src/ + include/
#   python3 scripts/check_json_dump_utf8.py FILE [FILE…] # scan named files

import re
import sys
from pathlib import Path

OPT_OUT = "JSON_DUMP_OK:"

# `.dump(` never matches `safe_dump(` — the helper has no member-access dot in
# front of it, which is what keeps the correct form out of both patterns.
PATTERNS = [
    (re.compile(r"<<[^;]*?\.dump\s*\("), "serialized into a stream"),
    (re.compile(r"\bbody\s*=\s*[^;]*?\.dump\s*\("), "serialized into an HTTP body"),
]

# `//` opens a comment unless it is the `//` of a URL scheme.
COMMENT = re.compile(r"(?<!:)//.*$")


def scan(path: Path, rel: str) -> list[str]:
    try:
        lines = path.read_text().splitlines()
    except (OSError, UnicodeDecodeError):
        return []

    hits = []
    for i, raw in enumerate(lines, 1):
        code = COMMENT.sub("", raw)
        for pattern, what in PATTERNS:
            if not pattern.search(code):
                continue
            window = "\n".join(lines[max(0, i - 3) : i])
            if OPT_OUT in window:
                break
            hits.append(f"{rel}:{i}: {what}: {raw.strip()}")
            break
    return hits


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent

    if argv[1:]:
        files = [Path(a) for a in argv[1:]]
    else:
        files = [
            p
            for d in ("src", "include")
            for ext in ("*.cpp", "*.h")
            for p in (root / d).rglob(ext)
        ]

    hits = []
    for f in files:
        try:
            rel = str(f.resolve().relative_to(root))
        except ValueError:
            rel = str(f)
        hits += scan(f, rel)

    if hits:
        print("Bare .dump() on a save path — invalid UTF-8 throws here:")
        for h in hits:
            print("  " + h)
        print()
        print('  Use the replacing serializer from #include "json_utils.h":')
        print("    ofs << helix::json_util::safe_dump(doc, 2);")
        print("    req->body = helix::json_util::safe_dump(batch);")
        print(f"  Genuinely needed? Annotate with // {OPT_OUT} <reason>")
        return 1

    print(f"✅ JSON save paths: 0 bare dumps ({len(files)} files scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
