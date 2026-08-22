#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generator: turn the architecture guide's backticked file citations into
# clickable links.
#
#   `src/printer/printer_state.cpp:622`
#     -> [`src/printer/printer_state.cpp:622`](../../../src/printer/printer_state.cpp#L622)
#
# The citation text is the source of truth; the URL is derived from it on every
# run. Nobody maintains a URL by hand: rewriting is idempotent and unconditional,
# so a link whose target was renamed, hand-edited, or copy-pasted from another
# line is repaired the next time this runs. Fix the text, regenerate, done.
#
# That is also why the link text keeps the exact backticked citation instead of
# prose: check_doc_refs.py reads those citations to prove the path resolves, the
# line is inside the file, and the symbol named beside it is still there. This
# generator and that gate share PATH_RE and EXEMPT_SUBSTRINGS by import so the
# two can never disagree about what counts as a citation.
#
# Usage:
#   gen_doc_links.py                 # rewrite the architecture guide in place
#   gen_doc_links.py --check         # exit 1 if any file would change (CI)
#   gen_doc_links.py --diff          # --check plus the first changed lines
#   gen_doc_links.py PATH [PATH...]  # limit to specific .md files/directories
#
# Not linked, deliberately:
#   - anything under lib/ — submodules render as gitlinks on GitHub, and CI
#     checks out without them, so check_doc_refs' link check would go red
#   - EXEMPT_SUBSTRINGS paths (build outputs, runtime-written config) — absent
#     from a clean checkout for the same reason
#   - ambiguous bare basenames (README.md) — a guess is worse than a code span
#   - a doc citing itself

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_doc_refs as gate  # noqa: E402  (PATH_RE / EXEMPT_SUBSTRINGS)

# Default scope: the architecture guide — the router plus the chapter series.
DEFAULT_TARGETS = ('docs/devel/ARCHITECTURE.md', 'docs/devel/architecture')

# Not walked, so nothing in them can ever become a link target. `lib` is the
# load-bearing one: submodules render as gitlinks on GitHub and CI checks out
# without them, so a link into lib/ would be dead in both places. Leaving them
# out of the index is also what keeps the walk fast.
SKIP_DIRS = {'.git', '.worktrees', 'build', 'node_modules', '.venv', 'venv', 'lib'}

# Tie-break for a bare basename that matches more than one file. A doc saying
# `theme_manager.cpp` means the one that ships, not the firmware audit override
# beside it, so the first root with any candidate wins — but only if it holds
# exactly one. `app_layout.xml` (ui_xml/ and ui_xml/portrait/) stays a plain
# span, because there the ambiguity is real and a guess would be wrong half the
# time.
PRIMARY_ROOTS = ('src/', 'include/', 'ui_xml/', 'scripts/', 'assets/', 'mk/',
                 'tests/')

# A doubled-backtick span is how markdown shows a literal backticked token —
# the "write `src/foo.cpp:12`, not a link" instruction in the guide's own
# README. Linking inside one would print the link markup at the reader.
DISPLAY_SPAN_RE = re.compile(r'``.+?``')

# One citation, either already wrapped in a link or bare. The linked form is
# matched first so a rewrite re-derives its URL rather than nesting a new link
# inside the old one; the bare form's lookbehind keeps it from matching the
# text of a link the first alternative failed to consume.
_REF = gate.PATH_RE.pattern[1:-1]      # drop PATH_RE's own backtick delimiters
CITATION_RE = re.compile(
    r'\[`' + _REF + r'`\]\([^)\s]*\)'
    r'|(?<!\[)`' + _REF + '`')


def repo_index(root='.'):
    """Every linkable repo file, plus lookup maps for partial citations.

    Docs cite paths three ways: repo-rooted (`src/printer/printer_state.cpp`),
    partial (`system/config.cpp`), and bare (`THREADING.md`). The last two only
    resolve to a link when exactly one file matches — see module header.
    """
    files = set()
    for dirpath, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith('.')]
        rel = os.path.relpath(dirpath, root)
        rel = '' if rel == '.' else rel
        for n in names:
            files.add(os.path.join(rel, n) if rel else n)
    by_basename = {}
    for p in files:
        by_basename.setdefault(p.rsplit('/', 1)[-1], []).append(p)
    return files, by_basename


def _prefer_primary(matches):
    """Narrow same-basename candidates to one shipping source root, if it can."""
    for root in PRIMARY_ROOTS:
        tier = [m for m in matches if m.startswith(root)]
        if tier:
            return tier
    return matches


def resolve(path, doc_dir, files, by_basename):
    """The repo-relative file a citation names, or None if it is not linkable."""
    if any(c in path for c in gate.PLACEHOLDER_CHARS):
        return None
    if any(s in path for s in gate.EXEMPT_SUBSTRINGS):
        return None

    hit = None
    for cand in (path, os.path.normpath(os.path.join(doc_dir, path))):
        if cand in files:
            hit = cand
            break
    if hit is None:
        if '/' in path:
            matches = [p for p in files if p.endswith('/' + path)]
        else:
            matches = by_basename.get(path, [])
        if len(matches) > 1:
            matches = _prefer_primary(matches)
        if len(matches) != 1:          # zero or ambiguous — leave it a code span
            return None
        hit = matches[0]

    return hit


def link_for(ref, doc, files, by_basename):
    """`[`ref`](url)` for a citation, or None to leave it as a plain span."""
    path, _, suffix = ref.partition(':')
    doc_dir = os.path.dirname(doc)
    target = resolve(path, doc_dir, files, by_basename)
    if target is None or target == doc:
        return None
    url = os.path.relpath(target, doc_dir) if doc_dir else target
    if suffix.isdigit():               # a `:sym()` suffix gets no line anchor
        url += '#L' + suffix
    return '[`%s`](%s)' % (ref, url)


def rewrite(text, doc, files, by_basename):
    """Link every citation outside a fenced code block; return (text, count).

    Fenced blocks are skipped whole: a mermaid node label or a shell sample is
    not prose, and a link inside one would render as literal brackets.
    """
    out = []
    made = 0
    fenced = False
    for line in text.split('\n'):
        if line.lstrip().startswith('```'):
            fenced = not fenced
            out.append(line)
            continue
        if fenced:
            out.append(line)
            continue

        display = [m.span() for m in DISPLAY_SPAN_RE.finditer(line)]

        def sub(m):
            nonlocal made
            ref = next(g for g in m.groups() if g)
            if any(a <= m.start() < b for a, b in display):
                return m.group(0)
            link = link_for(ref, doc, files, by_basename)
            made += link is not None
            return link or '`%s`' % ref

        out.append(CITATION_RE.sub(sub, line))
    return '\n'.join(out), made


def collect(paths):
    docs = []
    for p in paths:
        if os.path.isdir(p):
            for dirpath, dirs, names in os.walk(p):
                dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
                docs.extend(os.path.join(dirpath, n)
                            for n in names if n.endswith('.md'))
        elif p.endswith('.md') and os.path.isfile(p):
            docs.append(p)
    return sorted(set(docs))


def first_diff(before, after, limit=3):
    shown = []
    for n, (a, b) in enumerate(zip(before.split('\n'), after.split('\n')), 1):
        if a != b:
            shown.append('     line %d:\n       - %s\n       + %s' % (n, a, b))
            if len(shown) == limit:
                break
    return shown


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*', metavar='PATH',
                    help='.md files or directories (default: the architecture guide)')
    ap.add_argument('--check', action='store_true',
                    help='do not write; exit 1 if any file would change')
    ap.add_argument('--diff', action='store_true',
                    help='implies --check; show the first changed lines per file')
    args = ap.parse_args()

    docs = collect(args.paths or DEFAULT_TARGETS)
    if not docs:
        print('❌ No .md files to process')
        return 1

    files, by_basename = repo_index()
    changed, links = [], 0
    for doc in docs:
        before = open(doc).read()
        after, made = rewrite(before, doc, files, by_basename)
        links += made
        if after == before:
            continue
        changed.append(doc)
        if args.check or args.diff:
            if args.diff:
                print('   %s' % doc)
                for chunk in first_diff(before, after):
                    print(chunk)
        else:
            with open(doc, 'w') as f:
                f.write(after)

    if args.check or args.diff:
        if changed:
            print('❌ Doc links are stale in %d file(s):' % len(changed))
            for doc in changed:
                print('   %s' % doc)
            print('   Run: make regen-doc-links')
            return 1
        print('✅ Doc links: up to date (%d files, %d links)' % (len(docs), links))
        return 0

    print('✅ Doc links: %d links across %d files (%d rewritten)'
          % (links, len(docs), len(changed)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
