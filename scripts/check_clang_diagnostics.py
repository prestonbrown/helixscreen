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

Compile commands
----------------
A TU's command comes from ``scripts/compile_command.py``, which ``syntax_check.py``
asks too, so the two never disagree about a file's flags. In order:

1. The build's own record. Commands come from the build's emitted compile-command
   fragments (``build/obj/**/*.ccj``, written by ``emit-compile-command`` in
   ``mk/rules.mk``) unioned with ``compile_commands.json``. Reading both is
   required: ``compile_commands.json`` is regenerated from the fragments only at
   certain build steps, so it lags, and a test TU can be missing from it while its
   fragment is current. Several entries usually describe one file (one per object
   tree, plus the JSON's) and they need not agree; ``merge_compile_commands.py``
   picks the freshest, so this gate and an editor's index resolve a file the same
   way. Only an entry stamped with ``VERSION.txt``'s version counts here.
2. ``make print-compile-command F=<file>``: the command the file's object rule
   runs today, printed from the same Makefile function, so it is trusted as a
   fresh entry is. It needs no prior build, which is what lets an unbuilt
   worktree, or one whose copied database was recorded at another version, check
   its own changes. Replaying it needs the headers the object rules make first,
   so the first such TU runs ``make compile-prereqs`` (about 20s on a fresh
   ``--no-build`` worktree, then a second or two).
3. With no make rule for the file, a current entry for a neighbour in the same
   directory, borrowed and named in a note.

Trusting a command
------------------
An entry recorded at another version is never replayed to fail the gate. A fragment
is rewritten only when its object is, and make rebuilds an object when a
prerequisite is newer, not when the command line changes - so a version bump or a
new ``-DHELIX_HAS_*`` leaves untouched trees frozen at older flags. Replaying one
of those produces diagnostics about the *command*: a type behind a feature macro
the entry never received reads as ``unknown type name``, six or eight times,
indistinguishable in shape from a real clang finding. So trust is decided before
clang runs and from the entry alone, never from what clang said: a TU whose only
command is stale (no make rule could replace it) is skipped with a note, and only
a command that describes today's build gets to fail the gate.

A changed TU with no command from any of the three is named, and when nothing at
all could be checked the gate fails: it verified nothing.

Argument handling: entries carry either an ``arguments`` array or a ``command``
string. Both are normalised by ``merge_compile_commands.entry_argv`` and never handed
to a shell; see that function for why the database holds two quoting shapes of a
quoted define and why both must replay as ``-DHELIX_VERSION="0.99.118"``. Mishandle
either shape and the version macro stops being a string, which clang reports as
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
    scripts/check_clang_diagnostics.py              # files changed since the fork point
    scripts/check_clang_diagnostics.py --all        # every TU (slow, full audit)
    scripts/check_clang_diagnostics.py -j 4 f.cpp   # explicit files

Exits 0 and prints SKIP when clang is unavailable or no usable GCC toolchain is
found: this must never block a commit on a machine without clang.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))
from compile_command import (  # noqa: E402
    IGNORE_CLAIM_ENV,
    SIBLING,
    STALE,
    PrereqsFailed,
    PrereqsRefused,
    load_db,
    resolve,
)
from diff_base import base_line, resolve_base  # noqa: E402
from merge_compile_commands import (  # noqa: E402
    current_version,
    entry_argv,
    entry_source_path,
    is_stale,
    recorded_version,
)

# C++ translation units only. The tree's .c files are generated LVGL font
# data under assets/fonts/ (compiled -std=c11); replaying their commands
# through clang++ is a category error - "invalid argument '-std=c11' not
# allowed with 'C++'" - and hand-written C does not exist in src/ or include/.
TU_EXTENSIONS = (".cpp", ".cc", ".cxx", ".mm")
HEADER_EXTENSIONS = (".h", ".hpp", ".hh", ".hxx", ".inc", ".ipp")

# The native build compiles src/ and include/. A translation unit under firmware/
# is built by the ESP-IDF job and never enters this compile database, so its
# absence here is not evidence of an unbuilt tree and the "build and re-run"
# remedy cannot reach it. Counting it as unaccounted for blocks every
# firmware-only change from ever passing this gate.
OUTSIDE_NATIVE_BUILD = ("firmware/",)


def split_outside_native_build(paths: list[str], root: str) -> tuple[list[str], list[str]]:
    """Split into (checkable by this gate, built only by the firmware job).

    Paths arrive repo-relative from git and absolute from an explicit argument,
    so both are resolved against the repo root before the prefix is compared.
    """
    here: list[str] = []
    elsewhere: list[str] = []
    for path in paths:
        rel = os.path.relpath(os.path.join(root, path), root)
        (elsewhere if rel.startswith(OUTSIDE_NATIVE_BUILD) else here).append(path)
    return here, elsewhere

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

    Tokenisation is shared with every other replay consumer
    (merge_compile_commands.entry_argv): the database legitimately holds two
    quoting shapes of a quoted define - shell-quoted and post-expansion - and
    both must reach clang as -DHELIX_VERSION="1.1.0-beta.1" with the macro a
    string. Strip the quotes the wrong way for either shape and the version
    define stops being a string: every TU including helix_version.h reports an
    int where a const char* belongs, or an invalid suffix on a floating
    constant. Those read exactly like real findings about the code.
    """
    return entry_argv(entry)


def build_header_map(root: str, dep_root: str | None = None) -> dict[str, set[str]]:
    """header realpath -> set of TU realpaths, from the build's -MMD .d files.

    Exact and transitive: these list what the compiler actually opened. Note -MMD
    omits system headers, which is what we want -- we only resolve project headers.

    dep_root redirects the glob the same way the fragment glob is redirected, so a
    test can pin the header fan-out against its own dependency files.
    """
    mapping: dict[str, set[str]] = {}
    dep_dir = dep_root if dep_root else os.path.join(root, "build", "obj")
    for dep in glob.glob(os.path.join(dep_dir, "**", "*.d"), recursive=True):
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


def default_base(root: str) -> tuple[str, str]:
    """(base, why): the fork point a default run diffs against.

    The nearest fork point among the branches this one could have been cut from
    (scripts/diff_base.py, shared with mutate_diff.py and cov_diff.py), so a
    branch off a release branch is measured against that branch and not handed
    every file where the release and main differ. With nothing to resolve, the
    base is HEAD and the run covers uncommitted work.
    """
    return resolve_base(root)


def changed_files(root: str, base: str) -> list[str]:
    """Files added, copied or modified since base, plus whatever is staged."""
    out = git(root, "diff", "--name-only", "--diff-filter=ACM", base)
    files = [ln for ln in out.splitlines() if ln]
    staged = git(root, "diff", "--name-only", "--diff-filter=ACM", "--cached")
    files += [ln for ln in staged.splitlines() if ln]
    return sorted(set(files))


def describe_untrusted(untrusted: list[dict], root: str) -> list[str]:
    """Notes naming what was skipped and why, at a length worth reading."""
    if not untrusted:
        return []
    current = current_version(root) or "?"
    versions = sorted({recorded_version(e) or "?" for e in untrusted})
    head = (f"{len(untrusted)} TU(s) have only a stale compile command and no make rule "
            f"(recorded at {', '.join(versions)}; tree is {current}) -- not checked. "
            "Rebuild to refresh: make -j && make test")
    notes = [head]
    for entry in untrusted[:3]:
        rel = os.path.relpath(os.path.realpath(entry_source_path(entry, root)), root)
        notes.append(f"  stale compile command: {rel} (recorded at {recorded_version(entry)})")
    if len(untrusted) > 3:
        notes.append(f"  ... and {len(untrusted) - 3} more")
    return notes


class Selection:
    """What one run checks, skips and cannot check, with the notes to print."""

    def __init__(self) -> None:
        self.entries: list[dict] = []
        self.untrusted: list[dict] = []
        self.underivable: list[str] = []      # TUs with no flags from anywhere
        self.unresolved_headers: list[str] = []  # headers with no dependent known
        self.notes: list[str] = []


def select_all(db: dict[str, dict], root: str) -> Selection:
    """--all: every C++ TU in the database, stale entries skipped."""
    sel = Selection()
    current = current_version(root)
    # Same TU filter as the explicit-files path: --all must not bypass it, or
    # every .c in the compile database (generated fonts, the expat sources inside
    # lib/helix-xml) gets its -std=c11 command line replayed through clang++.
    for entry in sorted(db.values(), key=lambda e: e.get("file", "")):
        if not str(entry.get("file", "")).endswith(TU_EXTENSIONS):
            continue
        (sel.untrusted if is_stale(entry, current) else sel.entries).append(entry)
    return sel


def select_entries(paths: list[str], args, db: dict[str, dict], root: str) -> Selection:
    """The TUs paths name or reach through their headers, with a command for each.

    Each file's command comes from scripts/compile_command.py, the same helper
    syntax_check.py asks; may raise compile_command.PrereqsFailed.
    """
    sel = Selection()
    tus: list[str] = []
    headers: list[str] = []
    for p in paths:
        full = os.path.realpath(os.path.join(root, p))
        if p.endswith(TU_EXTENSIONS):
            tus.append(full)
        elif p.endswith(HEADER_EXTENSIONS):
            headers.append(full)

    dependents: dict[str, list[str]] = {}
    if headers:
        hmap = build_header_map(root, args.compile_db_dir)
        if not hmap:
            sel.notes.append(
                f"{len(headers)} header(s) changed but no .d files under build/obj -- "
                "header changes are UNCHECKED in this tree; build first for coverage"
            )
        for h in headers:
            deps = sorted(hmap.get(h, ()))
            if not deps:
                sel.notes.append(f"no known dependents for {os.path.relpath(h, root)}")
                sel.unresolved_headers.append(h)
                continue
            dependents[h] = deps

    wanted = sorted(set(tus) | {d for deps in dependents.values() for d in deps})
    choices = resolve(wanted, db, root, make_root=args.make_root)

    selected: dict[str, dict] = {}
    skipped: dict[str, dict] = {}
    for t in tus:
        choice = choices.get(t)
        if choice is None:
            sel.underivable.append(t)
        elif choice.source == STALE:
            skipped[t] = choice.entry
        else:
            selected[t] = choice.entry
            if choice.source == SIBLING:
                sel.notes.append(f"{os.path.relpath(t, root)}: no make rule, flags borrowed "
                                 f"from {choice.borrowed_from}")

    for h, deps in dependents.items():
        # Trust decides membership before the fan-out cap does, or a widely
        # included header spends its whole budget on TUs that get skipped and
        # the ones it could have checked never make the list.
        usable = [d for d in deps if choices.get(d) is not None and choices[d].source != STALE]
        for d in deps:
            if choices.get(d) is not None and choices[d].source == STALE:
                skipped.setdefault(d, choices[d].entry)
        if len(usable) > args.max_header_tus:
            sel.notes.append(
                f"{os.path.relpath(h, root)}: {len(usable)} dependent TUs, "
                f"checking first {args.max_header_tus} (raise --max-header-tus for all)"
            )
            usable = usable[: args.max_header_tus]
        for d in usable:
            selected.setdefault(d, choices[d].entry)

    sel.entries = [selected[k] for k in sorted(selected)]
    sel.untrusted = [e for k, e in sorted(skipped.items()) if k not in selected]
    return sel


# --------------------------------------------------------------------------------


def _default_jobs() -> int:
    """Parallel clang invocations, following the repo's own build rule.

    CLAUDE.md tells a human to throttle only when a build is actually running and
    otherwise to use the whole box, so this asks the same question rather than
    assuming a peer. On an idle machine every core is available; while a build
    holds the box, one quarter of them keeps this out of its way. The difference
    is not marginal: a 4-core box resolves to 1 when a build is running, and a
    sweep whose diff reaches a few hundred TUs then takes tens of minutes.
    """
    cpus = os.cpu_count() or 4
    try:
        busy = subprocess.run(["pgrep", "-x", "make"], capture_output=True).returncode == 0 or \
               subprocess.run(["pgrep", "-x", "cc1plus"], capture_output=True).returncode == 0
    except OSError:
        busy = True  # cannot tell: assume a peer and stay out of its way
    return max(1, min(8, cpus // 4)) if busy else max(1, min(8, cpus))


def main() -> int:
    default_jobs = _default_jobs()
    ap = argparse.ArgumentParser(description="Syntax-check TUs with clang to catch GCC/clang divergence.")
    ap.add_argument("files", nargs="*", help="explicit files (default: changed since the branch's fork point)")
    ap.add_argument("--all", action="store_true", help="check every TU in the compile database (slow)")
    ap.add_argument("-j", "--jobs", type=int, default=default_jobs,
                    help=f"parallel clang invocations (default {default_jobs}; kept low, "
                         "this box also runs builds)")
    ap.add_argument("--max-header-tus", type=int, default=40,
                    help="cap on TUs checked per changed header (default 40)")
    ap.add_argument("--max-report", type=int, default=5, help="how many failing TUs to print in full")
    ap.add_argument("--warnings", action="store_true", help="also print clang warnings (never fatal)")
    ap.add_argument("--compile-db-dir", default=None, metavar="DIR",
                    help="read *.ccj fragments and *.d dependency files from DIR "
                         "instead of build/obj, and skip compile_commands.json "
                         "(test isolation)")
    ap.add_argument("--make-root", default=None, metavar="DIR",
                    help="ask the Makefile in DIR for compile commands and prerequisites "
                         "instead of this tree's (test isolation)")
    args = ap.parse_args()

    root = REPO_ROOT

    clang_prefix, desc = find_clang()
    if clang_prefix is None:
        print(f"SKIP: clang syntax check -- {desc}")
        return 0

    db = load_db(root, args.compile_db_dir)

    if args.all:
        sel = select_all(db, root)
        firmware_only: list[str] = []
        wanted = len(sel.entries) + len(sel.untrusted)
    else:
        if args.files:
            paths = list(args.files)
        else:
            base, why = default_base(root)
            print(base_line(root, base, why))
            paths = changed_files(root, base)
        paths = [p for p in paths if p.endswith(TU_EXTENSIONS) or p.endswith(HEADER_EXTENSIONS)]
        paths, firmware_only = split_outside_native_build(paths, root)
        try:
            sel = select_entries(paths, args, db, root)
        except PrereqsRefused as exc:
            print("clang syntax check: a changed TU's compile command comes from make, and its "
                  "headers\nneed `make compile-prereqs` in "
                  f"{args.make_root or root}, where another session is building:")
            for line in str(exc).splitlines()[-15:]:
                print(f"    {line}")
            print(f"\nRun the gate again when that build finishes, or set {IGNORE_CLAIM_ENV}=1 "
                  "to make the\nprerequisites beside it.")
            return 1
        except PrereqsFailed as exc:
            print("clang syntax check: a changed TU's compile command comes from make, and "
                  "make could not\nproduce the headers that command reads (patched "
                  f"submodules, libhv, build/generated) in {args.make_root or root}:")
            for line in str(exc).splitlines()[-15:]:
                print(f"    {line}")
            print("\nMake them with:  make compile-prereqs")
            return 1
        wanted = len(paths)

    if firmware_only:
        print(
            f"  note: {len(firmware_only)} changed file(s) under firmware/ are "
            "built by the ESP-IDF job, not by this tree"
        )
    for t in sel.underivable:
        print(f"  note: no compile command for {os.path.relpath(t, root)}: no current "
              "database entry, no make rule, no neighbour to borrow from")
    for n in sel.notes + describe_untrusted(sel.untrusted, root):
        print(f"  note: {n}")

    untrusted = sel.untrusted
    entries = sel.entries
    skipped_note = f", {len(untrusted)} skipped (stale compile command)" if untrusted else ""

    if not entries:
        # A diff with no C++ in it has genuinely nothing to check. A diff whose
        # C++ yielded no command is a different thing wearing the same result:
        # the gate saw nothing and would report a clean bill of health for code
        # it never compiled. A TU skipped as stale is a deliberate decline, not a
        # gap. Only the files no command could be found for mean the gate
        # inspected nothing it was asked to, and they are named.
        unchecked = sel.underivable + sel.unresolved_headers
        if unchecked:
            print(
                f"clang syntax check: 0 of {wanted} changed C++ file(s) could be "
                f"checked{skipped_note}"
            )
            for f in unchecked:
                print(f"    {os.path.relpath(f, root)}")
            print(
                "\nNo compile command could be derived for these, so this gate "
                "verified nothing.\nA TU needs a current compile database entry, a "
                "rule in the Makefile\n(make print-compile-command F=<file>) or a "
                "neighbour in the database to borrow\nfrom; a header needs a TU "
                "known to include it (the build's .d files). A green\nresult that "
                "checked nothing is not a green result."
            )
            return 1
        if wanted:
            print(
                f"clang syntax check: nothing to check in {wanted} changed C++ "
                f"file(s){skipped_note}"
            )
            return 0
        if firmware_only:
            print(
                f"clang syntax check: {len(firmware_only)} changed file(s), all built "
                f"by the firmware job, none checkable here{skipped_note}"
            )
            return 0
        print(f"clang syntax check: no C++ files changed, nothing to check{skipped_note}")
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

    # quality-checks.sh prints only this line when the gate passes, so the skip
    # count has to ride on it or a run that checked almost nothing reads as a
    # clean bill of health.
    print(
        f"\nchecked {len(results)} TU(s): "
        f"{len(results) - len(failed)} clean, {len(failed)} with errors, "
        f"{len(warned)} with warnings (not fatal)"
        f"{skipped_note}"
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
