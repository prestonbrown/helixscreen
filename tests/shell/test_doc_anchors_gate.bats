#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the qc_doc_anchors gate: a doc citing a scope that does not
# resolve is reported (and fails in strict mode); a doc whose citations all
# resolve passes; the gate is wired into the quality checks.

load helpers

setup() {
    ROOT="$BATS_TEST_DIRNAME/../../"
    DOCS="$BATS_TEST_TMPDIR/docs"
    mkdir -p "$DOCS"
    printf 'See `src/zz_missing_fixture.cpp#nothing`.\n' > "$DOCS/bad.md"
    printf 'See `scripts/doc_anchors.py#main`.\n' > "$DOCS/good.md"
}

run_gate() {
    run bash -c 'cd "$1" && QC_TMP="$2" && . scripts/qc/_lib.sh && . scripts/qc/doc_anchors.sh && qc_doc_anchors' _ "$ROOT" "$BATS_TEST_TMPDIR"
}

@test "strict: an unresolvable citation fails and names the doc" {
    export QC_DOC_ANCHORS_STRICT=1 QC_DOC_ANCHORS_PATHS="$DOCS/bad.md"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_missing_fixture.cpp"* ]]
}

@test "report-only: the same finding is printed and the gate passes" {
    export QC_DOC_ANCHORS_STRICT=0 QC_DOC_ANCHORS_PATHS="$DOCS/bad.md"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"zz_missing_fixture.cpp"* ]] || fail "finding not printed"
    [[ "$output" == *"Report only"* ]]
}

@test "a doc whose citations resolve passes even in strict mode" {
    export QC_DOC_ANCHORS_STRICT=1 QC_DOC_ANCHORS_PATHS="$DOCS/good.md"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"resolves"* ]]
}

@test "qc_doc_anchors is wired into the quality checks and triggers on docs and code" {
    grep -q ' qc_doc_anchors ' "$ROOT/scripts/quality-checks.sh"
    run qc_trigger doc_anchors
    [ "$status" -eq 0 ]
    [[ "docs/devel/THREADING.md" =~ $output ]] || fail "docs not in the trigger"
    [[ "src/printer/printer_state.cpp" =~ $output ]]
}
