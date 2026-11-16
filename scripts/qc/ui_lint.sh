# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# the ui_xml code lints (scripts/code_lint.sh --group ui)
# ====================================================================
qc_ui_lint() {
  local EXIT_CODE=0 out="$QC_TMP/ui_lint.out"
  SECTION_START=$(date +%s)
  echo -n "🧩 Checking the ui_xml code lints..."
  if scripts/code_lint.sh --group ui >"$out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    echo "✅ $(tail -1 "$out")"
  else
    section_time "$SECTION_START"
    echo ""
    cat "$out"
    echo "   Each rule is a lint_<name> in scripts/code_lint.sh; its comment says why."
    EXIT_CODE=1
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_ui_lint='^ui_xml/|^src/|^include/|^assets/filaments\.json$|^scripts/code_lint\.sh$'
