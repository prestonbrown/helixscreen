#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Would mock code reach a packaged binary? Answered without a cross build.

A packaged build (HELIX_PACKAGING=1) sets ENABLE_MOCKS=no, which drops the mock
sources and HELIX_ENABLE_MOCKS. Code that is not behind that define still ships.
`make release-gate TIERS=heavy` sees it in the linked symbols of a docker cross
build; this sees it in a land gate's time, one object at a time:

  1. The packaged source list and flags come from make itself, parsed with
     HELIX_PACKAGING=1 and a scratch OBJ_DIR, so the tree's flags stamp is not
     touched and no objects go stale.
  2. Each unit compiles -c at -O0 with no debug info, which costs a fraction of
     the real flags and emits every inline function a unit uses.
  3. `nm -C` of the object is matched against scripts/mock_symbol_pattern.txt, the
     pattern release-gate.sh greps the linked symbols for. A definition ships; an
     undefined reference either fails the packaged link or names a definition
     another unit ships, so both are findings. So is a unit that does not compile
     with mocks off: that is a packaged build broken.

What it cannot see: code behind a #if for another platform. It compiles for this
host, so a cross target's own branches stay release-gate's.

usage: check_no_mocks.py [--base REF] [--all] [--max-units N] [--jobs N] [FILE...]

Named files, else the units changed since REF (default: the fork point, from
scripts/diff_base.py), plus those reaching a changed header up to --max-units; or
--all for every packaged unit. Exit 0 clean, 1 findings, 3 no answer.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SCRIPTS_DIR = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, SCRIPTS_DIR)
from compile_command import (  # noqa: E402
    PrereqsRefused,
    make_commands,
    make_program,
    run_prereqs,
)
from diff_base import resolve_base  # noqa: E402
from esp32_syntax_check import header_includers  # noqa: E402
from merge_compile_commands import entry_argv  # noqa: E402

PATTERN_FILE = os.path.join(SCRIPTS_DIR, "mock_symbol_pattern.txt")
# Where the app's own #include lines resolve; build/generated holds files no
# change touches, so it is not walked.
INCLUDE_ROOTS = (".", "include", "src", "src/generated")
HEADER_EXTENSIONS = (".h", ".hpp", ".hh", ".hxx", ".inc", ".ipp")
# Flags that cost time or hide symbols, and nothing a symbol's presence depends on.
# An -flto object holds bitcode, which nm cannot read. _FORTIFY_SOURCE warns at -O0.
DROPPED_FLAG_RE = re.compile(r"-O.*|-g.*|-flto.*|-D_FORTIFY_SOURCE.*")
# What a land gate can spend: about two minutes of compiles at MAX_JOBS on a loaded
# zeus. Most branches of 50 files reach fewer than this.
DEFAULT_MAX_UNITS = 400
MAX_JOBS = 24
SYMBOLS_SHOWN_PER_UNIT = 8
COMPILE_ERROR_LINES = 12
UNIT_TIMEOUT_SECS = 600
EXIT_NO_ANSWER = 3


def packaged_vars(obj_dir: str) -> tuple[str, ...]:
    """Make assignments for the packaged configuration, in a scratch OBJ_DIR.

    OBJ_DIR is what keeps the parse from rewriting the real flags stamp, which
    would rebuild every object the next ordinary make looks at.
    """
    return ("HELIX_PACKAGING=1", "ENABLE_MOCKS=no", f"OBJ_DIR={obj_dir}")


def packaged_units(root: str, obj_dir: str) -> set[str]:
    """The C++ sources a packaged build compiles, relative to root."""
    make = make_program()
    if not make:
        return set()
    proc = subprocess.run(
        [make, "-s", "--no-print-directory", "-C", root, "print-var-APP_SRCS",
         *packaged_vars(obj_dir)],
        capture_output=True, text=True, timeout=600,
    )
    if proc.returncode != 0:
        return set()
    return {tok for tok in proc.stdout.split() if tok.endswith((".cpp", ".mm"))}


def changed_paths(root: str, base: str) -> list[str]:
    """Paths added, copied or modified since base, a moved file included."""
    out = subprocess.run(
        ["git", "diff", "--name-only", "--no-renames", "--diff-filter=ACM", base],
        cwd=root, capture_output=True, text=True, timeout=60,
    ).stdout
    return sorted(line for line in out.splitlines() if line)


def even_sample(items: list[str], count: int) -> list[str]:
    """count of items, spread across the list rather than its alphabetical head."""
    return [items[i * len(items) // count] for i in range(count)] if count > 0 else []


def select_units(units: set[str], changed: list[str], read_source,
                 max_units: int) -> tuple[list[str], int]:
    """(units to check, units left out): changed sources, then header includers.

    A changed source is always checked. Includers come narrowest header first, so
    max_units (0: no limit) spends itself on complete coverage of the narrow headers
    and samples only the widest, whose includers mostly compile the same code.
    """
    headers = [p for p in changed if p.endswith(HEADER_EXTENSIONS)]
    picked = {p for p in changed if p in units}
    reaching = header_includers(units, headers, read_source, INCLUDE_ROOTS).values()
    left_out: set[str] = set()
    for includers in sorted(reaching, key=len):
        fresh = [u for u in includers if u not in picked]
        room = max_units - len(picked) if max_units else len(fresh)
        chosen = fresh if len(fresh) <= room else even_sample(fresh, room)
        picked.update(chosen)
        left_out.update(fresh)
    return sorted(picked), len(left_out - picked)


def object_command(entry: dict, obj: str) -> list[str]:
    """entry's command with the cheapest flags that still emit every symbol."""
    argv = [a for a in entry_argv(entry) if not DROPPED_FLAG_RE.fullmatch(a)]
    return [*argv, "-O0", "-w", "-o", obj]


def mock_symbols(nm_output: str, pattern: re.Pattern) -> list[tuple[str, str]]:
    """(kind, symbol) for each line of `nm -C` output the pattern matches."""
    found = []
    for line in nm_output.splitlines():
        # "<addr> <type> <name>", or "<blanks> U <name>" with no address.
        parts = line.split(None, 2) if line[:1].strip() else [""] + line.split(None, 1)
        if len(parts) < 3:
            continue
        kind, name = parts[1], parts[2]
        if pattern.search(name):
            found.append(("references" if kind == "U" else "defines", name))
    return found


def check_unit(rel: str, entry: dict, root: str, obj_dir: str, pattern: re.Pattern) -> dict:
    obj = os.path.join(obj_dir, rel.replace(os.sep, "__") + ".o")
    try:
        proc = subprocess.run(object_command(entry, obj), cwd=root, capture_output=True,
                              text=True, timeout=UNIT_TIMEOUT_SECS)
    except subprocess.TimeoutExpired:
        return {"unit": rel, "error": f"compile ran past {UNIT_TIMEOUT_SECS}s"}
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout).strip().splitlines()[:COMPILE_ERROR_LINES]
        return {"unit": rel, "error": "\n".join(tail)}
    nm = subprocess.run(["nm", "-C", obj], capture_output=True, text=True, timeout=120)
    os.unlink(obj)
    if nm.returncode != 0:
        return {"unit": rel, "no_answer": (nm.stderr or "nm failed").strip()}
    return {"unit": rel, "symbols": mock_symbols(nm.stdout, pattern)}


def report(results: list[dict]) -> tuple[int, int]:
    """Prints each finding; returns (units with mock symbols or errors, no-answers)."""
    bad = no_answer = 0
    for res in sorted(results, key=lambda r: r["unit"]):
        if res.get("no_answer"):
            no_answer += 1
            print(f"✗ {res['unit']}: {res['no_answer']}")
        elif res.get("error"):
            bad += 1
            print(f"❌ {res['unit']}: does not compile with mocks off")
            print("   " + res["error"].replace("\n", "\n   "))
        elif res["symbols"]:
            bad += 1
            for kind, name in res["symbols"][:SYMBOLS_SHOWN_PER_UNIT]:
                print(f"❌ {res['unit']}: {kind} {name}")
            more = len(res["symbols"]) - SYMBOLS_SHOWN_PER_UNIT
            if more > 0:
                print(f"   ... and {more} more mock symbols in {res['unit']}")
    return bad, no_answer


def main() -> int:
    parser = argparse.ArgumentParser(description="Mock code a packaged binary would carry.")
    parser.add_argument("files", nargs="*", help="check these sources or headers")
    parser.add_argument("--all", action="store_true", help="every packaged unit")
    parser.add_argument("--base", help="diff against this ref (default: the fork point)")
    parser.add_argument("--max-units", type=int, default=DEFAULT_MAX_UNITS,
                        help="units to spend on a change's header includers; 0: no limit")
    parser.add_argument("--jobs", type=int, help="compiles at once (default: JOBPOOL_SLOTS)")
    args = parser.parse_args()

    root = os.path.realpath(os.getcwd())
    jobs = args.jobs or int(os.environ.get("JOBPOOL_SLOTS", "0"))
    if not jobs:
        # Compiles are pool work: re-enter under a hold, which sets JOBPOOL_SLOTS. With
        # no pool a hold grants -n, so -n is what keeps this off a shared host's every core.
        claim = os.path.join(SCRIPTS_DIR, "helix-claim")
        slots = min(MAX_JOBS, os.cpu_count() or 1)
        os.execv(claim, [claim, "hold", "-n", str(slots), "--", sys.executable,
                         os.path.realpath(__file__), *sys.argv[1:]])

    with open(PATTERN_FILE) as fh:
        pattern = re.compile(fh.read().strip())

    started = time.monotonic()
    scratch = tempfile.mkdtemp(prefix="helix-no-mocks-")
    try:
        units = packaged_units(root, scratch)
        if not units:
            print("✗ no-mocks: make named no packaged sources, so nothing was checked")
            return EXIT_NO_ANSWER

        if args.all:
            selected = sorted(units)
        else:
            if args.files:
                absent = [f for f in args.files if not os.path.isfile(f)]
                if absent:
                    print(f"✗ no-mocks: no such file: {', '.join(absent)}")
                    return EXIT_NO_ANSWER
                changed = sorted(os.path.relpath(os.path.realpath(f), root) for f in args.files)
            else:
                base = args.base or resolve_base(root)[0]
                changed = changed_paths(root, base)

            def read_source(rel):
                try:
                    with open(os.path.join(root, rel), errors="replace") as fh:
                        return fh.read()
                except OSError:
                    return None

            selected, left_out = select_units(units, changed, read_source, args.max_units)
            if left_out:
                print(f"ℹ️  no-mocks: {left_out} units reaching a changed header are past the "
                      f"budget of {args.max_units} and not checked; --all covers them")

        if not selected:
            print("✓ no-mocks: the change reaches no packaged unit")
            return 0

        try:
            made, output = run_prereqs(root)
        except PrereqsRefused as exc:
            print(f"✗ no-mocks: another session is building this tree: {exc}")
            return EXIT_NO_ANSWER
        if not made:
            print(f"✗ no-mocks: make compile-prereqs failed:\n{output}")
            return EXIT_NO_ANSWER

        full = {os.path.join(root, rel): rel for rel in selected}
        entries = make_commands(list(full), root, make_vars=packaged_vars(scratch))
        missing = sorted(rel for path, rel in full.items() if path not in entries)
        if missing:
            print(f"✗ no-mocks: make printed no command for {', '.join(missing)}")
            return EXIT_NO_ANSWER

        print(f"→ no-mocks: {len(selected)} of {len(units)} packaged units, {jobs} at a time",
              flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(
                lambda path: check_unit(full[path], entries[path], root, scratch, pattern),
                full,
            ))
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    bad, no_answer = report(results)
    secs = round(time.monotonic() - started)
    if bad:
        print(f"❌ no-mocks: {bad} of {len(selected)} packaged units carry mock code ({secs}s). "
              "A packaged build has no HELIX_ENABLE_MOCKS: guard it with #ifdef, or name the "
              "file so the Makefile's ENABLE_MOCKS=no filter drops it.")
        return 1
    if no_answer:
        print(f"✗ no-mocks: {no_answer} units could not be read")
        return EXIT_NO_ANSWER
    print(f"✓ no-mocks: {len(selected)} packaged units carry no mock symbols ({secs}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
