#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The commit hook (quality-checks.sh --staged-only) runs only the fast gates.
# The slow ones are listed in QC_PUSH_ONLY and deferred to the pre-push and
# land-gate runs, which gate nothing; a commit waits for its slowest gate, so one
# slow gate left in costs every commit.

load helpers

setup() {
  cd "$BATS_TEST_DIRNAME/../.." || return 1
  QC_TREE=$PWD
}

# Loads QC_PUSH_ONLY, qc_trigger_re and qc_wanted from the real driver, then runs $2
# with STAGED_ONLY=$1.
with_driver() {
  bash -c '
    STAGED_ONLY="$1"
    QC_STAGED_ALL="src/foo.cpp"
    eval "$(grep "^QC_PUSH_ONLY=" scripts/quality-checks.sh)"
    eval "$(sed -n "/^qc_trigger_re() {/,/^}/p; /^QC_SKIPPED=/p; /^QC_DEFERRED=/p; /^qc_wanted() {/,/^}/p" scripts/quality-checks.sh)"
    eval "$2"
  ' _ "$1" "$2"
}

@test "a commit defers every push-only gate and names it" {
  run with_driver true 'for g in $QC_PUSH_ONLY; do qc_wanted "$g" && echo "RAN $g"; done; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "deferred: qc_python_tests" "$output"
  contains "qc_hidden_tests" "$output"
  contains "qc_decl_ui" "$output"
}

@test "the repo-wide test and vendor lints wait for the push" {
  run with_driver true 'for g in qc_test_tautology qc_test_widget_registry qc_vendor_branches qc_hold_ends_press; do qc_wanted "$g" && echo "RAN $g"; done; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "deferred: qc_test_tautology qc_test_widget_registry qc_vendor_branches qc_hold_ends_press" "$output"
}

# A branch may add a test in one commit and pay for it in the next, so the budget
# is a branch-level verdict: measured at a single commit it would fail work in flight.
@test "the test budget waits for the push" {
  run with_driver true 'qc_wanted qc_test_budget && echo "RAN qc_test_budget"; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "qc_test_budget" "$output"
}

# The growth ratchets measure a branch the same way: a commit may grow a file that a
# later commit on the branch splits.
@test "the growth ratchets wait for the push" {
  run with_driver true 'qc_wanted qc_growth_ratchets && echo "RAN qc_growth_ratchets"; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "qc_growth_ratchets" "$output"
}

# These are slow too, but a commit that fails them does not build or load.
@test "the gates that keep a commit buildable still run at commit" {
  run with_driver true 'QC_STAGED_ALL="ui_xml/a.xml"; for g in qc_phase2 qc_xml_tools qc_xml_linter qc_design_pixels; do qc_wanted "$g" && echo "RAN $g"; done; echo "deferred:[$QC_DEFERRED]"'
  [ "$status" -eq 0 ]
  contains "RAN qc_phase2" "$output"
  contains "RAN qc_xml_tools" "$output"
  contains "RAN qc_xml_linter" "$output"
  contains "RAN qc_design_pixels" "$output"
  contains "deferred:[]" "$output"
}

@test "pre-push runs no doc-anchor scan" {
  run bash -c "grep -v '^[[:space:]]*#' .githooks/pre-push | grep -nE 'doc_anchors|check-doc-anchors'"
  [ "$status" -eq 1 ] || fail "pre-push still runs the anchor scan: $output"
}

# qc_now_ms from the real driver, with a `date` stub that prints $1 for %s%N.
now_ms_with_date() {
  local bin="$BATS_TEST_TMPDIR/bin"
  mkdir -p "$bin"
  printf '#!/bin/sh\ncase "$1" in +%%s%%N) echo "%s" ;; *) echo 1700000000 ;; esac\n' "$1" > "$bin/date"
  chmod +x "$bin/date"
  PATH="$bin:$PATH" bash -c 'eval "$(sed -n "/^qc_now_ms() {/,/^}/p" scripts/quality-checks.sh)"; qc_now_ms'
}

@test "gate times are milliseconds where date has %N" {
  run now_ms_with_date 1700000000123456789
  [ "$status" -eq 0 ]
  [ "$output" = "1700000000123" ] || fail "$output"
}

@test "gate times fall back to whole seconds where date prints a literal N" {
  run now_ms_with_date 1700000000N
  [ "$status" -eq 0 ]
  [ "$output" = "1700000000000" ] || fail "$output"
}

@test "HELIX_QC_ALL_AT_COMMIT=1 runs the push-only gates at commit" {
  run with_driver true 'export HELIX_QC_ALL_AT_COMMIT=1; qc_wanted qc_python_tests && echo RAN; echo "deferred:[$QC_DEFERRED]"'
  [ "$status" -eq 0 ]
  contains "RAN" "$output"
  contains "deferred:[]" "$output"
}

@test "a full run (pre-push, land-gate) defers nothing" {
  run with_driver false 'for g in $QC_PUSH_ONLY; do qc_wanted "$g" || echo "DEFERRED $g"; done; echo done'
  [ "$status" -eq 0 ]
  lacks "DEFERRED" "$output"
}

@test "a fast gate with no trigger still runs at commit" {
  run with_driver true 'qc_wanted qc_some_fast_gate && echo RAN'
  [ "$status" -eq 0 ]
  contains "RAN" "$output"
}

@test "every push-only gate is a gate the driver runs" {
  run bash -c '
    eval "$(grep "^QC_PUSH_ONLY=" scripts/quality-checks.sh)"
    eval "$(grep "^QC_ALL=" scripts/quality-checks.sh)"
    for g in $QC_PUSH_ONLY; do
      case " $QC_ALL " in *" $g "*) ;; *) echo "NOT IN QC_ALL: $g" ;; esac
    done; echo checked'
  [ "$status" -eq 0 ]
  lacks "NOT IN QC_ALL" "$output"
  contains "checked" "$output"
}

# Runs the driver's real fan-out over four gates on two workers with wait -n refused,
# as bash 3.2 refuses it, and logs when each gate starts and ends.
fan_out_without_wait_n() {
  bash -c '
    log="$1"
    wait() { if [ "${1:-}" = -n ]; then return 2; fi; builtin wait; }
    qc_wanted() { return 0; }
    qc_run_buffered() { echo "start $1" >> "$log"; sleep 0.3; echo "end $1" >> "$log"; }
    QC_JOBS=2
    QC_PARALLEL="a b c d"
    eval "$(sed -n "/^running=0\$/,/^wait\$/p" scripts/quality-checks.sh)"
  ' _ "$1"
}

@test "without wait -n the gates still run a full batch at a time" {
  local log="$BATS_TEST_TMPDIR/fanout.log"
  run fan_out_without_wait_n "$log"
  [ "$status" -eq 0 ] || fail "$output"
  [ "$(grep -c '^end ' "$log")" -eq 4 ] || fail "not every gate ran: $(cat "$log")"
  # Gate d starts while gate c is still running: the later gates are not run alone.
  run awk '/^start d/ {sd=NR} /^end c/ {ec=NR} END {exit !(sd && ec && sd < ec)}' "$log"
  [ "$status" -eq 0 ] || fail "gate d waited for gate c: $(cat "$log")"
}

@test "a staged header is compile-checked through one source that includes it" {
  run bash -c '. scripts/qc/_lib.sh; qc_one_includer include/utils/network_validation.h'
  [ "$status" -eq 0 ]
  [[ "$output" == src/* || "$output" == tests/* ]] || fail "not a source under src/ or tests/: $output"
  run grep -E "#include[[:space:]]+[\"<]([^\">]*/)?network_validation\\.h[\">]" "$output"
  [ "$status" -eq 0 ]
}

@test "a header nothing includes yields no unit" {
  run bash -c '. scripts/qc/_lib.sh; qc_one_includer include/helix_no_such_header_xyz.h; echo "[end]"'
  [ "$status" -eq 0 ]
  [ "$output" = "[end]" ]
}

# qc_staged_compile_units over a staged list; prints both results.
staged_units() {
  bash -c '. "$2/scripts/qc/_lib.sh"; qc_staged_compile_units "$1"; echo "UNITS<$CHECK_UNITS>"; echo "UNREACHED<$UNREACHED_HDR>"' _ "$1" "$QC_TREE"
}

@test "a commit compiles each staged source and an includer of each staged header" {
  compile_repo
  cd "$BATS_TEST_TMPDIR/repo"
  # src/a.cpp does not include b.h, so its includer is a second unit in any traversal order.
  run staged_units $'src/a.cpp\ninclude/b.h\ndocs/x.md'
  [ "$status" -eq 0 ]
  [ "$output" = $'UNITS<src/a.cpp\nsrc/c.cpp>\nUNREACHED<>' ] || fail "$output"
}

@test "a header no source includes is named, not silently dropped" {
  run staged_units 'include/helix_no_such_header_xyz.h'
  [ "$status" -eq 0 ]
  contains "UNITS<>" "$output"
  contains "UNREACHED< include/helix_no_such_header_xyz.h>" "$output"
}

@test "a commit with no source staged compiles nothing" {
  run staged_units $'docs/a.md\nui_xml/x.xml'
  [ "$status" -eq 0 ]
  contains "UNITS<>" "$output"
  contains "UNREACHED<>" "$output"
}

# Runs phase2's own compile block (from the qc_staged_compile_units call to the
# branch's closing fi) in a scratch repo whose syntax_check.py is a stub that
# records the units it was handed and exits STUB_RC, or 1 for a unit named in
# STUB_FAIL_ON. helix-claim is a stub too: `hold` logs its arguments to hold.log.
compile_block() {
  local repo="$BATS_TEST_TMPDIR/repo"
  bash -c '
    cd "$1" || exit 99
    . "$2/scripts/qc/_lib.sh"
    EXIT_CODE=0
    QC_TMP="$PWD/qc_tmp"; mkdir -p "$QC_TMP"
    SECTION_START=$(date +%s)
    eval "$(awk "/^  qc_staged_compile_units /{p=1} p{print} p&&/^  fi\$/{exit}" "$2/scripts/qc/phase2.sh")"
    echo "EXIT_CODE=$EXIT_CODE"
  ' _ "$repo" "$QC_TREE"
}

compile_repo() {
  local repo="$BATS_TEST_TMPDIR/repo"
  mkdir -p "$repo/src" "$repo/include" "$repo/scripts" "$repo/docs"
  git -C "$repo" init -q
  printf 'int a;\n' > "$repo/src/a.cpp"
  printf '#include "b.h"\n' > "$repo/src/c.cpp"
  printf 'int b;\n' > "$repo/include/b.h"
  printf 'x\n' > "$repo/docs/x.md"
  printf '[]\n' > "$repo/compile_commands.json"
  cat > "$repo/scripts/syntax_check.py" <<'PY'
import os, sys
units = sys.argv[1:]
if units[0] == "--jobs":
    open("jobs.txt", "w").write(units[1] + "\n")
    units = units[2:]
open("called.txt", "w").write("\n".join(units) + "\n")
bad = os.environ.get("STUB_FAIL_ON")
for unit in units:
    print("%s: %s (0.1s)" % (unit, "FAILED" if unit == bad else "ok"))
print("summary: stub checked %d" % len(units))
sys.exit(1 if bad in units else int(os.environ.get("STUB_RC", "0")))
PY
  cat > "$repo/scripts/helix-claim" <<'SH'
#!/bin/sh
case "$1" in
  jobs) echo 6 ;;
  hold)
    shift
    echo "$*" > hold.log
    while [ "$1" != "--" ]; do shift; done
    shift
    JOBPOOL_SLOTS=3 exec "$@"
    ;;
esac
SH
  chmod +x "$repo/scripts/helix-claim"
}

@test "phase2 hands each staged source to the syntax check and reports its summary" {
  compile_repo
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp
  run compile_block
  contains "✅ summary: stub checked 1" "$output"
  contains "EXIT_CODE=0" "$output"
  [ "$(cat "$BATS_TEST_TMPDIR/repo/called.txt")" = "src/a.cpp" ] || fail "$(cat "$BATS_TEST_TMPDIR/repo/called.txt")"
}

@test "phase2 compiles an includer of a staged header" {
  compile_repo
  git -C "$BATS_TEST_TMPDIR/repo" add include/b.h
  run compile_block
  contains "EXIT_CODE=0" "$output"
  [ "$(cat "$BATS_TEST_TMPDIR/repo/called.txt")" = "src/c.cpp" ] || fail "$(cat "$BATS_TEST_TMPDIR/repo/called.txt")"
}

@test "phase2 fails the commit when a staged source does not compile" {
  compile_repo
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp
  STUB_RC=1 run compile_block
  contains "❌ A staged source does not compile" "$output"
  contains "EXIT_CODE=1" "$output"
}

@test "phase2 compiles nothing without compile_commands.json, and says so" {
  compile_repo
  rm "$BATS_TEST_TMPDIR/repo/compile_commands.json"
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp
  run compile_block
  contains "No compile_commands.json" "$output"
  contains "EXIT_CODE=0" "$output"
  [ ! -e "$BATS_TEST_TMPDIR/repo/called.txt" ] || fail "syntax check ran"
}

@test "phase2 compiles nothing for a commit with no source" {
  compile_repo
  git -C "$BATS_TEST_TMPDIR/repo" add docs/x.md
  run compile_block
  contains "✅ No staged source to compile" "$output"
  [ ! -e "$BATS_TEST_TMPDIR/repo/called.txt" ] || fail "syntax check ran"
}

# Stages N sources under each of src/w, src/x and tests/y.
wide_repo() {
  compile_repo
  local repo="$BATS_TEST_TMPDIR/repo" d i
  for d in src/w src/x tests/y; do
    mkdir -p "$repo/$d"
    for i in $(seq 1 "$1"); do printf 'int v;\n' > "$repo/$d/u$i.cpp"; done
  done
  git -C "$repo" add src/w src/x tests/y
}

called_count() { grep -c . "$BATS_TEST_TMPDIR/repo/called.txt"; }

compile_cap() { bash -c '. "$1/scripts/qc/_lib.sh"; echo "$QC_COMPILE_CAP"' _ "$QC_TREE"; }

@test "a few staged sources compile as a plain serial run with no pool hold" {
  compile_repo
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp
  run compile_block
  contains "EXIT_CODE=0" "$output"
  [ ! -e "$BATS_TEST_TMPDIR/repo/hold.log" ] || fail "took a hold: $(cat "$BATS_TEST_TMPDIR/repo/hold.log")"
  [ ! -e "$BATS_TEST_TMPDIR/repo/jobs.txt" ] || fail "passed --jobs"
}

@test "exactly the cap of staged sources all compile, in parallel, with no warning" {
  local cap; cap=$(compile_cap)
  wide_repo $((cap / 3))
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp src/c.cpp
  local staged; staged=$(git -C "$BATS_TEST_TMPDIR/repo" diff --cached --name-only | grep -c .)
  [ "$staged" -le "$cap" ] || fail "fixture staged $staged over the cap $cap"
  run compile_block
  contains "EXIT_CODE=0" "$output"
  lacks "Wide commit" "$output"
  [ "$(called_count)" -eq "$staged" ] || fail "compiled $(called_count) of $staged"
  contains "-n 6 --" "$(cat "$BATS_TEST_TMPDIR/repo/hold.log")"
  [ "$(cat "$BATS_TEST_TMPDIR/repo/jobs.txt")" = 3 ] || fail "jobs is not the granted slots"
}

@test "over the cap a commit compiles a deterministic spread, keeps includers, and says what it skipped" {
  local cap; cap=$(compile_cap)
  wide_repo 60
  local repo="$BATS_TEST_TMPDIR/repo"
  # src/c.cpp includes b.h and sits where an even stride need not land on it.
  git -C "$repo" add include/b.h
  run compile_block
  contains "EXIT_CODE=0" "$output"
  [ "$(called_count)" -eq "$cap" ] || fail "compiled $(called_count), cap $cap"
  grep -qx 'src/c.cpp' "$repo/called.txt" || fail "the header's includer was sampled out"
  grep -q '^src/w/' "$repo/called.txt" || fail "no unit from src/w"
  grep -q '^src/x/' "$repo/called.txt" || fail "no unit from src/x"
  grep -q '^tests/y/' "$repo/called.txt" || fail "no unit from tests/y"
  contains "Wide commit: compiled $cap of 181 units" "$output"
  contains "$((181 - cap)) were NOT compiled here" "$output"
  grep -q "NOT compiled here" "$repo/qc_tmp/compile_sample.note" || fail "no note for the driver's verdict"

  local first; first=$(cat "$repo/called.txt")
  run compile_block
  [ "$(cat "$repo/called.txt")" = "$first" ] || fail "the sample moved between runs"
}

@test "header includers past the cap all compile" {
  run bash -c '
    . "$1/scripts/qc/_lib.sh"
    CHECK_UNITS=$(printf "src/%s.cpp\n" a b c d e f)
    QC_INCLUDER_UNITS=$(printf "src/%s.cpp\n" a c e)
    qc_sample_compile_units 2
    echo "$CHECK_UNITS" | tr "\n" " "; echo "skipped=$COMPILE_SKIPPED"
  ' _ "$QC_TREE"
  [ "$output" = "src/a.cpp src/c.cpp src/e.cpp skipped=3" ] || fail "$output"
}

@test "a broken source in the sample fails the commit and its error survives the ok lines" {
  wide_repo 60
  local repo="$BATS_TEST_TMPDIR/repo" broken
  run compile_block
  broken=$(tail -1 "$repo/called.txt")
  STUB_FAIL_ON="$broken" run compile_block
  contains "❌ A staged source does not compile" "$output"
  contains "$broken: FAILED" "$output"
  contains "EXIT_CODE=1" "$output"
  contains "Wide commit" "$output"
  lacks "u1.cpp: ok" "$output"
}

@test "a failed compile keeps its whole log, in a place no other session writes" {
  wide_repo 60
  local repo="$BATS_TEST_TMPDIR/repo" broken cap
  cap=$(compile_cap)
  run compile_block
  broken=$(tail -1 "$repo/called.txt")
  STUB_FAIL_ON="$broken" run compile_block
  local kept="$BATS_TEST_TMPDIR/kept"
  run bash -c '
    eval "$(sed -n "/^qc_keep_gate_log() {/,/^}/p" "$1/scripts/quality-checks.sh")"
    : > "$2/qc_tmp/qc_phase2.out"
    qc_keep_gate_log qc_phase2 1 1000 "$2/qc_tmp" "$3"
  ' _ "$QC_TREE" "$repo" "$kept"
  [ "$(grep -c ': ok (' "$kept"/*-phase2/phase2.syntax.log)" -eq $((cap - 1)) ] || fail "$(ls -R "$kept")"
  run grep -n '/tmp/qc_syntax' "$QC_TREE/scripts/qc/phase2.sh"
  [ "$status" -eq 1 ] || fail "$output"
}

@test "HELIX_QC_ALL_AT_COMMIT compiles every unit of a wide commit" {
  wide_repo 60
  HELIX_QC_ALL_AT_COMMIT=1 run compile_block
  contains "EXIT_CODE=0" "$output"
  lacks "Wide commit" "$output"
  [ "$(called_count)" -eq 180 ] || fail "compiled $(called_count)"
}

@test "a wide commit with no compile database claims no sample" {
  wide_repo 60
  local repo="$BATS_TEST_TMPDIR/repo"
  rm "$repo/compile_commands.json"
  run compile_block
  contains "No compile_commands.json" "$output"
  lacks "Wide commit" "$output"
  [ ! -e "$repo/qc_tmp/compile_sample.note" ] || fail "the note claims units compiled"
}

# Each fake compile waits for the other to start, so a run that compiles one at a
# time fails rather than merely running slower.
syntax_repo() {
  local repo="$BATS_TEST_TMPDIR/sc"
  mkdir -p "$repo/src"
  git -C "$repo" init -q
  cp "$QC_TREE/VERSION.txt" "$repo/VERSION.txt"
  printf 'int a;\n' > "$repo/src/a.cpp"
  printf 'int b;\n' > "$repo/src/b.cpp"
  cat > "$repo/cc.sh" <<'SH'
#!/bin/sh
for last; do :; done
name=$(basename "$last" .cpp)
: > "started.$name"
other=a; [ "$name" = a ] && other=b
i=0
while [ ! -e "started.$other" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || { echo "error: $name saw no peer" >&2; exit 1; }
  sleep 0.1
done
[ "$name" != b ] || [ -z "${CC_FAIL_B:-}" ] || { echo "error: b is broken" >&2; exit 1; }
SH
  chmod +x "$repo/cc.sh"
  local version; version=$(cat "$QC_TREE/VERSION.txt")
  printf '[{"directory": "%s", "file": "src/a.cpp", "command": "./cc.sh -DHELIX_VERSION=\\"%s\\" -c src/a.cpp"},{"directory": "%s", "file": "src/b.cpp", "command": "./cc.sh -DHELIX_VERSION=\\"%s\\" -c src/b.cpp"}]\n' \
    "$repo" "$version" "$repo" "$version" > "$repo/compile_commands.json"
}

@test "syntax_check --jobs compiles at once and reports in the order given" {
  syntax_repo
  run bash -c 'cd "$1" && python3 "$2/scripts/syntax_check.py" --jobs 2 src/b.cpp src/a.cpp' _ "$BATS_TEST_TMPDIR/sc" "$QC_TREE"
  [ "$status" -eq 0 ] || fail "$output"
  [ "${lines[0]%%:*}" = src/b.cpp ] || fail "$output"
  [ "${lines[1]%%:*}" = src/a.cpp ] || fail "$output"
  contains "summary: 2 compiled, 0 skipped, 0 failed" "$output"
}

@test "syntax_check --jobs still fails on one broken unit and prints its error" {
  syntax_repo
  CC_FAIL_B=1 run bash -c 'cd "$1" && python3 "$2/scripts/syntax_check.py" --jobs 2 src/a.cpp src/b.cpp 2>&1' _ "$BATS_TEST_TMPDIR/sc" "$QC_TREE"
  [ "$status" -eq 1 ] || fail "$output"
  contains "src/b.cpp: FAILED" "$output"
  contains "error: b is broken" "$output"
  contains "summary: 2 compiled, 0 skipped, 1 failed" "$output"
}
