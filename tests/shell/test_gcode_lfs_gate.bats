#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_gcode_lfs.py - the gcode large-file gate.
#
# The gate exists because the compiler cannot cover this at PR time. The
# static_assert in gcode_data_source.cpp only fails on a 32-bit target, and
# pi32/ad5m/cc1/k1 are in release.yml's matrix, not build.yml's - on x86_64 PR CI
# off_t is already 8 bytes and the assertion passes trivially. Drop the
# mk/rules.mk override and everything stays green until release.
#
# The third case below is the one worth having: a `#define _FILE_OFFSET_BITS` in
# the .cpp looks like the obvious simplification of the build rule and is a
# complete no-op, because $(PCH_FLAGS) force-includes lvgl_pch.h ahead of the
# source and glibc latches the value first. That mistake reverts the fix while
# looking like a tidy-up.

GATE="scripts/check_gcode_lfs.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/gcode_lfs"
    mkdir -p "$FIXTURE"
}

# Good source: assert present, no file-scope define.
write_good_source() {
    cat > "$FIXTURE/good.cpp" <<'EOF'
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    static_assert(sizeof(off_t) == 8, "off_t must be 64-bit: see mk/rules.mk LFS override");
    fseeko(file_, 0, SEEK_END);
}
EOF
}

write_rules() {
    printf '%s\n' "$1" > "$FIXTURE/rules.mk"
}

GOOD_RULES='$(OBJ_DIR)/rendering/gcode_data_source.o: CXXFLAGS += -D_FILE_OFFSET_BITS=64'

# --- the real tree passes -----------------------------------------------------

@test "passes on the repository as committed" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"64-bit off_t"* ]]
}

# --- the regression this gate exists for --------------------------------------

@test "fails when the mk/rules.mk override is removed" {
    write_good_source
    write_rules '# nothing here'
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

@test "fails when the override is commented out" {
    write_good_source
    write_rules '#$(OBJ_DIR)/rendering/gcode_data_source.o: CXXFLAGS += -D_FILE_OFFSET_BITS=64'
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

@test "fails when the override targets a different object" {
    write_good_source
    write_rules '$(OBJ_DIR)/rendering/gcode_parser.o: CXXFLAGS += -D_FILE_OFFSET_BITS=64'
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

@test "fails when the override loses the define but keeps the target" {
    write_good_source
    write_rules '$(OBJ_DIR)/rendering/gcode_data_source.o: CXXFLAGS += -DSOMETHING_ELSE'
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/good.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
}

# --- the source-side backstops ------------------------------------------------

@test "fails when the static_assert backstop is removed" {
    write_rules "$GOOD_RULES"
    cat > "$FIXTURE/no_assert.cpp" <<'EOF'
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    fseeko(file_, 0, SEEK_END);
}
EOF
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/no_assert.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"backstop is gone"* ]]
}

@test "fails when the build rule is 'simplified' into a #define in the source" {
    write_rules "$GOOD_RULES"
    cat > "$FIXTURE/with_define.cpp" <<'EOF'
#define _FILE_OFFSET_BITS 64
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    static_assert(sizeof(off_t) == 8, "off_t must be 64-bit");
    fseeko(file_, 0, SEEK_END);
}
EOF
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/with_define.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"does nothing here"* ]]
}

@test "the guarded #ifndef form of the source define is caught too" {
    write_rules "$GOOD_RULES"
    cat > "$FIXTURE/guarded.cpp" <<'EOF'
#ifndef _FILE_OFFSET_BITS
    #define _FILE_OFFSET_BITS 64
#endif
#include "gcode_data_source.h"
FileDataSource::FileDataSource(const std::string& p) {
    static_assert(sizeof(off_t) == 8, "off_t must be 64-bit");
}
EOF
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/guarded.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"does nothing here"* ]]
}

# --- both sides broken at once ------------------------------------------------

@test "reports the rule and the source together" {
    write_rules '# nothing here'
    cat > "$FIXTURE/empty.cpp" <<'EOF'
#include "gcode_data_source.h"
EOF
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/empty.cpp"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no target-specific"* ]]
    [[ "$output" == *"backstop is gone"* ]]
}

# --- a correct fixture must pass, or the failures above prove nothing ---------

@test "passes on a correct rules + source pair" {
    write_good_source
    write_rules "$GOOD_RULES"
    run python3 "$GATE" --rules "$FIXTURE/rules.mk" --source "$FIXTURE/good.cpp"
    [ "$status" -eq 0 ]
}
