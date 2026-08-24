#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_translation_identity.py — the base-locale key
# identity gate.
#
# English loads NO translation pack at runtime (translation_loader.cpp skips
# kIdentityLocale): lv_translation_get() returns the tag on a miss, which only
# renders correct UI when the tag IS the English string. A semantic key
# (value != key) ships a raw dotted identifier to English users — v0.99.114
# showed "pre_print_option.timelapse.label" on the timelapse toggle row and
# raw tour.step.* strings across the whole first-run tour.
#
# These tests pin both halves of the contract: the gate must FAIL on a
# non-identity entry (the shipped bug), and must STAY SILENT on the shapes a
# healthy en.yml legitimately contains — non-en locales are not its business,
# and values that merely quote/fold differently than their key are fine as
# long as the parsed strings match.

GATE="scripts/check_translation_identity.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/trans_identity"
    mkdir -p "$FIXTURE_DIR"
}

write_yml() {
    printf '%s\n' "$1" > "$FIXTURE_DIR/en.yml"
    run python3 "$GATE" --file "$FIXTURE_DIR/en.yml"
}

# ---------------------------------------------------------------- must catch

@test "flags a semantic key whose value differs" {
    write_yml 'locale: en
translations:
  prints: prints
  pre_print_option.timelapse.label: Timelapse'
    [ "$status" -eq 1 ]
    [[ "$output" == *"pre_print_option.timelapse.label"* ]]
}

@test "flags an empty value under a semantic key" {
    # Empty placeholders (fresh translation-sync keys) are also non-identity;
    # in English there is no such thing as an untranslated English string.
    write_yml 'locale: en
translations:
  Timelapse: ""
  prints: prints'
    [ "$status" -eq 1 ]
    [[ "$output" == *"Timelapse"* ]]
}

@test "names every offending key, not just the first" {
    write_yml 'locale: en
translations:
  a.key.one: One
  a.key.two: Two
  prints: prints'
    [ "$status" -eq 1 ]
    [[ "$output" == *"a.key.one"* ]]
    [[ "$output" == *"a.key.two"* ]]
}

# ------------------------------------------------------------- must stay quiet

@test "accepts a pure-identity en.yml" {
    # Quoting mirrors the real translations/en.yml: bare apostrophe inside an
    # unquoted scalar; double quotes wrapped in single quotes.
    write_yml 'locale: en
translations:
  Cancel: Cancel
  Don'"'"'t Save: Don'"'"'t Save
  '"'"'"%s" failed to load'"'"': '"'"'"%s" failed to load'"'"''
    [ "$status" -eq 0 ]
    [[ "$output" == "ok:"* ]]
}

@test "accepts a key that folds across lines but parses to its value" {
    # YAML wraps long values; the gate compares parsed strings, not raw lines.
    write_yml 'locale: en
translations:
  Monitor for print abnormalities (K2 Plus camera-based): Monitor for print
    abnormalities (K2 Plus camera-based)'
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------ malformed inputs

@test "fails on a file that is not the en master" {
    printf '%s\n' 'locale: de
translations:
  Cancel: Abbrechen' > "$FIXTURE_DIR/en.yml"
    run python3 "$GATE" --file "$FIXTURE_DIR/en.yml"
    [ "$status" -eq 1 ]
    [[ "$output" == *"error:"* ]]
}

@test "fails on a missing file" {
    run python3 "$GATE" --file "$FIXTURE_DIR/nope.yml"
    [ "$status" -eq 1 ]
    [[ "$output" == *"error:"* ]]
}

# ------------------------------------------------------- the repo's own state

@test "the real translations/en.yml passes" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}
