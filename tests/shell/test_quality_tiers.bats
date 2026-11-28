#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the commit hook runs. Every gate either judges each staged file on its
# own, or it needs other files to decide. scripts/quality-checks.sh names a
# gate's tier in one table, and --staged-only runs the per-file tier only: a
# whole-tree gate belongs to the full sweep, which is a push to main, release/*
# or a tag, land-gate and CI. These cases pin the table and that split.

load helpers

setup() {
  cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# The driver's lists and its decisions, loaded from the real file without
# running a sweep. $1 is STAGED_ONLY; $2 runs with them in scope. With no
# second argument the staged set is one C++ file under src/ui, which every
# per-file C++ gate's trigger wakes.
with_driver() {
  bash -c '
    STAGED_ONLY="$1"
    QC_STAGED_ALL="${QC_STAGED_ALL:-src/ui/foo.cpp}"
    eval "$(grep "^QC_ALL=" scripts/quality-checks.sh)"
    eval "$(grep -E "^QC_(PUSH_ONLY|PER_FILE|WHOLE_TREE)=" scripts/quality-checks.sh)"
    # Each gate trigger as the driver sources it, so a case wakes a gate only
    # as a commit would.
    eval "$(grep "^QC_TRIGGER_[A-Z_]*=" scripts/qc/_lib.sh)"
    eval "$(grep -h "^QC_TRIGGER_qc_" scripts/qc/*.sh)"
    eval "$(sed -n "/^qc_trigger_re() {/,/^}/p; /^QC_SKIPPED=/p; /^QC_DEFERRED=/p; /^QC_LEFT=/p; /^QC_LEFT_WHOLE=/p; /^qc_check_tiers() {/,/^}/p; /^qc_wanted() {/,/^}/p" scripts/quality-checks.sh)"
    eval "$2"
  ' _ "$1" "$2"
}

# ---------------------------------------------------------------- the table

@test "every gate has a tier, per-file or whole-tree, and only one" {
  run with_driver false 'qc_check_tiers'
  [ "$status" -eq 0 ] || fail "a gate is missing from the tiers or named twice: $output"
}

@test "the table is not vacuous: real gates sit on both sides and add up to QC_ALL" {
  run with_driver true 'echo "per-file $(echo $QC_PER_FILE | wc -w) whole-tree $(echo $QC_WHOLE_TREE | wc -w) all $(echo $QC_ALL | wc -w)"'
  [ "$status" -eq 0 ]
  # The exact counts move as gates come and go; the shape is what holds.
  local shape='per-file [1-9][0-9]* whole-tree [1-9][0-9]* all ([0-9]+)'
  [[ "$output" =~ $shape ]] || fail "$output"
  local all="${BASH_REMATCH[1]}"
  run with_driver true '[ "$(( $(echo $QC_PER_FILE | wc -w) + $(echo $QC_WHOLE_TREE | wc -w) ))" -eq "$(echo $QC_ALL | wc -w)" ]'
  [ "$status" -eq 0 ] || fail "per-file and whole-tree do not add up to QC_ALL ($all): $output"
}

@test "a gate in QC_ALL and neither tier is refused" {
  run with_driver false 'QC_ALL="$QC_ALL qc_made_up"; qc_check_tiers'
  [ "$status" -eq 1 ]
  contains "qc_made_up" "$output"
}

@test "a gate in both tiers is refused" {
  run with_driver false 'QC_PER_FILE="$QC_PER_FILE $(echo $QC_WHOLE_TREE | cut -d" " -f1)"; qc_check_tiers'
  [ "$status" -eq 1 ]
}

@test "a tier naming a gate outside QC_ALL is refused" {
  run with_driver false 'QC_WHOLE_TREE="$QC_WHOLE_TREE qc_made_up"; qc_check_tiers'
  [ "$status" -eq 1 ]
  contains "qc_made_up" "$output"
}

# ------------------------------------------------------------ what a commit runs

@test "a commit starts no whole-tree gate, and names them as the full sweep's" {
  run with_driver true 'n=0; for g in $QC_WHOLE_TREE; do qc_wanted "$g" && echo "RAN $g" || n=$((n + 1)); done; echo "not run: $n of $(echo $QC_WHOLE_TREE | wc -w)"; echo "left:$QC_LEFT_WHOLE"; echo "deferred:$QC_DEFERRED"'
  [ "$status" -eq 0 ] || fail "$output"
  lacks "RAN " "$output"
  contains "not run: 43 of 43" "$output"
  contains " qc_orphan_callbacks" "$output"
  contains " qc_feature_gates" "$output"
  contains " qc_test_lint" "$output"
  # A gate that is push-only as well is named by that rule, the deferred line.
  contains "deferred:" "$output"
  contains " qc_translation_coverage" "$output"
}

@test "a commit still runs the per-file gates an XML file wakes" {
  run with_driver true 'QC_STAGED_ALL="ui_xml/a.xml"; for g in qc_phase1 qc_phase2 qc_xml_attr qc_dup_names qc_xml_rules; do qc_wanted "$g" && echo "RAN $g"; done'
  [ "$status" -eq 0 ] || fail "$output"
  for g in qc_phase1 qc_phase2 qc_xml_attr qc_dup_names qc_xml_rules; do
    contains "RAN $g" "$output"
  done
}

@test "a commit still runs the per-file gates a C++ file wakes" {
  run with_driver true 'for g in qc_phase1 qc_phase2 qc_mem_safety qc_null_safety qc_l081 qc_net_pii qc_head_labels qc_constant_naming qc_comment_archaeology qc_lvgl_subject_api qc_spdlog_only; do qc_wanted "$g" && echo "RAN $g"; done'
  [ "$status" -eq 0 ] || fail "$output"
  for g in qc_phase1 qc_phase2 qc_mem_safety qc_null_safety qc_l081 qc_net_pii qc_head_labels qc_constant_naming qc_comment_archaeology qc_lvgl_subject_api qc_spdlog_only; do
    contains "RAN $g" "$output"
  done
}

@test "a commit still skips a per-file gate when nothing it inspects is staged" {
  run with_driver true 'QC_STAGED_ALL="docs/a.md"; qc_wanted qc_phase2 && echo "RAN qc_phase2"; echo "skipped:$QC_SKIPPED"'
  [ "$status" -eq 0 ] || fail "$output"
  lacks "RAN " "$output"
  contains "skipped: qc_phase2" "$output"
}

@test "HELIX_QC_ALL_AT_COMMIT=1 runs the whole-tree gates at commit too" {
  run with_driver true 'export HELIX_QC_ALL_AT_COMMIT=1; n=0; for g in $QC_WHOLE_TREE; do qc_wanted "$g" && n=$((n + 1)); done; echo "ran $n of $(echo $QC_WHOLE_TREE | wc -w); left:[$QC_LEFT_WHOLE]"'
  [ "$status" -eq 0 ] || fail "$output"
  # Whole-tree gates keep their triggers: a commit that stages a .cpp does not wake the XML ones.
  contains "left:[]" "$output"
  local some='ran [1-9][0-9]* of '
  [[ "$output" =~ $some ]] || fail "$output"
}

@test "a full sweep runs every gate, whatever its tier" {
  run with_driver false 'n=0; for g in $QC_PER_FILE $QC_WHOLE_TREE; do qc_wanted "$g" && n=$((n + 1)); done; echo "ran $n of $(echo $QC_PER_FILE $QC_WHOLE_TREE | wc -w); left:[$QC_LEFT_WHOLE]"'
  [ "$status" -eq 0 ] || fail "$output"
  local total
  total="$(with_driver true 'echo $(( $(echo $QC_PER_FILE | wc -w) + $(echo $QC_WHOLE_TREE | wc -w) ))')"
  contains "ran $total of $total" "$output"
  contains "left:[]" "$output"
}

# The lists and the decision function above are read on their own. This is the
# driver as git runs it: a scratch index that stages nothing, so no gate has a
# file to judge and the run is the startup, the tiers and the verdict alone.
@test "the real driver, staged-only with nothing staged, reaches its verdict and lists the whole-tree tier" {
  local idx="$BATS_TEST_TMPDIR/index"
  run bash -c '
    unset GIT_DIR GIT_WORK_TREE
    export GIT_INDEX_FILE="$1"
    git read-tree HEAD
    QC_NO_CACHE=1 scripts/quality-checks.sh --staged-only
  ' _ "$idx"
  [ "$status" -eq 0 ] || fail "$output"
  contains "Mode: Staged files only (pre-commit)" "$output"
  contains "Whole-tree checks, left to the full sweep" "$output"
  contains " qc_orphan_callbacks" "$output"
  contains "Quality checks passed" "$output"
  lacks "A gate's tier is wrong" "$output"
}

# ------------------------------------------------- where a whole-tree gate runs

# A whole-tree gate leaves the commit hook, so each one needs a full sweep
# before main. A full sweep starts every gate whatever its tier (the case
# above), so what is left to pin is that the callers run one: the driver
# without --staged-only. What pre-push itself does is
# tests/shell/test_prepush_work_branches.bats.

@test "CI's Code Quality job runs the driver in full, on main and release/*" {
  run grep -nE '^[[:space:]]*\./scripts/quality-checks\.sh[[:space:]]*$' .github/workflows/quality.yml
  [ "$status" -eq 0 ] || fail "quality.yml no longer runs ./scripts/quality-checks.sh on a line of its own: $output"
  run grep -nE 'quality-checks\.sh[[:space:]]+.*--staged-only' .github/workflows/quality.yml
  [ "$status" -eq 1 ] || fail "quality.yml narrows its sweep to the staged files: $output"
  run bash -c "sed -n '/^on:/,/^env:/p' .github/workflows/quality.yml"
  [ "$status" -eq 0 ] || fail "$output"
  contains "- main" "$output"
  contains 'release/**' "$output"
  contains "pull_request:" "$output"
}

@test "land-gate's quality step is make quality, which runs the driver in full" {
  run grep -nE '^[[:space:]]*step quality .* quality ;;' scripts/land-gate.sh
  [ "$status" -eq 0 ] || fail "land-gate no longer has a quality step that makes its quality target: $output"
  run grep -nE '^[[:space:]]*\$\(Q\)\./scripts/quality-checks\.sh[[:space:]]*$' Makefile
  [ "$status" -eq 0 ] || fail "make quality no longer runs ./scripts/quality-checks.sh in full: $output"
  run grep -nE '^quality:' Makefile
  [ "$status" -eq 0 ] || fail "$output"
}
