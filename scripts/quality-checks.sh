#!/bin/bash
# SPDX-FileCopyrightText: 2024 Patrick Brown <opensource@pbdigital.org>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Quality checks script - single source of truth for pre-commit and CI
# Usage:
#   ./scripts/quality-checks.sh                      # Check all files (for CI)
#   ./scripts/quality-checks.sh --staged-only        # Check only staged files (for pre-commit)
#   ./scripts/quality-checks.sh --auto-fix           # Auto-fix formatting issues
#   ./scripts/quality-checks.sh --staged-only --auto-fix  # Fix staged files

set -e

# Parse arguments
STAGED_ONLY=false
AUTO_FIX=false
for arg in "$@"; do
  case "$arg" in
    --staged-only) STAGED_ONLY=true ;;
    --auto-fix) AUTO_FIX=true ;;
  esac
done

# Change to repo root
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT"

EXIT_CODE=0
SCRIPT_START=$(date +%s)

# Each gate is one file, scripts/qc/<gate>.sh: its qc_<gate> function and the
# QC_TRIGGER_qc_<gate> path regex that decides whether --staged-only runs it.
# Helpers they share live in scripts/qc/_lib.sh.
# shellcheck source=scripts/qc/_lib.sh
. scripts/qc/_lib.sh

echo "🔍 Running quality checks..."
if [ "$STAGED_ONLY" = true ]; then
  echo "   Mode: Staged files only (pre-commit)"
else
  echo "   Mode: All files (CI)"
fi
echo ""

# The copyright-header check runs inline, before any gate: it sets FILES,
# which qc_phase1 and qc_phase2 read.
# shellcheck source=scripts/qc/copyright.sh
. scripts/qc/copyright.sh

# Every staged path, including deletions - a removed .cpp can invalidate a doc
# that cites it, so the doc gate has to see D as well as ACMR.
QC_STAGED_ALL=""
if [ "$STAGED_ONLY" = true ]; then
  QC_STAGED_ALL="$(git diff --cached --name-only --diff-filter=ACMRD 2>/dev/null || true)"
fi

# Resolved here rather than inside a check: the checks run as separate
# subshells, so a variable one of them assigns is invisible to the next.
# VENV_PYTHON was set in the formatting section and read by the translation
# one; TRANS_FMT_PY was set there and read by the base-locale one.
VENV_PYTHON=".venv/bin/python"
TRANS_FMT_PY="${VENV_PYTHON:-python3}"
[ -x "$TRANS_FMT_PY" ] || TRANS_FMT_PY=python3

# ====================================================================
# Parallel driver
# ====================================================================
# The checks are independent greps and linters and the script ran strictly
# serially: 67s wall for 54s user + 15s sys, i.e. one core of 32.
#
# Two sections write to the tree; one of them only under --auto-fix:
#   qc_phase2     clang-format -i + git add   (checks only, without --auto-fix)
#   qc_xml_linter make regen-xml-schema       (always regenerates schema.json)
# Those run alone, first - a formatter rewriting a file while another check
# greps it is a race. Everything else fans out over $QC_JOBS workers.
#
# Output is buffered per section and replayed in declaration order, so the
# transcript matches the serial one apart from timings.
# Result stamp - full runs only.
#
# Path gating already handles "nothing relevant changed" for pre-commit. What is
# left is redoing an identical full sweep: pre-push straight after a manual run,
# or re-pushing with nothing touched in between. The stamp is the whole working
# state (HEAD, staged diff, unstaged diff, untracked listing), so any edit
# invalidates it. Set QC_NO_CACHE=1 to force, and note it only ever short-circuits
# a run that previously PASSED - failures are never cached.
QC_STAMP_DIR="build/.qc-stamps"
qc_state_hash() {
  {
    git rev-parse HEAD 2>/dev/null || echo no-head
    git diff --no-ext-diff 2>/dev/null || true
    git diff --cached --no-ext-diff 2>/dev/null || true
    # Names only: an untracked file's contents are not hashed, so a scratch file
    # edited in place will not invalidate the stamp. Tracked work always does.
    git ls-files --others --exclude-standard 2>/dev/null | sort || true
  } | sha256sum 2>/dev/null | cut -d' ' -f1
}
# A cached pass replays the counts the full run recorded (qc_count, _lib.sh), so a
# green that examined nothing cannot hide behind the cache: the stamp's first
# line names the run, the rest are its counted verdicts.
qc_stamp_write() {
  # $1 = stamp path, $2 = counts file
  mkdir -p "$(dirname "$1")" 2>/dev/null || true
  {
    echo "run: $(git rev-parse --short HEAD 2>/dev/null || echo no-head) at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    [ -f "$2" ] && cat "$2"
  } > "$1" 2>/dev/null || true
}
qc_stamp_replay() {
  # $1 = stamp path
  local first
  first="$(head -n 1 "$1" 2>/dev/null)"
  case "$first" in
    run:*)
      echo "✅ Quality checks passed! (cached: working tree unchanged since the full ${first#run: })"
      tail -n +2 "$1" | sed 's/^/   /'
      ;;
    *)
      echo "✅ Quality checks passed! (cached - working tree unchanged since the last full run)"
      echo "   (that run recorded no counts; QC_NO_CACHE=1 runs a counted sweep)"
      ;;
  esac
  echo "   Force a re-run with QC_NO_CACHE=1"
}
QC_STAMP=""
if [ "$STAGED_ONLY" != true ] && [ -z "${QC_NO_CACHE:-}" ]; then
  QC_STAMP="$QC_STAMP_DIR/$(qc_state_hash)"
  if [ -n "$QC_STAMP" ] && [ -f "$QC_STAMP" ]; then
    qc_stamp_replay "$QC_STAMP"
    exit 0
  fi
fi

QC_TMP="$(mktemp -d)"
trap 'rm -rf "$QC_TMP"' EXIT
# Verdicts that carry a count are recorded here as well as printed, so a cached
# pass can replay what the full run examined. Gates run in parallel subshells,
# and a one-line append is atomic, so one file serves them all.
QC_COUNTS="$QC_TMP/counts"
QC_JOBS="${QC_JOBS:-$(nproc 2>/dev/null || echo 4)}"

# Milliseconds where date has %N; BSD date (macOS) prints a literal N, so it
# falls back to whole seconds there.
qc_now_ms() {
  local ns
  ns=$(date +%s%N)
  case "$ns" in
    *N) echo $(( $(date +%s) * 1000 )) ;;
    *) echo $(( ns / 1000000 )) ;;
  esac
}

qc_run_buffered() {
  local fn="$1" t0 t1
  t0=$(qc_now_ms)
  set +e
  "$fn" > "$QC_TMP/$fn.out" 2>&1
  echo $? > "$QC_TMP/$fn.rc"
  set -e
  t1=$(qc_now_ms)
  echo $((t1 - t0)) > "$QC_TMP/$fn.ms"
}

# qc_xml_linter always regenerates the schema; qc_phase2 only rewrites files
# when asked to fix them.
QC_SERIAL="qc_xml_tools qc_xml_linter"
if [ "$AUTO_FIX" = true ]; then QC_SERIAL="$QC_SERIAL qc_phase2"; fi

QC_ALL="qc_phase1 qc_xml_tools qc_xml_const qc_xml_attr qc_dup_names qc_xml_linter qc_theme_artifacts qc_toolhead_census qc_xml_subtests qc_hidden_tests qc_overlay_width qc_icon_names qc_design_pixels qc_esp32_app_srcs qc_esp32_compile qc_esp32_xml_bindings qc_phase2 qc_icon_font qc_mdi_codepoints qc_todo_markers qc_mem_safety qc_null_safety qc_l081 qc_vendor_branches qc_hand_rolled_swatch qc_filament_ops_neutral qc_hold_ends_press qc_net_pii qc_decl_ui qc_namespace qc_constant_naming qc_spdlog_only qc_design_tokens qc_test_mirrors qc_test_budget qc_growth_ratchets qc_test_tautology qc_test_widget_registry qc_xml_create_registered qc_doc_refs qc_lvgl_event_codes qc_translation_fmt qc_base_locale qc_translation_coverage qc_plugin_i18n_shadow qc_cjk_fonts qc_shellcheck qc_installer_reachability qc_patch_drift qc_link_parity qc_feature_gates qc_workflow_submodules qc_ams_xml_mirror qc_bats_inert qc_python_tests"

# Push-only gates: the repo-wide scans that would set a commit's wall clock
# (QC_PROFILE=1 prints each gate's time). A commit waits for its slowest gate,
# so one of these alone costs every commit seconds to minutes; pre-push and
# land-gate run them ungated before anything reaches main.
# HELIX_QC_ALL_AT_COMMIT=1 runs them at commit too.
#
# Only a lint belongs here: a failure it finds is a policy breach, not a commit
# that fails to build or load. The slow gates a commit keeps are that kind:
# qc_phase2 (formatting, the staged compile), qc_xml_tools and qc_xml_linter
# (the XML the app loads at runtime) and qc_design_pixels (layout-variant
# parity, without which a breakpoint loses the widgets its code looks up).
QC_PUSH_ONLY="qc_python_tests qc_hidden_tests qc_decl_ui qc_translation_coverage qc_hand_rolled_swatch qc_esp32_xml_bindings qc_esp32_app_srcs qc_esp32_compile qc_translation_fmt qc_namespace qc_test_tautology qc_test_widget_registry qc_vendor_branches qc_hold_ends_press qc_test_budget qc_growth_ratchets"

for fn in $QC_ALL; do
  # shellcheck source=/dev/null
  . "scripts/qc/${fn#qc_}.sh"
done

QC_PARALLEL=""
for fn in $QC_ALL; do
  case " $QC_SERIAL " in *" $fn "*) ;; *) QC_PARALLEL="$QC_PARALLEL $fn" ;; esac
done

# Path gating - pre-commit only.
#
# A one-file commit paid for every repo-wide gate: the declarative-UI scan alone
# is ~9s and greps all of src/ even when you touched a .md. In --staged-only we
# skip a check when nothing it inspects was staged. The full run (pre-push, CI)
# gates nothing, so this can only defer work to the push, never drop it.
#
# A gate that sets no QC_TRIGGER_<gate> always runs, unless it is push-only.
qc_trigger_re() {
  local var="QC_TRIGGER_$1"
  echo "${!var:-}"
}

QC_SKIPPED=""
QC_DEFERRED=""
qc_wanted() {
  local re
  [ "$STAGED_ONLY" = true ] || return 0
  case " $QC_PUSH_ONLY " in
    *" $1 "*)
      if [ "${HELIX_QC_ALL_AT_COMMIT:-0}" != 1 ]; then
        QC_DEFERRED="$QC_DEFERRED $1"
        return 1
      fi
      ;;
  esac
  re="$(qc_trigger_re "$1")"
  [ -n "$re" ] || return 0
  if printf '%s\n' "$QC_STAGED_ALL" | grep -qE "$re"; then
    return 0
  fi
  QC_SKIPPED="$QC_SKIPPED $1"
  return 1
}

for fn in $QC_SERIAL; do
  if qc_wanted "$fn"; then qc_run_buffered "$fn"; fi
done

running=0
for fn in $QC_PARALLEL; do
  qc_wanted "$fn" || continue
  qc_run_buffered "$fn" &
  running=$((running + 1))
  if [ "$running" -ge "$QC_JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running - 1)); fi
done
wait

for fn in $QC_ALL; do
  [ -f "$QC_TMP/$fn.out" ] && cat "$QC_TMP/$fn.out"
  rc=$(cat "$QC_TMP/$fn.rc" 2>/dev/null || echo 0)
  [ "$rc" != "0" ] && EXIT_CODE=1
done

if [ -n "$QC_DEFERRED" ]; then
  echo ""
  echo "⏩ Deferred to pre-push (slow; HELIX_QC_ALL_AT_COMMIT=1 runs them now):$QC_DEFERRED"
fi
if [ -n "$QC_SKIPPED" ]; then
  echo ""
  echo "⏭️  Skipped (nothing staged that they inspect):$QC_SKIPPED"
  echo "   The pre-push hook runs all of them ungated."
fi

# Every gate that ran, slowest first. The transcript cannot time them: gates run
# in parallel and their output is replayed only once all have finished.
if [ -n "${QC_PROFILE:-}" ]; then
  echo ""; echo "gate times (ms), slowest first:"
  for fn in $QC_ALL; do
    [ -f "$QC_TMP/$fn.ms" ] && printf "%7d  %s\n" "$(cat "$QC_TMP/$fn.ms")" "$fn"
  done | sort -rn
fi
true

# ====================================================================
# Final Result
# ====================================================================
SCRIPT_END=$(date +%s)
TOTAL_SEC=$((SCRIPT_END - SCRIPT_START))

if [ $EXIT_CODE -eq 0 ]; then
  # Only a pass is cached; a failure must always re-run.
  if [ -n "$QC_STAMP" ]; then
    qc_stamp_write "$QC_STAMP" "$QC_COUNTS"
  fi
  echo "✅ Quality checks passed! (${TOTAL_SEC}s total)"
  exit 0
else
  echo "❌ Quality checks failed! (${TOTAL_SEC}s total)"
  exit 1
fi
