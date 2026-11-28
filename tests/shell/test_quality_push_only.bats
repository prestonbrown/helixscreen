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
    eval "$(grep -E "^QC_(PUSH_ONLY|WHOLE_TREE)=" scripts/quality-checks.sh)"
    eval "$(sed -n "/^qc_trigger_re() {/,/^}/p; /^QC_SKIPPED=/p; /^QC_DEFERRED=/p; /^QC_LEFT_WHOLE=/p; /^qc_wanted() {/,/^}/p" scripts/quality-checks.sh)"
    eval "$2"
  ' _ "$1" "$2"
}

@test "a commit defers every push-only gate and names it" {
  run with_driver true 'for g in $QC_PUSH_ONLY; do qc_wanted "$g" && echo "RAN $g"; done; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "deferred: qc_decl_ui" "$output"
  contains "qc_esp32_app_srcs" "$output"
}

@test "the repo-wide test lints wait for the push" {
  run with_driver true 'for g in qc_test_tautology qc_test_widget_registry qc_hold_ends_press; do qc_wanted "$g" && echo "RAN $g"; done; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "deferred: qc_test_tautology qc_test_widget_registry qc_hold_ends_press" "$output"
}

# The growth ratchets measure a branch: a commit may add a global call that a later
# commit on the branch routes through its parameter.
@test "the growth ratchets wait for the push" {
  run with_driver true 'qc_wanted qc_growth_ratchets && echo "RAN qc_growth_ratchets"; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ]
  lacks "RAN " "$output"
  contains "qc_growth_ratchets" "$output"
}

# Formatting judges each staged file on its own, so a commit meets it at commit.
# The XML linter, its tool build and the design-token checks need the whole tree,
# and are the full sweep's.
@test "formatting still runs at commit; the whole-tree ones wait" {
  run with_driver true 'QC_STAGED_ALL="ui_xml/a.xml"; for g in qc_phase2 qc_xml_tools qc_xml_linter qc_design_pixels; do qc_wanted "$g" && echo "RAN $g"; done; echo "deferred:[$QC_DEFERRED]"; echo "left:$QC_LEFT_WHOLE"
'
  [ "$status" -eq 0 ]
  contains "RAN qc_phase2" "$output"
  lacks "RAN qc_xml_tools" "$output"
  lacks "RAN qc_xml_linter" "$output"
  lacks "RAN qc_design_pixels" "$output"
  contains "deferred:[]" "$output"
  contains " qc_xml_tools" "$output"
  contains " qc_xml_linter" "$output"
  contains " qc_design_pixels" "$output"
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
  run with_driver true 'export HELIX_QC_ALL_AT_COMMIT=1; qc_wanted qc_decl_ui && echo RAN; echo "deferred:[$QC_DEFERRED]"'
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

@test "the compile check takes each staged source and an includer of each staged header" {
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

@test "a staged list with no source yields no unit" {
  run staged_units $'docs/a.md\nui_xml/x.xml'
  [ "$status" -eq 0 ]
  contains "UNITS<>" "$output"
  contains "UNREACHED<>" "$output"
}

# Runs phase2's own compile block (from the qc_staged_compile_units call to the
# branch's closing fi) in a scratch repo whose syntax_check.py is a stub that
# records the units it was handed and exits STUB_RC, or 1 for a unit named in
# STUB_FAIL_ON, or exits STUB_DIE_RC before printing anything. helix-claim is a
# stub too: `hold` logs its arguments to hold.log.
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
if os.environ.get("STUB_DIE_RC"):
    sys.exit(int(os.environ["STUB_DIE_RC"]))
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
  # qc_phase2 resolves the pinned clang-format before anything else: a stub that
  # reports the pin satisfies it, and with no .clang-format here it formats nothing.
  # `make` is a stub too: HELIX_QC_FULL_BUILD=1 builds through it.
  printf 'clang-format==18.1.8\n' > "$repo/requirements.txt"
  mkdir -p "$repo/bin"
  printf '#!/bin/sh\necho "clang-format version 18.1.8"\n' > "$repo/bin/clang-format"
  printf '#!/bin/sh\necho "$*" > built.txt\n' > "$repo/bin/make"
  chmod +x "$repo/bin/clang-format" "$repo/bin/make"
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

# Runs qc_phase2 whole, as the driver calls it, in a repo with a compile database
# and a staged source: everything before the compile block runs too.
phase2_gate() {
  local repo="$BATS_TEST_TMPDIR/repo"
  bash -c '
    cd "$1" || exit 99
    PATH="$1/bin:$PATH" CLANG_FORMAT="$1/bin/clang-format"
    STAGED_ONLY=true AUTO_FIX=false EXIT_CODE=0 REPO_ROOT="$PWD" QC_JOBS=2
    QC_TMP="$PWD/qc_tmp"; mkdir -p "$QC_TMP"
    QC_COUNTS="$QC_TMP/counts"; : > "$QC_COUNTS"
    QC_STAGED_DIR=""
    FILES="$(git diff --cached --name-only --diff-filter=ACM | grep -E "\\.(cpp|c|h|mm)$")"
    . "$2/scripts/qc/_lib.sh" && . "$2/scripts/qc/phase2.sh" && qc_phase2
  ' _ "$repo" "$QC_TREE"
}

@test "a commit compiles nothing; HELIX_QC_ALL_AT_COMMIT=1 compiles the staged sources" {
  compile_repo
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp
  local repo="$BATS_TEST_TMPDIR/repo"

  run phase2_gate
  [ "$status" -eq 0 ] || fail "$output"
  lacks "Verifying staged sources compile" "$output"
  [ ! -e "$repo/called.txt" ] || fail "a commit compiled: $(cat "$repo/called.txt")"

  HELIX_QC_ALL_AT_COMMIT=1 run phase2_gate
  [ "$status" -eq 0 ] || fail "$output"
  contains "summary: stub checked 1" "$output"
  [ "$(cat "$repo/called.txt")" = "src/a.cpp" ] || fail "$(cat "$repo/called.txt")"
}

@test "HELIX_QC_FULL_BUILD=1 builds the app in the hook instead of a syntax check" {
  compile_repo
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp
  local repo="$BATS_TEST_TMPDIR/repo" jobs
  jobs=$(cd "$repo" && bash -c '. "$1/scripts/qc/_lib.sh"; qc_build_jobs' _ "$QC_TREE")

  HELIX_QC_FULL_BUILD=1 run phase2_gate
  [ "$status" -eq 0 ] || fail "$output"
  contains "✅ Build successful" "$output"
  [ "$(cat "$repo/built.txt")" = "SKIP_COMPILE_COMMANDS=1 -j$jobs" ] || fail "$(cat "$repo/built.txt")"
  [ ! -e "$repo/called.txt" ] || fail "it syntax-checked instead of building"
}

@test "phase2 compiles nothing when no source is staged" {
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

compile_serial_max() { bash -c '. "$1/scripts/qc/_lib.sh"; echo "$QC_COMPILE_SERIAL_MAX"' _ "$QC_TREE"; }
compile_jobs() { bash -c '. "$1/scripts/qc/_lib.sh"; echo "$QC_COMPILE_JOBS"' _ "$QC_TREE"; }

@test "up to the serial width compile one at a time, with no pool hold" {
  local width; width=$(compile_serial_max)
  wide_repo $((width / 3))
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp
  local staged; staged=$(git -C "$BATS_TEST_TMPDIR/repo" diff --cached --name-only | grep -c .)
  [ "$staged" -eq "$width" ] || fail "fixture staged $staged, the serial width is $width"
  run compile_block
  contains "EXIT_CODE=0" "$output"
  [ "$(called_count)" -eq "$staged" ] || fail "compiled $(called_count) of $staged"
  [ ! -e "$BATS_TEST_TMPDIR/repo/hold.log" ] || fail "took a hold: $(cat "$BATS_TEST_TMPDIR/repo/hold.log")"
  [ ! -e "$BATS_TEST_TMPDIR/repo/jobs.txt" ] || fail "passed --jobs"
}

@test "above the serial width every unit compiles, in parallel at the fixed width" {
  local width; width=$(compile_serial_max)
  wide_repo $((width / 3))
  git -C "$BATS_TEST_TMPDIR/repo" add src/a.cpp src/c.cpp
  local staged; staged=$(git -C "$BATS_TEST_TMPDIR/repo" diff --cached --name-only | grep -c .)
  [ "$staged" -gt "$width" ] || fail "fixture staged $staged, no more than the serial width $width"
  run compile_block
  contains "EXIT_CODE=0" "$output"
  [ "$(called_count)" -eq "$staged" ] || fail "compiled $(called_count) of $staged"
  [ "$(cat "$BATS_TEST_TMPDIR/repo/jobs.txt")" = "$(compile_jobs)" ] || fail "not the fixed width: $(cat "$BATS_TEST_TMPDIR/repo/jobs.txt")"
}

@test "a broken source in a parallel run fails the commit and its error survives the ok lines" {
  wide_repo 60
  local repo="$BATS_TEST_TMPDIR/repo" broken
  run compile_block
  broken=$(tail -1 "$repo/called.txt")
  STUB_FAIL_ON="$broken" run compile_block
  contains "❌ A staged source does not compile" "$output"
  contains "$broken: FAILED" "$output"
  contains "EXIT_CODE=1" "$output"
  lacks "u1.cpp: ok" "$output"
}

@test "a compile check that dies with no verdict fails the commit without blaming a source" {
  wide_repo 60
  STUB_DIE_RC=3 run compile_block
  contains "❌ The compile check stopped with exit 3" "$output"
  lacks "A staged source does not compile" "$output"
  contains "EXIT_CODE=1" "$output"
}

@test "a failed compile keeps its whole log, in a place no other session writes" {
  wide_repo 60
  local repo="$BATS_TEST_TMPDIR/repo" broken
  run compile_block
  broken=$(tail -1 "$repo/called.txt")
  STUB_FAIL_ON="$broken" run compile_block
  local kept="$BATS_TEST_TMPDIR/kept"
  run bash -c '
    eval "$(sed -n "/^qc_keep_gate_log() {/,/^}/p" "$1/scripts/quality-checks.sh")"
    : > "$2/qc_tmp/qc_phase2.out"
    qc_keep_gate_log qc_phase2 1 1000 "$2/qc_tmp" "$3"
  ' _ "$QC_TREE" "$repo" "$kept"
  # 180 units ran: the broken one and the 179 that passed.
  [ "$(grep -c ': ok (' "$kept"/*-phase2/phase2.syntax.log)" -eq 179 ] || fail "$(ls -R "$kept")"
  run grep -n '/tmp/qc_syntax' "$QC_TREE/scripts/qc/phase2.sh"
  [ "$status" -eq 1 ] || fail "$output"
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

# A design ratchet judges the lines a change adds, so it is a per-file gate: its
# warning has to meet the author while the commit body is being written.
@test "the design ratchets run at commit: their warning belongs on the commit that adds the site" {
  run with_driver true 'qc_wanted qc_design_ratchets && echo RAN; echo "deferred:[$QC_DEFERRED]"; echo "left:[$QC_LEFT_WHOLE]"
'
  [ "$status" -eq 0 ]
  contains "RAN" "$output"
  contains "deferred:[]" "$output"
  contains "left:[]" "$output"
}
