#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Merge the build's per-TU compile-command fragments into compile_commands.json.

What a fragment is
------------------
``emit-compile-command`` in ``mk/rules.mk`` writes one ``.ccj`` next to every object
file, recording the exact command line that produced it. Merging them is how this
tree gets a compile database without a second build under ``bear``.

Why the merge has to select rather than concatenate
---------------------------------------------------
A fragment is a byproduct of compiling, so it is only rewritten when the object is.
Two consequences make a raw concatenation actively wrong:

*Several trees, one source.* ``build/obj``, ``build/obj-O0``, ``build/obj-asan`` and
``build/obj-tsan`` each hold their own object for the same ``.cpp``, so each emits its
own fragment. A concatenation therefore lists one file many times, and a consumer that
keys on the file path keeps whichever came last.

*Frozen flags.* Make rebuilds an object when a prerequisite is newer, not when the
command line changes, so a version bump or a new ``-DHELIX_HAS_*`` leaves untouched
trees recording flags the build no longer passes. Replaying such a command yields
diagnostics about the command - ``unknown type name`` for a type behind a feature
macro - that read exactly like findings about the code.

So entries are grouped by source file and the freshest is kept: the one stamped with
the tree's current ``-DHELIX_VERSION``, then the most recently written fragment.
Entries whose source no longer exists are dropped entirely; their command names a path
that is gone, and clang answers it with ``no such file or directory``.

``check_clang_diagnostics.py`` imports the selection helpers here so the clang gate and
the database an editor reads agree on which entry describes a file today.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# -DHELIX_VERSION=, not -DHELIX_VERSION_MAJOR=. The recorded value carries the quote
# characters the compiler needs, and emit-compile-command shell-quotes the token on
# top of them, so the stamp arrives as '"1.1.0"'. Match any run of quotes: one that
# accepts a single quote character reads every real entry as carrying no stamp.
VERSION_DEFINE_RE = re.compile(r'-DHELIX_VERSION=["\']*([0-9][^"\'\s]*)')

# Rank of the version stamp on an entry, high to low. An entry carrying no stamp at
# all is not evidence of staleness: submodule TUs (lib/lvgl, lib/libhv, generated
# font data) are built by their own recipes and never see VERSION_DEFINES.
_RANK_CURRENT = 2
_RANK_UNSTAMPED = 1
_RANK_STALE = 0


def current_version(root: str = REPO_ROOT) -> str | None:
    """The version every command line the build issues right now carries."""
    try:
        with open(os.path.join(root, "VERSION.txt")) as fh:
            return fh.read().strip() or None
    except OSError:
        return None


def entry_command_text(entry: dict) -> str:
    args = entry.get("arguments")
    if args:
        return " ".join(str(a) for a in args)
    return str(entry.get("command", ""))


def recorded_version(entry: dict) -> str | None:
    """The version stamped on this entry's command line, if it carries one."""
    m = VERSION_DEFINE_RE.search(entry_command_text(entry))
    return m.group(1) if m else None


def version_rank(entry: dict, current: str | None) -> int:
    v = recorded_version(entry)
    if v is None:
        return _RANK_UNSTAMPED
    if current is None or v == current:
        return _RANK_CURRENT
    return _RANK_STALE


def is_stale(entry: dict, current: str | None) -> bool:
    """True when this command was recorded by a build other than the current one.

    Its flag set may therefore not describe how the file is compiled today, which
    makes any diagnostic from replaying it a statement about the command rather than
    about the code.
    """
    return version_rank(entry, current) == _RANK_STALE


def entry_source_path(entry: dict, default_dir: str = REPO_ROOT) -> str:
    f = entry.get("file", "")
    if not f:
        return ""
    directory = entry.get("directory", default_dir)
    return f if os.path.isabs(f) else os.path.join(directory, f)


def freshness(entry: dict, current: str | None) -> tuple[int, float]:
    """Sort key choosing between several entries for one source file.

    The version stamp dominates: an entry matching the tree wins outright, however
    long ago its fragment was written. Recency only breaks ties between entries the
    stamp cannot separate - the parallel object trees, and unstamped submodule TUs.
    """
    return (version_rank(entry, current), float(entry.get("_mtime", 0.0)))


def load_fragments(build_dir: str) -> tuple[list[dict], int]:
    """(usable ``.ccj`` entries tagged with their write times, unreadable count).

    A fragment is written by a shell redirect inside the compile recipe, so one
    can be caught half-written: a build killed mid-compile leaves its in-flight
    fragments truncated, and so does reading a peer's build in the act. Such a
    fragment is not readable JSON, and neither is a whole ``.ccj`` tree a
    filesystem has damaged.

    The count is returned rather than swallowed because the database is rewritten
    from what parsed. Every fragment dropped here is a source file that loses its
    flags in ``compile_commands.json``, which surfaces only as clangd and
    ``syntax_check.py`` answering questions about that file wrongly, with nothing
    pointing back at this step.
    """
    out: list[dict] = []
    unreadable = 0
    for frag in glob.glob(os.path.join(build_dir, "**", "*.ccj"), recursive=True):
        try:
            with open(frag) as fh:
                entry = json.load(fh)
            # Valid JSON that is not an object - `null`, a number, a list - is a
            # fragment too, and rejecting it before the tag keeps a scalar from
            # taking item assignment.
            if not isinstance(entry, dict) or not entry.get("file"):
                unreadable += 1
                continue
            entry["_mtime"] = os.path.getmtime(frag)
        except (OSError, ValueError):
            unreadable += 1
            continue
        out.append(entry)
    return out, unreadable


def select_freshest(entries: list[dict], current: str | None,
                    root: str = REPO_ROOT) -> dict[str, dict]:
    """source realpath -> the one entry that describes how it is built today.

    Entries whose source file is gone are dropped rather than ranked: no command can
    describe a file that does not exist.
    """
    best: dict[str, dict] = {}
    for entry in entries:
        path = entry_source_path(entry, root)
        if not path or not os.path.exists(path):
            continue
        key = os.path.realpath(path)
        incumbent = best.get(key)
        if incumbent is None or freshness(entry, current) > freshness(incumbent, current):
            best[key] = entry
    return best


def merge(build_dir: str, root: str = REPO_ROOT) -> tuple[list[dict], dict[str, int]]:
    """(entries for compile_commands.json, counts for the build log)."""
    fragments, unreadable = load_fragments(build_dir)
    current = current_version(root)
    chosen = select_freshest(fragments, current, root)
    entries = [{k: v for k, v in e.items() if not k.startswith("_")}
               for _, e in sorted(chosen.items())]
    stats = {
        "fragments": len(fragments),
        "unreadable": unreadable,
        "entries": len(entries),
        "stale": sum(1 for e in chosen.values() if is_stale(e, current)),
    }
    return entries, stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", default="build", help="root to scan for .ccj fragments")
    ap.add_argument("--output", default="compile_commands.json", help="database to write")
    ap.add_argument("--quiet", action="store_true", help="write the file, print nothing")
    args = ap.parse_args()

    entries, stats = merge(args.build_dir)

    unreadable = stats["unreadable"]
    if unreadable:
        seen = stats["fragments"] + unreadable
        note = (f"{unreadable} of {seen} .ccj fragments under {args.build_dir} "
                f"are unreadable")
        # A fragment is written by a shell redirect, so a build running right now
        # holds a few mid-write while the thousands it already finished sit
        # complete beside them - and a build killed mid-compile leaves behind the
        # same bounded handful. Once the unreadable ones outnumber the readable,
        # no writer explains it, and rewriting the database from the survivors
        # replaces it with a minority of the tree.
        if unreadable > stats["fragments"]:
            print(f"{note}; not writing {args.output}", file=sys.stderr)
            return 1
        # Below that the readable majority is still worth merging. Say what was
        # lost anyway: the database is rewritten from what parsed, so a file
        # whose only fragment was unreadable simply has no entry, and the next
        # sign of it is clangd answering about that file with no flags.
        print(f"{note}; the files they describe may be missing from {args.output}",
              file=sys.stderr)

    if not entries:
        if stats["fragments"] == 0:
            # A fragment is a byproduct of compiling; zero of them means
            # nothing recompiled (a link-only run against cached objects), and
            # the database already on disk still describes the tree. This
            # script runs as the last step of every build target, so failing
            # here fails a build that succeeded.
            if not args.quiet:
                print(f"no fragments under {args.build_dir} (nothing recompiled); "
                      f"leaving {args.output} untouched")
            return 0
        # Fragments exist but every source they name is gone: writing the
        # database would wipe it, so refuse rather than truncate.
        if not args.quiet:
            print(f"{stats['fragments']} fragments under {args.build_dir} name no "
                  f"existing source; not writing {args.output}", file=sys.stderr)
        return 1

    # Written through a sibling and renamed over: os.replace is atomic, so a
    # reader never sees a half-written database, and an interrupt during the
    # dump leaves the previous one in place instead of a truncated file.
    tmp = args.output + ".tmp"
    try:
        with open(tmp, "w") as fh:
            json.dump(entries, fh, indent=2)
        os.replace(tmp, args.output)
    except OSError as exc:
        print(f"cannot write {args.output}: {exc}", file=sys.stderr)
        return 1
    finally:
        if os.path.isfile(tmp):
            os.remove(tmp)

    if not args.quiet:
        msg = f"{stats['entries']} entries from {stats['fragments']} fragments"
        if stats["stale"]:
            msg += (f"; {stats['stale']} recorded by an older build "
                    f"(rebuild to refresh)")
        print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
