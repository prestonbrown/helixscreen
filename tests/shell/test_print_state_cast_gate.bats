#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_print_state_cast.py - the print-enum hand-cast
# gate.
#
# lv_subject_get_int() returns int, so a static_cast into PrintState or
# PrintJobState compiles against whichever subject the author happened to
# name - and the two enums share no numbering past index 0 (STANDBY=0
# PRINTING=1 PAUSED=2 COMPLETE=3 vs Idle=0 Preparing=1 Printing=2 Paused=3),
# so a COMPLETE job reads back Paused and a PRINTING one Preparing. That
# exact mistake shipped twice while migrating guards onto the lifecycle, and
# was invisible both times until a test caught it.
#
# Both halves of the contract are pinned here. The loud cases matter because
# the gate's search was line-oriented until 2026-08-29 while every real site
# wraps its cast across the line break - it reported 0 hand-casts while
# wrapped sites existed. The silent cases matter as much: a gate that fires
# on the typed accessors, or on an annotated read, is a gate somebody
# switches off, after which it protects nothing at all.

GATE="scripts/check_print_state_cast.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/print_state_cast"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into $FIXTURE_DIR/$1.cpp and run the gate over that one file.
run_gate() {
    local name="$1" body="$2"
    printf '%s\n' "$body" > "$FIXTURE_DIR/$name.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/$name.cpp"
    printf '%s\n' "$output" > "$FIXTURE_DIR/out.txt"
}

flagged() {
    grep -q "Hand-cast of an lv_subject int" "$FIXTURE_DIR/out.txt" \
        || fail "expected the gate to flag this site, got: $(cat "$FIXTURE_DIR/out.txt")"
}

quiet() {
    refute_grep "Hand-cast of an lv_subject int" "$FIXTURE_DIR/out.txt"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags a single-line lifecycle hand-cast" {
    run_gate same_line '
bool go() {
    return job_holds_machine(static_cast<PrintState>(lv_subject_get_int(ps.get_print_lifecycle_subject())));
}'
    flagged
}

@test "flags a lifecycle hand-cast wrapped across the line break" {
    # The shape the line-oriented search was blind to: clang-format breaks
    # right after the cast's opening paren, so the cast and the subject read
    # that names the enum land on different lines.
    run_gate wrapped '
bool go() {
    const auto lifecycle = static_cast<PrintState>(
        lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
    return job_holds_machine(lifecycle);
}'
    flagged
}

@test "flags a wrapped wire hand-cast into PrintJobState" {
    # The second enum shares the wrap blind spot; the wire read being
    # deliberate (RAW_PRINT_STATE_OK) answers WHY it reads the wire, not the
    # hand-cast, which get_print_job_state() removes.
    run_gate wrapped_job '
bool go() {
    const auto job = static_cast<PrintJobState>(
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
    return job == PrintJobState::PRINTING;
}'
    flagged
}

@test "flags a cast with a comment between the paren and the read" {
    # The codebase idiom puts the RAW_PRINT_STATE_OK comment (which the
    # sibling raw-read gate demands) inside the parens; whitespace alone
    # cannot bridge it, so four sites hid in this shape while the gate
    # stayed quiet.
    run_gate comment_wrapped '
bool go() {
    const auto job = static_cast<PrintJobState>(
        // RAW_PRINT_STATE_OK: terminal-outcome formatting is about what the
        // printer reported, and the outcome enum derives from it directly.
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
    return job == PrintJobState::PRINTING;
}'
    flagged
}

@test "flags a cast of a plain int variable in an observer lambda" {
    # The second shape: the cast lands far from the get_*_subject() call that
    # picked the enum, so a reader cannot check the pairing at a glance.
    run_gate lambda_int '
void on_state(void* p, int state_int) {
    handler(p, static_cast<PrintState>(state_int));
}'
    flagged
}

@test "flags a wrapped cast whose int variable lands on the next line" {
    run_gate wrapped_var '
void on_state(void* p) {
    handler(p, static_cast<PrintState>(
        derived_int));
}'
    flagged
}

@test "a wrapped cast is reported exactly once" {
    # Every line whose window contains the whole match could report it; the
    # hit must anchor where the match starts or the gate cries double.
    run_gate wrapped '
bool go() {
    const auto lifecycle = static_cast<PrintState>(
        lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
    return job_holds_machine(lifecycle);
}'
    local sites
    sites=$(grep -cE "^  [^ ]+:[0-9]+: " "$FIXTURE_DIR/out.txt")
    [ "$sites" -eq 1 ] \
        || fail "expected exactly 1 reported site, got $sites: $(cat "$FIXTURE_DIR/out.txt")"
}

# ------------------------------------------------ shapes that must stay QUIET

@test "silent about the typed lifecycle accessor" {
    run_gate typed_lifecycle '
bool go() {
    return job_holds_machine(get_printer_state().get_print_lifecycle());
}'
    quiet
}

@test "silent about the typed wire accessor" {
    run_gate typed_job '
bool go() {
    return get_printer_state().get_print_job_state() == PrintJobState::PRINTING;
}'
    quiet
}

@test "silent about a static_cast into an unrelated type" {
    run_gate other_cast '
int go(float v) {
    return static_cast<int>(v * 100.0f);
}'
    quiet
}

@test "honours the PRINT_STATE_CAST_OK escape hatch over a wrapped cast" {
    run_gate annotated '
bool go() {
    // PRINT_STATE_CAST_OK: the previous lifecycle has no typed accessor; the
    // prev subject is PrintState-typed by construction.
    const auto prev = static_cast<PrintState>(
        lv_subject_get_int(get_printer_state().get_print_lifecycle_prev_subject()));
    return prev == PrintState::Printing;
}'
    quiet
}

@test "silent when the subject read falls outside the cast's window" {
    # The window is deliberately small (CAST_SPAN_LINES: a cast, up to two
    # comment lines, the read): the scan must not degrade into a whole-file
    # join, which would misattribute distant statements to the cast. A read
    # pushed past the window is out of reach, and the constant grows when a
    # real wrap needs it (it already has, once, for the comment shape).
    run_gate outside_window '
bool go() {
    const auto lifecycle = static_cast<PrintState>(



        lv_subject_get_int(get_printer_state().get_print_lifecycle_subject()));
    return job_holds_machine(lifecycle);
}'
    quiet
}

# ------------------------------------------------------------- the real tree

@test "the real tree reports zero hand-casts" {
    # Guards the gate against its own bugs as much as the tree: the 2026-08-29
    # audit found the gate green while wrapped sites existed, so the clean
    # run is itself part of the contract.
    run python3 "$GATE"
    [ "$status" -eq 0 ] || fail "gate flagged the real tree: $output"
    printf '%s\n' "$output" | grep -q "0 hand-casts" \
        || fail "unexpected summary format: $output"
}
