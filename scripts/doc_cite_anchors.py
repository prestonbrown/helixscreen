#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generator + gate: make a doc's `src/foo.cpp:123` line citations self-healing.
#
# Line numbers in prose rot silently. Code moves, the number stays, and the
# sentence now describes whatever slid into that slot. check_doc_refs.py catches
# two slices of that — a cite past EOF, and a symbol named beside a cite that is
# no longer near it — but the symbol shape only appears next to about a quarter
# of the citations, and files mostly grow, so a stale number usually still lands
# inside the file and nothing complains.
#
# The fix is the same one gen_doc_links.py already applies to the link URL: stop
# maintaining the derived thing by hand and derive it. Here the derived thing is
# the line number itself.
#
#   For every citation we store a CONTENT ANCHOR in a committed sidecar
#   (scripts/doc_cite_anchors.tsv). On regen we resolve the anchor against the
#   current file and rewrite the number in the doc. A human is interrupted only
#   when the cited line is genuinely gone.
#
# Two tiers, and both are load-bearing:
#
#   primary  — hash of the CITED LINE ALONE, whitespace-normalised. This is what
#              survives ordinary editing: a neighbouring line can be rewritten,
#              a function above can grow, the whole block can move a thousand
#              lines, and the anchor still finds it.
#   context  — hash of the 5-line window (cited line ±2), whitespace-normalised.
#              Used ONLY to break a tie when the primary matches more than one
#              line ("}" and "" are the obvious cases). Making the 5-line hash
#              primary instead would be a false economy: every edit to a
#              NEIGHBOUR would then read as "your citation is gone", the gate
#              would cry wolf on ordinary commits, and people would learn to
#              ignore it. Measured on this tree (`--audit`): primary alone is
#              unique for 759 of 797 anchored cites; context settles 38 of the
#              remaining 39.
#
# There is no third tier. An anchor the context cannot disambiguate is a hard
# failure, because the only other answer available — "take the candidate nearest
# the number the doc already had" — is a coin flip that reports success, and it
# silently walked three 15-known-debt.md citations onto the wrong `# ====`
# banner. See SourceIndex.resolve().
#
# A citation may name a RANGE — `foo.cpp:63-65`, or the architecture guide's
# `foo.cpp:63`-65 with the end outside the backticks. The START is the anchor;
# re-pinning moves the whole block and keeps the authored span, because the code
# relocated, it did not resize.
#
# Whitespace normalisation (strip + collapse runs) is not optional either. This
# repo runs clang-format in its pre-commit hook, so a reindent of one function
# would otherwise invalidate every anchor inside it at once. It costs about a
# point of uniqueness and buys immunity to the single most common mechanical
# edit in the tree.
#
# ZERO primary hits is a hard error, deliberately. The cited line's own text
# changed, which means the thing the sentence points at was edited or deleted —
# no amount of fuzzy matching can tell whether the prose is still true. A human
# has to re-read the sentence.
#
# Not anchored, and why:
#   - a citation whose cited line cannot identify itself: blank, a comment
#     banner, or almost pure punctuation. See anchor_quality(). It names nothing
#     specific, so an anchor built on it points at a coincidence. The bootstrap
#     found 32 blank ones and every one was a genuine defect: about half were off
#     by a line, the rest pointed at code that had moved hundreds of lines
#     (`theme_manager.cpp:1966` for a function at 1975, `ams_state.cpp:829` for a
#     block at 849). The finding prints the nearest distinctive code so the next
#     one is a one-line repair, and doc_cite_anchor_baseline.txt records the ones
#     not yet repaired. Adding this check surfaced 48 citations across 41 keys of
#     inherited rot; the semantic re-pin audit cleared most of it, leaving 13
#     entries. Every one is a citation pointing somewhere a reader cannot use,
#     and every one is off by a line or two — the finding names the distinctive
#     line to move to. SHRINK IT, never grow it.
#   - a citation whose file cannot be resolved to exactly one path. That is
#     check_doc_refs' dead-path check talking, not ours. Its anchor row is KEPT
#     rather than reaped — see the unreadable-target branch in run().
#   - EXEMPT_SUBSTRINGS paths, plus ANCHOR_EXEMPT_SUBSTRINGS below.
#   - a citation inside a fenced code block or a ``literal`` span — those are
#     samples of syntax, not claims about the tree.
#
# Usage:
#   doc_cite_anchors.py                  # repair docs + sidecar in place
#   doc_cite_anchors.py --check          # verify only; exit 1 on any finding
#   doc_cite_anchors.py --diff           # --check plus the rewrites it would make
#   doc_cite_anchors.py PATH [PATH...]   # limit to .md files / directories
#   doc_cite_anchors.py --write-baseline # re-list unanchorable cites after review
#
# Run from the repo root: docs, sources, sidecar and baseline all resolve from
# the working directory, the same way check_doc_refs.py does.

import argparse
import hashlib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_doc_refs as gate      # noqa: E402  (LINE_REF_RE, EXEMPT_SUBSTRINGS)
import gen_doc_links as gen        # noqa: E402  (basename -> one path resolution)

# Committed artifacts, resolved from the working directory (repo root).
SIDECAR = os.path.join('scripts', 'doc_cite_anchors.tsv')
BASELINE = os.path.join('scripts', 'doc_cite_anchor_baseline.txt')

# Anchoring skips everything the reference gate already exempts, plus anything
# whose CONTENT churns on every commit — a generated table or an embedded
# version string moves line-for-line each time it is regenerated, so an anchor
# there would show up in every review diff and teach people to skim past it.
# Extending the gate's tuple rather than forking one: a churny file that does
# exist still gets its path proven by check_refs, and only the line anchor is
# skipped. Nothing in the tree needs this today; add substrings here when one
# does.
ANCHOR_EXEMPT_SUBSTRINGS = gate.EXEMPT_SUBSTRINGS + ()

FENCE_RE = re.compile(r'^\s*```')
# A doubled-backtick span shows a citation literally (the "write `src/foo.cpp:12`"
# instruction in the architecture guide's own README). Rewriting the number in
# one would edit the teaching example.
DISPLAY_SPAN_RE = re.compile(r'``.+?``')

# 6 bytes of blake2b. The primary hash is only ever looked up inside ONE file's
# line table, so 48 bits is far more collision headroom than a few thousand
# lines need, and 12 hex characters keep the sidecar readable in a diff.
HASH_BYTES = 6


def norm(line):
    """Whitespace-normalised line: leading/trailing stripped, runs collapsed."""
    return ' '.join(line.split())


# A line that half the file also contains cannot identify anything. Anchoring to
# one produces a row that looks healthy — it hashes, it stores, it "resolves" —
# while pointing at a coincidence, and the citation then drifts to whichever
# twin is nearest whenever the file is edited.
#
# Three shapes, each drawn from something already found in this tree rather than
# imagined:
#   banner  `# ====================` and its `// ---` relatives. 29 identical
#           copies in scripts/quality-checks.sh alone; this is what three
#           15-known-debt.md citations were pinned to.
#   punct   `}`, `};`, `{`, `*/`, `);`. src/printer/ams_state.cpp has 400+ lines
#           that normalise to exactly `}`, and 07-filament-ams.md's tenth
#           reading-list entry was anchored to one of them.
#   thin    anything else carrying almost no identity — `fi`, `else`, `#endif`.
#
# MIN_ANCHOR_ALNUM counts letters and digits, not characters, so punctuation-
# heavy but genuinely distinctive code (`m_ = {};`) is judged on the identifiers
# it contains. Four is the value that rejected every junk anchor in the corpus
# without rejecting a single real one; the audit that chose it is in the meta-
# suite as the low-information test cases.
MIN_ANCHOR_ALNUM = 4

_ALNUM_RE = re.compile(r'[0-9A-Za-z]')
# Optional comment opener, then a run of rule characters, then nothing but more
# of the same. Deliberately requires 6+ so a real line like `a -= b` is safe.
_BANNER_RE = re.compile(r'^(?:[#/*<!;%-]|\s)*[=*_~#+-]{6,}[\s*/#=_~+-]*$')


def anchor_quality(text):
    """'' when `text` can identify a line, else the reason it cannot."""
    if not text:
        return 'blank'
    if _BANNER_RE.match(text):
        return 'banner'
    if len(_ALNUM_RE.findall(text)) < MIN_ANCHOR_ALNUM:
        return 'low-information'
    return ''


# How each rejection reads in a finding. The wording has to make the fix obvious,
# because unlike a stale line number this one cannot be repaired mechanically.
QUALITY_BLURB = {
    'blank': 'cites a blank line — it names nothing',
    'banner': 'is anchored to a comment banner, which repeats verbatim '
              'throughout the file and cannot identify a line',
    'low-information': 'is anchored to a line with almost no content '
                       '(punctuation or a bare keyword), which repeats '
                       'throughout the file and cannot identify a line',
}

# The three anchor_quality() verdicts, which are also the three things
# --write-baseline may be asked to record.
QUALITY_KINDS = frozenset(QUALITY_BLURB)

# Findings a human must resolve, as against the ones `make regen-doc-links`
# repairs on its own (stale, unanchored, orphan). Everything here needs somebody
# to re-read the sentence and choose a line, which is exactly why none of them
# may be auto-fixed: the generator would be guessing at prose.
HARD_KINDS = QUALITY_KINDS | {'gone', 'ambiguous'}


def h(text):
    return hashlib.blake2b(text.encode('utf-8', 'replace'),
                           digest_size=HASH_BYTES).hexdigest()


class SourceIndex:
    """One file's lines, with normalisation and hashing done lazily.

    Two access patterns, and the cheap one dominates. In the steady state every
    citation still points at its own line, so all we need is ONE line's hash per
    citation — see `matches()`. Only a citation whose line moved needs the full
    {line hash: [line numbers]} table, and only for that one file. Building the
    table eagerly for every cited file instead cost 0.42s on this tree against
    0.09s lazily, for an answer that was thrown away 725 times out of 725.

    Normalisation is memoised per line rather than computed inside the lookup:
    the throwaway prototype that re-normalised per citation spent 663ms where
    this spends 15ms.
    """

    __slots__ = ('raw', '_norm', '_by_hash', '_ctx')

    def __init__(self, path):
        try:
            self.raw = open(path, errors='ignore').read().splitlines()
        except OSError:
            self.raw = []
        self._norm = [None] * len(self.raw)
        self._by_hash = None
        self._ctx = {}

    def __len__(self):
        return len(self.raw)

    def norm_line(self, n):
        """Normalised line n (1-based); '' outside the file, so a window can
        run off either end without special-casing."""
        if not 1 <= n <= len(self.raw):
            return ''
        v = self._norm[n - 1]
        if v is None:
            v = self._norm[n - 1] = norm(self.raw[n - 1])
        return v

    def line_hash(self, n):
        return h(self.norm_line(n))

    def context_hash(self, n):
        """Hash of the cited line ±2, out-of-range slots as empty lines.

        Padding rather than clamping keeps the window five entries wide at both
        ends of the file, so a cite at line 1 and a cite at line 3 cannot
        accidentally share a context.
        """
        if n not in self._ctx:
            self._ctx[n] = h('\n'.join(self.norm_line(i)
                                       for i in range(n - 2, n + 3)))
        return self._ctx[n]

    def by_hash(self):
        if self._by_hash is None:
            table = {}
            for i in range(1, len(self.raw) + 1):
                table.setdefault(self.line_hash(i), []).append(i)
            self._by_hash = table
        return self._by_hash

    def matches(self, n, primary):
        """True when the citation is still correct exactly as written.

        This is the answer for a doc nobody has invalidated, and it is also the
        RIGHT answer rather than merely the fast one: if the cited line still
        carries the anchored content, moving the citation to some other line
        that happens to hash the same would be churn. It also keeps the
        resolution stable — two identical lines cannot make the number
        oscillate between regens.
        """
        return 1 <= n <= len(self.raw) and self.line_hash(n) == primary

    def nearest_content(self, n, reach=6):
        """A hint for a cite that landed on a blank line: the closest real code.

        Every blank-line cite found on this tree was a near miss — the number
        points one line above the declaration the sentence names — so printing
        the neighbour turns "this is wrong" into a one-line fix.
        """
        for d in range(1, reach + 1):
            for i in (n + d, n - d):
                if 1 <= i <= len(self.raw) and self.norm_line(i):
                    text = self.norm_line(i)
                    if len(text) > 60:
                        text = text[:57] + '...'
                    return 'line %d ("%s")' % (i, text)
        return 'nothing within %d lines' % reach

    def resolve(self, primary, context):
        """(line, how), or (None, why) when the anchor does not identify a line.

        Two ways to fail and they mean different things. 'gone' is "no line in
        this file carries the anchored text any more". 'ambiguous' is "several
        do, and the 5-line context cannot say which" — the anchor was never
        specific enough to be re-pinned.

        There used to be a third answer: pick whichever candidate sits closest
        to the number the doc already had. That is the only branch here that
        could return a WRONG line while reporting success, and it did. Three
        citations in docs/devel/architecture/15-known-debt.md were anchored to
        `# ====...` banners, of which scripts/quality-checks.sh has 29; a
        33-line insert elsewhere in that file slid them onto a different banner
        and the gate printed a checkmark. A gate that quietly rewrites a doc to
        a wrong line is worse than no gate, so ambiguity is now a hard failure
        that names the citation and asks for a better one. It costs almost
        nothing: on the tree where this landed, 758 of 846 citations resolved
        uniquely and 86 more by context, leaving 2 that genuinely had to be
        re-cited by hand.
        """
        cands = self.by_hash().get(primary)
        if not cands:
            return None, 'gone'
        if len(cands) == 1:
            return cands[0], 'unique'
        narrowed = [i for i in cands if self.context_hash(i) == context]
        if len(narrowed) == 1:
            return narrowed[0], 'context'
        return None, 'ambiguous'


class Resolver:
    """Cited text -> repo-relative source path, with the indexes cached."""

    def __init__(self, devel=True):
        self.devel = devel
        self._files = None
        self._by_basename = None
        self._paths = {}
        self._indexes = {}

    def _repo(self):
        if self._files is None:
            self._files, self._by_basename = gen.repo_index()
        return self._files, self._by_basename

    def path_for(self, ref, doc):
        """The one file a citation names, or None to leave the citation alone.

        Strict first (repo-relative, or relative to the doc's own directory) —
        that is check_doc_refs' own resolver, and it is right for 655 of this
        tree's citations. Docs also cite bare basenames (`ams_state.cpp:829`),
        which the strict resolver refuses because it cannot tell WHICH file is
        meant. gen_doc_links already answers exactly that question to build the
        link a reader clicks, and only when the answer is unique, so reusing it
        here anchors 97 more citations without inventing a second policy: if the
        rendered link is wrong the anchor is wrong in the same place, visibly.
        """
        key = (ref, doc)
        if key in self._paths:
            return self._paths[key]
        base = os.path.dirname(doc)
        hit = gate._resolve_cited(ref, base, self.devel)
        if hit is None:
            files, by_basename = self._repo()
            hit = gen.resolve(ref, base, files, by_basename)
            if hit is not None and not os.path.isfile(hit):
                hit = None
        if hit is not None:
            hit = os.path.normpath(hit)
        self._paths[key] = hit
        return hit

    def index(self, path):
        idx = self._indexes.get(path)
        if idx is None:
            idx = self._indexes[path] = SourceIndex(path)
        return idx


def iter_citations(text):
    """Yield (doc_line, span, ref, n) for every live `path:N` citation.

    Fenced blocks and ``literal`` spans are skipped: both hold syntax samples,
    not claims about where code lives. A citation already wrapped in a markdown
    link is NOT skipped — the backticked span survives the wrapper, so the
    number inside the link text is repaired and gen_doc_links re-derives the URL
    from it on the next pass. That ordering is why `make regen-doc-links` runs
    anchors before links.
    """
    fenced = False
    for doc_line, line in enumerate(text.split('\n'), 1):
        if FENCE_RE.match(line):
            fenced = not fenced
            continue
        if fenced or '`' not in line:
            continue          # no code span, so no citation: skip two regex scans
        display = [m.span() for m in DISPLAY_SPAN_RE.finditer(line)]
        for m in gate.LINE_REF_RE.finditer(line):
            if any(a <= m.start() < b for a, b in display):
                continue
            tail = gate.trailing_range(line, m.end())
            yield Cite(doc_line, m.span(), m.group('ref'), int(m.group('line')),
                       m.group('rdash'), m.group('rend'), tail)


class Cite:
    """One `path:N` (or `path:N-M`) citation as it stands in a doc."""

    __slots__ = ('doc_line', 'span', 'ref', 'line', 'rdash', 'rend', 'tail')

    def __init__(self, doc_line, span, ref, line, rdash, rend, tail=None):
        self.doc_line, self.span, self.ref, self.line = doc_line, span, ref, line
        self.rdash, self.rend = rdash, (int(rend) if rend else None)
        # A range end written outside the backticks, with its own span so the
        # generated link between the two is left for gen_doc_links to re-derive.
        self.tail = tail
        if tail and self.rend is None:
            self.rdash, self.rend = tail[0], tail[1]

    @property
    def as_written(self):
        """The citation exactly as it appears in the doc.

        Findings quote this rather than rebuilding `ref:line`: a range cite
        reported as its bare start line sends the reader searching the doc for
        a string that is not in it.
        """
        return self.rendered(self.line)

    def rendered(self, new_line):
        """The citation re-pinned to `new_line`, range and all.

        A range names a BLOCK, so re-pinning moves the whole block and keeps the
        authored span length — the code relocated, it did not resize. Rewriting
        only the start would produce `foo.cpp:70-65`, a backwards range that no
        reader and no later run could make sense of.
        """
        if self.rend is None or self.tail:
            return '`%s:%d`' % (self.ref, new_line)
        return '`%s:%d%s%d`' % (self.ref, new_line, self.rdash, self.moved_end(new_line))

    def moved_end(self, new_line):
        return new_line + (self.rend - self.line)

    def rendered_tail(self, new_line):
        """The outside-the-backticks range end, moved with the block."""
        return '%s%d' % (self.rdash, self.moved_end(new_line))


def anchorable(ref):
    return not any(s in ref for s in ANCHOR_EXEMPT_SUBSTRINGS)


# ---------------------------------------------------------------------------
# Sidecar
#
# TSV, one row per citation, sorted. Chosen over JSON/YAML for one reason that
# matters more than any other property here: this file is reviewed as a DIFF. A
# citation that moved is exactly one changed line, in a stable position, with no
# reflowed braces or shifted indentation around it, and `git diff` on it is
# readable without a tool. It is also greppable ("which docs cite this file?")
# and it matches the precedent already in this directory —
# doc_cite_symbol_baseline.txt and asan_leak_baseline.txt are both flat text.
#
#   doc <TAB> cited-path <TAB> line <TAB> resolved-path <TAB> primary <TAB> context
#
# The KEY is (doc, cited-path, line): the citation as it currently stands in the
# doc. The doc's own line number is deliberately NOT part of it, so editing
# prose above a citation does not churn the sidecar. `line` is part of the key
# and is rewritten in lockstep with the doc, which keeps the whole file a pure
# function of (docs, sources) — regen is idempotent, and a hand-edited row is
# repaired rather than maintained.
#
# resolved-path is written out even when it merely repeats cited-path: it is the
# only place a reviewer can see which file a bare-basename citation was taken to
# mean, and it makes the file grep-answerable by target.
# ---------------------------------------------------------------------------

SIDECAR_HEADER = (
    '# Content anchors for `file:line` citations in docs. GENERATED — run\n'
    '# `make regen-doc-links`, never hand-edit. See scripts/doc_cite_anchors.py.\n'
    '#\n'
    '# doc\tcited-path\tline\tresolved-path\tline-hash\tcontext-hash\n'
)

BASELINE_HEADER = (
    '# Citations whose cited line cannot identify itself, so no content anchor\n'
    '# can be built. SHRINK, NEVER GROW.\n'
    '#\n'
    '#   blank            the cite points at an empty line\n'
    '#   banner           it points at a `# ====` rule, which repeats verbatim\n'
    '#   low-information  it points at `}`, `*/`, `try {` or similar\n'
    '#\n'
    '# Every row is a citation a reader cannot follow: the number names a line\n'
    '# that says nothing about what the sentence claims. These were inherited —\n'
    '# the bootstrap anchored whatever each doc happened to point at, and the\n'
    '# quality check surfaced them all at once when it was added. An entry\n'
    '# leaves by re-citing the line the sentence actually describes, then\n'
    '# `make regen-doc-links`; the finding prints the nearest distinctive code.\n'
    '#\n'
    '# doc\tcited-path:line\treason\n'
)


def load_sidecar(path=SIDECAR):
    """{(doc, ref, line): (resolved, primary, context)}, or None if absent.

    None (file missing) means "this tree has not opted in" and switches the
    whole check off — which is what keeps the miniature-repo meta-tests of the
    sibling gates passing, and makes adopting the anchors a single commit
    rather than a flag day.
    """
    if not os.path.isfile(path):
        return None
    rows = {}
    for raw in open(path, errors='ignore'):
        raw = raw.rstrip('\n')
        if not raw or raw.startswith('#'):
            continue
        parts = raw.split('\t')
        if len(parts) != 6:
            continue
        doc, ref, line, resolved, primary, context = parts
        try:
            rows[(doc, ref, int(line))] = (resolved, primary, context)
        except ValueError:
            continue
    return rows


def write_sidecar(rows, path=SIDECAR):
    with open(path, 'w') as f:
        f.write(SIDECAR_HEADER)
        for (doc, ref, line), (resolved, primary, context) in sorted(
                rows.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
            f.write('%s\t%s\t%d\t%s\t%s\t%s\n'
                    % (doc, ref, line, resolved, primary, context))


UNRESOLVED_KEY = 'max-unresolved:'


def load_ceiling(path=BASELINE):
    """The `max-unresolved:` ceiling, or None when the file does not set one.

    A key set alone cannot see this class at all. A citation whose PATH does not
    resolve is skipped by every check here and deferred to check_refs — which is
    right, that is its job — but check_refs passes a bare basename the moment
    ANY file in the tree shares it, submodules included (`file.cpp` resolves to
    lib/cpp-terminal's). So a citation can be unanchorable AND unreported, and
    nothing counts how many of those exist. Combined with the range hole that
    used to hide dead paths, that is a bucket things fall into quietly. A count
    is the only handle: the members change as docs are edited, the SIZE is what
    must not grow.
    """
    if not os.path.isfile(path):
        return None
    for raw in open(path, errors='ignore'):
        raw = raw.strip()
        if raw.startswith(UNRESOLVED_KEY):
            try:
                return int(raw[len(UNRESOLVED_KEY):].strip())
            except ValueError:
                return None
    return None


def load_baseline(path=BASELINE):
    known = set()
    if not os.path.isfile(path):
        return known
    for raw in open(path, errors='ignore'):
        raw = raw.strip()
        if not raw or raw.startswith('#'):
            continue
        if raw.startswith(UNRESOLVED_KEY):
            continue
        parts = raw.split('\t')
        if len(parts) >= 2:
            known.add((parts[0], parts[1]))
    return known


def write_baseline(entries, path=BASELINE, unresolved=None):
    with open(path, 'w') as f:
        f.write(BASELINE_HEADER)
        if unresolved is not None:
            f.write('%s %d\n\n' % (UNRESOLVED_KEY, unresolved))
        for doc, ref, line, reason in sorted(entries):
            f.write('%s\t%s:%d\t%s\n' % (doc, ref, line, reason))


# ---------------------------------------------------------------------------
# The pass itself. One walk serves regen and check; `write` decides whether the
# repairs land on disk.
# ---------------------------------------------------------------------------

class Finding:
    __slots__ = ('doc', 'doc_line', 'kind', 'detail')

    def __init__(self, doc, doc_line, kind, detail):
        self.doc, self.doc_line, self.kind, self.detail = doc, doc_line, kind, detail

    def __lt__(self, other):
        return (self.doc, self.doc_line) < (other.doc, other.doc_line)


def run(targets, stored, devel=True, write=False, audit=False, rebaseline=False):
    """Resolve every citation's anchor; optionally rewrite docs and sidecar.

    Returns (findings, fresh_rows, blanks, rewrites, stats). `stored` is the
    sidecar as loaded; a citation missing from it is bootstrapped when writing
    and reported as unanchored when checking. `audit` forces the full two-tier
    resolution even for citations that are already correct, so the tier
    histogram reflects how much of the corpus leans on the context tiebreak.
    """
    # --write-baseline has to see the citations the baseline is already hiding,
    # or it re-derives the file from what is left after they were skipped —
    # which is nothing, so it empties the very list it was asked to rewrite.
    # (It did: one invocation wiped 13 entries and a second put them back.)
    baselined = set() if rebaseline else load_baseline()
    resolver = Resolver(devel=devel)
    findings, blanks, rewrites = [], [], []
    fresh = {}
    seen_keys = set()
    stats = {'in_place': 0, 'unique': 0, 'context': 0, 'unresolved': 0,
             'bootstrapped': 0, 'skipped': 0, 'cites': 0}

    target_set = set(targets)
    for doc in targets:
        try:
            text = open(doc, errors='ignore').read()
        except OSError:
            continue
        # Per-line edits collected first, applied afterwards, so every lookup
        # sees the numbers the doc was written with.
        edits = {}
        for cite in iter_citations(text):
            doc_line, ref, n = cite.doc_line, cite.ref, cite.line
            stats['cites'] += 1
            if not anchorable(ref):
                stats['skipped'] += 1
                continue
            if (doc, '%s:%d' % (ref, n)) in baselined:
                stats['skipped'] += 1
                continue
            key = (doc, ref, n)
            path = resolver.path_for(ref, doc)
            idx = resolver.index(path) if path is not None else None
            # An unreadable target is NOT an uncited one. A worktree merge runs
            # with lib/ swapped for empty directories (setup-worktree.sh
            # --unlink), so every lib/ citation resolves to nothing for the
            # length of the merge; reaping those rows dropped 11 anchors per
            # merge and silently re-bootstrapped them afterwards against
            # whatever the line held by then. Keep the row verbatim and let
            # check_refs own the dead-path verdict. An empty index is the same
            # case one step later — the path resolved but nothing came back, so
            # every anchor into it would read as 'gone'.
            if path is None or not len(idx):
                stats['skipped'] += 1
                if path is None:
                    stats['unresolved'] += 1
                if stored is not None and key in stored:
                    seen_keys.add(key)
                    fresh[key] = stored[key]
                continue

            row = stored.get(key) if stored is not None else None

            if row is None:
                # Nothing stored yet, so the cited line itself has to be worth
                # anchoring to. Out of range is check_line_refs' past-EOF error;
                # a line that cannot identify itself is our own finding.
                if not 1 <= n <= len(idx):
                    stats['skipped'] += 1
                    continue
                why = anchor_quality(idx.norm_line(n))
                if why:
                    blanks.append((doc, ref, n, why))
                    findings.append(Finding(
                        doc, doc_line, why,
                        '%s %s. Re-pin it to the line the sentence describes; '
                        'nearest distinctive code is %s.'
                        % (cite.as_written, QUALITY_BLURB[why],
                           idx.nearest_content(n))))
                    continue
                seen_keys.add(key)
                if not write:
                    findings.append(Finding(
                        doc, doc_line, 'unanchored',
                        '%s has no anchor — run `make regen-doc-links`.'
                        % cite.as_written))
                    continue
                # Bootstrap: whatever is on the cited line becomes the anchor.
                stats['bootstrapped'] += 1
                fresh[key] = (path, idx.line_hash(n), idx.context_hash(n))
                continue

            # An anchored citation is resolved even when its number is now past
            # EOF: a file that SHRANK is exactly the case the anchor repairs,
            # and skipping it would leave check_line_refs' hard error with no
            # way to fix itself.
            seen_keys.add(key)
            _, primary, context = row
            if idx.matches(n, primary) and not audit:
                new_line, how = n, 'in_place'
            else:
                new_line, how = idx.resolve(primary, context)
            if new_line is None:
                detail = (
                    'the cited line is gone — %s no longer matches its anchor. '
                    'Re-read the sentence and re-pin it by hand.'
                    if how == 'gone' else
                    '%s cannot be re-pinned — several lines in the file carry '
                    'its anchor and the surrounding context does not separate '
                    'them. Re-cite it at a line that names what the sentence '
                    'describes.') % cite.as_written
                findings.append(Finding(doc, doc_line, how, detail))
                # Keep the row: dropping it would silently downgrade the hard
                # error to "unanchored" on the next run.
                fresh[key] = row
                continue
            # An anchor that still resolves can still be worthless. Rows written
            # before this check existed were bootstrapped onto whatever the doc
            # happened to point at, banners and bare braces included, so the
            # quality gate has to run on the RESOLVED line every time and not
            # only where a row is created. This is what turns an inherited bad
            # anchor red instead of maintaining it forever.
            why = anchor_quality(idx.norm_line(new_line))
            if why:
                blanks.append((doc, ref, n, why))
                findings.append(Finding(
                    doc, doc_line, why,
                    '%s %s. Re-pin it to the line the sentence describes; '
                    'nearest distinctive code is %s.'
                    % (cite.as_written, QUALITY_BLURB[why],
                       idx.nearest_content(new_line))))
                fresh[key] = row
                continue
            stats[how] += 1
            if new_line != n:
                rewrites.append((doc, doc_line, ref, n, new_line, how))
                if not write:
                    findings.append(Finding(
                        doc, doc_line, 'stale',
                        '%s is out of date — its anchor is now at line %d '
                        '(%s). Run `make regen-doc-links`.'
                        % (cite.as_written, new_line, how)))
                    fresh[key] = row
                    continue
                edits[span_key(doc_line, cite.span)] = cite.rendered(new_line)
                if cite.tail:
                    edits[span_key(doc_line, cite.tail[2])] = \
                        cite.rendered_tail(new_line)
            # Re-derive both hashes from the line we landed on, so a neighbour
            # edit refreshes the tiebreak instead of ageing out of usefulness.
            fresh[(doc, ref, new_line)] = (path, idx.line_hash(new_line),
                                           idx.context_hash(new_line))

        if write and edits:
            text = apply_edits(text, edits)
            with open(doc, 'w') as f:
                f.write(text)

    # A row for a citation that is no longer in the doc. On a write run it is
    # simply dropped (`merged` is rebuilt from `fresh`); on a check run it means
    # the committed sidecar has drifted from the docs, which regen repairs.
    if stored is not None and not write:
        for key in stored:
            if key[0] in target_set and key not in seen_keys and key not in fresh:
                doc, ref, line = key
                findings.append(Finding(
                    doc, 0, 'orphan',
                    '`%s:%d` is no longer cited by this doc but still has an '
                    'anchor row — run `make regen-doc-links`.' % (ref, line)))
    return findings, fresh, blanks, rewrites, stats


def span_key(doc_line, span):
    return (doc_line, span[0], span[1])


def apply_edits(text, edits):
    """Splice pre-rendered citations in at their recorded (line, start, end) spans."""
    out = []
    for doc_line, line in enumerate(text.split('\n'), 1):
        pieces, cursor = [], 0
        for (dl, a, b), replacement in sorted(edits.items()):
            if dl != doc_line:
                continue
            pieces.append(line[cursor:a])
            pieces.append(replacement)
            cursor = b
        pieces.append(line[cursor:])
        out.append(''.join(pieces))
    return '\n'.join(out)


_DEFAULT_TARGETS = None


def default_targets():
    """Every doc the reference gate scans. Computed once per process."""
    global _DEFAULT_TARGETS
    if _DEFAULT_TARGETS is None:
        _DEFAULT_TARGETS = sorted(set(gate.scan_targets())
                                  | set(gate.scan_devel_targets([])))
    return _DEFAULT_TARGETS


def scoped_targets(paths):
    """The requested docs, narrowed to the ones the reference gate scans.

    The narrowing keeps a targeted run honest in both directions.

    Outward: a sibling gate's meta-test hands `--devel /tmp/fixture/doc.md`
    while sitting in the repo root, so the real sidecar is loaded against a
    fixture that has no anchors and never will. Without this the anchor check
    would fail the tree over a file that is not part of the corpus — the exact
    false-positive class that gets a gate switched off. (It cost three passing
    tests in tests/shell/test_doc_refs_gate.bats the first time round.)

    Inward: a doc IS in the default set whether or not the sidecar has heard of
    it, so a brand-new doc's citations are checked — as unanchored — from the
    very first run instead of quietly opting themselves out by having no rows.
    A point-in-time doc under docs/devel/plans/ is outside the default set by
    design and stays unchecked, which is the same rule the other doc checks use.
    """
    scanned = set(default_targets())
    wanted = gate.scan_devel_targets(paths) if paths else default_targets()
    return [t for t in wanted if t in scanned]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*', metavar='PATH',
                    help='.md files or directories (default: every doc the '
                         'reference gate scans)')
    ap.add_argument('--check', action='store_true',
                    help='do not write; exit 1 if anything is out of sync')
    ap.add_argument('--diff', action='store_true',
                    help='implies --check; list the rewrites it would make')
    ap.add_argument('--write-baseline', action='store_true',
                    help='re-list unanchorable citations after reviewing them')
    ap.add_argument('--audit', action='store_true',
                    help='implies --check; resolve every anchor the long way '
                         'and print how many need each tier')
    args = ap.parse_args()

    targets = scoped_targets(args.paths)
    if not targets:
        print('❌ No .md files to process')
        return 1

    check_only = args.check or args.diff or args.audit
    stored = load_sidecar()
    if stored is None and check_only:
        print('⚠️  Citation anchors: %s absent — nothing to verify' % SIDECAR)
        return 0
    if stored is None:
        stored = {}

    findings, fresh, blanks, rewrites, stats = run(
        targets, stored, write=not check_only, audit=args.audit,
        rebaseline=args.write_baseline)

    if not check_only:
        # A doc outside this run's scope keeps its rows: a targeted regen must
        # not silently strip anchors it never looked at. A doc that no longer
        # EXISTS is the opposite case and has to be swept, or the sidecar only
        # ever grows: out-of-scope rows are preserved by the rule above, and a
        # deleted doc is permanently out of scope, so nothing could ever remove
        # them. 36 rows naming six `.claude/worktrees/agent-*/` scratch trees
        # had accumulated that way — directories that existed on one machine for
        # one afternoon, committed into a shared file, unresolvable for everyone
        # else forever. Only a FULL run sweeps: a targeted run has no business
        # judging docs it was not pointed at.
        scope = set(targets)
        merged = {k: v for k, v in stored.items()
                  if k[0] not in scope and (args.paths or os.path.isfile(k[0]))}
        merged.update(fresh)
        write_sidecar(merged)
        if args.write_baseline:
            # A FULL run just walked every scanned doc, so `blanks` IS the whole
            # set and the file is rewritten from it — that is the only way an
            # entry whose citation has since been repaired ever leaves. Union
            # only on a TARGETED run, where the entries for docs this run never
            # opened would otherwise be dropped on the floor.
            #
            # Without the split the ratchet is a one-way valve: it can only
            # grow, which is exactly the slack 02da71dbe went round removing
            # from the count baselines. Measured after the semantic audit
            # landed: 28 of 41 entries named citations that no longer exist.
            entries = set(blanks)
            if args.paths:
                entries |= _existing_baseline_entries()
            # The ceiling is only re-derived by a FULL run, for the same reason
            # the key set is: a targeted run counted a subset and would ratchet
            # the number down to it, failing the next full run over citations it
            # never looked at.
            write_baseline(entries, unresolved=None if args.paths
                           else stats['unresolved'])
            blanks = []
            findings = [f for f in findings if f.kind not in QUALITY_KINDS]

    # A citation whose path does not resolve is nobody's finding here — it is
    # deferred to check_refs — so the only way this class can be held is by
    # counting it. Enforced on any run that did not just rewrite the number.
    ceiling = load_ceiling()
    over = (ceiling is not None and stats['unresolved'] > ceiling)

    hard = [f for f in findings if f.kind in HARD_KINDS]
    soft = [f for f in findings if f.kind not in HARD_KINDS]

    if args.diff and rewrites:
        print('   Rewrites this would make:')
        for doc, doc_line, ref, old, new, how in rewrites[:20]:
            print('     %s:%d: `%s:%d` -> :%d (%s)' % (doc, doc_line, ref, old, new, how))
        if len(rewrites) > 20:
            print('     ... and %d more' % (len(rewrites) - 20))

    if args.audit:
        # The histogram is the whole point of --audit, so it prints even when
        # the run also has findings. --audit re-resolves EVERY anchor the long
        # way instead of accepting the ones that still match in place, so it is
        # the only mode that surfaces a latent ambiguity: an anchor that is fine
        # today and unresolvable the moment its file shifts.
        total = stats['unique'] + stats['context']
        print('ℹ️  Anchor tiers: %d resolved — %d by the cited line alone, '
              '%d needed the 5-line context to break a tie'
              % (total, stats['unique'], stats['context']))

    if findings:
        print('❌ Citation anchors (%d):' % len(findings))
        for f in sorted(findings):
            where = '%s:%d' % (f.doc, f.doc_line) if f.doc_line else f.doc
            print('   %s: %s' % (where, f.detail))
        if hard:
            print('   %d of these name a line no generator can choose (gone, '
                  'ambiguous, or too low-information to anchor): re-read the '
                  'sentence, re-cite it, then `make regen-doc-links`.' % len(hard))
        elif soft:
            print('   Run: make regen-doc-links')
        if over:
            _report_over(stats['unresolved'], ceiling)
        return 1

    if over:
        _report_over(stats['unresolved'], ceiling)
        return 1

    if check_only:
        print('✅ Citation anchors: %d cited lines still resolve' % stats['in_place'])
    else:
        print('✅ Citation anchors: %d anchored across %d docs (%d rewritten, '
              '%d bootstrapped)' % (len(fresh), len(targets), len(rewrites),
                                    stats['bootstrapped']))
    return 0


def _report_over(count, ceiling):
    print('❌ Citation anchors: %d citations name a path that does not resolve, '
          'over the baseline of %d.' % (count, ceiling))
    print('   These are anchored by nothing and reported by nothing — '
          'check_refs passes a bare basename as soon as ANY file in the tree '
          'shares it, submodules included.')
    print('   Fix the path, or unbacktick it if it is external/device-side. '
          'The number may fall, never rise; `--write-baseline` re-derives it '
          'after review.')


def _existing_baseline_entries():
    """Baseline rows as (doc, ref, line, reason), so a rewrite is additive.

    A blank-line cite in a doc outside this run's scope was never inspected;
    dropping its row would un-baseline it and fail the next full run.
    """
    out = set()
    if not os.path.isfile(BASELINE):
        return out
    for raw in open(BASELINE, errors='ignore'):
        raw = raw.strip()
        if not raw or raw.startswith('#'):
            continue
        if raw.startswith(UNRESOLVED_KEY):
            continue
        parts = raw.split('\t')
        if len(parts) < 3:
            continue
        ref, _, line = parts[1].rpartition(':')
        try:
            out.add((parts[0], ref, int(line), parts[2]))
        except ValueError:
            continue
    return out


if __name__ == '__main__':
    sys.exit(main())
