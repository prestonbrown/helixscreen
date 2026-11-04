# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# the source-tree code lints (scripts/code_lint.sh --group src)
# ====================================================================
qc_code_lint() {
  local EXIT_CODE=0 out="$QC_TMP/code_lint.out"
  SECTION_START=$(date +%s)
  echo -n "🧭 Checking the source-tree code lints..."
  if scripts/code_lint.sh --group src >"$out" 2>&1; then
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

QC_TRIGGER_qc_code_lint='\.(cpp|c|cc|h|hpp)$|\.md$|^lib/helix-xml$|^scripts/(code_lint\.sh|check_(getenv_setenv|init_list_ternary)\.py|check_tool_labels\.sh)$'
