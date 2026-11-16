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
  local EXIT_CODE=0 out="$QC_TMP/test_budget.out"
  SECTION_START=$(date +%s)
  echo -n "🧮 Checking the branch's test budget..."
  # Branch-level by construction: a single commit may add a test that a later
  # commit on the same branch pays for, so this measures the fork point to
  # HEAD and runs at pre-push and land-gate only.
  if python3 scripts/check_test_budget.py --base auto >"$out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    tail -1 "$out"
  else
    section_time "$SECTION_START"
    echo ""
    cat "$out"
    EXIT_CODE=1
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_test_budget='^(tests|src|include)/.*\.(cpp|h)$|^scripts/check_test_budget\.py$'
