#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/ratchet.py, the under-baseline verdict every count ratchet
# shares, and for the stage decision in scripts/qc/_lib.sh that makes it strict. A count
# under its baseline passes a commit and fails the stages a push to main runs, naming the
# baseline to lower and the value to lower it to. Each ratchet's own bats file pins that
# the ratchet routes its under path here.

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "a count at or over its baseline is not this helper's verdict, in either mode" {
    run env HELIX_RATCHET_STRICT=1 python3 scripts/ratchet.py Gate where.sh KEY 5 5 lenient
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "printed at the baseline: $output"
    run env HELIX_RATCHET_STRICT=1 python3 scripts/ratchet.py Gate where.sh KEY 5 6 lenient
    [ "$status" -eq 0 ]
}

@test "under the baseline prints the commit message when lenient and fails when strict" {
    run env HELIX_RATCHET_STRICT=0 python3 scripts/ratchet.py "Hardcoded colors" \
        scripts/qc/design_tokens.sh HEX_BASELINE 33 27 "ratchet down please"
    [ "$status" -eq 0 ]
    [ "$output" = "ratchet down please" ]

    run env HELIX_RATCHET_STRICT=1 python3 scripts/ratchet.py "Hardcoded colors" \
        scripts/qc/design_tokens.sh HEX_BASELINE 33 27 "ratchet down please"
    [ "$status" -eq 1 ] || fail "under the baseline passed strict: $output"
    contains "❌ Hardcoded colors: under its baseline" "$output"
    contains "scripts/qc/design_tokens.sh" "$output"
    contains "HEX_BASELINE: 33 -> 27" "$output"
    lacks "ratchet down please" "$output"
}

@test "without the variable the helper is lenient, so a checker run by hand passes" {
    run env -u HELIX_RATCHET_STRICT python3 scripts/ratchet.py Gate where.sh KEY 5 4 lenient
    [ "$status" -eq 0 ]
    [ "$output" = "lenient" ]
}

@test "strict at a full sweep only: a commit and a work-branch push stay lenient" {
    run bash -c '. scripts/qc/_lib.sh; qc_ratchet_strictness true 0'
    [ "$output" = 0 ] || fail "a commit was strict"
    run bash -c '. scripts/qc/_lib.sh; qc_ratchet_strictness false 1'
    [ "$output" = 0 ] || fail "a work-branch push was strict"
    run bash -c '. scripts/qc/_lib.sh; qc_ratchet_strictness false 0'
    [ "$output" = 1 ] || fail "a full sweep (main push, land gate, CI) was lenient"
}

@test "quality-checks.sh exports the strictness it derives from its own mode" {
    run grep -F 'HELIX_RATCHET_STRICT="$(qc_ratchet_strictness "$STAGED_ONLY" "${HELIX_QC_SKIP_PUSH_ONLY:-0}")"' \
        scripts/quality-checks.sh
    [ "$status" -eq 0 ] || fail "quality-checks.sh no longer derives HELIX_RATCHET_STRICT"
    run grep -Fx 'export HELIX_RATCHET_STRICT' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "the hardcoded-colour baseline is the tree's count" {
    run bash -c 'HEX_ALLOW=$(sed -n "s/^HEX_ALLOW=\x27\(.*\)\x27$/\1/p" scripts/qc/design_tokens.sh)
        grep -rn "lv_color_hex(0x" src include | grep -vcE "$HEX_ALLOW"'
    count="$output"
    baseline=$(sed -n 's/^HEX_BASELINE=\([0-9]*\)$/\1/p' scripts/qc/design_tokens.sh)
    [ -n "$baseline" ] || fail "no HEX_BASELINE in scripts/qc/design_tokens.sh"
    [ "$count" -eq "$baseline" ] || fail "HEX_BASELINE $baseline, tree count $count"
}
