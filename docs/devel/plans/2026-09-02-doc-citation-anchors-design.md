# Doc citations: named anchors instead of line numbers

Status: design approved, not yet implemented.

## The problem

A doc citation today carries a line number in its prose, and two derived
things hang off that number: a content-hash sidecar that re-pins it when code
moves, and a markdown link whose URL repeats it. Keeping the number honest is
the entire reason the machinery exists.

Measured on the tree at `0cf7df847`:

| | |
|---|---|
| citations | 730 across 41 docs, pointing into 261 files |
| `scripts/doc_cite_anchors.py` | 1321 lines |
| `scripts/doc_cite_anchors.tsv` | 96KB, 734 rows, committed |
| `scripts/doc_cite_anchor_baseline.txt` | a permanent exemption list for cites that cannot self-identify |
| commits touching the sidecar | 142 of the last 4228 |

The churn is not theoretical. `e047c1cfa`, a one-line z-offset fix, rewrote 8
doc files and 56 sidecar rows with no content change. Two of the five commits
before this design (`bd65a0837`, `2531973e7`) are pure re-pin commits.

The strongest argument is not churn, though. `doc_cite_anchors.tsv` is keyed on
`(doc, ref, line)`, so two different citations in one doc can collide on a single
key. Last write wins, and both then re-pin to the same line. That happened twice in
one sync merge (`compact_database` in chapter 06, `shutdown`'s `m_client.reset()` in
chapter 04) and the anchor gate reported all-green on it - only the separate symbol
check caught one of them. The current scheme can therefore be silently, confidently
wrong, and the gate cannot see it.

That failure has a second form, seen four times in three days: a citation whose line
still hashes the same re-pins cleanly while pointing at unrelated text.
`AUTOSAVE_MIN_CONFIDENCE` was cited at a doc-comment fragment about alias matching and
passed every check. Deleting the sidecar removes both forms.

Three further costs follow from the number living in the prose:

1. **Churn.** Ordinary code commits become doc commits. Noise in diffs, merge
   conflicts across worktrees, extra staging steps.
2. **Hard stops.** When a cited line's own text changes, the gate blocks and a
   human re-reads and re-cites by hand. Comment rewording triggers this, and
   105 of the 730 citations point at comment text.
3. **Pre-commit cost.** The gate wakes on any of the 261 cited files, so code
   commits pay for a doc check.

## The design

Line numbers leave the repository. Docs cite a *name*; line numbers are
computed on demand by a generator that renders a pinned copy.

### Anchor grammar

A citation is a path, optionally followed by `#` and a `/`-separated segment
path. A segment is an identifier or a quoted snippet, and each segment is
resolved within the region the previous segment found.

```
src/printer/printer_state.cpp                                  the file
src/printer/printer_state.cpp#update_from_status               a function
include/ui_nav_manager.h#PanelRequest/overlay_root             a member
src/application/application.cpp#instance/"shutdown_requested"  a line inside a function
ui_xml/home_panel.xml#carousel_host                            an XML name= attribute
mk/cross.mk#PLATFORM_TARGET                                    a make variable
tests/shell/test_code_lint.bats#"temp files use unit helpers"  a bats test name
```

One grammar covers C++ scope nesting, XML nesting, make, python, shell, bats
and markdown headings, because all of them are "find this name, then find that
name inside it".

A snippet segment only has to be unique inside its parent, so it stays short.
It is the escape hatch for a line with no name of its own, and it replaces the
content-hash sidecar: the doc states the text it expects, in the open, where a
reader can see it.

### The resolver refuses to guess

This is the uniqueness guarantee, and it is a rule rather than a heuristic: a
fragment that resolves to zero places, or to two or more, is an error that
names the candidates. There is no "pick the first match" path.

The consequence that matters: an anchor cannot silently start pointing
somewhere new after a refactor. The current content-hash scheme re-pins
automatically and is therefore capable of moving a citation to a line the
sentence never meant.

Measured on a scope-aware prototype over the existing 730 citations:

| | |
|---|---|
| resolve to a unique anchor | 97% |
| degrade to file-level (no name) | 18 |
| collide with a sibling in the same doc | 70 |

The 70 collisions are all one shape: the same function cited several times for
different lines inside it. Each takes a snippet segment.

### `make docs-pinned`

Renders every doc into `build/docs-pinned/` (gitignored) with citations
expanded to real `path:LINE` text and real `#L` links. This is the copy for
terminal jump-to-line, for handing someone a snapshot, and for a docs site.
Generated fresh on demand, so it cannot rot and cannot churn a diff.

`--resolve '<citation>'` answers "where is this right now" for a single
citation without rendering the set.

### Links in the committed docs

The committed docs carry a relative file link whose text is the full citation,
plus a `#:~:text=` scroll-to-text fragment.

GitHub ships the complete file in the blob page's initial HTML (verified:
`application.cpp`, 5304 lines, `rawLines` payload present and
`"truncated":false`), but the viewer virtualizes what it renders, so a
fragment will only match a line inside the rendered window. That is acceptable
because an unmatched text fragment loads the page at the top, which is exactly
what a bare file link does. The fragment is free upside, never a regression,
and it costs the generator one `urlencode`.

No committed link contains a line number, so code motion rewrites nothing.

### The gate

One script, `scripts/doc_anchors.py`, with three modes: resolve, render,
check. `--check` reports unresolved paths, missing names, and ambiguous
fragments.

It runs in `.githooks/pre-push` as **advisory**: it prints findings and does
not set a failing exit code. Pre-commit loses the doc-citation trigger
entirely.

Advisory is the right level because the failure mode it catches is a renamed
or deleted symbol, which is rare, and because the check is two greps over 261
files rather than a hash comparison over 730 rows.

## What gets deleted

- `scripts/doc_cite_anchors.py` (1321 lines)
- `scripts/doc_cite_anchors.tsv` (96KB, committed, regenerated on 142 commits)
- `scripts/doc_cite_anchor_baseline.txt`, and with it the whole
  "low-information line" category: a citation cannot point at a bare `}` when
  it has to point at a name
- `scripts/doc_cite_symbol_baseline.txt`
- `make regen-doc-anchors`, `make check-doc-anchors`
- the 261-path pre-commit trigger in `scripts/quality-checks.sh` (`qc_wanted`)

## What `check_doc_refs.py` is for afterwards

It stays, and it is not a leftover. It inspects **4055 path mentions across
118 docs**; only **879** of those carry a line number. The other 3176 are
path-only mentions that citation anchoring never touched. Anchoring was a
bolt-on to 22% of its input, not its purpose.

It keeps four checks and loses three:

| Check | Fate |
|---|---|
| `check_refs` - every path mention resolves, submodule-aware | keeps |
| `check_links` - markdown link targets resolve | keeps |
| `check_index` - all 88 docs routed from an index | keeps, never touched citations |
| `check_stale` - staleness by last-commit date | keeps |
| `check_line_refs` - cited line within file bounds | dies, no line numbers to bound |
| `check_symbol_cites` | dies, subsumed: the resolver proves a name exists as a precondition of resolving it |
| `check_anchors` - content-hash re-pin | dies, replaced |

**The seam.** `check_doc_refs.py` owns *does the thing exist* - paths, links,
index, staleness, across all 4055 mentions. `doc_anchors.py` owns *where
inside the file is it* - fragment to line, for the citations that carry a
fragment. No overlap, and the resolver imports the scanner rather than
reimplementing it. `gen_doc_links.py` already sets that precedent by importing
`PATH_RE` and `EXEMPT_SUBSTRINGS`.

Folding the two together was considered and rejected: it would drag 3176
path-only mentions into a script whose subject is fragments.

`gen_doc_links.py` folds into `doc_anchors.py`, since link generation and
fragment resolution are the same pass over the same citations.

## Migration

The converter runs over all 730 citations and writes a report before touching
anything. Each row gets a proposed anchor and a confidence:

- **automatic** - resolves uniquely, no sibling collision
- **needs a snippet** - collides with a sibling; the tool proposes a snippet
  segment taken from the current line's text
- **degrades to file-level** - no name available, and the cite was already
  pointing at line 1 or a file header in most cases
- **ambiguous** - resolves to more than one place; needs a human

Measured against the trunk's 742 citations with the finished resolver, not
estimated: **644 automatic (86%), 57 file-level, 41 wanting a human (5%)** - so the
rewrite is tool-driven at ~95%, not per-citation authoring.

The cases that look like hand work are not judgement calls. The chain builder picks a
LOCAL VARIABLE as the innermost definition: `auto err = reject_if_flat_schema(...)`
anchors to `err`, and eleven other `err` declarations in the file make it Ambiguous.
Same for `gcode` in `led_controller.cpp`. Preferring a real scope over a one-line local
moved it to 28 (3%), at the cost of pushing 25 citations to file-level - so neither
extreme is right, and the chain wants a rule that keeps a declaration when the citation
IS the declaration. Tune that here, in the report, before `--apply` runs.

Expect to review more than the collisions. The prototype mis-picks when a
cited comment sits inside a function and the walk-back lands on a nearby
member declaration instead. That is resolver tuning, and the report is where
it surfaces.

Migration lands as its own commit, separate from the tooling change, so the
730-citation rewrite is reviewable on its own.

## Testing

- Resolver unit tests per language: C++ scope nesting, XML `name=`, make
  variable vs target, python nested def, bats test name, markdown heading.
- Ambiguity is an error: a fragment matching two places must fail, not pick.
- Zero-match is an error.
- Snippet scoping: the same snippet text in two functions resolves under each
  parent without collision.
- Round trip: `--render` output for a known doc contains the same line numbers
  the old sidecar held, for citations that survive migration unchanged. This
  is the one check that proves the new resolver agrees with the old anchors.
- The advisory gate prints findings and exits 0.

## Risks

**Snippet segments reintroduce the "text changed" failure** for roughly 70
citations. The difference is that the expected text is visible in the doc, so
repair is a grep rather than an archaeology dig, and the check is advisory
rather than blocking.

**Resolver quality is the whole product.** A resolver that mis-picks silently
is worse than the current scheme. The refuse-to-guess rule is what contains
this, and it has to hold even when it is inconvenient during migration.

**Committed docs lose exact-line links on GitHub** for anything outside the
rendered window of a large file. `make docs-pinned` is the answer for anyone
who needs them.

## Resolved during design

- **`check_doc_refs.py` stays separate**, minus three checks. See the section
  above.
- **`build/docs-pinned/` is a make target only.** Not a pre-push artifact, not
  committed, until there is a concrete reason.
