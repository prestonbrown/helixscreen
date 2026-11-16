# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Growth ratchets: file size, global access beside an injected
# dependency, static active-instance pointers (#1945)
# ====================================================================
qc_growth_ratchets() {
  local EXIT_CODE=0 out="$QC_TMP/growth_ratchets.out"
  SECTION_START=$(date +%s)
  echo -n "📏 Checking the branch's growth ratchets..."
  # Branch-level by construction: a commit may grow a file that a later commit
  # on the same branch splits, so this measures the fork point to HEAD and
  # runs at pre-push and land-gate only.
  if python3 scripts/check_growth_ratchets.py --base auto >"$out" 2>&1; then
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

QC_TRIGGER_qc_growth_ratchets='^(tests|src|include)/.*\.(cpp|h)$|^scripts/check_growth_ratchets\.py$'
