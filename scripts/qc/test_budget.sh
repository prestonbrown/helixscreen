# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Test budget: a branch does not grow the C++ test suite
# ====================================================================
qc_test_budget() {
  local out="$QC_TMP/test_budget.out" rc=0 args=(--base auto)
  SECTION_START=$(date +%s)
  echo -n "🧮 Checking the branch's test budget..."
  # Branch-level by construction: a commit may add a test that a later commit
  # on the same branch pays for, so this measures from the fork point. A commit
  # measures to the index, so the commit being made counts. Growth is a
  # bookkeeping warning and never fails the run.
  [ "$STAGED_ONLY" != true ] || args+=(--staged)
  python3 scripts/check_test_budget.py "${args[@]}" >"$out" 2>&1 || rc=$?
  section_time "$SECTION_START"
  echo ""
  qc_ratchet_verdict "$rc" "$out" || { echo ""; return 1; }
  echo ""
  return 0
}

QC_TRIGGER_qc_test_budget='^(tests|src|include)/.*\.(cpp|h)$|^scripts/check_test_budget\.py$'
