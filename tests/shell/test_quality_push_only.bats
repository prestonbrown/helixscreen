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
# records the units it was handed and exits STUB_RC.
compile_block() {
  local repo="$BATS_TEST_TMPDIR/repo"
  bash -c '
    cd "$1" || exit 99
    . "$2/scripts/qc/_lib.sh"
    EXIT_CODE=0
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
open("called.txt", "w").write("\n".join(sys.argv[1:]) + "\n")
print("summary: stub checked %d" % (len(sys.argv) - 1))
sys.exit(int(os.environ.get("STUB_RC", "0")))
PY
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
