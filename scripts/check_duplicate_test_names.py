#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail on two Catch2 TEST_CASEs that share a name string.

Catch2 runs every case with a given name when a run names it, so a gate that
selects cases by name and counts what ran sees one more case than it chose,
and `--input-file` cannot pick one twin without the other. A name commented
out is not a test, so comments are blanked first (source_strip.blank_cxx).

usage: check_duplicate_test_names.py DIR...
Prints one line per duplicated name, with every file:line that declares it,
and exits 1 when there is one.
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from source_strip import blank_cxx  # noqa: E402

# TEST_CASE("name" ...) and TEST_CASE_METHOD(Fixture, "name" ...). Adjacent
# literals ("a" "b") are one name, as the compiler joins them.
DECL = re.compile(
    r'\b(?:TEST_CASE|SCENARIO)\s*\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)'
    r'|\bTEST_CASE_METHOD\s*\(\s*[^,()]+(?:\([^()]*\))?\s*,\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)'
)
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def names_in(path: Path):
    text = path.read_text(encoding="utf-8", errors="replace")
    code = blank_cxx(text, strings="keep")
    for m in DECL.finditer(code):
        literals = m.group(1) or m.group(2)
        name = "".join(LITERAL.findall(literals))
        line = code.count("\n", 0, m.start()) + 1
        yield name, line


ELSE = re.compile(r"^[ \t]*#[ \t]*(?:else|elif)\b", re.MULTILINE)


def alternatives(where):
    """Two declarations in one file with an #else or #elif between them are the
    branches of one test (a platform stub), and only one is ever compiled."""
    files = {w.rsplit(":", 1)[0] for w in where}
    if len(where) != 2 or len(files) != 1:
        return False
    path = Path(next(iter(files)))
    first, second = sorted(int(w.rsplit(":", 1)[1]) for w in where)
    between = "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[first:second - 1])
    return bool(ELSE.search(between))


def main(argv):
    if len(argv) < 2:
        print("usage: check_duplicate_test_names.py DIR...", file=sys.stderr)
        return 2
    seen = defaultdict(list)
    for root in argv[1:]:
        for path in sorted(Path(root).rglob("*")):
            if path.suffix in (".cpp", ".h") and path.is_file():
                for name, line in names_in(path):
                    seen[name].append(f"{path}:{line}")
    if not seen:
        # An empty scan means the pattern stopped matching, not a clean tree.
        print(f"no TEST_CASE found under {' '.join(argv[1:])}")
        return 1
    dups = {n: where for n, where in seen.items() if len(where) > 1 and not alternatives(where)}
    for name, where in sorted(dups.items()):
        print(f'"{name}": {" ".join(where)}')
    return 1 if dups else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
