#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for check_doc_images.py and its qc_doc_images wiring: a user-facing
# page showing an image the manifest lacks fails; a covered tree passes.

load helpers

setup() {
    CHECK="$BATS_TEST_DIRNAME/../../scripts/check_doc_images.py"
    ROOT="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$ROOT/docs/user" "$ROOT/docs/images/user"
    printf 'PNG' > "$ROOT/docs/images/user/a.png"
}

@test "an image a user page shows with no manifest entry fails, naming the page" {
    echo '![a](../images/user/a.png)' > "$ROOT/docs/user/page.md"
    printf 'images:\n' > "$ROOT/docs/images/doc-screenshots.yml"
    run python3 "$CHECK" --repo-root "$ROOT"
    [ "$status" -eq 1 ]
    [[ "$output" == *"unrecipe: docs/images/user/a.png (referenced from docs/user/page.md)"* ]]
}

@test "a tree whose every image has an entry passes" {
    echo '![a](../images/user/a.png)' > "$ROOT/docs/user/page.md"
    printf 'images:\n  - path: docs/images/user/a.png\n    kind: manual\n    source: s\n    recapture: r\n' \
        > "$ROOT/docs/images/doc-screenshots.yml"
    run python3 "$CHECK" --repo-root "$ROOT"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "qc_doc_images is wired into the quality checks and triggers on docs" {
    grep -q ' qc_doc_images ' "$BATS_TEST_DIRNAME/../../scripts/quality-checks.sh"
    run qc_trigger doc_images
    [ "$status" -eq 0 ]
    [[ "docs/user/page.md" =~ $output ]] || fail "the trigger misses a user page"
    [[ "docs/images/user/a.png" =~ $output ]]
}
