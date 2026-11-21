#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# An in-app update forks the installer from helix-screen, which raises its own
# oom_score_adj so the kernel kills it before Klipper. The installer drops a
# positive inherited score to 0 before starting services, so dropbear and the
# Creality backend are not early OOM victims (#2121).

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    unset _HELIX_COMMON_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    OOM="$BATS_TEST_TMPDIR/oom_score_adj"
}

@test "an inherited positive score is reset to 0" {
    echo 300 > "$OOM"
    run reset_inherited_oom_score_adj "$OOM"
    [ "$status" -eq 0 ]
    [ "$(cat "$OOM")" = 0 ]
    [[ "$output" == *"Reset inherited oom_score_adj 300 to 0"* ]]
}

@test "a score of 0 is left alone and logs nothing" {
    echo 0 > "$OOM"
    run reset_inherited_oom_score_adj "$OOM"
    [ "$status" -eq 0 ]
    [ "$(cat "$OOM")" = 0 ]
    [ -z "$output" ]
}

@test "a negative score is never raised" {
    echo -500 > "$OOM"
    run reset_inherited_oom_score_adj "$OOM"
    [ "$status" -eq 0 ]
    [ "$(cat "$OOM")" = -500 ]
}

@test "a missing file is not an error" {
    run reset_inherited_oom_score_adj "$BATS_TEST_TMPDIR/absent"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "main resets the score before anything else runs" {
    first=$(sed -n '/^main() {/,/^}/p' "$WORKTREE_ROOT/scripts/lib/installer/main.sh" \
        | grep -v '^\s*#' | sed -n '2p')
    [[ "$first" == *reset_inherited_oom_score_adj* ]]
}
