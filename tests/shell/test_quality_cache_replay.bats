#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/quality-checks.sh caches a passing full sweep on the working-tree
# state and short-circuits the next identical run. That cached pass has to say
# what the full run examined: a tick that names no counts is indistinguishable
# from a gate that looked at nothing, which is the failure the counted verdicts
# exist to rule out (prestonbrown/helixscreen#1488).

load helpers

setup() {
  cd "$BATS_TEST_DIRNAME/../.." || return 1
  STAMP="$BATS_TEST_TMPDIR/stamps/abc"
  COUNTS="$BATS_TEST_TMPDIR/counts"
  export STAMP COUNTS
}

# Loads qc_stamp_write / qc_stamp_replay from the real driver and the count
# helpers from scripts/qc/_lib.sh, with QC_COUNTS pointed at the test's own file.
with_stamp_helpers() {
  bash -c '
    QC_COUNTS="$1"
    . scripts/qc/_lib.sh
    eval "$(sed -n "/^qc_stamp_write() {/,/^}/p; /^qc_stamp_replay() {/,/^}/p; /^qc_stamp_for_run() {/,/^}/p; /^qc_stamp_covering() {/,/^}/p" scripts/quality-checks.sh)"
    eval "$2"
  ' _ "$COUNTS" "$1"
}

@test "qc_count prints the verdict and records it" {
  run with_stamp_helpers 'qc_count "✅ All shell scripts pass shellcheck (78 file(s) linted)"'
  [ "$status" -eq 0 ]
  contains "78 file(s) linted" "$output"
  run cat "$COUNTS"
  contains "78 file(s) linted" "$output"
}

@test "a cached pass replays the counts of the run that made the stamp" {
  run with_stamp_helpers 'qc_note "✅ All shell scripts pass shellcheck (78 file(s) linted)"; qc_note "✅ All files properly formatted (1183 file(s) checked)"; qc_stamp_write "$STAMP" "$QC_COUNTS"; qc_stamp_replay "$STAMP"'
  [ "$status" -eq 0 ]
  contains "cached" "$output"
  contains "78 file(s) linted" "$output"
  contains "1183 file(s) checked" "$output"
  contains "QC_NO_CACHE=1" "$output"
}

@test "the stamp names the commit the full run was made on" {
  run with_stamp_helpers 'qc_stamp_write "$STAMP" "$QC_COUNTS"; head -n 1 "$STAMP"'
  [ "$status" -eq 0 ]
  head_sha="$(git rev-parse --short HEAD)"
  contains "run: $head_sha at " "$output"
}

@test "an empty stamp from an older run replays as uncounted, not as a count" {
  mkdir -p "$(dirname "$STAMP")"
  : > "$STAMP"
  run with_stamp_helpers 'qc_stamp_replay "$STAMP"'
  [ "$status" -eq 0 ]
  contains "recorded no counts" "$output"
  lacks "file(s)" "$output"
}

@test "a work-branch pass never stands in for a full sweep; a full pass covers both" {
  mkdir -p "$(dirname "$STAMP")"
  : > "$STAMP.commit-level"
  run with_stamp_helpers 'qc_stamp_covering "$STAMP"'
  [ -z "$output" ]
  run with_stamp_helpers 'HELIX_QC_SKIP_PUSH_ONLY=1 qc_stamp_covering "$STAMP"'
  [ "$output" = "$STAMP.commit-level" ]
  run with_stamp_helpers 'HELIX_QC_SKIP_PUSH_ONLY=1 qc_stamp_for_run "$STAMP"'
  [ "$output" = "$STAMP.commit-level" ]
  : > "$STAMP"
  run with_stamp_helpers 'qc_stamp_covering "$STAMP"'
  [ "$output" = "$STAMP" ]
}

@test "a gate that failed or ran long keeps its logs under a timestamped dir; a quick pass does not" {
  local tmp="$BATS_TEST_TMPDIR/qc" kept="$BATS_TEST_TMPDIR/kept"
  mkdir -p "$tmp"
  echo "gate out" > "$tmp/qc_python_tests.out"
  echo "pytest log" > "$tmp/python_tests.log"
  eval "$(sed -n "/^qc_keep_gate_log() {/,/^}/p" scripts/quality-checks.sh)"

  run qc_keep_gate_log qc_python_tests 0 2000 "$tmp" "$kept"
  [ -z "$output" ]
  [ ! -e "$kept" ]

  run qc_keep_gate_log qc_python_tests 0 400000 "$tmp" "$kept"
  contains "qc_python_tests log kept: $kept/" "$output"
  run cat "$kept"/*-python_tests/python_tests.log "$kept"/*-python_tests/summary
  contains "pytest log" "$output"
  contains "exit 0 after 400s" "$output"

  rm -rf "$kept"
  run qc_keep_gate_log qc_python_tests 1 2000 "$tmp" "$kept"
  contains "log kept" "$output"
  run cat "$kept"/*-python_tests/qc_python_tests.out
  contains "gate out" "$output"
}

