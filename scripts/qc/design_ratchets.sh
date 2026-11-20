# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, QC_TMP, ...) and reads the QC_TRIGGER_<gate> assignment through
# qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Design ratchets: namespace, layering, vendor branches, declarative UI,
# swatches, LVGL 9.6 names, work markers, comment SHAs
# ====================================================================
# Warns and never fails (prestonbrown/helixscreen#2004). Each block names the
# rule, why it exists, its doc and the new file:line, and asks for the better
# design first. The driver repeats the blocks beside the final verdict
# (design_ratchets.note), so they do not scroll away with the gates' output.
# A commit reads its staged lines; every other run, the fork point onward.
qc_design_ratchets() {
  local out="$QC_TMP/design_ratchets.out" rc=0 mode=(--base auto)
  SECTION_START=$(date +%s)
  echo -n "🧭 Checking the design ratchets for new sites..."
  [ "$STAGED_ONLY" != true ] || mode=(--staged)
  python3 scripts/design_ratchets.py "${mode[@]}" >"$out" 2>&1 || rc=$?
  section_time "$SECTION_START"
  echo ""
  case "$rc" in
    0) tail -1 "$out" ;;
    "$QC_RATCHET_WARN_EXIT")
      echo "⚠️  Design ratchets grew; the blocks are repeated below the gate list."
      cp "$out" "$QC_TMP/design_ratchets.note" ;;
    *) cat "$out"; echo ""; return 1 ;;
  esac
  echo ""
  return 0
}

QC_TRIGGER_qc_design_ratchets='^(src|include|tests|firmware|scripts)/.*\.(cpp|c|cc|h|hpp|inc|mm|sh|py)$|^ui_xml/.*\.xml$|^scripts/(design_ratchets|check_(namespace_compliance|layering|vendor_branches|hand_rolled_swatch|imperative_ui|orphan_subjects|overlay_boilerplate|cached_widget_pointers|todo_markers|comment_archaeology)|lvgl96_rename)\.py$'
