#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/doc_cite_anchors.py — the content anchors that make a
# doc's `file.cpp:123` citation self-healing, and for the check_doc_refs.py gate
# that proves the docs and the sidecar agree.
#
# The generator is entirely cwd-relative (docs, sources, and the sidecar all
# resolve from the working directory), so each test builds a miniature repo in
# BATS_TEST_TMPDIR and cds into it.

load helpers

setup() {
    ANCH="$BATS_TEST_DIRNAME/../../scripts/doc_cite_anchors.py"
    CHECK="$BATS_TEST_DIRNAME/../../scripts/check_doc_refs.py"
    REPO="$BATS_TEST_TMPDIR/repo"
    DOC="$REPO/docs/devel/zz_doc.md"
    SRC="$REPO/src/zz_anchor.cpp"
    mkdir -p "$REPO/docs/devel" "$REPO/src" "$REPO/scripts"
}

# A source file whose line 10 is a unique, greppable marker.
seed_src() {
    {
        for i in $(seq 9); do echo "// filler $i"; done
        echo "void zz_marker_alpha(void) { return; }"
        for i in $(seq 10 20); do echo "// filler $i"; done
    } > "$SRC"
}

sidecar() { cat "$REPO/scripts/doc_cite_anchors.tsv"; }

@test "bootstrap: a citation gets one sidecar row and then verifies clean" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    [ -f scripts/doc_cite_anchors.tsv ]
    grep -q $'docs/devel/zz_doc.md\tsrc/zz_anchor.cpp\t10\t' scripts/doc_cite_anchors.tsv
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "repair: a moved code block rewrites the citation and the sidecar" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    # Five new lines at the top push the marker from line 10 to line 15.
    { for i in $(seq 5); do echo "// prologue $i"; done; cat "$SRC"; } > "$SRC.new"
    mv "$SRC.new" "$SRC"

    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"out of date"* ]]

    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    grep -q '`src/zz_anchor.cpp:15`' "$DOC"
    refute_grep 'zz_anchor.cpp:10' "$DOC"
    grep -q $'docs/devel/zz_doc.md\tsrc/zz_anchor.cpp\t15\t' scripts/doc_cite_anchors.tsv
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "hard error: deleting the cited line is not auto-repairable" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    sed -i 's/void zz_marker_alpha.*/void zz_renamed_entirely(void) { return; }/' "$SRC"

    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"cited line is gone"* ]]
    [[ "$output" == *"src/zz_anchor.cpp:10"* ]]

    # Regen cannot invent an anchor for it either: it reports and fails.
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"cited line is gone"* ]]
    grep -q '`src/zz_anchor.cpp:10`' "$DOC"
}

@test "neighbour edit: changing an adjacent line does NOT break the anchor" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    sed -i '9s/.*/\/\/ a totally rewritten neighbour above the cite/' "$SRC"
    sed -i '11s/.*/\/\/ and a rewritten neighbour below it/' "$SRC"
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "whitespace: a clang-format style reindent does NOT break the anchor" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    sed -i '10s/.*/        void   zz_marker_alpha(void)   { return; }/' "$SRC"
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "ambiguous: identical lines are disambiguated by the 5-line context" {
    # The twinned line must be distinctive enough to survive anchor_quality() —
    # a bare `}` is rejected before the tiebreak is ever consulted — so this uses
    # a real statement that legitimately appears twice.
    {
        echo "// unique head A"
        echo "    return compute_total(items);"   # line 2  — twin A
        echo "// unique tail A"
        for i in $(seq 4 9); do echo "// filler $i"; done
        echo "// unique head B"
        echo "    return compute_total(items);"   # line 11 — twin B, cited
        echo "// unique tail B"
    } > "$SRC"
    echo 'The second return at `src/zz_anchor.cpp:11`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel

    # Pad at the TOP so both twins shift by the same amount and twin B keeps its
    # ±2 window intact — that window is the only thing that can tell B from A.
    # (Padding between them, as this test used to do, changes B's window too and
    # nothing can disambiguate; that case is the next test, and it used to be
    # answered by a nearest-to-old-line guess.)
    sed -i '1i\// pad 1\n// pad 2\n// pad 3' "$SRC"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    grep -q '`src/zz_anchor.cpp:14`' "$DOC"
}

@test "ambiguous: no distinguishing context is a hard failure, never a guess" {
    # Three identical twins. The cited one is then overwritten, so the anchor no
    # longer matches in place and has to be re-resolved — and the two survivors
    # are equally plausible, with neither carrying the stored context.
    {
        echo "// filler 1"
        echo "    return compute_total(items);"   # twin A
        echo "// filler 3"
        echo "// filler 4"
        echo "    return compute_total(items);"   # line 5 — cited
        echo "// filler 6"
        echo "// filler 7"
        echo "    return compute_total(items);"   # twin C
        echo "// filler 9"
    } > "$SRC"
    echo 'See `src/zz_anchor.cpp:5`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    sed -i '5s/.*/\/\/ the cited line is now something else/' "$SRC"

    # The old behaviour picked whichever twin sat nearest the number already in
    # the doc and reported success. That is a coin flip presented as a repair,
    # and on the real tree it walked three citations onto the wrong `# ====`
    # banner while printing a checkmark.
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"cannot be re-pinned"* ]]
    # It must not have rewritten the doc to a guess.
    grep -q '`src/zz_anchor.cpp:5`' "$DOC"

    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 1 ]
}

@test "quality: a citation anchored to punctuation is rejected at bootstrap" {
    {
        for i in $(seq 9); do echo "// filler $i"; done
        echo "}"
        echo "void zz_marker_alpha(void) { return; }"
    } > "$SRC"
    echo 'The close brace at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"almost no content"* ]]
    # It names the real code nearby, so the repair is one edit.
    [[ "$output" == *"zz_marker_alpha"* ]]
    # No row may be written for an anchor that cannot identify a line.
    ! grep -q $'src/zz_anchor.cpp\t10\t' scripts/doc_cite_anchors.tsv
}

@test "quality: a citation anchored to a comment banner is rejected" {
    {
        for i in $(seq 9); do echo "// filler $i"; done
        echo "// ============================================================"
        echo "void zz_marker_alpha(void) { return; }"
    } > "$SRC"
    echo 'The section at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"comment banner"* ]]
}

@test "quality: an INHERITED low-information anchor is reported, not maintained" {
    # The row is written while the line still looks fine, then the line decays
    # into a bare brace. A gate that only checked at bootstrap would keep
    # re-pinning this one forever; this is the shape that hid three wrong
    # citations in 15-known-debt.md behind a green run.
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    grep -q $'src/zz_anchor.cpp\t10\t' scripts/doc_cite_anchors.tsv

    python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]
lines = open(p).read().split('\n')
lines[9] = '}'
open(p, 'w').write('\n'.join(lines))
PYEOF
    # Re-point the sidecar row at the decayed line so it is "anchored" to it.
    python3 "$ANCH" docs/devel >/dev/null 2>&1 || true

    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 1 ]
}

@test "unreadable target: the anchor row survives, it is not reaped" {
    # setup-worktree.sh --unlink swaps lib/ for empty directories so git can
    # scan the tree, and a regen during that window used to drop every lib/
    # anchor and silently re-bootstrap it afterwards against whatever the line
    # held by then. 11 rows per worktree merge.
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    grep -q $'src/zz_anchor.cpp\t10\t' scripts/doc_cite_anchors.tsv

    mv "$SRC" "$SRC.hidden"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    grep -q $'src/zz_anchor.cpp\t10\t' scripts/doc_cite_anchors.tsv

    # And it still verifies once the file is back, with no re-bootstrap.
    mv "$SRC.hidden" "$SRC"
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "sweep: a row for a DELETED doc goes, but only on a full run" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    OTHER="$REPO/docs/devel/zz_other.md"
    echo 'Also at `src/zz_anchor.cpp:10`.' > "$OTHER"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    grep -q '^docs/devel/zz_other.md' scripts/doc_cite_anchors.tsv

    rm "$OTHER"
    # A TARGETED run has no business judging docs it was not pointed at.
    python3 "$ANCH" docs/devel/zz_doc.md
    grep -q '^docs/devel/zz_other.md' scripts/doc_cite_anchors.tsv

    # A full run sweeps it: out-of-scope rows are preserved, so a deleted doc is
    # permanently out of scope and nothing else could ever remove it.
    python3 "$ANCH"
    ! grep -q '^docs/devel/zz_other.md' scripts/doc_cite_anchors.tsv
}

@test "blank line: a citation pointing at nothing is a finding, not an anchor" {
    { echo "int a;"; echo ""; echo "int b;"; } > "$SRC"
    echo 'See `src/zz_anchor.cpp:2`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"blank line"* ]]
    [[ "$output" == *"src/zz_anchor.cpp:2"* ]]
    refute_grep 'zz_anchor.cpp' scripts/doc_cite_anchors.tsv

    # Baselining it is the documented escape hatch, and it stays visible there.
    run python3 "$ANCH" --write-baseline docs/devel
    [ "$status" -eq 0 ]
    grep -q 'src/zz_anchor.cpp:2' scripts/doc_cite_anchor_baseline.txt
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "unanchored: a newly added citation fails until regen" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    echo 'And another at `src/zz_anchor.cpp:3`.' >> "$DOC"
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"no anchor"* ]]
    python3 "$ANCH" docs/devel
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "orphan: a sidecar row whose citation was deleted is reported" {
    seed_src
    printf 'A `src/zz_anchor.cpp:10`.\nB `src/zz_anchor.cpp:3`.\n' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    printf 'A `src/zz_anchor.cpp:10`.\n' > "$DOC"
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"no longer cited"* ]]
    python3 "$ANCH" docs/devel
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
    refute_grep 'zz_anchor.cpp\t3\t' scripts/doc_cite_anchors.tsv
}

@test "idempotent: a second regen changes nothing" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    cp "$DOC" "$BATS_TEST_TMPDIR/doc.once"
    cp scripts/doc_cite_anchors.tsv "$BATS_TEST_TMPDIR/tsv.once"
    python3 "$ANCH" docs/devel
    diff "$BATS_TEST_TMPDIR/doc.once" "$DOC"
    diff "$BATS_TEST_TMPDIR/tsv.once" scripts/doc_cite_anchors.tsv
}

@test "ordering: sidecar rows are sorted, so review diffs stay small" {
    seed_src
    cp "$SRC" "$REPO/src/zz_anchor_b.cpp"
    printf 'Z `src/zz_anchor_b.cpp:10`.\nA `src/zz_anchor.cpp:10`.\nM `src/zz_anchor.cpp:3`.\n' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    run bash -c "grep -v '^#' scripts/doc_cite_anchors.tsv | cut -f2,3"
    [ "${lines[0]}" = $'src/zz_anchor.cpp\t3' ]
    [ "${lines[1]}" = $'src/zz_anchor.cpp\t10' ]
    [ "${lines[2]}" = $'src/zz_anchor_b.cpp\t10' ]
}

@test "linked citations: the text is repaired inside a generated markdown link" {
    seed_src
    printf 'See [`src/zz_anchor.cpp:10`](../../src/zz_anchor.cpp#L10).\n' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    { for i in $(seq 5); do echo "// prologue $i"; done; cat "$SRC"; } > "$SRC.new"
    mv "$SRC.new" "$SRC"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    grep -q '\[`src/zz_anchor.cpp:15`\]' "$DOC"
}

@test "fenced blocks: a citation inside a code sample is left alone" {
    seed_src
    cat > "$DOC" <<'EOF'
Prose cites `src/zz_anchor.cpp:10`.

```bash
grep -n foo `src/zz_anchor.cpp:10`
```
EOF
    cd "$REPO"
    python3 "$ANCH" docs/devel
    { for i in $(seq 5); do echo "// prologue $i"; done; cat "$SRC"; } > "$SRC.new"
    mv "$SRC.new" "$SRC"
    python3 "$ANCH" docs/devel
    grep -q 'Prose cites `src/zz_anchor.cpp:15`' "$DOC"
    grep -q 'grep -n foo `src/zz_anchor.cpp:10`' "$DOC"
}

@test "exempt: an EXEMPT_SUBSTRINGS target is never anchored" {
    mkdir -p "$REPO/build/generated"
    seed_src
    cp "$SRC" "$REPO/build/generated/zz_gen.h"
    echo 'See `build/generated/zz_gen.h:10` and `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    refute_grep 'zz_gen.h' scripts/doc_cite_anchors.tsv
    grep -q 'zz_anchor.cpp' scripts/doc_cite_anchors.tsv
}

@test "gate: check_doc_refs.py reports anchor drift" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 0 ]
    [[ "$output" == *"Citation anchors"* ]]

    sed -i 's/void zz_marker_alpha.*/void zz_renamed_entirely(void);/' "$SRC"
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"cited line is gone"* ]]
}

@test "gate: with no sidecar present the anchor check stays inert" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    [ ! -f scripts/doc_cite_anchors.tsv ]
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 0 ]
}

@test "scope: a doc outside the scanned corpus is never measured against the sidecar" {
    # How the sibling gates' meta-tests drive check_doc_refs.py: an explicit
    # --devel path into a scratch fixture, run from the repo root, where the
    # REAL sidecar is on disk. The fixture's citations have no anchors and
    # never will, so consulting the sidecar here would fail the tree over a
    # file that is not part of the corpus.
    mkdir -p "$BATS_TEST_TMPDIR/scratch/src" "$BATS_TEST_TMPDIR/scratch/devel"
    for i in $(seq 50); do echo "// line $i"; done \
        > "$BATS_TEST_TMPDIR/scratch/src/zz_outside.cpp"
    echo 'Cites `src/zz_outside.cpp:42`.' > "$BATS_TEST_TMPDIR/scratch/devel/x.md"
    cd "$BATS_TEST_DIRNAME/../.."
    [ -f scripts/doc_cite_anchors.tsv ]     # the real one, deliberately
    run python3 "$CHECK" --devel "$BATS_TEST_TMPDIR/scratch/devel/x.md"
    [ "$status" -eq 0 ]
    [[ "$output" != *"no anchor"* ]]
}

@test "range: a block citation moves whole and keeps its authored span" {
    seed_src
    echo 'The block at `src/zz_anchor.cpp:10-13` does it.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    grep -q $'docs/devel/zz_doc.md\tsrc/zz_anchor.cpp\t10\t' scripts/doc_cite_anchors.tsv
    { for i in $(seq 5); do echo "// prologue $i"; done; cat "$SRC"; } > "$SRC.new"
    mv "$SRC.new" "$SRC"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    # start 10 -> 15, and the 3-line span is preserved (13 -> 18), never 15-13.
    grep -q '`src/zz_anchor.cpp:15-18`' "$DOC"
    refute_grep 'zz_anchor.cpp:15-13' "$DOC"
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

@test "range: the END of a range is past-EOF checked, not just the start" {
    seed_src   # 21 lines
    echo 'See `src/zz_anchor.cpp:10-900`.' > "$DOC"
    cd "$REPO"
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"past the end of the file"* ]]
    [[ "$output" == *"zz_anchor.cpp:10-900"* ]]
}

@test "range: an end written OUTSIDE the backticks moves with the block too" {
    seed_src
    # The architecture guide's dominant spelling: the range end sits after the
    # citation (and after the generated link wrapper), so it is a separate token
    # the rewriter has to find by looking ahead. Missing it silently SHRINKS the
    # range on every move — start re-pinned, end left behind.
    printf 'Block [`src/zz_anchor.cpp:10`](../../src/zz_anchor.cpp#L10)-13 does it.\n' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    { for i in $(seq 5); do echo "// prologue $i"; done; cat "$SRC"; } > "$SRC.new"
    mv "$SRC.new" "$SRC"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    grep -q '\[`src/zz_anchor.cpp:15`\](../../src/zz_anchor.cpp#L10)-18' "$DOC"
    refute_grep 'zz_anchor.cpp:15`\](../../src/zz_anchor.cpp#L10)-13' "$DOC"
}

@test "range: an end outside the backticks is past-EOF checked" {
    seed_src   # 21 lines
    printf 'See `src/zz_anchor.cpp:10`-900 for the block.\n' > "$DOC"
    cd "$REPO"
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"past the end of the file"* ]]
}

@test "unresolved ceiling: a new dead-path citation fails the ratchet" {
    # A citation whose PATH does not resolve is skipped by every check here and
    # deferred to check_refs — which passes a bare basename as soon as ANY file
    # in the tree shares it. So this class can be both unanchorable and
    # unreported, and only a count can hold it.
    seed_src
    echo 'Real: `src/zz_anchor.cpp:10`. Dead: `src/zz_gone_a.cpp:3`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" >/dev/null
    # Only a FULL run may re-derive the ceiling: a targeted run counted a subset
    # and would ratchet the number down to it, failing the next full run over
    # citations it never looked at.
    python3 "$ANCH" --write-baseline >/dev/null
    grep -q '^max-unresolved: 1$' scripts/doc_cite_anchor_baseline.txt

    run python3 "$ANCH" --check
    [ "$status" -eq 0 ]

    # One more dead path and the bucket has grown.
    echo 'Real: `src/zz_anchor.cpp:10`. Dead: `src/zz_gone_a.cpp:3` `src/zz_gone_b.cpp:4`.' > "$DOC"
    run python3 "$ANCH" --check
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not resolve, over the baseline of 1"* ]]
}

@test "unresolved ceiling: absent from the baseline, the count is not enforced" {
    seed_src
    echo 'Real: `src/zz_anchor.cpp:10`. Dead: `src/zz_gone_a.cpp:3`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" >/dev/null
    # No baseline file at all: the ratchet has not been adopted here, and the
    # miniature-repo meta-tests of the sibling gates rely on that staying inert.
    [ ! -f scripts/doc_cite_anchor_baseline.txt ]
    run python3 "$ANCH" --check
    [ "$status" -eq 0 ]
}

@test "write-baseline: rewriting is idempotent, not self-erasing" {
    # --write-baseline used to re-derive the file from what was left AFTER the
    # existing entries were skipped — which is nothing, so one invocation wiped
    # the list and a second put it back.
    {
        for i in $(seq 9); do echo "// filler $i"; done
        echo "}"
        echo "void zz_marker_alpha(void) { return; }"
    } > "$SRC"
    echo 'The brace at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" --write-baseline docs/devel
    grep -q 'src/zz_anchor.cpp:10' scripts/doc_cite_anchor_baseline.txt
    cp scripts/doc_cite_anchor_baseline.txt "$BATS_TEST_TMPDIR/first"

    run python3 "$ANCH" --write-baseline docs/devel
    grep -q 'src/zz_anchor.cpp:10' scripts/doc_cite_anchor_baseline.txt
    diff "$BATS_TEST_TMPDIR/first" scripts/doc_cite_anchor_baseline.txt
}

@test "bare refs: the gate NEVER anchors a `:N` shorthand" {
    # The constraint the whole feature hangs on. A census put this class at 459
    # citations, 35% of every line reference in the docs, and eleven of twelve
    # hand-checked were wrong. Making them visible and letting the bootstrapper
    # anchor them where they sit would freeze ~400 bad citations and make them
    # look maintained.
    seed_src
    echo 'The marker at `src/zz_anchor.cpp:10`, and also at `:12`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    # Exactly one row: the full citation. The shorthand gets none.
    [ "$(grep -c $'\tsrc/zz_anchor.cpp\t' scripts/doc_cite_anchors.tsv)" -eq 1 ]
    ! grep -q $'\tsrc/zz_anchor.cpp\t12\t' scripts/doc_cite_anchors.tsv
}

@test "bare refs: --bare-refs resolves against the preceding same-line citation" {
    seed_src
    echo 'The marker at `src/zz_anchor.cpp:10`, and also at `:12`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" --bare-refs docs/devel
    [ "$status" -eq 0 ]
    [[ "$output" == *"1 resolved against the preceding citation"* ]]
}

@test "bare refs: a shorthand with no antecedent ON ITS LINE is left alone" {
    # SYMBOL_CITE_B_RE already learned what a newline-spanning pattern does: it
    # paired a citation ending one bullet with the symbol opening the next, four
    # times over. A shorthand whose antecedent is in another paragraph is not
    # resolvable by rule, and guessing is how this bug class started.
    seed_src
    printf 'Cited at `src/zz_anchor.cpp:10`.\n\nA later line mentions `:12`.\n' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" --bare-refs docs/devel
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 resolved against the preceding citation"* ]]
}

@test "bare refs: two candidate files that both fit means NO attribution" {
    # Refusing beats guessing. Containment may only decide when it decides
    # outright — two files that could both hold the line is exactly the case
    # that produced wrong citations in the first place.
    for i in $(seq 400); do echo "// a $i"; done > "$REPO/src/zz_a.cpp"
    for i in $(seq 400); do echo "// b $i"; done > "$REPO/src/zz_b.cpp"
    echo 'See `src/zz_a.cpp:5` and `src/zz_b.cpp:6`, plus `:300`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" --bare-refs docs/devel
    [ "$status" -eq 0 ]
    [[ "$output" == *"two or more candidate files   : 1"* ]]
    [[ "$output" == *"attribute to exactly one file : 0"* ]]
}

@test "bare refs: the antecedent must be a file that CONTAINS the line" {
    # A sentence citing two files leaves the NEAREST antecedent as the wrong one
    # whenever the shorthand belongs to the earlier, longer file. Nearest-that-
    # fits keeps the pairing honest; falling back to truly-nearest then surfaces
    # as past-EOF, which is a real finding rather than a mispairing.
    for i in $(seq 400); do echo "// long $i"; done > "$REPO/src/zz_long.cpp"
    echo "short" > "$REPO/src/zz_short.h"
    echo 'See `src/zz_long.cpp:5` and `src/zz_short.h:1`, plus `:300`.' > "$DOC"
    cd "$REPO"
    run python3 "$ANCH" --bare-refs docs/devel
    [ "$status" -eq 0 ]
    # 300 is past the 1-line header, so it must pair with zz_long.cpp and be
    # reported as anchorable rather than as past-EOF.
    [[ "$output" != *"past-EOF"* ]]
}

# ---------------------------------------------------------------------------
# Gate integrity: the three ways the anchor check could be switched off without
# anyone noticing. Each of these was a live hole — the ceiling was enforced only
# on a code path nothing ran, a missing sidecar printed a warning and passed,
# and every write truncated its target before emitting a byte.
# ---------------------------------------------------------------------------

# Two files sharing one basename, in directories outside gen_doc_links'
# PRIMARY_ROOTS so the tie cannot be broken. This is the real shape of an
# "unresolved path": check_doc_refs PASSES it (the basename exists somewhere)
# while the anchor resolver refuses it (it cannot tell which file is meant), so
# the citation is anchored by nothing and reported by nothing. Only a count
# holds the class, which is why the ceiling exists.
seed_ambiguous() {
    local name="$1"
    mkdir -p "$REPO/zz_${name}_a" "$REPO/zz_${name}_b"
    printf 'int one;\nint two;\nint three;\n' > "$REPO/zz_${name}_a/$name.cpp"
    cp "$REPO/zz_${name}_a/$name.cpp" "$REPO/zz_${name}_b/$name.cpp"
}

@test "ceiling: check_doc_refs.py enforces the unresolved-path ratchet too" {
    # The ceiling used to live ONLY in doc_cite_anchors.py's own --check, whose
    # sole caller was `make check-doc-anchors` — a target nothing invoked. On
    # the path quality-checks.sh, both git hooks and CI actually take, a new
    # dead-path citation produced no finding at all: the walk just bumps a
    # counter and continues, and check_doc_refs read only ['in_place'].
    seed_src
    seed_ambiguous zz_dup
    echo 'Real `src/zz_anchor.cpp:10`. Ambiguous `zz_dup.cpp:3`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" >/dev/null
    python3 "$ANCH" --write-baseline >/dev/null
    grep -q '^max-unresolved: 1$' scripts/doc_cite_anchor_baseline.txt

    # The quiet half: AT the ceiling the whole gate is still green, so the fix
    # cannot degenerate into "always fail".
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 0 ]
    [[ "$output" == *"Citation anchors: 1 cited lines still resolve"* ]]

    # One more unresolvable path and the bucket has grown.
    seed_ambiguous zz_dup2
    echo 'Real `src/zz_anchor.cpp:10`. Ambiguous `zz_dup.cpp:3` `zz_dup2.cpp:2`.' > "$DOC"
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"does not resolve, over the baseline of 1"* ]]
    # And it is the ceiling talking, not some other check picking it up: the
    # path checks all pass, which is exactly why only a count can hold this.
    [[ "$output" == *"✅ Doc references: all resolve"* ]]
}

@test "ceiling: with no baseline file the ratchet stays inert on the gate path" {
    # The sibling gates' meta-tests build miniature repos with no baseline, and
    # the ceiling must not fail them into red over citations nobody baselined.
    seed_src
    seed_ambiguous zz_dup
    echo 'Real `src/zz_anchor.cpp:10`. Ambiguous `zz_dup.cpp:3`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" >/dev/null
    [ ! -f scripts/doc_cite_anchor_baseline.txt ]
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 0 ]
}

@test "sidecar: deleting a COMMITTED sidecar fails both gate paths closed" {
    # `rm scripts/doc_cite_anchors.tsv` turned the anchor check green for the
    # whole corpus — load_sidecar() returned None, which both entry points read
    # as "this tree has not opted in" and passed with a ⚠️ nobody reads as a
    # failure. git is what separates the two meanings.
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    git init -q .
    git add scripts/doc_cite_anchors.tsv
    rm scripts/doc_cite_anchors.tsv

    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"committed but absent from the working tree"* ]]

    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"committed but absent from the working tree"* ]]

    # Restoring it puts the gate straight back to green — the failure is about
    # the file being gone, not about the tree having git in it.
    git checkout -- scripts/doc_cite_anchors.tsv
    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 0 ]
}

@test "sidecar: a git-rm removal does not walk the gate off either" {
    # Consulting the index alone would let a commit that removes the sidecar
    # pass on its way out, so HEAD is consulted too.
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" docs/devel
    git init -q .
    git add scripts/doc_cite_anchors.tsv
    git -c user.name=t -c user.email=t@t commit -qm seed
    git rm -q scripts/doc_cite_anchors.tsv

    run python3 "$CHECK" --devel docs/devel
    [ "$status" -eq 1 ]
    [[ "$output" == *"committed but absent from the working tree"* ]]
}

@test "sidecar: bootstrap still works in a tree that has none" {
    # The escape hatch the fail-closed check must not take away: the WRITE path
    # never consults git, so a tree can always create its first sidecar.
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    git init -q .
    [ ! -f scripts/doc_cite_anchors.tsv ]
    run python3 "$ANCH" docs/devel
    [ "$status" -eq 0 ]
    [ -f scripts/doc_cite_anchors.tsv ]
    run python3 "$ANCH" --check docs/devel
    [ "$status" -eq 0 ]
}

# A driver that runs ONE doc_cite_anchors write with the process dying inside
# it. Both file-opening primitives are wrapped, so the model holds whichever one
# the implementation reaches for, and the first write() raises — which is what a
# Ctrl-C in .githooks/pre-commit looks like from the writer's point of view.
#
# A truncating write has already emptied its target by then (open(path,'w')
# truncates at open, before any write call), so the assertion "the target still
# holds its old bytes" is exactly the mutation signal: put `with open(path,'w')`
# back in atomic_write() and all three cases fail.
write_crasher() {
    cat > "$BATS_TEST_TMPDIR/crashwrite.py" <<'PYEOF'
import builtins, os, sys
sys.path.insert(0, sys.argv[1])
import doc_cite_anchors as anchors


class _Dying:
    def __init__(self, f):
        self._f = f

    def write(self, s):
        raise KeyboardInterrupt

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return self._f.__exit__(*a)

    def __getattr__(self, k):
        return getattr(self._f, k)


_open, _fdopen = builtins.open, os.fdopen
builtins.open = lambda p, mode='r', *a, **kw: (
    _Dying(_open(p, mode, *a, **kw)) if 'w' in mode else _open(p, mode, *a, **kw))
os.fdopen = lambda fd, mode='r', *a, **kw: (
    _Dying(_fdopen(fd, mode, *a, **kw)) if 'w' in mode else _fdopen(fd, mode, *a, **kw))

try:
    what = sys.argv[2]
    if what == 'sidecar':
        anchors.write_sidecar({('d.md', 's.cpp', 1): ('s.cpp', 'a' * 12, 'b' * 12)})
    elif what == 'baseline':
        anchors.write_baseline({('d.md', 's.cpp', 1, 'blank')}, unresolved=99)
    else:
        anchors.run(anchors.scoped_targets(['docs/devel']),
                    anchors.load_sidecar(), write=True)
except KeyboardInterrupt:
    sys.exit(130)
sys.exit(0)
PYEOF
}

@test "atomic: an interrupted write leaves the sidecar and baseline intact" {
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" >/dev/null
    python3 "$ANCH" --write-baseline >/dev/null
    cp scripts/doc_cite_anchors.tsv "$BATS_TEST_TMPDIR/tsv.good"
    cp scripts/doc_cite_anchor_baseline.txt "$BATS_TEST_TMPDIR/bl.good"
    write_crasher

    run python3 "$BATS_TEST_TMPDIR/crashwrite.py" "$(dirname "$ANCH")" sidecar
    [ "$status" -eq 130 ]
    diff "$BATS_TEST_TMPDIR/tsv.good" scripts/doc_cite_anchors.tsv

    run python3 "$BATS_TEST_TMPDIR/crashwrite.py" "$(dirname "$ANCH")" baseline
    [ "$status" -eq 130 ]
    diff "$BATS_TEST_TMPDIR/bl.good" scripts/doc_cite_anchor_baseline.txt

    # No half-written scratch file left behind for the next reader to trip on.
    run bash -c "ls -a scripts | grep -c '\.tmp\$'"
    [ "$output" = "0" ]

    # And the gate still reads the file it protected.
    run python3 "$ANCH" --check
    [ "$status" -eq 0 ]
}

@test "atomic: an interrupted write leaves the DOC intact, never truncated" {
    # The doc write is the one a committer meets: --auto-fix re-pins in place
    # from inside .githooks/pre-commit, and half a rewritten doc in the working
    # tree reads as a legitimate edit rather than as damage.
    seed_src
    echo 'The marker lives at `src/zz_anchor.cpp:10`.' > "$DOC"
    cd "$REPO"
    python3 "$ANCH" >/dev/null
    cp "$DOC" "$BATS_TEST_TMPDIR/doc.good"
    # Move the code so a rewrite is genuinely pending — without this the write
    # path is never reached and the test proves nothing.
    { for i in $(seq 5); do echo "// prologue $i"; done; cat "$SRC"; } > "$SRC.new"
    mv "$SRC.new" "$SRC"
    write_crasher

    run python3 "$BATS_TEST_TMPDIR/crashwrite.py" "$(dirname "$ANCH")" doc
    [ "$status" -eq 130 ]
    diff "$BATS_TEST_TMPDIR/doc.good" "$DOC"
    run bash -c "ls -a docs/devel | grep -c '\.tmp\$'"
    [ "$output" = "0" ]

    # Uninterrupted, the very same run DOES rewrite it — so the assertion above
    # is about atomicity and not about the write being skipped.
    run python3 "$ANCH"
    [ "$status" -eq 0 ]
    grep -q '`src/zz_anchor.cpp:15`' "$DOC"
}
