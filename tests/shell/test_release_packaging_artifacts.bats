#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for release packaging — gitignore, install.sh, LZ4 compression
# Split from test_release_packaging.bats for parallel execution.

load helpers

# Overridable so these gates can be proven to FIRE against a hand-broken copy:
#   sed '/^release-x86:/,/^$/{/release-clean-assets/d}' mk/cross.mk > /tmp/c.mk
#   HELIX_TEST_CROSS_MK=/tmp/c.mk bats tests/shell/test_release_packaging_artifacts.bats
CROSS_MK="${HELIX_TEST_CROSS_MK:-mk/cross.mk}"

# ============================================================================
# Release targets call release-clean-assets
# ============================================================================

# The release targets, enumerated from mk/cross.mk itself. Aggregates and
# housekeeping targets are excluded: release-all only depends on the others and
# release-clean/-clean-assets ship nothing.
#
# Deriving the list is the whole point. Both tests below used to compare an
# occurrence count against a hand-written floor (>= 7 against real counts of 12
# and 10), so a target could drop its cleanup call — or a whole new platform
# could arrive without one — and the numbers still cleared the bar. The stale
# 7-element array in the second test had already missed release-ad5x,
# release-cc1 and release-x86.
release_targets() {
    grep -oE '^release-[a-z0-9-]+:' "$CROSS_MK" \
        | sed 's/:$//' \
        | grep -vxE 'release-all|release-clean|release-clean-assets' \
        | sort -u
}

# Recipe body of target $1: the lines after it, up to the first line that is
# neither indented nor blank.
release_recipe() {
    awk -v t="$1" '
        index($0, t ":") == 1 { inside = 1; next }
        inside && /^[^\t ]/ && NF { inside = 0 }
        inside { print }
    ' "$CROSS_MK"
}

@test "every release target calls release-clean-assets" {
    local targets
    targets=$(release_targets)
    # A regex that stops matching would empty the list and pass vacuously.
    [ "$(printf '%s\n' "$targets" | wc -l)" -ge 8 ] || fail "release_targets() found only: $targets"

    local t missing=""
    for t in $targets; do
        release_recipe "$t" | grep -q 'release-clean-assets' || missing="$missing $t"
    done
    [ -z "$missing" ] || fail "release targets missing the release-clean-assets call:$missing"
}

# ============================================================================
# Source tree doesn't contain files that shouldn't be deployed
# ============================================================================

@test "CJK fonts are gitignored" {
    grep -q 'NotoSansCJK' .gitignore
}

# ============================================================================
# install.sh included in release packages
# ============================================================================

@test "every release target copies install.sh into the package root" {
    # update_checker.cpp extracts helixscreen/INSTALLER_FILENAME from the tarball
    # to run the version-matched installer. A target that skips the cp ships a
    # package whose self-update fails with "Installer not found".
    local targets
    targets=$(release_targets)
    [ "$(printf '%s\n' "$targets" | wc -l)" -ge 8 ] || fail "release_targets() found only: $targets"

    local t missing=""
    for t in $targets; do
        release_recipe "$t" \
            | grep -qE 'cp scripts/\$\(INSTALLER_FILENAME\).*\$\(RELEASE_DIR\)' \
            || missing="$missing $t"
    done
    [ -z "$missing" ] || fail "release targets not packaging the installer:$missing"
}

# ============================================================================
# LZ4 compression enabled
# ============================================================================

@test "LV_USE_LZ4_INTERNAL is enabled in lv_conf.h" {
    grep -q '#define LV_USE_LZ4_INTERNAL.*1' lv_conf.h
}

@test "image generation scripts use LZ4 compression" {
    grep -q '\-\-compress.*LZ4' scripts/lib/lvgl_image_lib.sh
}

@test "3D splash generation uses LZ4 compression" {
    grep -q '"--compress".*"LZ4"' scripts/gen_splash_3d.py
}
