#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the raw-print-state rule of scripts/check_source_rules.py: every read of the raw
# print wire (PrintJobState::PRINTING/PAUSED/STANDBY, get_print_job_state(),
# get_print_state_enum_subject()) carries `// RAW_PRINT_STATE_OK: <reason>`
# within the lines above it. The gate does not forbid the wire, only reading it
# silently, so the quiet cases (terminal states, comments, a marked switch, the
# derivation layer) matter as much as the loud ones.

GATE="scripts/check_source_rules.py"
RULE="--rule raw-print-state"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers
    FIX="${BATS_TEST_TMPDIR:-$(mktemp -d)}/raw_print_state"
    mkdir -p "$FIX"
}

run_gate() {
    printf '%s\n' "$2" > "$FIX/$1.cpp"
    run python3 "$GATE" $RULE "$FIX/$1.cpp"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags an unmarked comparison against a pre-print-blind state" {
    run_gate cmp '
bool busy(const PrinterState& s) {
    return s.get_print_job_state() == PrintJobState::PRINTING;
}'
    [ "$status" -eq 1 ]
    contains "cmp.cpp:3:" "$output"
}

@test "flags the enum subject accessor on its own" {
    run_gate subj '
void Panel::bind() {
    observe(state.get_print_state_enum_subject(), this);
}'
    [ "$status" -eq 1 ]
    contains "get_print_state_enum_subject" "$output"
}

@test "the marker covers twelve lines above the site and no further" {
    far_fixture() {
        echo '// RAW_PRINT_STATE_OK: the marker'
        for i in $(seq 1 "$1"); do echo "int x$i = 0;"; done
        echo 'bool p = s == PrintJobState::PAUSED;'
    }
    far_fixture 11 > "$FIX/near.cpp"
    run python3 "$GATE" $RULE "$FIX/near.cpp"
    [ "$status" -eq 0 ]
    far_fixture 12 > "$FIX/far.cpp"
    run python3 "$GATE" $RULE "$FIX/far.cpp"
    [ "$status" -eq 1 ]
}

@test "two sites further apart than the group gap are reported separately" {
    run_gate apart '
bool a = s == PrintJobState::STANDBY;
int b1 = 0;
int b2 = 0;
int b3 = 0;
int b4 = 0;
int b5 = 0;
int b6 = 0;
bool c = s == PrintJobState::PAUSED;'
    [ "$status" -eq 1 ]
    contains "Raw print-state reads with no stated reason: 2 > 0" "$output"
}

# ------------------------------------------------ idioms that must stay QUIET

@test "a marked read passes" {
    run_gate marked '
// RAW_PRINT_STATE_OK: the parse itself
bool p = s.get_print_job_state() == PrintJobState::PRINTING;'
    [ "$status" -eq 0 ]
}

@test "one marker covers a switch whose arms sit within the window" {
    run_gate sw '
// RAW_PRINT_STATE_OK: terminal-outcome formatting
switch (st) {
case PrintJobState::PRINTING: return "p";
case PrintJobState::PAUSED: return "z";
case PrintJobState::STANDBY: return "s";
}'
    [ "$status" -eq 0 ]
}

@test "terminal states carry no blind spot and pass unmarked" {
    run_gate terminal '
bool done = st == PrintJobState::COMPLETE || st == PrintJobState::CANCELLED ||
            st == PrintJobState::ERROR;'
    [ "$status" -eq 0 ]
}

@test "a comment naming the wire is documentation, not a read" {
    run_gate comment '
// Previously this read PrintJobState::PRINTING via get_print_job_state().
bool b = job_holds_machine(lifecycle);'
    [ "$status" -eq 0 ]
}

@test "adjacent arms count as one decision against --max-allowed" {
    run_gate grouped '
bool a = s == PrintJobState::PRINTING ||
         s == PrintJobState::PAUSED;'
    [ "$status" -eq 1 ]
    contains "1 > 0" "$output"
    run python3 "$GATE" $RULE --max-allowed=1 "$FIX/grouped.cpp"
    [ "$status" -eq 0 ]
}

@test "the derivation layer is allowlisted" {
    run python3 - <<'EOF'
import sys
sys.path.insert(0, "scripts")
import check_source_rules as g
root = g.SCRIPT_ROOT
f = root / "src/printer/print_lifecycle_state.cpp"
assert f.exists(), f
print(len(g.raw_print_state_sites(root, [f])))
EOF
    [ "$status" -eq 0 ]
    [ "$output" = "0" ]
}

@test "quality-checks runs the gate from qc_decl_ui" {
    grep -q -- '--rule raw-print-state' scripts/qc/decl_ui.sh
}
