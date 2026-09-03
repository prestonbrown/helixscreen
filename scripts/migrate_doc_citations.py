#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One-shot converter: `path:NNN` citations become `path#anchor` citations.
#
# For each cited line it finds the innermost NAMED SCOPE containing that line and
# proposes it as an anchor. A one-line local variable declaration is not a scope:
# a citation inside a function means "this function", and anchoring to a local
# named `err` collides with every other `err` in the file. When a name alone
# resolves to more than one place the cited line's own text is appended as a
# snippet segment, so the proposal is unambiguous by construction.
#
# Every proposal is re-resolved before it is offered. Anything that still will
# not resolve is reported rather than written.
#
# Usage:
#   migrate_doc_citations.py --report [PATH...]   # writes nothing
#   migrate_doc_citations.py --apply  [PATH...]   # rewrites the docs

import argparse
import os
import re
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from doc_anchors import (  # noqa: E402
    Ambiguous,
    Citation,
    NotFound,
    Region,
    Segment,
    _FENCE_RE,
    _fence_advance,
    definitions,
    format_citation,
    resolve,
    resolve_segments,
)

# A backticked `path:NNN` or `path:NNN-MMM` citation.
LINE_CITE_RE = re.compile(r"`([A-Za-z0-9_./-]+\.[A-Za-z0-9]+):(\d+)(?:-\d+)?`")

# A scope worth anchoring to spans more than its own line. A one-line match is a
# local declaration, which is a name but not a place.
_MIN_SCOPE_SPAN = 2


@dataclass(frozen=True)
class Proposal:
    citation: str
    confidence: str  # automatic | needs-snippet | file-level | ambiguous
    note: str = ""


def scope_chain(lines, ext, target):
    """Names of the nested SCOPES containing line index `target`.

    Regions spanning a single line are skipped: they are declarations, and a
    citation inside a function is about the function, not about whichever local
    happens to sit on the cited line.
    """
    chain = []
    region = Region(0, len(lines))
    while True:
        containing = [
            (name, r)
            for name, r in definitions(lines, region, ext)
            if r.start <= target < r.end
            and not (r.start == region.start and r.end == region.end)
            and (r.end - r.start) >= _MIN_SCOPE_SPAN
        ]
        if not containing:
            break
        name, region = min(containing, key=lambda nr: nr[1].end - nr[1].start)
        chain.append(name)
    return chain


def _declared_on(lines, ext, target):
    """The name this line itself declares, when the line IS the declaration."""
    for name, r in definitions(lines, Region(target, target + 1), ext):
        if r.start == target:
            return name
    return None


def propose(path, line_number, repo_root="."):
    """Propose an anchor citation for `path:line_number`."""
    full = os.path.join(str(repo_root), path)
    try:
        with open(full, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError as exc:
        return Proposal(path, "file-level", f"unreadable: {exc}")
    ext = os.path.splitext(path)[1]
    target = line_number - 1
    if target >= len(lines):
        return Proposal(path, "file-level", "cited line is past end of file")

    # A citation that lands ON a declaration means that declaration.
    own = _declared_on(lines, ext, target)
    candidates = []
    if own:
        candidates.append([own])
    chain = scope_chain(lines, ext, target)
    if chain:
        candidates.append(chain)
        if own:
            candidates.append(chain + [own])
    if not candidates:
        return Proposal(path, "file-level", "no named scope contains this line")

    # An anchor names a PLACE, not a line: a citation inside a function anchors
    # to the function, whose own line differs from the cited one. The proposal is
    # right when the region it resolves to CONTAINS the cited line.
    for names in candidates:
        segs = tuple(Segment(n, False) for n in names)
        text = format_citation(Citation(path=path, segments=segs))
        try:
            region = resolve_segments(lines, segs, ext)
        except (NotFound, Ambiguous, ValueError):
            continue
        if region.start <= target < region.end:
            return Proposal(text, "automatic")

    # Nothing resolved to the right line on a name alone; narrow with the text.
    snippet = lines[target].strip()
    base = chain or ([own] if own else [])
    if not snippet:
        return Proposal(path, "ambiguous", "cited line is blank, cannot disambiguate")
    with_snip = Citation(
        path=path,
        segments=tuple(Segment(n, False) for n in base) + (Segment(snippet, True),),
    )
    text = format_citation(with_snip)
    try:
        region = resolve_segments(lines, with_snip.segments, ext)
    except (NotFound, Ambiguous, ValueError) as exc:
        return Proposal(path, "ambiguous", str(exc))
    if region.start == target:
        return Proposal(text, "needs-snippet", "name alone did not pin the line")
    return Proposal(path, "ambiguous", "snippet resolved to a different line")


def iter_line_citations(paths):
    """(doc, lineno, match) for every `path:NNN` citation outside a fence."""
    out = []
    for path in paths:
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                raw = fh.read()
        except OSError:
            continue
        marker = opened_at = None
        for lineno, line in enumerate(raw.split("\n"), start=1):
            marker, opened_at, is_content = _fence_advance(line, marker, opened_at, lineno)
            if not is_content:
                continue
            for m in LINE_CITE_RE.finditer(line):
                out.append((path, lineno, m))
    return out


_RESOLVED = None


def _resolved_path(doc, cited):
    """The on-disk file a citation names.

    The existing sidecar already carries this mapping in its resolved-path
    column, computed by the gate that has been maintaining these citations.
    A bare basename or a doc-relative path resolves there and would not
    resolve against the repo root alone.
    """
    global _RESOLVED
    if _RESOLVED is None:
        _RESOLVED = {}
        try:
            with open("scripts/doc_cite_anchors.tsv", encoding="utf-8") as fh:
                for row in fh:
                    if row.startswith("#") or not row.strip():
                        continue
                    parts = row.rstrip("\n").split("\t")
                    if len(parts) >= 4:
                        _RESOLVED[(parts[0], parts[1])] = parts[3]
        except OSError:
            pass
    return _RESOLVED.get((doc, cited)) or cited


def _proposal_for(match, repo_root, doc=None):
    cited = match.group(1)
    real = _resolved_path(doc, cited) if doc else cited
    p = propose(real, int(match.group(2)), repo_root=repo_root)
    # keep the citation spelled against the file it actually names
    return p


def report(paths, repo_root="."):
    rows, counts = [], {}
    for doc, lineno, m in iter_line_citations(paths):
        p = _proposal_for(m, repo_root, doc)
        counts[p.confidence] = counts.get(p.confidence, 0) + 1
        rows.append((doc, lineno, m.group(0).strip("`"), p))
    return rows, counts


def apply_to(doc, repo_root="."):
    """Rewrite one doc's line citations. Returns (changed, skipped)."""
    with open(doc, encoding="utf-8") as fh:
        original = fh.read()
    out, skipped, changed = [], [], 0
    marker = opened_at = None
    for lineno, line in enumerate(original.split("\n"), start=1):
        marker, opened_at, is_content = _fence_advance(line, marker, opened_at, lineno)
        if not is_content:
            out.append(line)
            continue

        def replace(m):
            nonlocal changed
            p = _proposal_for(m, repo_root, doc)
            if p.confidence == "ambiguous":
                skipped.append((lineno, m.group(0), p.note))
                return m.group(0)
            changed += 1
            return f"`{p.citation}`"

        out.append(LINE_CITE_RE.sub(replace, line))
    text = "\n".join(out)
    if text != original:
        with open(doc, "w", encoding="utf-8") as fh:
            fh.write(text)
    return changed, skipped


def _default_targets():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import check_doc_refs as refs

    seen, out = set(), []
    for t in list(refs.scan_targets()) + list(refs.scan_devel_targets(["docs/devel"])):
        t = str(t)
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", nargs="*", metavar="PATH")
    ap.add_argument("--apply", nargs="*", metavar="PATH")
    args = ap.parse_args(argv)

    if args.report is not None:
        paths = args.report or _default_targets()
        rows, counts = report(paths)
        print("doc\tdoc_line\told\tnew\tconfidence\tnote")
        for doc, lineno, old, p in rows:
            print(f"{doc}\t{lineno}\t{old}\t{p.citation}\t{p.confidence}\t{p.note}")
        print(f"\n# {len(rows)} citation(s)", file=sys.stderr)
        for k in ("automatic", "needs-snippet", "file-level", "ambiguous"):
            if counts.get(k):
                print(f"#   {k:14} {counts[k]}", file=sys.stderr)
        return 0

    if args.apply is not None:
        paths = args.apply or _default_targets()
        total, all_skipped = 0, []
        for doc in paths:
            changed, skipped = apply_to(doc)
            total += changed
            all_skipped += [(doc, *s) for s in skipped]
            if changed:
                print(f"  {doc}: {changed}")
        print(f"\nrewrote {total} citation(s)")
        for doc, lineno, text, note in all_skipped:
            print(f"  SKIPPED {doc}:{lineno} {text} - {note}")
        return 1 if all_skipped else 0

    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
