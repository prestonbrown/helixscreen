# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, QC_STAGED_ALL, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# No LVGL name 9.6 deprecates
# ====================================================================
# LV_DISABLE_API_MAPPING makes a renamed LVGL name a compile error, but the generic
# flag calls (lv_obj_add_flag, lv_obj_has_flag, ...) are real functions LVGL only
# marks deprecated, and the build keeps that a warning. This fails a new one, and a
# helix::ui::obj_set_flags()/obj_has_flags() call naming a built-in flag that has its
# own setter or getter. A commit checks its staged sources; a full run, the tree.
qc_lvgl_deprecated() {
  local EXIT_CODE=0 out="$QC_TMP/lvgl_deprecated.out"
  local -a files=()
  SECTION_START=$(date +%s)
  echo -n "🧭 Checking for LVGL names 9.6 deprecates..."
  if [ "$STAGED_ONLY" = true ]; then
    while IFS= read -r f; do
      [ -f "$f" ] && files+=("$f")
    done < <(printf '%s\n' "$QC_STAGED_ALL" | grep -E '^(src|include|tests|firmware)/.*\.(c|cc|cpp|h|hpp|inc)$')
  fi
  if [ "$STAGED_ONLY" = true ] && [ ${#files[@]} -eq 0 ]; then
    : >"$out"
  elif ! python3 scripts/lvgl96_rename.py --check "${files[@]}" >"$out" 2>&1; then
    EXIT_CODE=1
  fi
  section_time $SECTION_START
  echo ""
  if [ $EXIT_CODE -ne 0 ]; then
    cat "$out"
    echo "   Rewrite with scripts/lvgl96_rename.py <file>, or see include/ui_obj_flags.h for a USER_n flag."
  else
    tail -1 "$out"
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_lvgl_deprecated='^(src|include|tests|firmware)/.*\.(c|cc|cpp|h|hpp|inc)$|^lib/(lvgl|helix-xml)$|^scripts/lvgl96_rename\.py$|^scripts/qc/lvgl_deprecated\.sh$'
