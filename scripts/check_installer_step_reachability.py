#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject an installer step function that nothing in the install flow calls.

scripts/lib/installer/*.sh is a set of modules whose functions are wired
together by exactly one orchestrator, main(). A step that is written, reviewed,
unit-tested and then never wired in is silent: the shell defines the function,
never calls it, and exits 0. Nothing in the build, the bundler, or shellcheck
notices, and the bats suites call these functions DIRECTLY, so a full green
suite proves the step works without proving it ever runs.

That is prestonbrown/helixscreen#1343. install_permission_rules() shipped with
34 passing tests and no call site, so the backlight udev rule was never written
to /etc/udev/rules.d, /sys/class/backlight/*/brightness stayed root:root 0644,
and backlight, dimming and sleep failed on every non-root install with
"Cannot write to ... (permission denied?)".

A definition is considered reached when its name appears, outside its own
definition line and outside a comment, in any of the caller files: the installer
modules themselves plus the hand-written entry points that drive them
(install-dev.sh and the two bundlers, whose generated main() bodies live inside
their heredocs). The generated scripts/install.sh and scripts/uninstall.sh are
deliberately NOT scanned - they are output, and reading them would let a stale
bundle vouch for a call site the sources no longer have.

Deliberately uncalled? Annotate the definition:

    # UNCALLED_OK: <reason>
    some_function() {
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_LIB_ROOT = "scripts/lib/installer"

# Hand-written scripts that source the modules and call into them. The two
# bundlers count because bundle-uninstaller.sh writes an entire main() inside a
# heredoc - those calls are real production call sites even though the file that
# runs them is generated.
DEFAULT_CALLERS = (
    "scripts/install-dev.sh",
    "scripts/bundle-installer.sh",
    "scripts/bundle-uninstaller.sh",
)

ANNOTATION = "UNCALLED_OK"

# POSIX-sh function definition at the start of a line: `name() {`. The installer
# modules use no `function` keyword and no indented definitions. The trailing
# brace is required so a bare `name()` inside prose or a case pattern is not
# mistaken for one; one-liner bodies (`_rcd_dir() { echo ...; }`) still match,
# and any call they make on that same line still counts for the callee.
DEF_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\(\)\s*\{")



def find_definitions(lib_files: list[Path]) -> dict[str, list[tuple[Path, int]]]:
    """Map function name -> [(file, 1-based line), ...] for every module."""
    defs: dict[str, list[tuple[Path, int]]] = {}
    for path in lib_files:
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            match = DEF_RE.match(line)
            if match:
                defs.setdefault(match.group(1), []).append((path, lineno))
    return defs


def is_annotated(lineno: int, lines: list[str]) -> bool:
    """True when ANNOTATION is on the definition line or its comment block.

    The block is walked upwards and stops at the first line that is not a
    comment, rather than looking back a fixed number of lines. A fixed window
    reaches over a short neighbouring function and silences the NEXT definition
    too - the fixture in tests/shell/test_installer_step_reachability_gate.bats
    caught exactly that, with a five-line function between the annotation and
    the definition it wrongly excused.
    """
    if ANNOTATION in lines[lineno - 1]:
        return True
    i = lineno - 2  # 0-based index of the line above the definition
    while i >= 0 and lines[i].lstrip().startswith("#"):
        if ANNOTATION in lines[i]:
            return True
        i -= 1
    return False


def find_callers(
    defs: dict[str, list[tuple[Path, int]]], caller_files: list[Path]
) -> dict[str, int]:
    """Count non-definition, non-comment mentions of each function name.

    Comment lines are skipped on purpose: a function whose only remaining
    mention is prose describing it is exactly the dead step this gate exists to
    catch, and three of them were found that way on the first run.
    """
    counts = {name: 0 for name in defs}
    patterns = {
        name: re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])")
        for name in defs
    }
    for path in caller_files:
        try:
            lines = path.read_text().splitlines()
        except (OSError, UnicodeDecodeError):
            continue
        for lineno, line in enumerate(lines, 1):
            stripped = line.lstrip()
            if stripped.startswith("#"):
                continue
            for name, pattern in patterns.items():
                if (path, lineno) in defs[name]:
                    continue
                if pattern.search(line):
                    counts[name] += 1
    return counts


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--lib-root",
        default=DEFAULT_LIB_ROOT,
        help="directory of installer modules whose functions are checked "
        f"(default: {DEFAULT_LIB_ROOT})",
    )
    parser.add_argument(
        "--caller",
        action="append",
        default=None,
        help="extra file that may contain call sites; repeatable. Passing any "
        "--caller REPLACES the default list "
        f"({', '.join(DEFAULT_CALLERS)}).",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="print only the uncalled function names, one per line",
    )
    args = parser.parse_args(argv[1:])

    lib_root = Path(args.lib_root)
    if not lib_root.is_absolute():
        lib_root = REPO_ROOT / lib_root
    if not lib_root.is_dir():
        print(f"❌ installer lib root not found: {lib_root}")
        return 2

    lib_files = sorted(lib_root.glob("*.sh"))
    if not lib_files:
        print(f"❌ no installer modules under {lib_root}")
        return 2

    extra = args.caller if args.caller is not None else list(DEFAULT_CALLERS)
    caller_files = list(lib_files)
    for name in extra:
        path = Path(name)
        if not path.is_absolute():
            path = REPO_ROOT / path
        if path.is_file():
            caller_files.append(path)

    defs = find_definitions(lib_files)
    counts = find_callers(defs, caller_files)

    source_lines = {path: path.read_text().splitlines() for path in lib_files}

    annotated = 0
    uncalled: list[tuple[str, Path, int]] = []
    for name, locations in sorted(defs.items()):
        if counts[name] > 0:
            continue
        path, lineno = locations[0]
        if is_annotated(lineno, source_lines[path]):
            annotated += 1
            continue
        uncalled.append((name, path, lineno))

    if args.list:
        for name, _, _ in uncalled:
            print(name)
        return 1 if uncalled else 0

    if uncalled:
        print("❌ installer step functions with no production call site:")
        for name, path, lineno in uncalled:
            try:
                rel = path.resolve().relative_to(REPO_ROOT)
            except ValueError:
                rel = path
            print(f"   {rel}:{lineno}: {name}()")
        print()
        print("   Nothing in the install or uninstall flow reaches these, so the")
        print("   work they do never happens on a device. Wire each one into the")
        print("   flow (scripts/lib/installer/main.sh or uninstall.sh), or, if it")
        print(f"   is deliberately unreached, annotate it: # {ANNOTATION}: <reason>")
        return 1

    print(
        f"✅ installer step reachability: 0 uncalled functions "
        f"({len(defs)} defined, {annotated} annotated, "
        f"{len(caller_files)} files scanned)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
