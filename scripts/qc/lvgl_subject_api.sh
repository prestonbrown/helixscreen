# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, QC_TMP, ...) and reads the QC_TRIGGER_<gate> assignment through
# qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Deprecated LVGL subject API and by-value lv_subject_t (wall)
# ====================================================================
qc_lvgl_subject_api() {
  local EXIT_CODE=0
  local SECTION_START
  SECTION_START=$(date +%s)
  echo -n "🧷 Checking for deprecated LVGL subject storage..."

  local SUBJECT_ARGS=""
  [ "$STAGED_ONLY" = true ] && SUBJECT_ARGS="--staged-only"
  if python3 scripts/check_lvgl_subject_api.py $SUBJECT_ARGS >"$QC_TMP/lvgl_subject_api.out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    tail -1 "$QC_TMP/lvgl_subject_api.out"
  else
    section_time "$SECTION_START"
    echo ""
    cat "$QC_TMP/lvgl_subject_api.out"
    echo "   Run: python3 scripts/check_lvgl_subject_api.py --list"
    EXIT_CODE=1
  fi

  echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_lvgl_subject_api='^(src|include|tests|firmware)/.*\.(c|cc|cpp|h|hpp|inc)$|^scripts/(check_lvgl_subject_api\.py|qc/lvgl_subject_api\.sh)$'
