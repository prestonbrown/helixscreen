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
# copy of the script.
SCRIPT="${HELIX_TEST_MANIFEST_SH:-scripts/gen-packaging-manifest.sh}"

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

@test "manifest rejects non-project-root argument" {
    run "$SCRIPT" /tmp
    [ "$status" -ne 0 ]
}

@test "the seed-dir lint names a required directory the manifest drops" {
    # lint_packaging_seed_dirs (scripts/code_lint.sh) is the verdict over the real
    # tree, run by qc_code_lint: if a seed directory drops out of the manifest,
    # the APK and the release tarballs ship without printer detection, themes or
    # presets. Here it reads a fixture root holding every seed dir but one.
    local root="$BATS_TEST_TMPDIR/seeds"
    . scripts/code_lint.sh
    mkdir -p "$root/scripts"
    cp "$SCRIPT" "$root/scripts/gen-packaging-manifest.sh"
    while IFS= read -r dir; do mkdir -p "$root/$dir"; done < <(packaging_seed_dirs)
    cd "$root" || return 1
    run lint_packaging_seed_dirs
    [ "$status" -eq 0 ] || { echo "$output"; return 1; }

    rmdir assets/zoneinfo
    run lint_packaging_seed_dirs
    [ "$status" -eq 1 ]
    [[ "$output" == *"assets/zoneinfo"* ]]
}
