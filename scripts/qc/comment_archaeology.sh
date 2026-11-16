# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Comment archaeology: no history phrases in added comment lines
# ====================================================================
# Reads added lines only: the pre-commit run checks the index, the full run the
# branch's committed work since its fork point. The whole-tree SHA ratchet in
# the same script is the push-time half, run from qc_decl_ui.
qc_comment_archaeology() {
  local EXIT_CODE=0 out="$QC_TMP/comment_archaeology.out" mode="--base auto"
  SECTION_START=$(date +%s)
  echo -n "🕰️  Checking added comments for history phrases..."
  [ "$STAGED_ONLY" = true ] && mode="--staged"
  # shellcheck disable=SC2086
  if python3 scripts/check_comment_archaeology.py $mode >"$out" 2>&1; then
    section_time $SECTION_START
    echo ""
    tail -1 "$out"
  else
    section_time $SECTION_START
    echo ""
    cat "$out"
    EXIT_CODE=1
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_comment_archaeology='^(src|include|tests|scripts|mk)/.*\.(cpp|h|hpp|c|py|bats|sh|mk)$|^Makefile$|check_comment_archaeology\.py$|mutate_diff\.py$'
