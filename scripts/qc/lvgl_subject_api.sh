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
# The rule is lvgl-subject-api in scripts/check_source_rules.py: at commit it
# reads the staged blobs, at push the whole tree. It prints each site as
# `file:line: call|storage` over the source line, and nothing when it passes;
# the message around the sites is printed here.
qc_lvgl_subject_api() {
  local EXIT_CODE=0
  local SECTION_START
  SECTION_START=$(date +%s)
  echo -n "🧷 Checking for deprecated LVGL subject storage..."

  local SUBJECT_ARGS=""
  [ "$STAGED_ONLY" = true ] && SUBJECT_ARGS="--staged-only"
  if python3 scripts/check_source_rules.py --rule lvgl-subject-api $SUBJECT_ARGS >"$QC_TMP/lvgl_subject_api.out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    echo "OK: no deprecated subject call or by-value lv_subject_t."
  else
    section_time "$SECTION_START"
    echo ""
    cat "$QC_TMP/lvgl_subject_api.out"
    echo ""
    echo "FAIL: an LVGL subject set up through the deprecated API or held by value."
    echo "      Declare it helix::Subject (include/core/subject.h) and call its"
    echo "      init_*()/reset() members."
    echo ""
    echo "  A test pinning LVGL's own subject API takes \`SUBJECT_API_OK: <reason>\` on or"
    echo "  directly above the line."
    echo "   Run: python3 scripts/check_source_rules.py --rule lvgl-subject-api --max-allowed 1000000"
    EXIT_CODE=1
  fi

  echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_lvgl_subject_api='^(src|include|tests|firmware)/.*\.(c|cc|cpp|h|hpp|inc)$|^scripts/(check_source_rules\.py|qc/lvgl_subject_api\.sh)$'
