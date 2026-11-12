# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Filament-op code asks capability questions, never the backend type
# ====================================================================
qc_filament_ops_neutral() {
  local EXIT_CODE=0
  local SECTION_START
  SECTION_START=$(date +%s)
  echo -n "🧵 Checking filament-op code for backend-type branches..."

  if python3 scripts/check_source_rules.py --rule filament-ops-neutral >"$QC_TMP/filament_ops_neutral.out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    echo "✅ Filament-op code is backend-neutral"
  else
    section_time "$SECTION_START"
    echo ""
    cat "$QC_TMP/filament_ops_neutral.out"
    echo "   See docs/devel/FILAMENT_MANAGEMENT.md (backend-neutral filament operations)."
    EXIT_CODE=1
  fi

  echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_filament_ops_neutral='^(src|include)/.*\.(cpp|h)$|^scripts/(check_source_rules|source_strip)\.py$'
