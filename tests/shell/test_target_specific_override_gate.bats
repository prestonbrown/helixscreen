#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_target_specific_override.py - the gate requiring
# `override` on target-specific flag rules.
#
# GNU make discards makefile assignments to a variable that came from the
# command line unless the assignment says `override`, and the sanitizer targets
# re-invoke make with CXXFLAGS and LDFLAGS as command-line variables. A rule
# without the keyword therefore compiles its object without the flag under
# test-asan and test-tsan, while the rule sits in the makefile looking correct.
#
# The quiet half carries the weight here. Makefiles are dense with colons and
# with assignments to these same variables, so a gate that fired on plain global
# flags, on recipe lines, or on the `override` form it is asking for would be red
# constantly and would get switched off.

load helpers

GATE="scripts/check_target_specific_override.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/mk"
    : > "$ROOT/Makefile"
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT"
}

write_mk() {
    printf '%s\n' "$1" > "$ROOT/mk/a.mk"
}

# ------------------------------------------------------------- the quiet half

@test "passes on makefiles with no target-specific flag rules" {
    write_mk 'all:
	@echo hi'
    run_gate
    [ "$status" -eq 0 ]
}

@test "the override form is accepted" {
    write_mk '$(OBJ_DIR)/system/pwm_sound_backend.o: override CXXFLAGS += -DHELIX_PWM_AUTO_EXPORT'
    run_gate
    [ "$status" -eq 0 ]
}

@test "a plain global assignment is not a target-specific rule" {
    write_mk 'CXXFLAGS += -DHELIX_HAS_ACE=1
LDFLAGS += -lm'
    run_gate
    [ "$status" -eq 0 ]
}

@test "a commented-out rule is not a rule" {
    write_mk '#$(OBJ_DIR)/foo.o: CXXFLAGS += -DSOMETHING'
    run_gate
    [ "$status" -eq 0 ]
}

@test "a recipe line that mentions CXXFLAGS is shell, not an assignment" {
    # Leading tab. This is the shape of every compile recipe in the tree.
    printf 'build:\n\t$(CXX) $(CXXFLAGS) -c $< -o $@\n' > "$ROOT/mk/a.mk"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a rule setting a non-flag variable is left alone" {
    write_mk '$(TEST_BIN): TEST_SHARDS = 8'
    run_gate
    [ "$status" -eq 0 ]
}

@test "a target-specific ?= is skipped, since override cannot rescue it" {
    write_mk '$(OBJ_DIR)/foo.o: CXXFLAGS ?= -DSOMETHING'
    run_gate
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------- the catch half

@test "flags a target-specific CXXFLAGS rule without override" {
    write_mk '$(OBJ_DIR)/rendering/gcode_data_source.o: CXXFLAGS += -D_FILE_OFFSET_BITS=64'
    run_gate
    [ "$status" -eq 1 ]
    contains "without \`override\`" "$output"
    [[ "$output" == *"gcode_data_source.o"* ]]
}

@test "flags the binary-target form as well as the object form" {
    write_mk '$(TEST_BIN): CXXFLAGS += -DHELIX_HAS_ACE=1'
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"TEST_BIN"* ]]
}

@test "flags LDFLAGS and CPPFLAGS, not just CXXFLAGS" {
    write_mk '$(BIN): LDFLAGS += -lfoo'
    run_gate
    [ "$status" -eq 1 ]
    contains "LDFLAGS" "$output"

    write_mk '$(OBJ_DIR)/foo.o: CPPFLAGS += -DBAR'
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"CPPFLAGS"* ]]
}

@test "flags a plain = as well as +=" {
    write_mk '$(OBJ_DIR)/foo.o: CXXFLAGS = -DSOMETHING'
    run_gate
    [ "$status" -eq 1 ]
}

@test "scans the top-level Makefile too, not only mk/" {
    printf '$(OBJ_DIR)/foo.o: CXXFLAGS += -DSOMETHING\n' > "$ROOT/Makefile"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"Makefile:1"* ]]
}

@test "reports every offender, not just the first" {
    write_mk '$(OBJ_DIR)/a.o: CXXFLAGS += -DA
$(OBJ_DIR)/b.o: CXXFLAGS += -DB'
    run_gate
    [ "$status" -eq 1 ]
    contains "a.o" "$output"
    [[ "$output" == *"b.o"* ]]
}

# ------------------------------------------------------------- the real tree

@test "the checked-in makefiles satisfy the gate" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}
