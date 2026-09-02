#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: a target-specific flag rule must use `override`.
#
# GNU make discards every makefile assignment to a variable that arrived on the
# command line - target-specific assignments included - unless the assignment
# says `override`. The sanitizer targets re-invoke make with CXXFLAGS and
# LDFLAGS as command-line variables (mk/tests.mk, ASAN_MAKE_OVERRIDES /
# TSAN_MAKE_OVERRIDES), so a plain
#
#     $(OBJ_DIR)/foo.o: CXXFLAGS += -DSOMETHING
#
# compiles foo.o WITHOUT -DSOMETHING under test-asan and test-tsan, silently and
# with no diagnostic. The same line with `override CXXFLAGS +=` survives.
#
# The failure is invisible by construction: the object still builds, the rule is
# still in the makefile, and any gate that greps for the rule's presence stays
# green. Only the compile line tells the truth, which is why this checks the
# form of the assignment rather than that some flag reached some object.
#
# Scope: assignments to a flags variable qualified by a target, in this project's
# own makefiles. A plain global `CXXFLAGS +=` is not affected - the command-line
# rule applies to it too, but the sanitizer sub-makes deliberately pass the
# parent's fully-expanded CXXFLAGS forward, so global flags do arrive.
#
# Usage:
#   ./scripts/check_target_specific_override.py
#   ./scripts/check_target_specific_override.py --repo-root /path/to/checkout

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Makefiles this project owns. Submodule and dependency makefiles are not ours
# to police and do not participate in the sanitizer sub-make.
SCAN = ("Makefile", "mk/*.mk")

FLAG_VARS = ("CXXFLAGS", "CFLAGS", "CPPFLAGS", "LDFLAGS", "ASFLAGS", "OBJCXXFLAGS")

# A target-specific assignment: <targets> : [override] VAR <op> value
#
# Requiring at least one ':' before the variable is what separates this from a
# plain global assignment. A double-colon rule and a `::=` assignment both parse
# here; the target part is captured only for the message.
RULE = re.compile(
    r"^(?P<target>[^#=\n]*?[^:=\s])\s*::?\s*"
    r"(?P<override>override\s+)?"
    r"(?P<var>" + "|".join(FLAG_VARS) + r")\s*"
    r"(?P<op>\+=|:?=|::=|\?=)"
)


def scan_file(path: Path, rel: str) -> list[str]:
    problems: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return problems

    for i, line in enumerate(lines, start=1):
        stripped = line.lstrip()
        # A comment is not a rule, and a recipe line (leading tab) is shell.
        if stripped.startswith("#") or line.startswith("\t"):
            continue
        match = RULE.match(line)
        if not match or match.group("override"):
            continue
        # `?=` never wins against a command-line variable even with override, so
        # flagging it would demand a fix that does not exist. It is also not a
        # form this tree uses for target-specific flags.
        if match.group("op") == "?=":
            continue
        problems.append(
            f"{rel}:{i}: `{match.group('var')}` set for target "
            f"`{match.group('target').strip()}` without `override`.\n"
            f"      {line.strip()[:120]}"
        )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that target-specific flag rules survive a command-line CXXFLAGS."
    )
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    root = args.repo_root

    problems: list[str] = []
    checked = 0
    for pattern in SCAN:
        for path in sorted(root.glob(pattern)):
            if not path.is_file():
                continue
            checked += 1
            problems.extend(scan_file(path, path.relative_to(root).as_posix()))

    if problems:
        print()
        print("FAIL: a target-specific flag rule is missing `override`.\n")
        for problem in problems:
            print(f"  ❌ {problem}")
        print()
        print("  test-asan and test-tsan re-invoke make with CXXFLAGS on the command")
        print("  line, and a command-line variable discards makefile assignments to it")
        print("  unless they say `override`. Without it the flag never reaches the")
        print("  compile under those targets, and nothing reports the loss.\n")
        print("  Fix: insert the keyword after the colon -\n")
        print("        $(OBJ_DIR)/foo.o: override CXXFLAGS += -DSOMETHING\n")
        return 1

    print(f"✓ every target-specific flag rule uses `override` ({checked} makefile(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
