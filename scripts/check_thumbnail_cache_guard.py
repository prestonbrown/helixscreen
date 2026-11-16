#!/usr/bin/env python3
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: consumers must reach ThumbnailCache through the guarded entry points.
#
# Background: ThumbnailCache exposes two shapes of the same two operations.
#
#   guarded (required in src/):
#       cache.fetch(req, ctx, on_success, on_error);
#       cache.get_if_cached(req);
#
#   legacy (tests only):
#       cache.fetch(api, "relative/path.png", on_success, on_error);
#       cache.get_if_cached("relative/path.png", source_modified);
#
# The ThumbnailLoadContext is the whole point. It carries a generation counter
# plus the caller's lifetime, and fetch() drops on_success when the context is
# no longer valid. Without it an in-flight download that has been superseded
# still lands: the async result overwrites a NEWER thumbnail, and the widget
# shows the previous file's image. That is the bug class the request-shaped API
# was introduced to close, and the legacy overloads reintroduce it silently --
# they compile, they run, and the damage only shows up as an occasional wrong
# picture on a user's printer.
#
# The legacy overloads stay public because tests exercise them deliberately, so
# the compiler cannot enforce this. This gate does.
#
# Approved:
#
#   ThumbnailRequest req;
#   ThumbnailLoadContext ctx = ThumbnailLoadContext::create(lifetime_, &gen_);
#   get_thumbnail_cache().fetch(req, ctx, on_ok, on_err);
#   std::string cached = get_thumbnail_cache().get_if_cached(req);
#
# Banned (without a // THUMB_LEGACY_OK comment):
#
#   get_thumbnail_cache().fetch(api_, path, on_ok, on_err);
#   get_thumbnail_cache().get_if_cached(path);
#   get_thumbnail_cache().get_if_cached(path, mtime);
#
# Per-line opt-out (on the receiver line of the call):
#
#   cache.get_if_cached(path); // THUMB_LEGACY_OK: <reason>
#
# tests/ and thumbnail_cache.cpp itself are never scanned: the implementation
# builds the request forms ON TOP of the legacy ones, and the tests pin the
# legacy behaviour on purpose.
#
# Usage:
#   ./scripts/check_thumbnail_cache_guard.py [files...]
#   ./scripts/check_thumbnail_cache_guard.py --staged-only
#   (no args = scan src/ recursively)

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable

from staged_content import read_index_blobs, staged_paths

from source_strip import blank_cxx

SCAN_DIRS = ("src",)
SCAN_SUFFIXES = (".cpp", ".cc", ".h", ".hpp", ".mm")

# thumbnail_cache.cpp implements the request forms by delegating to the legacy
# ones, so it is the one place the legacy calls are the correct code. Matched on
# the path tail so the exclusion survives being handed an absolute path.
IMPL_PATH_SUFFIX = ("print/thumbnail_cache.cpp",)

OPT_OUT_RE = re.compile(r"//\s*THUMB_LEGACY_OK\b")

# The singleton accessor, plus any identifier the file binds to a ThumbnailCache.
SINGLETON = "get_thumbnail_cache"
CACHE_DECL_RE = re.compile(
    r"\bThumbnailCache\s*[&*]?\s*([A-Za-z_]\w*)\s*(?=[;=,){])")
CACHE_ALIAS_RE = re.compile(
    r"\bauto\s*[&*]?\s*([A-Za-z_]\w*)\s*=\s*[^;]*\bget_thumbnail_cache\s*\(")

# Identifiers this file declares as a request / context. Used to PROVE a call is
# the guarded form; anything unproven is reported rather than assumed safe.
REQUEST_DECL_RE = re.compile(r"\bThumbnailRequest\s+([A-Za-z_]\w*)\s*(?=[;={])")
REQUEST_ALIAS_RE = re.compile(
    r"\bauto\s*[&*]?\s*([A-Za-z_]\w*)\s*=\s*[^;]*\bThumbnailRequest\b")
CONTEXT_DECL_RE = re.compile(r"\bThumbnailLoadContext\s+([A-Za-z_]\w*)\s*(?=[;={])")
CONTEXT_ALIAS_RE = re.compile(
    r"\bauto\s*[&*]?\s*([A-Za-z_]\w*)\s*=\s*[^;]*\bThumbnailLoadContext\s*::")

GUARDED_METHODS = ("fetch", "get_if_cached")


def split_args(text: str, open_idx: int) -> tuple[list[str], int] | None:
    """Split the argument list of the call whose '(' sits at open_idx.

    Returns (args, index_just_past_the_closing_paren), or None if the parens are
    unbalanced (a truncated file). Commas nested inside (), [] or {} do not
    split, so lambda capture lists, brace-init and nested calls stay whole.
    """
    depth = 0
    args: list[str] = []
    start = open_idx + 1
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i])
                break
        elif c == "," and depth == 1:
            args.append(text[start:i])
            start = i + 1
        i += 1
    else:
        return None

    args = [a.strip() for a in args]
    if len(args) == 1 and not args[0]:
        args = []
    return args, i + 1


def collect_names(pattern: re.Pattern[str], text: str) -> set[str]:
    return {m.group(1) for m in pattern.finditer(text)}


def is_request(arg: str, requests: set[str]) -> bool:
    """True only when the argument is PROVABLY a ThumbnailRequest.

    Either an identifier the file declared as one, or an inline construction.
    Unproven means reported -- the gate fails closed, because a legacy call that
    slips through is exactly the silent stale-write bug it exists to prevent.
    """
    if arg in requests:
        return True
    return bool(re.match(r"^ThumbnailRequest\s*[({]", arg))


def is_context(arg: str, contexts: set[str]) -> bool:
    """True only when the argument is PROVABLY a ThumbnailLoadContext.

    This is the sound half of the fetch() check: the legacy overload takes a
    path string in this position, so a proven context rules the legacy overload
    out entirely rather than merely making it unlikely.
    """
    if arg in contexts:
        return True
    return bool(re.search(r"\bThumbnailLoadContext\s*(::|[({])", arg))


def describe(arg: str) -> str:
    """Render an argument for the diagnostic.

    blank_cxx(strings="body") blanks string bodies but keeps the quotes, so a path literal
    arrives here as '"        "'. Printing that verbatim is unreadable; name it
    for what it is instead.
    """
    if arg.startswith('"'):
        return "a string literal"
    return f"'{arg}'"


def check_file(path: Path) -> list[tuple[int, str, str, str]]:
    """Return (line, method, reason, snippet) for each unguarded call."""
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return check_source(raw)


def check_source(raw: str) -> list[tuple[int, str, str, str]]:
    """check_file's body, operating on already-sourced text - disk, or the
    STAGED blob a --staged-only caller reads instead."""
    if SINGLETON not in raw and "ThumbnailCache" not in raw:
        return []

    text = blank_cxx(raw, strings="body")
    raw_lines = raw.splitlines()

    receivers = collect_names(CACHE_DECL_RE, text) | collect_names(CACHE_ALIAS_RE, text)
    requests = collect_names(REQUEST_DECL_RE, text) | collect_names(REQUEST_ALIAS_RE, text)
    contexts = collect_names(CONTEXT_DECL_RE, text) | collect_names(CONTEXT_ALIAS_RE, text)

    # A call on a ThumbnailCache: either straight off the singleton accessor or
    # off an identifier bound to one. Member access spellings (`.`/`->`) both
    # count, and `this->` / `a.b.` prefixes are absorbed by the receiver group.
    recv_alt = "|".join(sorted({re.escape(SINGLETON) + r"\s*\(\s*\)"} |
                               {r"\b" + re.escape(r) for r in receivers}))
    call_re = re.compile(
        r"(?:" + recv_alt + r")\s*(?:\.|->)\s*(" + "|".join(GUARDED_METHODS) + r")\s*\(")

    findings: list[tuple[int, str, str, str]] = []
    for m in call_re.finditer(text):
        method = m.group(1)
        parsed = split_args(text, m.end() - 1)
        if parsed is None:
            continue
        args, _ = parsed
        line = text.count("\n", 0, m.start()) + 1

        if method == "get_if_cached":
            if len(args) >= 2:
                reason = ("legacy get_if_cached(relative_path, source_modified) -- "
                          "no ThumbnailLoadContext")
            elif len(args) == 1 and not is_request(args[0], requests):
                reason = (f"get_if_cached(...) argument {describe(args[0])} is not a "
                          "ThumbnailRequest declared in this file")
            else:
                continue
        else:  # fetch
            if len(args) < 2:
                continue  # not one of these overloads
            if not is_context(args[1], contexts):
                reason = (f"fetch(...) 2nd argument {describe(args[1])} is not a "
                          "ThumbnailLoadContext -- this is the unguarded overload")
            elif not is_request(args[0], requests):
                reason = (f"fetch(...) 1st argument {describe(args[0])} is not a "
                          "ThumbnailRequest declared in this file")
            else:
                continue

        snippet = raw_lines[line - 1].strip() if line - 1 < len(raw_lines) else ""
        if OPT_OUT_RE.search(snippet):
            continue
        findings.append((line, method, reason, snippet))

    return findings


def is_excluded(path: Path) -> bool:
    posix = path.as_posix()
    if any(posix.endswith(s) for s in IMPL_PATH_SUFFIX):
        return True
    # tests exercise the legacy overloads on purpose.
    return "tests" in path.parts


def iter_targets(explicit: Iterable[str], check_disk: bool = True) -> Iterable[Path]:
    """`check_disk=False` for a staged path list: it already came from the
    index (`git diff --cached`), so a `Path.is_file()` gate would drop a
    violation that is staged and then deleted from the working tree without
    being staged for deletion - exactly the content the commit will still
    carry."""
    explicit = list(explicit)
    if explicit:
        for f in explicit:
            p = Path(f)
            if p.suffix in SCAN_SUFFIXES and not is_excluded(p) and (not check_disk or p.is_file()):
                yield p
        return
    for d in SCAN_DIRS:
        root = Path(d)
        if not root.is_dir():
            continue
        for suffix in SCAN_SUFFIXES:
            for p in root.rglob(f"*{suffix}"):
                if not is_excluded(p):
                    yield p


def staged_files() -> list[str]:
    return staged_paths(suffixes=SCAN_SUFFIXES)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="*", help="files to scan (default: src/)")
    ap.add_argument("--staged-only", action="store_true", help="scan staged files only")
    args = ap.parse_args()

    targets = staged_files() if args.staged_only else args.files
    if args.staged_only and not targets:
        return 0

    total = 0
    paths = sorted(set(iter_targets(targets, check_disk=not args.staged_only)))
    if args.staged_only:
        # The set above is the staged diff; the CONTENT has to come from the
        # same place - the index, not whatever a re-read of the path finds on
        # disk, which may have been reverted after staging a violation.
        staged_src = dict(read_index_blobs([str(p) for p in paths]))
        findings = ((path, hit) for path in paths
                    for hit in check_source(staged_src.get(str(path), "")))
    else:
        findings = ((path, hit) for path in paths for hit in check_file(path))
    for path, (line, method, reason, snippet) in findings:
        total += 1
        print(f"{path}:{line}: unguarded ThumbnailCache::{method}()")
        print(f"    {reason}")
        if snippet:
            print(f"    {snippet}")

    if total:
        print()
        print(f"❌ {total} site(s) call ThumbnailCache without a ThumbnailLoadContext.")
        print()
        print("   The context carries the caller's lifetime plus a generation counter,")
        print("   and fetch() drops on_success once it is stale. Without it a superseded")
        print("   download still lands and overwrites a NEWER thumbnail.")
        print()
        print("   Fix: build a ThumbnailRequest and a ThumbnailLoadContext, then call")
        print("        cache.fetch(req, ctx, on_success, on_error)")
        print("        cache.get_if_cached(req)")
        print("   See include/thumbnail_cache.h and ThumbnailLoadContext::create().")
        print("   Deliberate exception: append  // THUMB_LEGACY_OK: <reason>")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
