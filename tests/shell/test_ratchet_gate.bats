#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/ratchet.py, the under-baseline verdict every count ratchet
# shares, and for qc_ratchet_verdict in scripts/qc/_lib.sh, which passes a checker's
# ratchet warnings. A count under its baseline passes at every stage and fails only when
# HELIX_RATCHET_STRICT=1 asks, naming the baseline to lower and the value to lower it to.
# Each ratchet's own bats file pins that the ratchet routes its under path here.

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

@test "quality-checks.sh leaves the under-baseline verdict lenient at every stage" {
    run bash -c 'HELIX_RATCHET_STRICT=; . scripts/qc/_lib.sh; echo "${HELIX_RATCHET_STRICT:-unset}"'
    [ "$output" = unset ]
    run grep -c 'HELIX_RATCHET_STRICT=' scripts/quality-checks.sh
    [ "$output" = 0 ] || fail "quality-checks.sh sets HELIX_RATCHET_STRICT again"
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
