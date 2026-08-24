#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_drm_mmap_lfs.py - the DRM 64-bit mmap gate.
#
# The gate exists because the failure it guards is invisible. LVGL's
# drm_allocate_dumb() mmaps at an offset DRM allocates from 4 GiB upward; on a
# 32-bit target off_t is 32 bits without large-file support, the offset is
# truncated, the mmap fails, and HelixScreen quietly falls back to fbdev. A Pi
# running a 32-bit userland then never uses KMS and the only evidence is one
# "mmap fail" line. Nothing in the test suite can observe that from x86_64, so
# what is enforceable is the shape of the fix: the patch exists, the build
# applies it, and the define still sits above the first #include.
#
# These tests pin that each of those can actually fail.

GATE="scripts/check_drm_mmap_lfs.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/drm_gate"
    mkdir -p "$FIXTURE/patches" "$FIXTURE/mk"
}

# A repo-root fixture whose patch and patches.mk are copies of the real ones.
make_repo() {
    cp "patches/lvgl-drm-mmap64.patch" "$FIXTURE/patches/"
    cp "mk/patches.mk" "$FIXTURE/mk/"
}

# --- the real tree passes ----------------------------------------------------

@test "passes on the repository as committed" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"64-bit file offset"* ]]
}

# --- patch-level regressions -------------------------------------------------

@test "fails when the patch is deleted" {
    make_repo
    rm "$FIXTURE/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --repo-root "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"has been dropped"* ]]
}

@test "fails when mk/patches.mk stops applying the patch" {
    make_repo
    grep -v 'lvgl-drm-mmap64.patch' "mk/patches.mk" > "$FIXTURE/mk/patches.mk"
    run python3 "$GATE" --repo-root "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no apply block"* ]]
}

@test "fails when the patch no longer adds the _FILE_OFFSET_BITS define" {
    make_repo
    grep -v '_FILE_OFFSET_BITS 64' "patches/lvgl-drm-mmap64.patch" \
        > "$FIXTURE/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --repo-root "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"_FILE_OFFSET_BITS"* ]]
}

@test "fails when the patch no longer adds the off_t static assertion" {
    make_repo
    grep -v '_Static_assert' "patches/lvgl-drm-mmap64.patch" \
        > "$FIXTURE/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --repo-root "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"backstop"* ]]
}

@test "a define that only appears as patch CONTEXT does not satisfy the gate" {
    make_repo
    # Turn the added define into a context line: it is present in the patch text
    # but the patch no longer introduces it.
    sed 's/^+    #define _FILE_OFFSET_BITS 64/     #define _FILE_OFFSET_BITS 64/' \
        "patches/lvgl-drm-mmap64.patch" > "$FIXTURE/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --repo-root "$FIXTURE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"_FILE_OFFSET_BITS"* ]]
}

# --- source-level regressions in an already-patched tree ---------------------

# Minimal stand-in for a patched lv_linux_drm.c: the two things that matter are
# where the define sits and whether the assertion survived.
write_source() {
    printf '%s\n' "$1" > "$FIXTURE/lv_linux_drm.c"
}

PATCHED_SOURCE='/**
 * @file lv_linux_drm.c
 */
#ifndef _FILE_OFFSET_BITS
    #define _FILE_OFFSET_BITS 64
#endif
#include "lv_linux_drm.h"
#include <sys/mman.h>
static int drm_allocate_dumb(void)
{
    _Static_assert(sizeof(off_t) == 8, "off_t must be 64-bit");
    return 0;
}'

@test "passes on a correctly patched source" {
    write_source "$PATCHED_SOURCE"
    run python3 "$GATE" --drm-source "$FIXTURE/lv_linux_drm.c"
    [ "$status" -eq 0 ]
}

@test "fails when the define drifts below the first #include" {
    write_source '/**
 * @file lv_linux_drm.c
 */
#include "lv_linux_drm.h"
#ifndef _FILE_OFFSET_BITS
    #define _FILE_OFFSET_BITS 64
#endif
#include <sys/mman.h>
static int drm_allocate_dumb(void)
{
    _Static_assert(sizeof(off_t) == 8, "off_t must be 64-bit");
    return 0;
}'
    run python3 "$GATE" --drm-source "$FIXTURE/lv_linux_drm.c"
    [ "$status" -eq 1 ]
    [[ "$output" == *"below the first"* ]]
}

@test "fails when a patched source loses the static assertion" {
    write_source '/**
 * @file lv_linux_drm.c
 */
#ifndef _FILE_OFFSET_BITS
    #define _FILE_OFFSET_BITS 64
#endif
#include "lv_linux_drm.h"
#include <sys/mman.h>
static int drm_allocate_dumb(void)
{
    return 0;
}'
    run python3 "$GATE" --drm-source "$FIXTURE/lv_linux_drm.c"
    [ "$status" -eq 1 ]
    [[ "$output" == *"_Static_assert"* ]]
}

# --- states that are legitimate, not failures --------------------------------

@test "an unpatched submodule is reported, not failed" {
    write_source '/**
 * @file lv_linux_drm.c
 */
#include "lv_linux_drm.h"
#include <sys/mman.h>
static int drm_allocate_dumb(void)
{
    return 0;
}'
    run python3 "$GATE" --drm-source "$FIXTURE/lv_linux_drm.c"
    [ "$status" -eq 0 ]
    [[ "$output" == *"unpatched"* ]]
}

@test "a missing submodule checkout is reported, not failed" {
    run python3 "$GATE" --drm-source "$FIXTURE/does-not-exist.c"
    [ "$status" -eq 0 ]
    [[ "$output" == *"not present"* ]]
}
