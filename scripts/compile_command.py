#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which compile command describes a source file today.

scripts/check_clang_diagnostics.py (the clang gate) and scripts/syntax_check.py
both replay a file's compile command with -fsyntax-only. They ask this module for
that command, so the two cannot disagree about a file's flags.

The order
---------
1. A compile-database entry stamped with the tree's current version
   (``merge_compile_commands.select_freshest`` picks it among the database and the
   build's ``.ccj`` fragments).
2. The command make prints for the file (``make print-compile-command F=...``).
   It expands the same ``*_COMPILE`` function the object rule runs, so it is
   today's build by construction, and it needs no prior build: a ``--no-build``
   worktree, or one whose copied database was recorded at another version, gets
   real flags from it. Replaying it needs the headers the object rules make
   first (patched submodules, libhv's installed headers, build/generated), so the
   first file resolved this way runs ``make compile-prereqs`` once.
3. Only when make has no rule for the file: its own entry recorded at another
   version, kept so a caller can say it was there. The clang gate never lets one
   fail it: its flags may not be today's, and a diagnostic from replaying it is
   about the command, not the code.
4. With no rule and no entry of its own: a current entry for another file in the
   same directory, borrowed. A brand-new file in a directory the database knows
   compiles with its neighbours' flags.

Anything else has no flags at all, and the caller says so by name.
"""

from __future__ import annotations

import glob
import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass

from merge_compile_commands import (
    REPO_ROOT,
    current_version,
    entry_argv,
    entry_source_path,
    is_stale,
    select_freshest,
)

FRESH = "db"
MAKE = "make"
SIBLING = "sibling"
STALE = "stale"

# What ``make print-compile-command`` needs; 4.2 is the Makefile's own floor.
_MAKE_FLOOR = (4, 2)
_MAKE_VERSION_RE = re.compile(r"GNU Make (\d+)\.(\d+)")


@dataclass
class Choice:
    """The command chosen for one file, and where it came from.

    ``source`` is FRESH, MAKE, SIBLING or STALE; ``entry`` is a compile-database
    shaped dict (``directory``, ``file``, ``command`` or ``arguments``).
    ``borrowed_from`` names the sibling whose flags a SIBLING choice uses.
    """

    entry: dict
    source: str
    borrowed_from: str = ""


def load_db(root: str = REPO_ROOT, frag_root: str | None = None) -> dict[str, dict]:
    """file realpath -> the freshest entry for it, from the database and fragments.

    Both are read and ranked together: ``compile_commands.json`` is regenerated
    from the fragments only at certain build steps, so it lags them, and a test TU
    can be missing from it while its fragment is current.

    frag_root replaces build/obj as the fragment root and skips the JSON, which
    lets a test build a database that cannot leak into or depend on this tree.
    """
    entries: list[dict] = []
    cc_json = os.path.join(root, "compile_commands.json")
    if frag_root is None and os.path.exists(cc_json):
        try:
            with open(cc_json) as fh:
                entries.extend(e for e in json.load(fh) if isinstance(e, dict))
        except (OSError, ValueError):
            pass

    frag_dir = frag_root if frag_root else os.path.join(root, "build", "obj")
    for frag in glob.glob(os.path.join(frag_dir, "**", "*.ccj"), recursive=True):
        try:
            with open(frag) as fh:
                entry = json.load(fh)
            if not isinstance(entry, dict):
                continue
            entry["_mtime"] = os.path.getmtime(frag)
        except (OSError, ValueError):
            continue
        entries.append(entry)

    return select_freshest(entries, current_version(root), root)


def _gnu_make_version(prog: str) -> tuple[int, int] | None:
    try:
        out = subprocess.run([prog, "--version"], capture_output=True, text=True,
                             timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = _MAKE_VERSION_RE.search(out)
    return (int(m.group(1)), int(m.group(2))) if m else None


def make_program() -> str | None:
    """A GNU make new enough for this Makefile: ``make`` first, else ``gmake``.

    ``make`` first because on a machine with a jobpool it is the shim every
    build draws from; a Mac's ``make`` is Apple's 3.81 and only ``gmake`` works.
    """
    for prog in ("make", "gmake"):
        if shutil.which(prog) and (_gnu_make_version(prog) or (0, 0)) >= _MAKE_FLOOR:
            return prog
    return None


def make_commands(paths: list[str], make_root: str, make: str | None = None) -> dict[str, dict]:
    """realpath -> entry, for each of paths that a rule in make_root compiles.

    One make run for all of them: parsing the Makefile costs a couple of seconds,
    and -k keeps a file with no rule from hiding the others' lines. Each line ends
    in ``-c <path as passed>``, which is how it is matched back to its file.
    """
    make = make or make_program()
    if not make or not paths or not os.path.isfile(os.path.join(make_root, "Makefile")):
        return {}
    rel_of: dict[str, str] = {}
    for p in paths:
        rel = os.path.relpath(p, make_root)
        if not rel.startswith(".."):
            rel_of[rel] = p
    if not rel_of:
        return {}
    try:
        proc = subprocess.run(
            [make, "-s", "-k", "--no-print-directory", "-C", make_root,
             "print-compile-command", "F=" + " ".join(sorted(rel_of))],
            capture_output=True, text=True, timeout=600,
        )
    except (OSError, subprocess.SubprocessError):
        return {}
    found: dict[str, dict] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        head, sep, rel = line.rpartition(" -c ")
        if not sep or rel not in rel_of:
            continue
        found[rel_of[rel]] = {
            "directory": make_root,
            "file": rel_of[rel],
            "command": line,
        }
    return found


def run_prereqs(make_root: str, make: str | None = None) -> tuple[bool, str]:
    """(made, output) for ``make compile-prereqs`` in make_root."""
    make = make or make_program()
    if not make:
        return False, "no GNU make 4.2 or newer on PATH"
    try:
        proc = subprocess.run(
            [make, "-s", "--no-print-directory", "-C", make_root, "compile-prereqs"],
            capture_output=True, text=True, timeout=1800,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return False, str(exc)
    return proc.returncode == 0, (proc.stdout + proc.stderr).strip()


def _borrow(sibling: dict, path: str, root: str) -> dict:
    """sibling's command with its source argument replaced by path."""
    directory = sibling.get("directory", root)
    own = os.path.realpath(entry_source_path(sibling, root))
    argv = [path if os.path.realpath(os.path.join(directory, a)) == own else a
            for a in entry_argv(sibling)]
    return {"directory": directory, "file": path, "arguments": argv}


class PrereqsFailed(RuntimeError):
    """``make compile-prereqs`` failed, so no make-derived command can be replayed."""


def resolve(paths: list[str], db: dict[str, dict], root: str = REPO_ROOT,
            make_root: str | None = None, make: str | None = None,
            prereqs: bool = True) -> dict[str, Choice | None]:
    """realpath -> the Choice for it (see the module docstring), or None.

    paths are realpaths. make_root is where the Makefile is asked (default root).
    With prereqs, the first make-derived choice runs ``make compile-prereqs`` and a
    failure raises PrereqsFailed, carrying make's output: replaying a command
    whose headers are missing reports their absence as findings about the code.
    """
    # Realpath, as paths are: a /var/folders make root under a /private/var
    # realpath would otherwise put every file outside it.
    make_root = os.path.realpath(make_root or root)
    current = current_version(root)
    out: dict[str, Choice | None] = {}
    need_make: list[str] = []
    for p in paths:
        entry = db.get(p)
        if entry is not None and not is_stale(entry, current):
            out[p] = Choice(entry, FRESH)
        else:
            need_make.append(p)

    derived = make_commands(need_make, make_root, make) if need_make else {}
    if derived and prereqs:
        made, output = run_prereqs(make_root, make)
        if not made:
            raise PrereqsFailed(output)

    by_dir: dict[str, list[str]] = {}
    for key, entry in db.items():
        if not is_stale(entry, current):
            by_dir.setdefault(os.path.dirname(key), []).append(key)

    for p in need_make:
        if p in derived:
            out[p] = Choice(derived[p], MAKE)
            continue
        entry = db.get(p)
        if entry is not None:
            out[p] = Choice(entry, STALE)
            continue
        siblings = sorted(k for k in by_dir.get(os.path.dirname(p), ()) if k != p)
        if siblings:
            sib = db[siblings[0]]
            out[p] = Choice(_borrow(sib, p, root), SIBLING,
                            os.path.relpath(entry_source_path(sib, root), root))
            continue
        out[p] = None
    return out
