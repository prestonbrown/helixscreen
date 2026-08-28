#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lint gate: agent-facing docs, docs/README.md (the public docs index), and
# docs/devel/ docs must not cite files that don't exist.
#
# CLAUDE.md files and skills work by progressive disclosure — they are mostly
# pointers, and a pointer to a renamed or deleted file is worse than no pointer.
# It sends the reader (human or agent) looking for something that isn't there, and
# it silently teaches a wrong name. Several rounds of this have been cleaned up by
# hand:
#
#   - .claude/checklist.md cited six docs that had moved to docs/devel/
#   - CLAUDE.md taught ui_nav_push_overlay(), a function with zero call sites
#   - scripts/CLAUDE.md documented package.sh, deleted in 1ddbbbdba
#   - a lesson taught lv_xml_component_register_from_file(), a transposed name
#     that exists nowhere
#
# Six checks:
#   refs   — every backticked path in an agent-facing doc resolves
#   links  — every markdown [text](target) link in a scanned doc resolves
#   index  — every doc in docs/devel/ is listed in docs/devel/CLAUDE.md
#   lines  — a cited `file.cpp:123` must land inside the file (past-EOF = rot)
#   cites  — the symbol a sentence names next to a `file.cpp:123` cite must
#            still appear near that line (ratcheted; a resolved path pointing
#            at the wrong lines is the drift class the path check cannot see)
#   anchors— every `file.cpp:123` cite has a committed content hash of the line
#            it points at (scripts/doc_cite_anchors.tsv), and the number in the
#            doc must still resolve to it. This is the check that covers the
#            other three quarters of the citations: the symbol shape only
#            appears beside about a quarter of them, and files mostly grow, so
#            a stale number normally still lands inside the file. Moved code
#            self-heals via `make regen-doc-links`; only a line whose own text
#            changed reaches a human. See scripts/doc_cite_anchors.py.
#
# The index check is what makes lazy loading trustworthy: a doc missing from the
# routing table is a doc nobody will find.
#
# KNOWN GAPS — each wanting its own change:
#
#   bare `:857` shorthand, the rest of it. 447 in the scanned docs. 276 now
#     resolve against the nearest preceding full citation on their own line
#     (`doc_cite_anchors.py --bare-refs`, a REVIEW LIST that anchors nothing —
#     11 of 12 hand-checked were wrong, so bootstrapping them would freeze the
#     rot). Of the remainder, 28 sit after a citation that names a PATH with no
#     line — `` `scripts/quality-checks.sh` … (`:1583`) ``, the table form —
#     which LINE_REF_RE cannot be the antecedent for; widening the antecedent
#     search to PATH_RE matches would pick those up. The last 143 have no
#     antecedent on their line at all, and some have none anywhere: one names
#     three call sites "in the detail view", a prose antecedent no rule can
#     resolve. Those need prose edits, not a better regex.
#
#   templated symbol spellings. SYMBOL_CITE_*_RE's symbol charset is
#     [A-Za-z_][A-Za-z0-9_]* plus `::`, so anything carrying angle brackets is
#     invisible to the symbol-proximity check — 8 in the tree today
#     (`observe_int_sync<Panel>()`, `std::unique_ptr<MoonrakerAPI>`,
#     `panel_widget_from_event<T>()`, `lvgl_make_unique<T>()`, …). Widening the
#     charset alone would MISFIRE: check_symbol_cites compares
#     sym.split('(')[0].split('::')[-1] against the source window, and for
#     `std::shared_ptr<bool>` that yields `shared_ptr<bool>`, which does not
#     appear literally in code that says `std::shared_ptr<bool>` with different
#     spacing. Extract the bare identifier first, then widen.
#
#   a bare basename passes check_refs as soon as ANY file in the tree shares it,
#     submodules included — `file.cpp` in prose resolves to
#     lib/cpp-terminal/cpp-terminal/private/file.cpp. Harmless for a deliberate
#     placeholder, but it means the path check cannot be read as proof that the
#     file the sentence MEANT exists. doc_cite_anchors.py holds the size of that
#     bucket at `max-unresolved:` instead.
#
#   NOT a gap, measured rather than assumed: citations inside fenced code blocks.
#     Both this gate and the anchor generator skip fences deliberately, and a
#     census of the corpus found ZERO real citations inside one, so the
#     exclusion costs nothing today. Non-backticked prose mentions
#     (`zmod_ifs.py:1149`, unbackticked as external) are likewise deliberate.
#
# Usage:
#   check_doc_refs.py            # everything: every CLAUDE.md, .claude/skills/,
#                                # docs/README.md, docs/devel/**/*.md,
#                                # refs+links+lines/cites, plus the index check
#   check_doc_refs.py --refs     # broken references only
#   check_doc_refs.py --index    # index completeness only
#   check_doc_refs.py --list     # show what was scanned
#   check_doc_refs.py --stale    # report-only: cites older than the code they
#                                # describe (release-time re-verify list)
#   check_doc_refs.py --write-cite-baseline
#                                # re-baseline the symbol-cite ratchet after
#                                # reviewing the findings it prints
#   check_doc_refs.py --devel [PATHS...]
#                                # refs+links over docs/devel/**/*.md, or only
#                                # the given PATHS (.md files, or directories
#                                # walked for .md). Point-in-time subdirs
#                                # (plans/, printer-research/) are exempt.

import argparse
import os
import re
import sys
from re import error

SKIP_DIRS = {'.git', '.worktrees', 'build', 'node_modules', '.venv', 'venv'}

# Pruned by PATH rather than by name, because the name alone is load-bearing
# elsewhere. `.claude` CANNOT go in SKIP_DIRS: the agent-doc walk deliberately
# collects `.claude/skills/*.md`, and skipping the whole directory would drop
# them silently. But `.claude/worktrees/` holds the harness's own ephemeral
# agent checkouts, each a complete copy of this repo — CLAUDE.md, docs/ and all.
# Walking those made every doc and every citation appear once per live agent:
# six copies of CLAUDE.md's `temperature_service.cpp:667`, anchored against a
# source tree that is not the one being checked. It went unnoticed for as long
# as the resolver had a nearest-to-old-line fallback to swallow them with, and
# turned main red the moment ambiguity became a hard failure. Deleting the
# worktrees is not the fix: they carry other agents' uncommitted work.
SKIP_PATHS = {os.path.join('.claude', 'worktrees')}


def prune_dirs(root, dirs, extra=()):
    """In-place prune of a walk's directory list, by name AND by path.

    Every walk in this file has to use it. A by-name prune cannot express
    "`.claude/skills` yes, `.claude/worktrees` no", and that distinction is the
    whole point.
    """
    rel = os.path.relpath(root, '.')
    keep = []
    for d in dirs:
        if d in SKIP_DIRS or d in extra:
            continue
        if os.path.normpath(os.path.join(rel, d)) in SKIP_PATHS:
            continue
        keep.append(d)
    dirs[:] = keep
    return dirs

# Paths that are intentionally absent from a clean checkout.
# NOTE: a developer machine has a built, patched tree, so these all resolve
# locally and only ever fail in CI - which checks out clean and does not build.
# That asymmetry is why main went red here for a day and a half without anyone
# reproducing it. Add to this list rather than rewording a doc: the citations
# are correct, the files simply do not exist yet at check time.
EXEMPT_SUBSTRINGS = (
    'superpowers/',        # docs/superpowers/ is local-only working space; nothing
                           # there is tracked; refs to it are not resolvable on a
                           # fresh clone
    # Written at runtime.
    'settings-test.json',  # seeded by --test
    'config/settings.json',
    # Created by `make apply-patches` (patches/libhv-dns-resolver-fallback.patch).
    'dns_resolv.',
    # Build outputs.
    'compile_commands.json',
    'build/generated/',    # helix_git_hash.h and friends
    'MANIFEST.txt',        # written by genPackagingManifest at assets-build time
    # libhv installs its public headers into include/hv/ during its own build.
    'libhv/include/',
    'hv/requests.h',
    'hv/hlog.h',
    # Klipper extras in UPSTREAM repos, not ours. FILAMENT_BACKEND_MEDUSAHC.md
    # cites the two MedusaHC controllers by file and line because the schema
    # difference between them is the whole point of that section, and the cites
    # were read from those sources rather than inferred. Nothing named
    # medusahc.py will ever exist in this repo.
    'medusahc.py',
)

# Tokens that are obviously placeholders rather than real paths.
PLACEHOLDER_CHARS = ('<', '>', '*', '$', '…', '{')

# `some/path/file.ext` in prose or a table cell. The path may carry a `:123`
# line, a `:123-145` RANGE, or a `:func_name()` symbol suffix; the path charset
# excludes ':' so the suffix can never be part of a real path and is stripped
# before checking.
#
# The range form used not to match here at all, which meant a citation naming a
# block was exempt from the one check that proves its FILE exists. That is how
# `scripts/quality-checks.sh:1311-1336` sat in RPC_ERROR_OWNERSHIP.md pointing
# ~355 lines away from the gate it described: the point form would have been
# caught the moment the path rotted, the range form was invisible to every gate
# at once. Verified by probe — a `:70` cite to a bogus path fails the refs
# check, the same path as `:63-65` produced no finding from anything.
# ONE extension list, shared by both citation regexes below. They are documented
# to agree on what counts as a citation and they silently did not: `cfg` had been
# added to LINE_REF_RE alone, so a `.cfg` citation was content-anchored while its
# PATH was never checked. Spelling the list once is the only version that cannot
# drift, and it is the same shape as the range hole - a one-line edit away from
# recurring for whatever extension somebody adds next.
#
# `cfg` is deliberately NOT here. A `.cfg` citation in these docs is a Klipper
# config that lives on the PRINTER - `printer.cfg`, `box.cfg`,
# `Macros/toolchanger.cfg` - and 45 of the 48 in the tree are exactly that, so
# admitting the extension would fail the path check on all of them for being
# correct. It bought nothing on the anchor side either: every `.cfg` citation
# LINE_REF_RE made visible sat permanently unresolved, since the file is not in
# the repo to hash. The three real in-repo .cfg files have never been line-cited.
# Checking device-side config would need a different mechanism, not this one.
CITED_EXTENSIONS = ('md', 'cpp', 'cc', 'h', 'hpp', 'c', 'xml', 'py', 'sh',
                    'json', 'mk', 'bats', 'yml', 'yaml', 'html', 'txt')
_EXT = '|'.join(CITED_EXTENSIONS)

PATH_RE = re.compile(
    r'`([A-Za-z0-9_./-]+\.(?:' + _EXT + r')'
    r'(?::\d+(?:[-–]\d+)?|:[A-Za-z0-9_]+\(\))?)`')

# Markdown [text](target) links. Link text must be non-empty — `[](...)` is a
# C++ lambda with a parenthesized parameter list (e.g. `.on_destroy = [](lv_obj_t*)`
# in a doc's code sample), never a markdown link. The anchor part (#+...) is
# optional and dropped; the target is resolved relative to the doc's own directory.
LINK_RE = re.compile(r'\[[^\]]+\]\(([^)#\s]+)(?:#[^)]*)?\)')

# A citation wrapped in a link — [`src/foo.cpp:12`](../../../src/foo.cpp#L12) —
# is still a citation. scripts/gen_doc_links.py generates those wrappers from
# the very text the checks below parse, so the path/line/symbol checks unwrap
# first. Without it, linking a doc would silently switch off the drift checks
# that make its citations trustworthy: the symbol-cite shapes below expect
# `sym` (`file:N`) with nothing between the paren and the backtick, so across
# the architecture guide the wrappers alone drop the check from 243 matches to
# 5. Only a link around a code span is unwrapped; a prose link is left alone.
# Removing the wrapper cannot move a reported line number — a markdown link
# holds no newline, and every finding is located by counting '\n'.
LINKED_SPAN_RE = re.compile(r'\[(`[^`\n]+`)\]\([^)\s]*\)')


# A range end written OUTSIDE the citation's backticks — `file.cpp:10`-20, and in
# the architecture guide [`file.cpp:10`](url)-20, which is the guide's dominant
# range spelling. LINE_REF_RE stops at the closing backtick, so the "-20" is a
# separate token that has to be found by looking ahead from the citation's end,
# skipping any generated link wrapper on the way.
TRAILING_RANGE_RE = re.compile(r'(?:\]\([^)\s]*\))?([-–])(\d+)')


def trailing_range(text, pos):
    """(dash, end, span) for a range end just after a citation, or None."""
    m = TRAILING_RANGE_RE.match(text, pos)
    if not m:
        return None
    return m.group(1), int(m.group(2)), (m.start(1), m.end(2))


def unwrap_links(text):
    return LINKED_SPAN_RE.sub(lambda m: m.group(1), text)

# `path/file.cpp:123` citations. The path charset mirrors PATH_RE's so the two
# agree on what counts as a line-cited reference.
# The line part may be a RANGE: `src/foo.cpp:63-65` names a block, not a point.
# Ranges used not to match here at all, which is the worst possible failure mode
# for a lint — 96 citations in this tree, 13% of the corpus, silently exempt
# from the past-EOF check, the symbol check and the content anchor alike, with
# nothing anywhere reporting that they were being skipped. The START is the
# anchor; `rend` is carried so a re-pin can move the whole block and keep the
# authored span length.
#
# Named groups, because this pattern is embedded inside SYMBOL_CITE_A_RE and
# SYMBOL_CITE_B_RE: positional numbering there shifts with every group added
# here, and it shifts SILENTLY into reading the wrong capture.
LINE_REF_RE = re.compile(
    r'`(?P<ref>[A-Za-z0-9_./-]+\.(?:' + _EXT + r')):(?P<line>\d+)'
    r'(?:(?P<rdash>[-–])(?P<rend>\d+))?`')

# A line-cited reference plus the symbol the sentence claims lives there, in the
# two shapes the docs actually use:
#   `symbol()` (`path/file.cpp:123`)   — symbol first, cite parenthesized
#   `path/file.cpp:123` — `symbol`     — cite first, symbol after a dash
# The symbol's base identifier (trailing () and ::qualifiers stripped) must
# appear within ±SYMBOL_WINDOW lines of the cited line, or the cite has drifted:
# the file grew, the number still resolves, and the sentence now describes
# whatever code moved into that slot. That is the failure the path check cannot
# see — the .115 audit found 30+ such cites, every one of them a resolved path
# pointing at the wrong lines (printer_state.h:208 citing a class that had moved
# 23 lines up; ams_backend_afc.cpp:2219 landing inside a different parse block).
# Both shapes carry a TAIL between the cite and the delimiter, and the tail must
# never switch the check off — a citation that says a bit more about itself is
# not making a weaker claim. It did switch it off: 16 citations rode through
# unchecked behind a range suffix, one of them (`manages_active_spool()` at
# `src/printer/ams_state.cpp:2647`-2658) 277 lines stale AND pointing at a blank
# line, while every gate reported green.
#
# Only shape A gets a tail. Its tail sits INSIDE the parenthesis, so ')' and '`'
# already fence it and it can safely hold a range end or a short aside
# ("-2658", ", null on real runs") without reaching a second citation.
#
# Shape B deliberately gets NONE. Allowing even ')' there inverts what the
# citation attaches to: `(...h:327`) — `MoonrakerAPIMock` ...` is a parenthetical
# on the text BEFORE it, and the dash opens a new clause about a different
# symbol, so pairing the two reports drift on a citation that is correct. Tried
# it; it produced exactly one true pairing and one false one, which is not a
# check, it is a coin flip.
A_CITE_TAIL = r'[^)`\n]{0,24}'

SYMBOL_CITE_A_RE = re.compile(          # `sym` (`path:N`), (`path:N`-M)
    r'`(?P<sym>[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z0-9_]+)*(?:\(\))?)`\s*\((?:`)?'
    + LINE_REF_RE.pattern[1:-1] + '(?:`)?'
    + r'(?:[-–](?P<aend>\d+))?' + A_CITE_TAIL + '\\)')

# Shape B's separator must stay on the citation's OWN line. `\s*` spanned
# newlines, which pairs a cite that merely ENDS a bullet with the symbol opening
# the NEXT one ("...print_status.cpp:379).\n- `decide_render_mode()`") — four
# such false pairings in this tree. No current match spans a newline, so
# horizontal-only whitespace costs nothing and buys immunity to markdown lists.
SYMBOL_CITE_B_RE = re.compile(          # `path:N` — `sym`
    LINE_REF_RE.pattern[1:-1]
    + r'`[^\S\n]*[—–-]+[^\S\n]*`(?P<sym>[A-Za-z_][A-Za-z0-9_]*'
    r'(?:::[A-Za-z0-9_]+)*(?:\(\))?)`')
SYMBOL_WINDOW = 5


# Link targets that cannot be verified on disk.
LINK_SKIP_PREFIXES = ('http://', 'https://', 'mailto:', '#')
LINK_SKIP_SUFFIXES = ('.d2', '.png', '.jpg', '.jpeg', '.gif', '.svg', '.webp',
                      '.bmp', '.ico')

DOC_INDEX = 'docs/devel/CLAUDE.md'
DOC_DIR = 'docs/devel'

# Point-in-time docs under docs/devel/ — dated plans and device research notes
# whose citations rot by design. Matched as directory-name components during a
# walk, so a scan rooted anywhere (meta-test fixture, targeted run) exempts a
# plans/ or printer-research/ subdir the same way the default walk does.
#
# 'plans' is the single tracked home for the in-flight set: dated
# implementation plans, their design specs, and the ESP32 program docs all
# live in docs/devel/plans/. They are written against the tree as it stood
# on their date, so their citations are historical record, not promises.
DEVEL_EXEMPT_SUBDIRS = ('plans', 'printer-research')

# Docs deliberately not routed from the index.
INDEX_EXEMPT = {
    'CLAUDE.md',           # the index itself
}

# Scanned with the agent-facing docs (same refs+links checks) although it is
# neither a CLAUDE.md nor a skill: docs/README.md is the public index every
# other docs/ page is reached through, so a dead link there is the first one
# a reader hits.
EXTRA_SCAN_DOCS = ('docs/README.md',)


def repo_files():
    """Every file in the repo, for suffix resolution.

    followlinks=True because setup-worktree.sh symlinks the lib/ submodules back
    into the main tree. Without it a worktree indexes none of them, and a doc
    citing a submodule file (lv_sdl_window.c in the patch workflow) reads as
    broken there while resolving fine on the main tree. `seen` guards against a
    symlink cycle, which followlinks would otherwise recurse into forever.
    """
    out = set()
    seen = set()
    for root, dirs, files in os.walk('.', followlinks=True):
        prune_dirs(root, dirs)
        keep = []
        for d in dirs:
            real = os.path.realpath(os.path.join(root, d))
            if real in seen:
                continue
            seen.add(real)
            keep.append(d)
        dirs[:] = keep
        for f in files:
            out.add(os.path.join(root, f)[2:])
    return out


def uninitialized_submodules():
    """Submodule paths that are declared but not checked out.

    CI checks out the superproject without submodules, so lib/lvgl and friends are
    empty there while they are fully populated on a developer machine. A doc that
    legitimately cites a submodule file (e.g. lv_sdl_window.c in the patch workflow)
    would resolve locally and fail in CI. We cannot verify a file that is not on
    disk, so we report those separately instead of asserting they are broken.
    """
    missing = []
    if not os.path.isfile('.gitmodules'):
        return missing
    for m in re.finditer(r'^\s*path\s*=\s*(.+?)\s*$', open('.gitmodules').read(), re.M):
        p = m.group(1)
        if not os.path.isdir(p) or not os.listdir(p):
            missing.append(p)
    return missing


def scan_targets():
    """Agent-facing docs: every CLAUDE.md, everything under .claude/skills/,
    plus the extra scanned docs (the public docs index)."""
    targets = []
    for root, dirs, files in os.walk('.'):
        prune_dirs(root, dirs)
        rel = root[2:]
        if rel.startswith('lib/'):
            continue
        for f in files:
            path = os.path.join(rel, f) if rel else f
            if f == 'CLAUDE.md' or (rel.startswith('.claude/skills') and f.endswith('.md')):
                targets.append(path)
    targets.extend(EXTRA_SCAN_DOCS)
    return sorted(targets)


def scan_devel_targets(paths):
    """--devel mode targets: docs/devel/**/*.md, or only the given paths.

    An explicit .md path is scanned as-is (even one under an exempt subdir — an
    explicit request is explicit). A directory is walked for .md files the same
    way the default docs/devel walk is, exemptions included. Anything else is
    dropped.
    """
    def walk_md(root):
        out = []
        for dirpath, dirs, files in os.walk(root):
            prune_dirs(dirpath, dirs, extra=DEVEL_EXEMPT_SUBDIRS)
            for f in files:
                if f.endswith('.md'):
                    out.append(os.path.join(dirpath, f))
        return out

    if paths:
        targets = []
        for p in paths:
            if os.path.isdir(p):
                targets.extend(walk_md(p))
            elif p.endswith('.md'):
                targets.append(p)
        return sorted(set(targets))
    return sorted(walk_md(DOC_DIR))


def check_refs(targets, allpaths, devel=False):
    problems = []
    # Basename index so a bare `THREADING.md` resolves without scanning all
    # ~70k repo paths per citation — the suffix fallback used to dominate the
    # whole run (8s of the gate's cost). Refs containing '/' keep the exact
    # suffix scan (p.endswith('/'+path)); those are rare.
    by_basename = {}
    for p in allpaths:
        by_basename.setdefault(p.rsplit('/', 1)[-1], []).append(p)
    allpaths_set = allpaths if isinstance(allpaths, set) else set(allpaths)
    for target in targets:
        base = os.path.dirname(target)
        try:
            text = unwrap_links(open(target, errors='ignore').read())
        except OSError:
            continue
        for m in PATH_RE.finditer(text):
            ref = m.group(1)
            if any(c in ref for c in PLACEHOLDER_CHARS):
                continue
            if any(s in ref for s in EXEMPT_SUBSTRINGS):
                continue
            path = ref.split(':', 1)[0]  # strip a :123 / :func_name() suffix
            if os.path.exists(path):
                continue
            if base and os.path.exists(os.path.join(base, path)):
                continue
            if devel and base:
                # A devel doc cites repo-rooted paths (src/…, include/…). In the
                # repo those resolve from the cwd above; a scratch tree handed to
                # --devel has the same shape one level above the doc's directory
                # (<root>/devel/doc.md citing <root>/src/…).
                parent = os.path.dirname(base)
                if parent and os.path.exists(os.path.join(parent, path)):
                    continue
            # a bare or partial path is fine if exactly that suffix exists somewhere
            if '/' in path:
                if any(p.endswith('/' + path) for p in allpaths):
                    continue
            elif path in by_basename:
                continue
            if path in allpaths_set:
                continue
            line = text.count('\n', 0, m.start()) + 1
            problems.append((target, line, ref))
    return problems


def check_links(targets):
    """Markdown links must resolve relative to the doc's own directory.

    http(s)/mailto targets, bare #anchors, and .d2/image references cannot be
    verified on disk and are skipped.
    """
    problems = []
    for target in targets:
        base = os.path.dirname(target)
        try:
            text = open(target, errors='ignore').read()
        except OSError:
            continue
        for m in LINK_RE.finditer(text):
            ref = m.group(1)
            if ref.startswith(LINK_SKIP_PREFIXES):
                continue
            if ref.lower().endswith(LINK_SKIP_SUFFIXES):
                continue
            if os.path.exists(os.path.join(base, ref) if base else ref):
                continue
            line = text.count('\n', 0, m.start()) + 1
            problems.append((target, line, ref))
    return problems


def check_index():
    if not os.path.isfile(DOC_INDEX):
        return [], []
    index_text = open(DOC_INDEX, errors='ignore').read()
    present = set()
    for f in os.listdir(DOC_DIR):
        full = os.path.join(DOC_DIR, f)
        if os.path.isfile(full) and (f.endswith('.md') or f.endswith('.html')):
            present.add(f)
    unindexed = sorted(f for f in present - INDEX_EXEMPT
                       if '`%s`' % f not in index_text)
    return unindexed, sorted(present)


def _resolve_cited(path, base, devel):
    """The on-disk file a backticked citation names, or None.

    Only a direct hit (repo-relative, or doc-dir-relative) is returned: the
    suffix fallback in check_refs ("a bare basename that exists somewhere")
    cannot tell WHICH file the line number refers to, so a line check against
    it would verify the wrong file.
    """
    for cand in (path, os.path.join(base, path) if base else None,
                 os.path.join(os.path.dirname(base), path)
                 if devel and base else None):
        if cand and os.path.isfile(cand):
            return cand
    return None


def _file_lines(path, cache):
    if path not in cache:
        try:
            cache[path] = open(path, errors='ignore').read().splitlines()
        except OSError:
            cache[path] = None
    return cache[path]


def check_line_refs(targets, devel=False):
    """A cited `file:N` past the file's end is always stale. Hard error.

    Files mostly grow, so a stale cite usually still lands inside the file —
    that case is the symbol check's job. But a cite beyond EOF proves the doc
    was never going to be right against this tree, with zero false positives.
    """
    problems = []
    cache = {}
    for target in targets:
        base = os.path.dirname(target)
        try:
            text = unwrap_links(open(target, errors='ignore').read())
        except OSError:
            continue
        for m in LINE_REF_RE.finditer(text):
            ref, n = m.group('ref'), int(m.group('line'))
            if any(s in ref for s in EXEMPT_SUBSTRINGS):
                continue
            resolved = _resolve_cited(ref, base, devel)
            if not resolved:
                continue          # unresolved paths are check_refs' failure, not ours
            lines = _file_lines(resolved, cache)
            if not lines:
                continue
            # Both ends of a range have to land inside the file. Checking only
            # the start would pass `foo.cpp:900-9000` on a 1000-line file.
            end = int(m.group('rend')) if m.group('rend') else n
            if end == n:
                tail = trailing_range(text, m.end())
                if tail:
                    end = tail[1]
            worst = max(n, end)
            if worst > len(lines):
                line = text.count('\n', 0, m.start()) + 1
                shown = ('%s:%d-%d' % (ref, n, end)) if end != n else '%s:%d' % (ref, n)
                problems.append((target, line, '%s (file has %d lines)'
                                 % (shown, len(lines))))
    return problems


def _cite_end(m):
    """Last line a citation claims: the range end if it has one, else the start.

    A range reaches the checks two ways — inside the backticks (`f.cpp:10-20`)
    or trailing the closing one (`f.cpp:10`-20) — and both mean the same thing.
    """
    for group in ('rend', 'aend'):
        try:
            v = m.group(group)
        except (IndexError, error):
            v = None
        if v:
            return int(v)
    return int(m.group('line'))


def check_symbol_cites(targets, devel=False):
    """Symbol-near-cite: the symbol a sentence names must be near the cited line.

    Returns findings as (doc, doc_line, key, detail). Ratcheted against
    scripts/doc_cite_symbol_baseline.txt — a doc:docline key already listed is
    known drift under repair; a new one fails the gate.
    """
    findings = []
    cache = {}
    for target in targets:
        base = os.path.dirname(target)
        try:
            text = unwrap_links(open(target, errors='ignore').read())
        except OSError:
            continue
        pairs = []
        for m in SYMBOL_CITE_A_RE.finditer(text):
            pairs.append((m, m.group('ref'), int(m.group('line')),
                          m.group('sym'), _cite_end(m)))
        for m in SYMBOL_CITE_B_RE.finditer(text):
            pairs.append((m, m.group('ref'), int(m.group('line')),
                          m.group('sym'), _cite_end(m)))
        for m, ref, n, sym, hi in pairs:
            if any(s in ref for s in EXEMPT_SUBSTRINGS):
                continue
            resolved = _resolve_cited(ref, base, devel)
            if not resolved:
                continue
            lines = _file_lines(resolved, cache)
            if lines is None:
                continue
            ident = sym.split('(')[0].split('::')[-1]
            # A range names a BLOCK, so the symbol may sit anywhere inside it.
            # Windowing on the start line alone turned three correct citations
            # into findings the moment ranges started being checked at all —
            # `snapmaker_resume.h:11-19` naming `sdcard`, which lives at 17.
            window = lines[max(0, n - 1 - SYMBOL_WINDOW):hi + SYMBOL_WINDOW]
            if any(ident in w for w in window):
                continue
            line = text.count('\n', 0, m.start()) + 1
            findings.append((target, line, '%s:%d' % (target, line),
                             '`%s` cited at `%s:%d` — "%s" is not within '
                             '±%d lines' % (sym, ref, n, ident, SYMBOL_WINDOW)))
    return findings


def check_anchors(targets, devel=True):
    """Content anchors: the number in a `file:N` cite must still resolve.

    Returns (findings, stats), or (None, None) when scripts/doc_cite_anchors.tsv
    is absent — a tree without the sidecar has not opted in, which is what lets
    the miniature-repo meta-tests of the other gates keep passing.

    The import is deliberately lazy. scripts/doc_cite_anchors.py imports THIS
    module (for LINE_REF_RE / EXEMPT_SUBSTRINGS / _resolve_cited) and
    gen_doc_links.py, so importing it at module scope here would be a cycle. By
    the time this function runs, everything it needs is defined. Running this
    file as __main__ does mean doc_cite_anchors ends up importing a second copy
    of it under its real name — harmless, since everything it reaches for is a
    compiled regex, a constant tuple, or a pure function.
    """
    import doc_cite_anchors as anchors
    stored = anchors.load_sidecar()
    if stored is None:
        return None, None
    # Narrow to docs this repo actually scans. A --devel run pointed at a
    # scratch fixture (which is how the sibling gates' meta-tests drive this
    # script) would otherwise be measured against the real repo's sidecar and
    # fail on citations that were never meant to be anchored.
    scanned = set(anchors.default_targets())
    scoped = [t for t in targets if t in scanned]
    if not scoped:
        return [], {'in_place': 0}
    findings, _fresh, _blanks, _rewrites, stats = anchors.run(
        scoped, stored, devel=devel, write=False)
    return findings, stats


CITE_BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             'doc_cite_symbol_baseline.txt')


def load_cite_baseline():
    if not os.path.isfile(CITE_BASELINE):
        return set()
    return {l.strip() for l in open(CITE_BASELINE, errors='ignore')
            if l.strip() and not l.startswith('#')}


def last_commit_dates(paths):
    """Newest commit date (ISO) per path, from ONE streaming `git log` walk.

    Spawning `git log -1 -- <path>` per citation made --stale take 100s+ (a
    process spawn per cited file, several hundred of them). Instead we stream
    full history newest-first with --name-only and record the first commit
    each wanted path appears in — then stop the walk as soon as every wanted
    path has been seen. Docs cite living code, so the walk almost always ends
    within a few hundred commits of HEAD.
    """
    import subprocess
    wanted = set(paths)
    found = {}
    if not wanted:
        return found
    try:
        proc = subprocess.Popen(
            ['git', 'log', '--format=\x01%cI', '--name-only', '--no-renames'],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            errors='ignore')
    except OSError:
        return found
    date = None
    try:
        for line in proc.stdout:
            line = line.rstrip('\n')
            if line.startswith('\x01'):
                date = line[1:]
                continue
            if not line or date is None:
                continue
            if line in wanted and line not in found:
                found[line] = date
                if len(found) == len(wanted):
                    proc.kill()
                    break
    finally:
        proc.stdout.close()
        proc.wait()
    return found


def check_stale(targets, devel=False):
    """Release-time report: cites whose file moved after the doc last did.

    Not a gate. A resolved cite can still be wrong (the line/symbol checks
    catch the worst of that), and a count table can be silently out of date —
    both rot exactly when the cited tree changes after the doc was written.
    This names those pairs so a release audit starts from a list instead of a
    fan-out. Docs carrying a "recounted <date>" census annotation are compared
    against that date too: a recount older than the cited tree's last change
    is the count-drift the .115 audit kept finding.
    """
    RECOUNT_RE = re.compile(r'recounted (\d{4}-\d{2}-\d{2})')
    PATH_ONLY_RE = re.compile(
        r'`([A-Za-z0-9_./-]+\.(?:cpp|cc|h|hpp|c|xml|py|sh|json|mk|bats|yml|'
        r'yaml|html|txt))(?::\d+|:[A-Za-z0-9_]+\(\))?`')
    docs = {}
    for target in targets:
        try:
            text = unwrap_links(open(target, errors='ignore').read())
        except OSError:
            continue
        docs[target] = text
    all_cited = set()
    for target, text in docs.items():
        all_cited.update(m.group(1) for m in PATH_ONLY_RE.finditer(text)
                         if not any(s in m.group(1) for s in EXEMPT_SUBSTRINGS))
    dates = last_commit_dates(all_cited | set(docs))
    reports = []
    for target, text in docs.items():
        doc_date = dates.get(target)
        if not doc_date:
            continue
        recounts = RECOUNT_RE.findall(text)
        cited = sorted({m.group(1) for m in PATH_ONLY_RE.finditer(text)
                        if not any(s in m.group(1) for s in EXEMPT_SUBSTRINGS)})
        for ref in cited:
            fdate = dates.get(ref)
            if not fdate:
                continue
            if fdate[:10] > doc_date[:10]:
                reports.append('%s cites `%s` — file last changed %s, doc %s'
                               % (target, ref, fdate[:10], doc_date[:10]))
            elif recounts and fdate[:10] > max(recounts):
                reports.append('%s recounts %s but `%s` changed %s — recount '
                               'is stale' % (target, max(recounts), ref,
                                             fdate[:10]))
    return reports


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--refs', action='store_true', help='Only check references resolve')
    ap.add_argument('--index', action='store_true', help='Only check index completeness')
    ap.add_argument('--devel', nargs='*', dest='devel_paths', metavar='PATH',
                    help='Check docs/devel/**/*.md (or only the given .md files '
                         'and directories): path refs and markdown links')
    ap.add_argument('--stale', action='store_true',
                    help='Report-only: cites whose file changed after the doc '
                         'last did (release-time re-verification list)')
    ap.add_argument('--write-cite-baseline', action='store_true',
                    help='Rewrite doc_cite_symbol_baseline.txt from the current '
                         'symbol-cite findings instead of failing on them')
    ap.add_argument('--list', action='store_true', help='List scanned files')
    args = ap.parse_args()

    devel = args.devel_paths is not None
    if devel:
        targets = scan_devel_targets(args.devel_paths)
        do_refs = True
        do_index = False
    else:
        # Default scope = agent-facing docs + the docs/devel target set. The
        # docs/devel half used to be opt-in via --devel; it flipped to default
        # enforcement once the surviving docs were swept clean, so CI catches
        # future citation rot in feature docs instead of accumulating it.
        targets = sorted(set(scan_targets()) | set(scan_devel_targets([])))
        # devel=True enables the parent-dir resolution fallback for docs/devel
        # citations; for repo-root CLAUDE.md files it is a harmless no-op.
        devel = True
        do_refs = args.refs or not args.index
        do_index = args.index or not args.refs

    if args.list:
        for t in targets:
            label = 'scanned (devel)' if t.startswith('docs/devel') else 'scanned'
            print('  %s: %s' % (label, t))

    if args.stale:
        reports = check_stale(targets, devel=devel)
        if reports:
            print('⚠️  Citations older than the code they describe (%d):' % len(reports))
            for r in reports:
                print('   %s' % r)
            print('   Re-verify these before a release; not a commit-time failure.')
        else:
            print('✅ No citation is older than its doc (%d files scanned)' % len(targets))
        return 0

    exit_code = 0

    if do_refs:
        problems = check_refs(targets, repo_files(), devel=devel)
        link_problems = check_links(targets)
        skipped = uninitialized_submodules()

        def under_missing_submodule(ref):
            return any(ref == s or ref.startswith(s.rstrip('/') + '/') for s in skipped)

        def unverifiable(ref):
            """True when a missing submodule could plausibly explain this ref.

            This used to be all-or-nothing: one unpopulated submodule suppressed
            every problem in the run, so in CI - where the shell-test job checks
            out no submodules - the gate could not fail at all, and
            tests/shell/test_doc_refs_gate.bats asserting exit 1 failed instead.
            Attribute per ref: a path under a missing submodule is genuinely
            unverifiable, and so is a bare basename (CLAUDE.md's patch workflow
            cites lv_sdl_window.c with no directory, and we cannot know where it
            lives without the checkout). Anything else names a directory we do
            have, so a miss there is a real stale reference.
            """
            if not skipped:
                return False
            if '/' not in ref:
                return True
            return under_missing_submodule(ref)

        # Markdown links resolve against the citing doc's own directory, so a
        # missing submodule cannot explain a bare `nope.md` the way it can
        # explain a bare `lv_sdl_window.c`. Links only get the path-prefix
        # allowance - otherwise the gate would go quiet on genuinely dead links
        # in exactly the CI job that has no submodules.
        def link_unverifiable(ref):
            return bool(skipped) and under_missing_submodule(ref)

        unverified = ([x for x in problems if unverifiable(x[2])]
                      + [x for x in link_problems if link_unverifiable(x[2])])
        problems = [x for x in problems if not unverifiable(x[2])]
        link_problems = [x for x in link_problems if not link_unverifiable(x[2])]

        if unverified:
            print('⚠️  Doc references unverifiable — submodule(s) not checked out: %s'
                  % ', '.join(skipped))
            for target, line, ref in unverified:
                print('   %s:%d: `%s`' % (target, line, ref))
            print('   Run locally with submodules populated to check these strictly.')
        if problems:
            print('❌ Doc references that do not resolve:')
            for target, line, ref in problems:
                print('   %s:%d: `%s`' % (target, line, ref))
            print('   Fix the path, or use a <placeholder> if it is illustrative.')
            exit_code = 1
        else:
            print('✅ Doc references: all resolve (%d files scanned)' % len(targets))
        if link_problems:
            print('❌ Doc links that do not resolve:')
            for target, line, ref in link_problems:
                print('   %s:%d: `%s`' % (target, line, ref))
            print('   Fix the target, or use a full URL if it is not in-tree.')
            exit_code = 1
        else:
            print('✅ Doc links: all resolve (%d files scanned)' % len(targets))

        # A :N cite past EOF is unambiguous rot.
        eof_problems = check_line_refs(targets, devel=devel)
        if eof_problems:
            print('❌ Cited line is past the end of the file:')
            for target, line, detail in eof_problems:
                print('   %s:%d: `%s`' % (target, line, detail))
            print('   Re-pin the citation to the current line.')
            exit_code = 1
        else:
            print('✅ Cited line numbers: all within their files')

        # Symbol-near-cite, ratcheted: the doc names a symbol next to a
        # file:line cite; the symbol must still be near that line. Baseline
        # keys are doc:line so fixing one cite never un-lists another.
        cite_findings = check_symbol_cites(targets, devel=devel)
        known = load_cite_baseline()
        if args.write_cite_baseline:
            with open(CITE_BASELINE, 'w') as f:
                f.write('# Symbol-cite drift accepted during calibration. '
                        'doc:line keys; shrink, never grow.\n')
                for _, _, key, _ in sorted(cite_findings):
                    f.write(key + '\n')
            print('Wrote %d keys to %s' % (len(cite_findings), CITE_BASELINE))
            return exit_code
        new = [x for x in cite_findings if x[2] not in known]
        if new:
            print('❌ Symbol-cite drift (symbol not near the cited line):')
            for target, line, key, detail in new:
                print('   %s: %s' % (key, detail))
            print('   Re-pin the line, or re-run with --write-cite-baseline '
                  'after reviewing.')
            exit_code = 1
        else:
            print('✅ Symbol cites: %d findings, all %d baselined'
                  % (len(cite_findings), len(cite_findings) - len(new)))

        # Content anchors: the strongest of the four citation checks and the
        # only self-healing one. Every cited line's own text is hashed into
        # scripts/doc_cite_anchors.tsv, so code moving does not rot the doc —
        # `make regen-doc-links` re-derives the number. What lands here is
        # either auto-repairable (stale/unanchored/orphan) or a line whose text
        # genuinely changed, which no generator can decide for a human.
        anchor_findings, anchor_stats = check_anchors(targets, devel=devel)
        if anchor_findings is None:
            print('⚠️  Citation anchors: %s absent — line numbers unverified'
                  % 'scripts/doc_cite_anchors.tsv')
        elif anchor_findings:
            # Same lazy import as check_anchors, for the same cycle reason.
            import doc_cite_anchors as anchors
            hard = [f for f in anchor_findings if f.kind in anchors.HARD_KINDS]
            print('❌ Citation anchors out of sync (%d):' % len(anchor_findings))
            for f in sorted(anchor_findings):
                where = '%s:%d' % (f.doc, f.doc_line) if f.doc_line else f.doc
                print('   %s: %s' % (where, f.detail))
            if hard:
                print('   %d of these name a line a generator cannot choose '
                      '(gone, ambiguous, or too low-information to anchor): '
                      're-read the sentence and re-cite by hand.' % len(hard))
            if len(hard) < len(anchor_findings):
                print('   Run: make regen-doc-links')
            exit_code = 1
        else:
            print('✅ Citation anchors: %d cited lines still resolve'
                  % anchor_stats['in_place'])

    if do_index:
        unindexed, present = check_index()
        if unindexed:
            print('❌ Docs in %s/ missing from %s:' % (DOC_DIR, DOC_INDEX))
            for f in unindexed:
                print('   %s' % f)
            print('   Add a row, or add to INDEX_EXEMPT if it is deliberately unrouted.')
            exit_code = 1
        else:
            print('✅ Doc index: all %d docs routed' % len(present))

    return exit_code


if __name__ == '__main__':
    sys.exit(main())
