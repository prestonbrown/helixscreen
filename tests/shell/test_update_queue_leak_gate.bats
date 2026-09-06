#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_update_queue_leaks.py — the cross-test UpdateQueue
# leak ratchet (#1166).
#
# The gate's hard problem is that a leak has two names and only one of them is
# stable. The PRODUCER is the queue_update() site that left a callback behind; the
# VICTIM is whichever test ran next and drained it. Keying the baseline on the
# victim was #1170: four identical unsharded runs of one binary moved the victim
# set by 4 tests while the producer tag set did not move at all, so nightly went
# red naming an innocent test.
#
# Keying everything on the producer is not available either — 266 of ~305 reports
# are <untagged>. So the key depends on attribution quality, and these tests pin
# both halves of that bargain: the flap it must now tolerate, and the genuinely new
# leaks it must still catch. The second half is the property most at risk, because
# every step that buys stability here buys it by ignoring something.

load helpers

GATE="scripts/check_update_queue_leaks.py"
BASELINE="scripts/update_queue_leak_baseline.txt"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/uq_leaks"
    mkdir -p "$FIXTURE_DIR"
    LOG="$FIXTURE_DIR/run.log"
    BASE="$FIXTURE_DIR/baseline.txt"
    : > "$LOG"
}

# Append one [ISOLATION-LEAK] report: leak <test> <count> <producers>
leak() {
    printf '[ISOLATION-LEAK] test "%s" left %s queued UpdateQueue callback(s); discarded. Producers: %s\n' \
        "$1" "$2" "$3" >> "$LOG"
}

# Catch2 writes this to stdout; without it the gate refuses to read silence as
# "zero leaks". Every fixture log needs one.
finish_run() {
    printf 'test cases: 100 | 100 passed\nassertions: 500 | 500 passed\n' >> "$LOG"
}

run_gate() { run python3 "$GATE" --baseline "$BASE" "$LOG"; }

# --- the #1170 flap, which must now be tolerated ---------------------------

@test "a known tagged producer landing on a brand-new victim passes" {
    # The exact shape that failed nightly twice on two different innocent tests.
    # zcolor_debounce_apply is debounce-driven, so which test drains it depends on
    # wall-clock timing. The producer is baselined; the victim is deliberately not.
    leak "some innocent test that happened to drain next" 3 "Ad5xIfsBackend::zcolor_debounce_apply x3"
    finish_run
    printf 'max-untagged-callbacks: 0\ntag:Ad5xIfsBackend::zcolor_debounce_apply\n' > "$BASE"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *'no new leak keys'* ]]
}

@test "the same tagged producer moving between victims across runs is stable" {
    # Two runs, same producer, different victims, one baseline: both pass.
    printf 'max-untagged-callbacks: 0\ntag:AmsState::set_active_tool_port_present\n' > "$BASE"

    leak "victim A" 2 "AmsState::set_active_tool_port_present x2"
    finish_run
    run_gate
    [ "$status" -eq 0 ]

    : > "$LOG"
    leak "victim B" 6 "AmsState::set_active_tool_port_present x6"
    finish_run
    run_gate
    [ "$status" -eq 0 ]
}

# --- genuinely new leaks, which must still fail ----------------------------

@test "a new untagged leaker fails, keyed by the test that leaked it" {
    # The common case: most queue_update() sites pass no tag, so the victim name is
    # the only identity available. If this stopped failing, the change would have
    # blinded the gate to 84% of the population.
    leak "known leaker" 1 "<untagged>"
    leak "brand new test nobody baselined" 1 "<untagged>"
    finish_run
    printf 'max-untagged-callbacks: 2\ntest:known leaker\n' > "$BASE"
    run_gate
    [ "$status" -eq 1 ]
    contains 'test:brand new test nobody baselined' "$output"
    [[ "$output" != *'test:known leaker'* ]]
}

@test "a new tagged producer fails even when its victim is already baselined" {
    # Producer-keyed means a new queue_update() site is caught wherever it lands —
    # including inside a test that is already a known leaker, which a victim-keyed
    # baseline would have waved through.
    leak "known leaker" 1 "<untagged>"
    leak "known leaker" 2 "NewThing::brand_new_site x2"
    finish_run
    printf 'max-untagged-callbacks: 1\ntest:known leaker\n' > "$BASE"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *'tag:NewThing::brand_new_site'* ]]
}

@test "an extra untagged callback inside a baselined test trips the ceiling" {
    # The key set alone is blind here: the victim is already listed, so no new key
    # appears. This is the whole reason the baseline carries a count as well.
    leak "known leaker" 4 "<untagged> x4"
    finish_run
    printf 'max-untagged-callbacks: 3\ntest:known leaker\n' > "$BASE"
    run_gate
    [ "$status" -eq 1 ]
    contains 'exceeds the baseline ceiling' "$output"
    [[ "$output" == *'4 untagged'* ]]
}

@test "the ceiling counts untagged callbacks only, not tagged ones" {
    # Tagged callbacks are the timing-driven half (155/148/152/152 across four
    # identical runs). Counting them would put the #1170 flap straight back in.
    leak "known leaker" 1 "<untagged>"
    leak "known leaker" 40 "Ad5xIfsBackend::zcolor_debounce_apply x40"
    finish_run
    printf 'max-untagged-callbacks: 1\ntest:known leaker\ntag:Ad5xIfsBackend::zcolor_debounce_apply\n' > "$BASE"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a mixed report is keyed on both halves independently" {
    # "<untagged> x2, LedController::led_cmd_settled x2" is a real shape. The tagged
    # half must key on the tag and the untagged half on the victim, or one of the
    # two goes unwatched.
    leak "mixed victim" 4 "<untagged> x2, LedController::led_cmd_settled x2"
    finish_run

    # Baseline missing the tag half -> fails on the tag.
    printf 'max-untagged-callbacks: 2\ntest:mixed victim\n' > "$BASE"
    run_gate
    [ "$status" -eq 1 ]
    contains 'tag:LedController::led_cmd_settled' "$output"

    # Baseline missing the untagged half -> fails on the victim.
    printf 'max-untagged-callbacks: 2\ntag:LedController::led_cmd_settled\n' > "$BASE"
    run_gate
    [ "$status" -eq 1 ]
    contains 'test:mixed victim' "$output"

    # Both halves listed -> passes.
    printf 'max-untagged-callbacks: 2\ntest:mixed victim\ntag:LedController::led_cmd_settled\n' > "$BASE"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a bare producer with no xN counts as one callback" {
    # "Producers: AmsState::set_active_tool_port_present" (no suffix) means 1.
    # Mis-parsing that as 0 would let the ceiling drift upward unnoticed.
    leak "known leaker" 1 "<untagged>"
    finish_run
    printf 'max-untagged-callbacks: 0\ntest:known leaker\n' > "$BASE"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *'1 untagged callback(s) exceeds'* ]]
}

# --- the companion SITES line must stay invisible to the gate --------------

@test "the [ISOLATION-LEAK-SITES] line is ignored, not parsed as another leak" {
    # The listener prints untagged call sites on their OWN line so the report
    # above it keeps the exact shape this gate parses. If a future regex change
    # started matching it, every untagged leak would be counted twice and the
    # ceiling would need doubling to stay green — which is how a ratchet quietly
    # stops ratcheting.
    leak "known leaker" 2 "<untagged> x2"
    printf '[ISOLATION-LEAK-SITES] untagged from: observer_factory.h:371 x2\n' >> "$LOG"
    finish_run
    printf 'max-untagged-callbacks: 2\ntest:known leaker\n' > "$BASE"
    run_gate
    [ "$status" -eq 0 ]
    lacks 'ISOLATION-LEAK-SITES' "$output"
    [[ "$output" != *'observer_factory'* ]]
}

# --- the log must be a real, complete run ----------------------------------

@test "a log with no Catch2 summary is rejected rather than read as zero leaks" {
    # The listener reports on stderr and Catch2 summarizes on stdout, so a log
    # missing 2>&1 looks clean. That must not pass.
    printf 'max-untagged-callbacks: 717\ntest:known leaker\n' > "$BASE"
    run_gate
    [ "$status" -eq 2 ]
    [[ "$output" == *'no Catch2 run summary'* ]]
}

# --- baseline file handling ------------------------------------------------

@test "a pre-#1170 bare-name baseline is rejected with a regenerate hint" {
    # Bare names would read as zero known keys and fail on all ~272 at once, which
    # is a confusing way to say "this file is stale".
    leak "known leaker" 1 "<untagged>"
    finish_run
    printf '# old format\nAll panels are accessible\nknown leaker\n' > "$BASE"
    run_gate
    [ "$status" -eq 2 ]
    contains 'bare-name format' "$output"
    [[ "$output" == *'--write-baseline'* ]]
}

@test "--write-baseline round-trips: what it writes, it accepts" {
    leak "untagged leaker" 2 "<untagged> x2"
    leak "tagged victim" 3 "SomeThing::tag x3"
    finish_run
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"
    [ "$status" -eq 0 ]
    run_gate
    [ "$status" -eq 0 ]
}

@test "--write-baseline over several logs unions keys and takes the highest ceiling" {
    # Summing the ceiling across runs would write 4x the real number and neuter it.
    local l1="$FIXTURE_DIR/r1.log" l2="$FIXTURE_DIR/r2.log"
    printf '[ISOLATION-LEAK] test "only in run 1" left 2 queued UpdateQueue callback(s); discarded. Producers: <untagged> x2\ntest cases: 1 | 1 passed\n' > "$l1"
    printf '[ISOLATION-LEAK] test "only in run 2" left 5 queued UpdateQueue callback(s); discarded. Producers: <untagged> x5\ntest cases: 1 | 1 passed\n' > "$l2"

    run python3 "$GATE" --write-baseline "$BASE" "$l1" "$l2"
    [ "$status" -eq 0 ]

    run grep -c '^test:' "$BASE"
    [ "$output" -eq 2 ]                       # union, not just one run's
    run grep '^max-untagged-callbacks:' "$BASE"
    [[ "$output" == *': 5'* ]]                # max(2,5), not 2+5=7
}

# --- the committed baseline ------------------------------------------------

@test "the committed baseline carries exactly one ceiling line" {
    # The ceiling is the half of the contract the key set cannot express, so its
    # presence is load-bearing however few keys remain.
    #
    # This deliberately does NOT require a non-empty key set. It used to, back
    # when 272 keys made "at least one of each" a free assertion. The debt is now
    # worked down to zero: every leaking test was a TEST_CASE with no fixture (or
    # a hand-rolled one), so none of them drained, and putting them on
    # HelixTestFixture emptied the list. A ratchet that cannot represent its own
    # goal state would have to be loosened the moment the goal is reached.
    run grep -c '^max-untagged-callbacks: [0-9][0-9]*$' "$BASELINE"
    [ "$output" -eq 1 ]
}

@test "an empty key set is accepted, and any new leak against it fails" {
    # The pairing that makes zero-keys safe: the gate must not read "no keys" as
    # "nothing to check". Guards the regression where an emptied baseline turns
    # the ratchet into a no-op.
    printf 'max-untagged-callbacks: 0\n' > "$BASE"

    finish_run
    run_gate
    [ "$status" -eq 0 ]

    leak "a test that regressed" 1 "<untagged>"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *'test:a test that regressed'* ]]
}

@test "the committed baseline has no stray unprefixed entries" {
    # A hand-edited bare name would silently never match any key and rot there.
    run grep -vE '^(#|$|tag:|test:|max-untagged-callbacks:)' "$BASELINE"
    [ "$status" -ne 0 ]
}
