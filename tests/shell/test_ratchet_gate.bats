#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/ratchet.py, the under-baseline verdict every count ratchet
# shares. A count under its baseline passes at every stage and asks for the baseline to
# come down; nothing in the quality sweep makes it fail.

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "a count at or over its baseline is not this helper's verdict" {
    run python3 scripts/ratchet.py Gate where.sh KEY 5 5 lenient
    [ "$status" -eq 0 ]
    [ -z "$output" ] || fail "printed at the baseline: $output"
    run python3 scripts/ratchet.py Gate where.sh KEY 5 6 lenient
    [ "$status" -eq 0 ]
}

@test "under the baseline passes and prints the message asking for a lower baseline" {
    run python3 scripts/ratchet.py "Hardcoded colors" \
        scripts/qc/design_tokens.sh HEX_BASELINE 33 27 "ratchet down please"
    [ "$status" -eq 0 ] || fail "under the baseline failed: $output"
    [ "$output" = "ratchet down please" ]
}

@test "a leftover HELIX_RATCHET_STRICT=1 in the environment no longer fails an improvement" {
    run env HELIX_RATCHET_STRICT=1 python3 scripts/ratchet.py Gate where.sh KEY 5 4 lenient
    [ "$status" -eq 0 ] || fail "strictness came back: $output"
    [ "$output" = "lenient" ]
}

@test "quality-checks.sh sets no ratchet strictness" {
    run grep -n 'RATCHET_STRICT' scripts/quality-checks.sh scripts/qc/_lib.sh
    [ "$status" -eq 1 ] || fail "strictness is wired again: $output"
}

@test "the hardcoded-colour baseline is the tree's count" {
    run bash -c 'HEX_ALLOW=$(sed -n "s/^HEX_ALLOW=\x27\(.*\)\x27$/\1/p" scripts/qc/design_tokens.sh)
        grep -rn "lv_color_hex(0x" src include | grep -vcE "$HEX_ALLOW"'
    count="$output"
    baseline=$(sed -n 's/^HEX_BASELINE=\([0-9]*\)$/\1/p' scripts/qc/design_tokens.sh)
    [ -n "$baseline" ] || fail "no HEX_BASELINE in scripts/qc/design_tokens.sh"
    [ "$count" -eq "$baseline" ] || fail "HEX_BASELINE $baseline, tree count $count"
}

@test "qc_ratchet_verdict passes a ratchet warning with its output and fails anything else" {
    out="$BATS_TEST_TMPDIR/out"
    printf 'first\n⚠️  grew\n' > "$out"
    run bash -c '. scripts/qc/_lib.sh; qc_ratchet_verdict 3 "$1"' _ "$out"
    [ "$status" -eq 0 ]
    contains "⚠️  grew" "$output"
    contains "blocks nothing" "$output"
    run bash -c '. scripts/qc/_lib.sh; qc_ratchet_verdict 0 "$1"' _ "$out"
    [ "$status" -eq 0 ]
    [ "$output" = "⚠️  grew" ]
    run bash -c '. scripts/qc/_lib.sh; qc_ratchet_verdict 1 "$1"' _ "$out"
    [ "$status" -eq 1 ]
    contains "first" "$output"
    run bash -c '. scripts/qc/_lib.sh; qc_ratchet_verdict 2 "$1"' _ "$out"
    [ "$status" -eq 1 ]
}

@test "ratchet.WARN_EXIT and the shell's QC_RATCHET_WARN_EXIT agree" {
    py=$(python3 -c 'import sys; sys.path.insert(0, "scripts"); import ratchet; print(ratchet.WARN_EXIT)')
    sh=$(bash -c '. scripts/qc/_lib.sh; echo "$QC_RATCHET_WARN_EXIT"')
    [ "$py" = "$sh" ]
}
