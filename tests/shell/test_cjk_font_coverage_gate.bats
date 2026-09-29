#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_cjk_font_coverage.py — the CJK artifact gate.
#
# The staleness gate compares translations against the MANIFEST, which is what
# the bake intended. This gate parses the .bin files themselves — the cmap
# tables lv_binfont_loader.c walks at runtime — so a stale, truncated or
# half-written bake cannot hide behind a freshly written manifest. Both gates
# green together mean: every CJK codepoint any locale needs resolves to a
# glyph in every shipped runtime font.
#
# The green fixture is the repo's real noto_sans_cjk_14.bin plus the real
# manifest, not a hand-built .bin: a synthetic fixture could not prove the
# parser against the format lv_font_conv actually emits (four cmap subtable
# types whose numeric codes come from the writer, not from LVGL's enum order).

GATE="scripts/check_cjk_font_coverage.py"

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/cjk_cov"
    mkdir -p "$FIXTURE/assets/fonts/cjk"
    cp assets/fonts/cjk/noto_sans_cjk_14.bin "$FIXTURE/assets/fonts/cjk/"
    cp assets/fonts/cjk/.cjk_codepoints.manifest "$FIXTURE/assets/fonts/cjk/"
}

run_gate() {
    run python3 "$GATE" --root "$FIXTURE" "$@"
}

@test "green when every bin covers the manifest, with a visible corpus count" {
    run_gate --summary
    [ "$status" -eq 0 ] || fail "expected exit 0, got $status: $output"
    echo "$output" | grep -Eq "1 CJK .bin fonts cover all [1-9][0-9]+ manifest codepoints" \
        || fail "no non-zero corpus count in output: $output"
}

@test "red when the manifest names a codepoint the bin does not carry" {
    printf '0x9fa5\n' >> "$FIXTURE/assets/fonts/cjk/.cjk_codepoints.manifest"
    run_gate
    [ "$status" -eq 1 ] || fail "uncovered manifest entry must fail, got $status: $output"
    echo "$output" | grep -q "0x9fa5" || fail "did not name the uncovered codepoint: $output"
    echo "$output" | grep -q "noto_sans_cjk_14.bin" || fail "did not name the file: $output"
}

@test "red when the manifest is missing or empty" {
    rm "$FIXTURE/assets/fonts/cjk/.cjk_codepoints.manifest"
    run_gate
    [ "$status" -eq 1 ] || fail "missing manifest must fail, got $status: $output"

    : > "$FIXTURE/assets/fonts/cjk/.cjk_codepoints.manifest"
    run_gate
    [ "$status" -eq 1 ] || fail "empty manifest must fail, got $status: $output"
    echo "$output" | grep -q "vacuously" || fail "did not name the vacuous pass: $output"
}

@test "red when there are no bins to check" {
    rm "$FIXTURE/assets/fonts/cjk/noto_sans_cjk_14.bin"
    run_gate
    [ "$status" -eq 1 ] || fail "no bins must fail, got $status: $output"
    echo "$output" | grep -q "no .bin fonts" || fail "did not explain: $output"
}

@test "red when a bin is not parseable as an LVGL font" {
    # Truncate mid-section: the label walker must refuse, not guess.
    head -c 200 assets/fonts/cjk/noto_sans_cjk_14.bin > "$FIXTURE/assets/fonts/cjk/broken.bin"
    run_gate
    [ "$status" -eq 1 ] || fail "truncated bin must fail, got $status: $output"
    echo "$output" | grep -q "cannot parse" || fail "did not name the parse failure: $output"
}
