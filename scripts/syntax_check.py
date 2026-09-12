#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile one file the way the build compiles it, without building anything.

`make test` re-links the whole test binary, so asking "does this file compile"
costs minutes. This pulls the file's own flags out of compile_commands.json and
runs the compiler with -fsyntax-only, which answers the same question in a few
seconds. It proves nothing about behaviour: run the suite for that.

    scripts/syntax_check.py tests/unit/test_wifi_backend_netd.cpp
    scripts/syntax_check.py src/ui/*.cpp

A file with no entry in the database (a file the build has never seen, which is
every brand-new source) borrows the flags of a sibling in the same directory,
and says so. A file no native build compiles at all - firmware/, android/ - is
reported as skipped rather than failed: there are no flags to borrow, and the
cross build in CI is what gates it.
"""

import json
import os
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from merge_compile_commands import entry_argv  # noqa: E402

DB_NAME = "compile_commands.json"


def flags_for(entry: dict) -> list[str]:
    argv = entry_argv(entry)
    out: list[str] = []
    skip = False
    for arg in argv:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if arg.endswith((".cpp", ".cc", ".cxx", ".mm", ".c")):
            continue
        out.append(arg)
    return out


def leading_guard(path: str) -> str | None:
    """The macro a whole file hangs on, or None.

    A file whose body sits inside `#ifdef HELIX_DISPLAY_DRM` preprocesses to
    nothing when that macro is off, and an empty translation unit compiles
    clean no matter what the file says. Finding the guard is what lets the
    caller compile the code instead of the void.
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError:
        return None
    guard = None
    for line in lines[:30]:
        parts = line.split()
        if len(parts) >= 2 and parts[0] in ("#ifdef", "#if") and parts[1].startswith(
            ("HELIX_", "ENABLE_")
        ):
            guard = parts[1]
            break
    if guard is None:
        return None
    last_endif = max(
        (i for i, ln in enumerate(lines) if ln.startswith("#endif")), default=None
    )
    if last_endif is None or len(lines) - last_endif > 4:
        return None
    return guard


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    root = pathlib.Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=False
        ).stdout.strip()
        or "."
    )
    db_path = root / DB_NAME
    if not db_path.exists():
        print(f"{DB_NAME} not found: run 'make compile_commands' (or a build) first", file=sys.stderr)
        return 2

    db = json.loads(db_path.read_text())
    by_file = {os.path.relpath(e["file"], e.get("directory", root)): e for e in db}
    by_dir: dict[str, dict] = {}
    for rel, entry in by_file.items():
        by_dir.setdefault(os.path.dirname(rel), entry)

    failures = 0
    checked = 0
    skipped = 0
    for target in sys.argv[1:]:
        rel = os.path.relpath(os.path.abspath(target), root)
        if not os.path.exists(os.path.join(root, rel)):
            print(f"{rel}: no such file", file=sys.stderr)
            failures += 1
            continue

        entry = by_file.get(rel)
        borrowed = ""
        if entry is None:
            entry = by_dir.get(os.path.dirname(rel))
            if entry is None:
                print(f"{rel}: skipped, no native build compiles it")
                skipped += 1
                continue
            borrowed = f" (flags borrowed from {os.path.relpath(entry['file'], root)})"

        flags = flags_for(entry)
        # A file wholly inside a guard the native build never defines would
        # otherwise compile as an empty translation unit and pass regardless of
        # its contents. Turn the guard on so the code is what gets checked.
        guard = leading_guard(os.path.join(root, rel))
        forced = ""
        if guard and not any(f == f"-D{guard}" or f.startswith(f"-D{guard}=") for f in flags):
            flags = flags + [f"-D{guard}"]
            # Guarded code often includes headers the native build has no
            # reason to put on the search path. mk/cross.mk adds the same
            # directory for the device builds that do compile these files.
            for extra in ("/usr/include/libdrm",):
                if os.path.isdir(extra):
                    flags = flags + ["-I" + extra]
            forced = f" (forced -D{guard})"

        cmd = flags + ["-fsyntax-only", rel]
        started = time.monotonic()
        result = subprocess.run(
            cmd, cwd=entry.get("directory", root), capture_output=True, text=True, check=False
        )
        elapsed = time.monotonic() - started

        # A guard that is off natively often gates headers this host has no
        # reason to carry. That is "not checked here", which is worth saying;
        # it is not a pass and it is not a defect in the file.
        missing_header = result.returncode != 0 and (
            "No such file or directory" in result.stderr or "file not found" in result.stderr
        )
        if forced and missing_header:
            print(f"{rel}: UNCHECKED ({elapsed:.1f}s) - {guard} is off natively "
                  f"and its headers are unavailable here")
            skipped += 1
            continue

        status = "ok" if result.returncode == 0 else "FAILED"
        print(f"{rel}: {status} ({elapsed:.1f}s){borrowed}{forced}")
        checked += 1
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            failures += 1

    print(f"summary: {checked} compiled, {skipped} skipped, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
