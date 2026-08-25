#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_installer_step_reachability.py - the gate that
# fails an installer step function nothing in the flow calls.
#
# The failure mode it exists for (#1343): install_permission_rules() was
# written, reviewed, and covered by 34 direct unit tests, and never wired into
# main(). Shell does not complain about an uncalled function, shellcheck does
# not either, and the bats suites call these functions directly - so every
# signal the project had said the code was fine while the backlight udev rule
# was never written on any device.
#
# Both halves are pinned here. The catch half is the point. The quiet half
# matters just as much: a gate that fired on the modules' internal helpers, or
# on a deliberate compatibility alias, would be noise on every installer commit
# and would get switched off.

GATE="scripts/check_installer_step_reachability.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1

    # A miniature installer: a lib/ of modules plus one entry point that
    # sources them and calls main, mirroring scripts/install-dev.sh.
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/inst"
    mkdir -p "$ROOT/lib"

    cat > "$ROOT/lib/main.sh" <<'MAIN_EOF'
#!/bin/sh
main() {
    step_wired "$1"
}
MAIN_EOF

    cat > "$ROOT/lib/steps.sh" <<'STEPS_EOF'
#!/bin/sh
# A step main() actually wires in.
step_wired() {
    _helper_called "$1"
}

# An internal helper, reached only from step_wired.
_helper_called() {
    echo "$1"
}

# The bug: defined, unit-tested, never called by anything.
step_orphan() {
    echo orphan
}

# UNCALLED_OK: fixture - stands in for a deliberate compatibility alias
step_annotated() {
    echo annotated
}

# Only ever mentioned in prose like this: step_comment_only is not a call.
step_comment_only() {
    echo comment_only
}
STEPS_EOF

    cat > "$ROOT/entry.sh" <<'ENTRY_EOF'
#!/bin/sh
. lib/main.sh
. lib/steps.sh
main "$@"
ENTRY_EOF
}

run_gate() {
    run python3 "$GATE" --lib-root "$ROOT/lib" --caller "$ROOT/entry.sh"
}

# Wire the two orphans in, so a test can start from a clean fixture.
wire_everything() {
    cat > "$ROOT/lib/main.sh" <<'MAIN_EOF'
#!/bin/sh
main() {
    step_wired "$1"
    step_orphan
    step_comment_only
}
MAIN_EOF
}

# ----------------------------------------------------------- the catch half

@test "flags a step function with no call site (the #1343 case)" {
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"step_orphan"* ]]
    [[ "$output" == *"no production call site"* ]]
}

@test "a mention in a comment does not count as a call site" {
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"step_comment_only"* ]]
}

@test "reports the file and line of each uncalled definition" {
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"steps.sh:"* ]]
}

@test "the failure names the escape hatch" {
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"UNCALLED_OK"* ]]
}

@test "--list prints just the uncalled names" {
    run python3 "$GATE" --lib-root "$ROOT/lib" --caller "$ROOT/entry.sh" --list
    [ "$status" -eq 1 ]
    [[ "$output" == *"step_orphan"* ]]
    [[ "$output" != *"no production call site"* ]]
}

# ----------------------------------------------------------- the quiet half

@test "wiring the orphans in turns the gate green" {
    wire_everything
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 uncalled functions"* ]]
}

@test "a step reached only from another module is not flagged" {
    wire_everything
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" != *"step_wired"* ]]
}

@test "an internal _-prefixed helper called by a step is not flagged" {
    wire_everything
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" != *"_helper_called"* ]]
}

@test "main() is not flagged - the entry point calls it" {
    run_gate
    [[ "$output" != *"main()"* ]]
}

@test "UNCALLED_OK on the definition silences it and is counted" {
    wire_everything
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" != *"step_annotated"* ]]
    [[ "$output" == *"1 annotated"* ]]
}

@test "an unannotated orphan is still caught alongside an annotated one" {
    # Mutation check on the annotation itself: dropping UNCALLED_OK must make
    # step_annotated fail, or the escape hatch is not doing the work.
    wire_everything
    sed -i 's/^# UNCALLED_OK.*$/# just a comment/' "$ROOT/lib/steps.sh"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"step_annotated"* ]]
}

# ----------------------------------------------------------- the real tree

@test "the repo's own installer modules pass the gate" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 uncalled functions"* ]]
}

@test "the gate sees install_permission_rules as reached (#1343 stays fixed)" {
    # Belt and braces with test_permission_rules.bats: that one proves main()
    # runs it, this one proves the gate would notice if the call went away.
    run python3 "$GATE" --list
    [ "$status" -eq 0 ]
    [[ "$output" != *"install_permission_rules"* ]]
}

@test "gate is wired into quality-checks.sh" {
    run grep -q "check_installer_step_reachability.py" scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate section is registered in the quality-checks section list" {
    run grep -q 'QC_ALL=.*qc_installer_reachability' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}
