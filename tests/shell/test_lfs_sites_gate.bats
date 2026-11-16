#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_lfs_sites.py: every site that needs a 64-bit off_t
# on a 32-bit target still gets one. Each site's cases run it alone with --only;
# the last cases prove a default run checks every site.
#
# gcode: the compiler cannot cover this at PR time. The static_assert in
# gcode_data_source.cpp only fails on a 32-bit target, and pi32/ad5m/cc1/k1 are in
# release.yml's matrix, not build.yml's - on x86_64 PR CI off_t is already 8 bytes
# and the assertion passes trivially. Drop the mk/rules.mk override and everything
# stays green until release. The source-define case is the one worth having: a
# `#define _FILE_OFFSET_BITS` in the .cpp looks like the obvious simplification of
# the build rule and is a complete no-op, because $(FORCED_INCLUDE) force-includes
# lvgl_pch.h ahead of the source and glibc latches the value first.
#
# drm: the failure is invisible. LVGL's drm_allocate_dumb() mmaps at an offset DRM
# allocates from 4 GiB upward; on a 32-bit target off_t is 32 bits without
# large-file support, the offset is truncated, the mmap fails, and HelixScreen
# quietly falls back to fbdev. What is enforceable is the shape of the fix: the
# patch exists, the build applies it, and the define still sits above the first
# #include.

GATE="scripts/check_lfs_sites.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    GFIX="${BATS_TEST_TMPDIR:-$(mktemp -d)}/gcode_lfs"
    DFIX="${BATS_TEST_TMPDIR:-$(mktemp -d)}/drm_gate"
    mkdir -p "$GFIX" "$DFIX/patches" "$DFIX/mk"
}

# ============================================================== gcode reader

# Good source: assert present, no file-scope define.
write_good_source() {
    cat > "$GFIX/good.cpp" <<'EOF'
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    static_assert(sizeof(off_t) == 8, "off_t must be 64-bit: see mk/rules.mk LFS override");
    fseeko(file_, 0, SEEK_END);
}
EOF
}

write_rules() {
    printf '%s\n' "$1" > "$GFIX/rules.mk"
}

GOOD_RULES='$(OBJ_DIR)/rendering/gcode_data_source.o: CXXFLAGS += -D_FILE_OFFSET_BITS=64'

# --- the regression this gate exists for --------------------------------------

@test "fails when the mk/rules.mk override is removed" {
    write_good_source
    write_rules '# nothing here'
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

@test "fails when the override is commented out" {
    write_good_source
    write_rules '#$(OBJ_DIR)/rendering/gcode_data_source.o: CXXFLAGS += -D_FILE_OFFSET_BITS=64'
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

@test "fails when the override targets a different object" {
    write_good_source
    write_rules '$(OBJ_DIR)/rendering/gcode_parser.o: CXXFLAGS += -D_FILE_OFFSET_BITS=64'
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

@test "fails when the override loses the define but keeps the target" {
    write_good_source
    write_rules '$(OBJ_DIR)/rendering/gcode_data_source.o: CXXFLAGS += -DSOMETHING_ELSE'
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

# --- the source-side backstops ------------------------------------------------

@test "fails when the static_assert backstop is removed" {
    write_rules "$GOOD_RULES"
    cat > "$GFIX/no_assert.cpp" <<'EOF'
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    fseeko(file_, 0, SEEK_END);
}
EOF
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/no_assert.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"backstop is gone"* ]]
}

@test "fails when the build rule is 'simplified' into a #define in the source" {
    write_rules "$GOOD_RULES"
    cat > "$GFIX/with_define.cpp" <<'EOF'
#define _FILE_OFFSET_BITS 64
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    static_assert(sizeof(off_t) == 8, "off_t must be 64-bit");
    fseeko(file_, 0, SEEK_END);
}
EOF
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/with_define.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"does nothing here"* ]]
}

@test "the guarded #ifndef form of the source define is caught too" {
    write_rules "$GOOD_RULES"
    cat > "$GFIX/guarded.cpp" <<'EOF'
#ifndef _FILE_OFFSET_BITS
    #define _FILE_OFFSET_BITS 64
#endif
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    static_assert(sizeof(off_t) == 8, "off_t must be 64-bit");
}
EOF
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/guarded.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"does nothing here"* ]]
}

# --- both sides broken at once ------------------------------------------------

@test "reports the rule and the source together" {
    write_rules '# nothing here'
    cat > "$GFIX/empty.cpp" <<'EOF'
#include "gcode_data_source.h"
EOF
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/empty.cpp"
    [ "$status" -eq 1 ]
    contains "no target-specific" "$output"
    [[ "$output" == *"backstop is gone"* ]]
}

# --- a correct fixture must pass, or the failures above prove nothing ---------

@test "passes on a correct rules + source pair" {
    write_good_source
    write_rules "$GOOD_RULES"
    run python3 "$GATE" --only gcode --rules "$GFIX/rules.mk" --source "$GFIX/good.cpp"
    [ "$status" -eq 0 ]
}

# ========================================================== DRM dumb buffer

# A repo-root fixture whose patch and patches.mk are copies of the real ones.
make_repo() {
    cp "patches/lvgl-drm-mmap64.patch" "$DFIX/patches/"
    cp "mk/patches.mk" "$DFIX/mk/"
}

# --- patch-level regressions -------------------------------------------------

@test "fails when the patch is deleted" {
    make_repo
    rm "$DFIX/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --only drm --repo-root "$DFIX"
    [ "$status" -eq 1 ]
    [[ "$output" == *"has been dropped"* ]]
}

@test "fails when mk/patches.mk stops applying the patch" {
    make_repo
    grep -v 'lvgl-drm-mmap64.patch' "mk/patches.mk" > "$DFIX/mk/patches.mk"
    run python3 "$GATE" --only drm --repo-root "$DFIX"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no apply block"* ]]
}

@test "fails when the patch no longer adds the _FILE_OFFSET_BITS define" {
    make_repo
    grep -v '_FILE_OFFSET_BITS 64' "patches/lvgl-drm-mmap64.patch" \
        > "$DFIX/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --only drm --repo-root "$DFIX"
    [ "$status" -eq 1 ]
    [[ "$output" == *"_FILE_OFFSET_BITS"* ]]
}

@test "fails when the patch no longer adds the off_t static assertion" {
    make_repo
    grep -v '_Static_assert' "patches/lvgl-drm-mmap64.patch" \
        > "$DFIX/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --only drm --repo-root "$DFIX"
    [ "$status" -eq 1 ]
    [[ "$output" == *"backstop"* ]]
}

@test "a define that only appears as patch CONTEXT does not satisfy the gate" {
    make_repo
    # Turn the added define into a context line: it is present in the patch text
    # but the patch no longer introduces it.
    sed 's/^+    #define _FILE_OFFSET_BITS 64/     #define _FILE_OFFSET_BITS 64/' \
        "patches/lvgl-drm-mmap64.patch" > "$DFIX/patches/lvgl-drm-mmap64.patch"
    run python3 "$GATE" --only drm --repo-root "$DFIX"
    [ "$status" -eq 1 ]
    [[ "$output" == *"_FILE_OFFSET_BITS"* ]]
}

# --- source-level regressions in an already-patched tree ---------------------

# Minimal stand-in for a patched lv_linux_drm.c: the two things that matter are
# where the define sits and whether the assertion survived.
write_source() {
    printf '%s\n' "$1" > "$DFIX/lv_linux_drm.c"
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
    run python3 "$GATE" --only drm --drm-source "$DFIX/lv_linux_drm.c"
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
    run python3 "$GATE" --only drm --drm-source "$DFIX/lv_linux_drm.c"
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
    run python3 "$GATE" --only drm --drm-source "$DFIX/lv_linux_drm.c"
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
    run python3 "$GATE" --only drm --drm-source "$DFIX/lv_linux_drm.c"
    [ "$status" -eq 0 ]
    [[ "$output" == *"unpatched"* ]]
}

@test "a missing submodule checkout is reported, not failed" {
    run python3 "$GATE" --only drm --drm-source "$DFIX/does-not-exist.c"
    [ "$status" -eq 0 ]
    [[ "$output" == *"not present"* ]]
}

# ============================================================ every site

@test "a default run checks every site: a broken gcode rule fails beside a sound DRM patch" {
    make_repo
    mkdir -p "$DFIX/src/rendering"
    printf '# nothing here\n' > "$DFIX/mk/rules.mk"
    printf 'static_assert(sizeof(off_t) == 8, "x");\n' > "$DFIX/src/rendering/gcode_data_source.cpp"
    run python3 "$GATE" --repo-root "$DFIX" --drm-source "$DFIX/none.c"
    [ "$status" -eq 1 ]
    contains "no target-specific" "$output"
    contains "✓ DRM dumb-buffer mmap uses a 64-bit file offset" "$output"
}

@test "a default run checks every site: a dropped DRM patch fails beside a sound gcode rule" {
    make_repo
    rm "$DFIX/patches/lvgl-drm-mmap64.patch"
    mkdir -p "$DFIX/src/rendering"
    printf '%s\n' "$GOOD_RULES" > "$DFIX/mk/rules.mk"
    printf 'static_assert(sizeof(off_t) == 8, "x");\n' > "$DFIX/src/rendering/gcode_data_source.cpp"
    run python3 "$GATE" --repo-root "$DFIX" --drm-source "$DFIX/none.c"
    [ "$status" -eq 1 ]
    contains "has been dropped" "$output"
    contains "✓ gcode reader builds with a 64-bit off_t" "$output"
}
