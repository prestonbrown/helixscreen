#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Syntax-check translation units with clang to catch GCC/clang divergence locally.

Why this exists
---------------
Every local build here uses g++; the Ubuntu CI job builds with clang (``CXX: ccache
clang++`` in ``.github/workflows/build.yml``). A diagnostic that only clang emits is
therefore invisible until CI goes red. v0.99.118 shipped exactly that: the pre-fix
``safe_size_t`` in ``include/json_utils.h`` compared a ``uint64_t`` against
``size_t``'s max inside an ``if constexpr``. Where the two types are the same width
that comparison is tautologically false, and clang diagnoses the body of a discarded
branch outside a template. GCC does not. Fixed in 5d3ea331c.

This gate runs clang with ``-fsyntax-only`` over the TUs a change touches. No
codegen, no linking, no cross-compiling: a few seconds per TU.

Compile database
----------------
Commands come from the build's own emitted compile-command fragments
(``build/obj/**/*.ccj``, written by ``emit-compile-command`` in ``mk/rules.mk``)
unioned with ``compile_commands.json``, fragments winning on conflict.

Reading both is not belt-and-braces, it is required. ``compile_commands.json`` is
regenerated from the fragments only at certain build steps, so it lags: at the time
this script was written the tree had 2265 fragments but 1303 entries in the JSON,
and *none* of the 972 test TUs were in the JSON. ``tests/unit/test_json_utils.cpp``
-- the TU that actually broke CI -- was one of the missing ones. A checker driven by
``compile_commands.json`` alone could not have caught the bug it exists to catch.

Argument handling: entries carry either an ``arguments`` array or a ``command``
string. A command string is split with ``shlex.split`` and never handed to a shell.
Re-parsing through a shell strips one layer of quoting and turns
``-DHELIX_VERSION="0.99.118"`` into a bare ``0.99.118``, which clang then reports as
``invalid suffix '.118' on floating constant`` -- phantom errors that look like real
findings.

Which warnings fail
-------------------
The Ubuntu CI job does not set ``WERROR=1``; it runs plain ``make`` and ``make test``
with clang. So the only warning CI promotes to an error is ``-Werror=type-limits``,
from ``TEST_WARN_FLAGS`` in ``mk/tests.mk``, and that variable is applied only to the
``tests/`` compile rules -- deliberately, per the comment there. This script mirrors
that scoping exactly: ``-Werror=type-limits`` is added for TUs under ``tests/`` and
for nothing else. Under clang that flag covers the tautological-comparison group
(``-Wtautological-unsigned-zero-compare``,
``-Wtautological-type-limit-compare``), which is the family that broke v0.99.118.

Everything else clang reports is printed but does not fail the run: clang's warning
set is much wider than GCC's, and failing on warnings CI tolerates would make this
gate unlandable.

Header changes
--------------
A changed header has no compile-command entry of its own, and header-only changes are
precisely the case that broke us -- ``json_utils.h`` is a header. Ignoring them would
make this gate miss its own motivating bug.

So a changed header is resolved to the TUs that include it via the ``-MMD`` dependency
files the build already writes (``build/obj/**/*.d``). That map is exact and
transitive -- it is what the compiler actually opened, not a grep for ``#include`` --
and building it costs about 25ms for the whole tree. Widely-included headers can pull
in hundreds of TUs, so the fan-out is capped (``--max-header-tus``, default 40); when
the cap bites, the script says so explicitly rather than silently checking a subset.
If no ``.d`` files exist (never-built tree) header changes are reported as unresolved
rather than passed over in silence.

Usage
-----
    scripts/check_clang_diagnostics.py              # files changed vs origin/main
    scripts/check_clang_diagnostics.py --all        # every TU (slow, full audit)
    scripts/check_clang_diagnostics.py -j 4 f.cpp   # explicit files

Exits 0 and prints SKIP when clang is unavailable or no usable GCC toolchain is
found: this must never block a commit on a machine without clang.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# C++ translation units only. The tree's .c files are generated LVGL font
# data under assets/fonts/ (compiled -std=c11); replaying their commands
# through clang++ is a category error - "invalid argument '-std=c11' not
# allowed with 'C++'" - and hand-written C does not exist in src/ or include/.
TU_EXTENSIONS = (".cpp", ".cc", ".cxx", ".mm")
HEADER_EXTENSIONS = (".h", ".hpp", ".hh", ".hxx", ".inc", ".ipp")

# Compiler wrappers that may lead the command line; dropped before argv[0] is
# rewritten to clang.
WRAPPERS = {"ccache", "sccache", "distcc", "icecc", "time"}

# GCC-only flags to drop up front. Measured on this tree (2026-08-28): a census of
# every flag across all 2265 compile-command fragments found only
#   -O2 -g -Wall -Wextra -fexceptions -fno-omit-frame-pointer -fstack-protector-strong
# all of which clang accepts, so *this list is currently unused here*. It is a seed
# for the fast path, not the real safety net -- `strip_unknown_arguments` below
# recovers from anything not listed by reading clang's own complaint, so a GCC-only
# flag added to the build later is handled without editing this list.
#
# Only `-f`/`-m`-family unknowns need stripping at all. Measured with clang 18.1.3:
# an unknown `-W` flag is a warning (`-Wunknown-warning-option`) and exits 0, while
# an unknown `-f` flag is a hard `error: unknown argument` and exits 1. We pass
# `-Wno-unknown-warning-option` to neutralise the whole `-W` family generically
# rather than enumerating it.
GCC_ONLY_FLAG_SEEDS = (
    "-fno-var-tracking-assignments",
    "-fvar-tracking-assignments",
    "-fno-var-tracking",
    "-fvar-tracking",
    "-flifetime-dse",
    "-fno-lifetime-dse",
    "-fira-loop-pressure",
    "-fno-ipa-icf",
    "-fno-tree-loop-distribute-patterns",
    "-fno-sched-interblock",
)

# Flags taking a separate path argument that must not survive into a syntax-only run.
# -MF/-MT/-MQ in particular would let clang overwrite the build's own .d files.
ARG_TAKING_DROPS = {"-o", "-MF", "-MT", "-MQ", "-MD", "-MMD"}
STANDALONE_DROPS = {"-c", "-MD", "-MMD", "-MP", "-M", "-MM", "--"}

UNKNOWN_ARG_RE = re.compile(r"unknown argument:? '([^']+)'")
DIAG_RE = re.compile(r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+): (?P<kind>error|warning|fatal error): ")


# --------------------------------------------------------------------------------
# Toolchain discovery
# --------------------------------------------------------------------------------


def _probe(argv: list[str]) -> bool:
    """True when argv can syntax-check a TU that includes a libstdc++ header."""
    src = "#include <cstddef>\n#include <string>\nint main(){return 0;}\n"
    try:
        proc = subprocess.run(
            argv + ["-x", "c++", "-std=c++17", "-fsyntax-only", "-"],
            input=src,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return proc.returncode == 0


def find_clang() -> tuple[list[str], str] | tuple[None, str]:
    """Locate a clang++ that can actually see the C++ standard library.

    On stock Ubuntu, clang++ needs an explicit --gcc-install-dir or <cstddef> is not
    found; -stdlib=libc++ is not an answer because libc++ is not installed. Returns
    (prefix_argv, description) or (None, reason).
    """
    clang = os.environ.get("HELIX_CLANGXX") or shutil.which("clang++")
    if not clang:
        return None, "clang++ not found on PATH"

    # Bare clang first: correct on macOS and on any properly configured install.
    if _probe([clang]):
        return [clang], f"{clang} (no toolchain flag needed)"

    candidates = []
    for d in glob.glob("/usr/lib/gcc/*/*") + glob.glob("/usr/lib64/gcc/*/*"):
        if not os.path.isdir(d):
            continue
        triple = os.path.basename(os.path.dirname(d))
        ver = os.path.basename(d)
        # Bare-metal cross toolchains (arm-none-eabi) can never provide a hosted
        # libstdc++; sort them last rather than excluding by name.
        hosted = 0 if "none" in triple or "elf" in triple else 1
        try:
            key = tuple(int(p) for p in ver.split("."))
        except ValueError:
            key = (0,)
        candidates.append((hosted, key, d))

    for _, _, d in sorted(candidates, reverse=True):
        prefix = [clang, f"--gcc-install-dir={d}"]
        if _probe(prefix):
            return prefix, f"{clang} --gcc-install-dir={d}"

    return None, f"{clang} cannot compile #include <cstddef> with any GCC toolchain found"


# --------------------------------------------------------------------------------
# Compile database
# --------------------------------------------------------------------------------


def entry_args(entry: dict) -> list[str]:
    """Argument vector for a compile-command entry.

    Never routed through a shell, and split with shlex rather than on whitespace.

    The subtlety is which shlex mode. `emit-compile-command` (mk/rules.mk) builds
    the string as CMD="$(CXX) $(CXXFLAGS) ..." -- make has already expanded
    -DHELIX_VERSION=\\"0.99.118\\" and the shell assignment has already eaten the
    backslashes, so what lands in the JSON is the *post-expansion argv*, joined by
    spaces, in which the quote characters are literal parts of the argument. Posix
    shlex would strip them a second time, leaving -DHELIX_VERSION=0.99.118, and
    clang then reports `invalid suffix '.118' on floating constant` plus a cascade
    of undeclared identifiers from -DINSTALLER_FILENAME=install.sh -- phantom
    errors that read exactly like real clang findings. (Observed here before this
    was fixed: 6+ bogus diagnostics in src/system/update_checker.cpp alone.)

    So: posix=False, which keeps quote characters inside tokens while still
    treating a quoted run of spaces as one token. A token that is quoted end to
    end is genuine shell quoting (a path with spaces, as a conventional
    cmake/Bear-produced database would emit) and is unwrapped; a token with quotes
    only in the interior is the -DFOO="bar" shape and is left exactly as is.
    """
    args = entry.get("arguments")
    if args:
        return list(args)

    tokens = shlex.split(entry.get("command", ""), posix=False)
    out = []
    for t in tokens:
        if len(t) >= 2 and t[0] == t[-1] and t[0] in "\"'" and t[0] not in t[1:-1]:
            t = t[1:-1]
        out.append(t)
    return out


def load_compile_db(root: str, frag_root: str | None = None) -> dict[str, dict]:
    """file realpath -> entry, from .ccj fragments unioned over compile_commands.json."""
    db: dict[str, dict] = {}

    def absorb(entry: dict) -> None:
        f = entry.get("file")
        if not f:
            return
        directory = entry.get("directory", root)
        path = f if os.path.isabs(f) else os.path.join(directory, f)
        db[os.path.realpath(path)] = entry

    cc_json = os.path.join(root, "compile_commands.json")
    if frag_root is None and os.path.exists(cc_json):
        try:
            with open(cc_json) as fh:
                for entry in json.load(fh):
                    absorb(entry)
        except (OSError, ValueError):
            pass

    # Fragments last: they are per-TU and written at compile time, so they are
    # never staler than the aggregated JSON. frag_root redirects the fragment
    # glob to a caller-supplied directory: the meta-test builds a throwaway
    # database there so its fixtures can never leak into (or depend on) this
    # tree's build/obj - a leftover fixture under build/obj would be unioned
    # into every real --all audit until something cleaned it up.
    frag_dir = frag_root if frag_root else os.path.join(root, "build", "obj")
    for frag in glob.glob(os.path.join(frag_dir, "**", "*.ccj"), recursive=True):
        try:
            with open(frag) as fh:
                absorb(json.load(fh))
        except (OSError, ValueError):
            continue

    return db


def build_header_map(root: str) -> dict[str, set[str]]:
    """header realpath -> set of TU realpaths, from the build's -MMD .d files.

    Exact and transitive: these list what the compiler actually opened. Note -MMD
    omits system headers, which is what we want -- we only resolve project headers.
    """
    mapping: dict[str, set[str]] = {}
    for dep in glob.glob(os.path.join(root, "build", "obj", "**", "*.d"), recursive=True):
        try:
            with open(dep) as fh:
                text = fh.read()
        except OSError:
            continue
        # First logical line is "target: source header header ...", continued with
        # trailing backslashes. Later lines are the phony "header:" targets from -MP.
        first = text.split("\n\n")[0].replace("\\\n", " ")
        head, _, rest = first.partition(":")
        if not rest:
            continue
        parts = rest.split()
        if not parts:
            continue
        tu = os.path.realpath(os.path.join(root, parts[0]))
        if not tu.endswith(TU_EXTENSIONS):
            continue
        for token in parts[1:]:
            if token.endswith(HEADER_EXTENSIONS):
                mapping.setdefault(os.path.realpath(os.path.join(root, token)), set()).add(tu)
        _ = head
    return mapping


# --------------------------------------------------------------------------------
# Command construction
# --------------------------------------------------------------------------------


def clang_command(entry: dict, clang_prefix: list[str], extra_drop: frozenset[str]) -> list[str] | None:
    args = entry_args(entry)
    if not args:
        return None

    i = 0
    while i < len(args) and os.path.basename(args[i]) in WRAPPERS:
        i += 1
    if i >= len(args):
        return None
    i += 1  # skip the compiler itself; clang_prefix replaces it

    out: list[str] = []
    while i < len(args):
        a = args[i]
        if a in ARG_TAKING_DROPS:
            # -MD/-MMD take no argument; the rest do.
            i += 1 if a in ("-MD", "-MMD") else 2
            continue
        if a in STANDALONE_DROPS:
            i += 1
            continue
        base = a.split("=", 1)[0]
        if a in extra_drop or base in extra_drop or base in GCC_ONLY_FLAG_SEEDS:
            i += 1
            continue
        out.append(a)
        i += 1

    cmd = list(clang_prefix) + out
    cmd += [
        "-fsyntax-only",
        # Unknown -W flags are only a warning under clang, but become fatal next to
        # any -Werror. Silencing them generically beats maintaining a strip list.
        "-Wno-unknown-warning-option",
        "-ferror-limit=8",
        "-fno-caret-diagnostics",
        "-fno-color-diagnostics",
    ]

    # CI parity: mk/tests.mk applies -Werror=type-limits to the tests/ compile rules
    # only. Mirror that scoping rather than promoting it tree-wide.
    src = entry.get("file", "")
    rel = os.path.relpath(os.path.realpath(src), REPO_ROOT)
    if rel.startswith("tests" + os.sep):
        cmd.append("-Werror=type-limits")

    return cmd


def strip_unknown_arguments(stderr: str) -> frozenset[str]:
    """Flags clang rejected outright, so a retry can drop them."""
    return frozenset(UNKNOWN_ARG_RE.findall(stderr))


def check_tu(entry: dict, clang_prefix: list[str], root: str) -> dict:
    """Syntax-check one TU. Retries once, dropping flags clang named as unknown."""
    dropped: set[str] = set()
    for _ in range(3):
        cmd = clang_command(entry, clang_prefix, frozenset(dropped))
        if cmd is None:
            return {"file": entry.get("file", "?"), "status": "skipped", "output": "no command"}
        try:
            proc = subprocess.run(
                cmd, cwd=entry.get("directory", root), capture_output=True, text=True, timeout=300
            )
        except subprocess.TimeoutExpired:
            return {"file": entry.get("file", "?"), "status": "timeout", "output": "clang timed out"}
        except OSError as exc:
            return {"file": entry.get("file", "?"), "status": "skipped", "output": str(exc)}

        unknown = strip_unknown_arguments(proc.stderr) - dropped
        if unknown and proc.returncode != 0:
            dropped |= set(unknown)
            continue
        break

    errors, warnings = [], []
    for line in proc.stderr.splitlines():
        m = DIAG_RE.match(line)
        if not m:
            continue
        (errors if m.group("kind") != "warning" else warnings).append(line.strip())

    status = "error" if (errors or proc.returncode != 0) else "ok"
    return {
        "file": entry.get("file", "?"),
        "status": status,
        "errors": errors,
        "warnings": warnings,
        "dropped": sorted(dropped),
        "output": proc.stderr.strip(),
    }


# --------------------------------------------------------------------------------
# File selection
# --------------------------------------------------------------------------------


def git(root: str, *args: str) -> str:
    try:
        return subprocess.run(
            ["git", *args], cwd=root, capture_output=True, text=True, timeout=60
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def changed_files(root: str) -> list[str]:
    base = git(root, "merge-base", "HEAD", "origin/main")
    if not base:
        base = git(root, "merge-base", "HEAD", "main")
    if not base:
        # No upstream to diff against; fall back to the working tree.
        out = git(root, "diff", "--name-only", "--diff-filter=ACM", "HEAD")
    else:
        out = git(root, "diff", "--name-only", "--diff-filter=ACM", base)
    files = [ln for ln in out.splitlines() if ln]
    staged = git(root, "diff", "--name-only", "--diff-filter=ACM", "--cached")
    files += [ln for ln in staged.splitlines() if ln]
    return sorted(set(files))


def select_entries(args, db, root) -> tuple[list[dict], list[str]]:
    """Returns (entries to check, notes to print)."""
    notes: list[str] = []

    if args.all:
        # Same TU filter as the explicit-files path: --all must not bypass it,
        # or every .c in the compile database (generated fonts, the expat
        # sources inside lib/helix-xml) gets its -std=c11 command line replayed
        # through clang++ - the exact misclassification the filter exists for.
        return sorted((e for e in db.values()
                       if str(e.get("file", "")).endswith(TU_EXTENSIONS)),
                      key=lambda e: e.get("file", "")), notes

    paths = args.files or changed_files(root)
    if not paths:
        return [], notes

    tus: list[str] = []
    headers: list[str] = []
    for p in paths:
        full = os.path.realpath(os.path.join(root, p))
        if p.endswith(TU_EXTENSIONS):
            tus.append(full)
        elif p.endswith(HEADER_EXTENSIONS):
            headers.append(full)

    selected: dict[str, dict] = {}
    for t in tus:
        if t in db:
            selected[t] = db[t]
        else:
            notes.append(f"no compile command for {os.path.relpath(t, root)} (never built?)")

    if headers:
        hmap = build_header_map(root)
        if not hmap:
            notes.append(
                f"{len(headers)} header(s) changed but no .d files under build/obj -- "
                "header changes are UNCHECKED in this tree; build first for coverage"
            )
        for h in headers:
            dependents = sorted(hmap.get(h, ()))
            if not dependents:
                notes.append(f"no known dependents for {os.path.relpath(h, root)}")
                continue
            usable = [d for d in dependents if d in db]
            if len(usable) > args.max_header_tus:
                notes.append(
                    f"{os.path.relpath(h, root)}: {len(usable)} dependent TUs, "
                    f"checking first {args.max_header_tus} (raise --max-header-tus for all)"
                )
                usable = usable[: args.max_header_tus]
            for d in usable:
                selected.setdefault(d, db[d])

    return sorted(selected.values(), key=lambda e: e.get("file", "")), notes


# --------------------------------------------------------------------------------


def main() -> int:
    default_jobs = max(1, min(8, (os.cpu_count() or 4) // 4))
    ap = argparse.ArgumentParser(description="Syntax-check TUs with clang to catch GCC/clang divergence.")
    ap.add_argument("files", nargs="*", help="explicit files (default: changed vs origin/main)")
    ap.add_argument("--all", action="store_true", help="check every TU in the compile database (slow)")
    ap.add_argument("-j", "--jobs", type=int, default=default_jobs,
                    help=f"parallel clang invocations (default {default_jobs}; kept low, "
                         "this box also runs builds)")
    ap.add_argument("--max-header-tus", type=int, default=40,
                    help="cap on TUs checked per changed header (default 40)")
    ap.add_argument("--max-report", type=int, default=5, help="how many failing TUs to print in full")
    ap.add_argument("--warnings", action="store_true", help="also print clang warnings (never fatal)")
    ap.add_argument("--compile-db-dir", default=None, metavar="DIR",
                    help="read *.ccj compile-command fragments from DIR instead of "
                         "build/obj, and skip compile_commands.json (test isolation)")
    args = ap.parse_args()

    root = REPO_ROOT

    clang_prefix, desc = find_clang()
    if clang_prefix is None:
        print(f"SKIP: clang syntax check -- {desc}")
        return 0

    db = load_compile_db(root, args.compile_db_dir)
    if not db:
        print("SKIP: clang syntax check -- no compile database (build the tree first)")
        return 0

    entries, notes = select_entries(args, db, root)
    for n in notes:
        print(f"  note: {n}")

    if not entries:
        print("clang syntax check: no translation units to check")
        return 0

    print(f"clang syntax check: {len(entries)} TU(s) via {desc} (-j{args.jobs})")

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(check_tu, e, clang_prefix, root) for e in entries]
        for fut in concurrent.futures.as_completed(futures):
            results.append(fut.result())

    failed = [r for r in results if r["status"] in ("error", "timeout")]
    warned = [r for r in results if r["status"] == "ok" and r.get("warnings")]

    if args.warnings:
        for r in warned:
            print(f"\n  warnings in {os.path.relpath(r['file'], root)}:")
            for w in r["warnings"][:5]:
                print(f"    {w}")

    print(
        f"\nchecked {len(results)} TU(s): "
        f"{len(results) - len(failed)} clean, {len(failed)} with errors, "
        f"{len(warned)} with warnings (not fatal)"
    )

    if not failed:
        return 0

    print("\nclang rejected code that g++ accepts (this is what breaks Ubuntu CI):\n")
    for r in sorted(failed, key=lambda r: r["file"])[: args.max_report]:
        print(f"  {os.path.relpath(r['file'], root)}")
        for line in (r.get("errors") or [r.get("output", "")])[:6]:
            print(f"    {line}")
        if r.get("dropped"):
            print(f"    (dropped GCC-only flags: {' '.join(r['dropped'])})")
        print()
    if len(failed) > args.max_report:
        print(f"  ... and {len(failed) - args.max_report} more failing TU(s)")

    return 1


if __name__ == "__main__":
    sys.exit(main())
