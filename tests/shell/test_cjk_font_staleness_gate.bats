#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_cjk_font_staleness.sh — the CJK bake gate.
#
# The runtime CJK font (assets/fonts/cjk/*.bin) is baked from the codepoints
# the translations and C++ sources actually use. A translated string whose
# glyph never got baked renders as tofu in zh/ja: no build error, no runtime
# warning, just boxes on the screen. The gate diffs the needed set (extracted
# by scripts/translations/cjk_charset.py — the same extractor the bake uses)
# against the manifest the bake records.
#
# The fixtures deliberately contain no NotoSansCJK*.otf source fonts: those
# are gitignored downloads needed only to re-bake, and gating on their
# presence made every fresh clone and CI runner skip the check silently —
# a green gate that had checked nothing.

GATE="scripts/check_cjk_font_staleness.sh"

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/cjk_stale"
    mkdir -p "$FIXTURE/translations" "$FIXTURE/assets/fonts/cjk"
    cat > "$FIXTURE/translations/zh.yml" <<'YML'
bed_status:
  dirty: '热床被报告为脏污'
YML
}

write_manifest() {
    printf '%s\n' "$@" > "$FIXTURE/assets/fonts/cjk/.cjk_codepoints.manifest"
}

@test "green when the manifest covers every needed codepoint" {
    # 热 床 被 报 告 为 脏 污 (0x70ed 0x5e8a 0x88ab 0x62a5 0x544a 0x4e3a 0x810f 0x6c61)
    write_manifest 0x4e3a 0x544a 0x5e8a 0x62a5 0x6c61 0x70ed 0x810f 0x88ab
    run bash "$GATE" --root "$FIXTURE"
    [ "$status" -eq 0 ] || fail "expected exit 0, got $status: $output"
}

@test "red when a translation needs a codepoint the manifest lacks" {
    # Drop 污 (0x6c61): the dirty-bed string now renders tofu.
    write_manifest 0x4e3a 0x544a 0x5e8a 0x62a5 0x70ed 0x810f 0x88ab
    run bash "$GATE" --root "$FIXTURE"
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    echo "$output" | grep -q "0x6c61" || fail "did not name the missing codepoint: $output"
    echo "$output" | grep -q "污" || fail "did not show the missing character: $output"
    echo "$output" | grep -q "regen-text-fonts" || fail "did not say how to fix it: $output"
}

@test "red when the manifest is missing entirely" {
    run bash "$GATE" --root "$FIXTURE"
    [ "$status" -eq 1 ] || fail "missing manifest must fail, got $status: $output"
    echo "$output" | grep -q "manifest not found" || fail "did not explain: $output"
}

@test "red when a scan side is empty rather than passing vacuously" {
    # A wiped manifest parses to an empty compiled set — that is a broken
    # state, not "nothing needed".
    write_manifest ""
    run bash "$GATE" --root "$FIXTURE"
    [ "$status" -eq 1 ] || fail "empty manifest must fail, got $status: $output"
    echo "$output" | grep -q "vacuously" || fail "did not name the vacuous pass: $output"
}

@test "red when the extraction itself finds nothing (broken scan)" {
    # No translations directory contents at all: cjk_charset.py refuses to
    # emit an empty needed-set, so a drifted matcher cannot read as a pass.
    rm -rf "$FIXTURE/translations"
    write_manifest 0x4e3a
    run bash "$GATE" --root "$FIXTURE"
    [ "$status" -eq 1 ] || fail "empty needed-set must fail, got $status: $output"
}
