# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Growth ratchets: global access beside an injected dependency,
# static active-instance pointers (#1945)
# ====================================================================
qc_growth_ratchets() {
  local out="$QC_TMP/growth_ratchets.out" rc=0
  SECTION_START=$(date +%s)
  echo -n "📏 Checking the branch's growth ratchets..."
  # Branch-level by construction: a commit may add a global call that a later
  # commit on the same branch routes through its parameter, so this measures
  # from the fork point, at push and in make land-gate.
  python3 scripts/check_growth_ratchets.py --base auto >"$out" 2>&1 || rc=$?
  section_time "$SECTION_START"
  echo ""
  qc_ratchet_verdict "$rc" "$out" || { echo ""; return 1; }
  echo ""
  return 0
}

QC_TRIGGER_qc_growth_ratchets='^(tests|src|include)/.*\.(cpp|h)$|^scripts/check_growth_ratchets\.py$'
