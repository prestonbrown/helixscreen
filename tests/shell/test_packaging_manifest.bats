#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regression tests for scripts/gen-packaging-manifest.sh — the single source
# of truth for which source-tree directories ship in HelixScreen packages.
#
# The Android extractor, cross.mk RELEASE_ASSETS, and (eventually) package.sh
# all consume this manifest. Prior incidents (e0840a4b6, 87452586f, the
# Android extractor drift that motivated this file) all had the same shape:
# a hand-maintained list of directories falls out of sync with the source
# tree and a release silently ships without its RO seed bundle.
#
# These tests catch that class of bug at CI time.

# Overridable so the fixture case can be proven to FIRE against a hand-broken
# copy of the script (same convention as HELIX_PLUGIN_INSTALL_SH in
# test_moonraker_plugin_install.bats).
SCRIPT="${HELIX_TEST_MANIFEST_SH:-scripts/gen-packaging-manifest.sh}"

@test "manifest runs cleanly and produces non-empty output" {
    run "$SCRIPT"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
}

@test "manifest enumerates a known fixture tree exactly" {
    # Run the script against a tree whose correct answer is written out by hand.
    #
    # This case used to re-type the script's own find/prune pipeline as the
    # "expected" value, so a wrong prune idiom produced the same wrong answer on
    # both sides and the test stayed green. The fixture pins the behaviour
    # instead: three roots, a nested subdir, and hidden dirs at two depths —
    # `-name '.*' -prune` must drop a hidden dir AND everything beneath it.
    local root="$BATS_TEST_TMPDIR/fixture"
    mkdir -p "$root"/ui_xml/components \
             "$root"/ui_xml/translations \
             "$root"/ui_xml/.hidden/nested \
             "$root"/assets/images/printers/prerendered \
             "$root"/assets/fonts \
             "$root"/assets/.cache \
             "$root"/config/themes

    run "$SCRIPT" "$root"
    [ "$status" -eq 0 ]

    local expected
    expected="assets
assets/fonts
assets/images
assets/images/printers
assets/images/printers/prerendered
config
config/themes
ui_xml
ui_xml/components
ui_xml/translations"

    if [ "$output" != "$expected" ]; then
        echo "--- expected ---"; printf '%s\n' "$expected"
        echo "--- actual ---";   printf '%s\n' "$output"
        return 1
    fi
}

@test "manifest skips a root that does not exist in the tree" {
    # config/ is optional; ui_xml/ and assets/ are the project-root check.
    local root="$BATS_TEST_TMPDIR/noconfig"
    mkdir -p "$root/ui_xml" "$root/assets/sounds"

    run "$SCRIPT" "$root"
    [ "$status" -eq 0 ]
    [ "$output" = "assets
assets/sounds
ui_xml" ]
}

@test "manifest includes critical seed dirs introduced by bfeba7c26" {
    # If any of these drop out of the manifest, the Android APK and/or release
    # tarballs will ship without printer detection, themes, presets, etc.
    local required=(
        assets/config
        assets/config/presets
        assets/config/print_start_profiles
        assets/config/themes/defaults
        assets/config/platform
        assets/config/panel_widgets
        assets/fonts
        assets/images
        assets/sounds
        assets/zoneinfo
        ui_xml
        ui_xml/components
        ui_xml/translations
    )
    local manifest
    manifest=$("$SCRIPT")
    for dir in "${required[@]}"; do
        if ! grep -qxF "$dir" <<<"$manifest"; then
            echo "Manifest missing required directory: $dir"
            return 1
        fi
    done
}

@test "manifest excludes hidden directories (.git, .claude-recall, ...)" {
    run "$SCRIPT"
    [ "$status" -eq 0 ]
    # No line should contain a path component starting with '.'
    ! grep -qE '(^|/)\.[^/]' <<<"$output"
}

@test "manifest accepts explicit project root argument" {
    run "$SCRIPT" "$PWD"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
}

@test "manifest rejects non-project-root argument" {
    run "$SCRIPT" /tmp
    [ "$status" -ne 0 ]
}
