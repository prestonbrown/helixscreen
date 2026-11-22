# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# A header's actions slot holds header_action and nothing else
# ====================================================================
qc_header_actions() {
  local EXIT_CODE=0
  local SECTION_START
  SECTION_START=$(date +%s)
  echo -n "🔘 Checking header actions are header_action..."

  if python3 scripts/check_header_actions.py >"$QC_TMP/header_actions.out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    if grep -q '^warning:' "$QC_TMP/header_actions.out"; then
      cat "$QC_TMP/header_actions.out"
    fi
    echo "✅ Every header button is a header_action"
  else
    section_time "$SECTION_START"
    echo ""
    cat "$QC_TMP/header_actions.out"
    EXIT_CODE=1
  fi

  echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_header_actions='^ui_xml/.*\.xml$|^scripts/check_header_actions\.py$'
