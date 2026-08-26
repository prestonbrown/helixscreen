#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for release packaging — deploy asset excludes, clean-assets function
# Split from test_release_packaging.bats for parallel execution.

load helpers

# Cache make -n -p output once per file (expensive: parses entire Makefile tree)
setup_file() {
    export MAKE_DB_CACHE="$BATS_FILE_TMPDIR/make_db.txt"
    make -n -p 2>/dev/null > "$MAKE_DB_CACHE" || true
}

# ============================================================================
# Deploy asset exclude patterns
# ============================================================================

@test "DEPLOY_ASSET_EXCLUDES excludes .c font files" {
    local excludes
    excludes=$(grep '^DEPLOY_ASSET_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    [[ "$excludes" == *"assets/fonts/*.c"* ]]
}

@test "DEPLOY_ASSET_EXCLUDES excludes printer source PNGs" {
    # 24MB of full-res artwork no device renders — every printer draws from its
    # prerendered .bin. Asserted against the EXPANDED value, which is what
    # catches the := ordering trap: defining DEPLOY_PRINTER_PNG_EXCLUDES after
    # its use expands to empty and ships the PNGs with no error anywhere.
    local excludes
    excludes=$(grep '^DEPLOY_ASSET_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    [[ "$excludes" == *"assets/images/printers/*.png"* ]]
}

@test "DEPLOY_TAR_EXCLUDES excludes printer source PNGs" {
    local excludes
    excludes=$(grep '^DEPLOY_TAR_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    [[ "$excludes" == *"assets/images/printers/*.png"* ]]
}

@test "deploy excludes do NOT drop prerendered printer .bin files" {
    # The .bin set is what every printer actually renders from. Excluding it
    # would leave the UI drawing the generic fallback for every machine.
    local asset tar
    asset=$(grep '^DEPLOY_ASSET_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    tar=$(grep '^DEPLOY_TAR_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    [[ "$asset" != *"prerendered"* ]]
    [[ "$tar" != *"prerendered"* ]]
    [[ "$asset" != *"*.bin"* ]]
    [[ "$tar" != *"*.bin"* ]]
}

@test "DEPLOY_ASSET_EXCLUDES excludes .icns files" {
    local excludes
    excludes=$(grep '^DEPLOY_ASSET_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    [[ "$excludes" == *"*.icns"* ]]
}

@test "DEPLOY_ASSET_EXCLUDES excludes mdi-icon-metadata" {
    local excludes
    excludes=$(grep '^DEPLOY_ASSET_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    [[ "$excludes" == *"mdi-icon-metadata.json.gz"* ]]
}

@test "DEPLOY_TAR_EXCLUDES matches DEPLOY_ASSET_EXCLUDES patterns" {
    local rsync_excludes tar_excludes
    rsync_excludes=$(grep '^DEPLOY_ASSET_EXCLUDES' "$MAKE_DB_CACHE" | head -1)
    tar_excludes=$(grep '^DEPLOY_TAR_EXCLUDES' "$MAKE_DB_CACHE" | head -1)

    # Both should exclude .c font files
    [[ "$rsync_excludes" == *"assets/fonts/*.c"* ]]
    [[ "$tar_excludes" == *"assets/fonts/*.c"* ]]

    # Both should exclude .icns
    [[ "$rsync_excludes" == *"*.icns"* ]]
    [[ "$tar_excludes" == *"*.icns"* ]]
}

# ============================================================================
# Release clean-assets function
# ============================================================================

@test "release-clean-assets is defined in cross.mk" {
    grep -q 'define release-clean-assets' mk/cross.mk
}

@test "release-clean-assets removes .c font files" {
    grep -A5 'define release-clean-assets' mk/cross.mk | grep -q "fonts.*\*\.c.*-delete"
}

@test "release-clean-assets removes .icns files" {
    grep -A5 'define release-clean-assets' mk/cross.mk | grep -q "\*\.icns.*-delete"
}
