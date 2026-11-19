# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Translation packs match the YAML they are generated from
# ====================================================================
qc_translation_packs_fresh() {
  local out="$QC_TMP/translation_packs_fresh.out"
  SECTION_START=$(date +%s)
  echo -n "🌐 Checking the translation packs against translations/*.yml..."
  # A merge keeps one side's packs (merge=helix-generated), and a rebase or
  # cherry-pick can leave them behind the YAML the same way. The check reads the
  # index, never the working tree, so it judges what is being committed.
  if python3 scripts/check_translation_packs_fresh.py >"$out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    tail -1 "$out"
    echo ""
    return 0
  fi
  section_time "$SECTION_START"
  echo ""
  cat "$out"
  echo ""
  return 1
}

QC_TRIGGER_qc_translation_packs_fresh='^translations/|^ui_xml/translations/|^scripts/(generate_translations\.py|translations/|check_translation_packs_fresh\.py)'
