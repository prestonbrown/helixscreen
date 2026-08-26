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
