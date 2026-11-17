# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Locale files in the shape Weblate reads and writes
# ====================================================================
qc_translation_files() {
  local EXIT_CODE=0
# Weblate re-serializes a whole locale file on every commit. A file in any other
# shape gets reflowed wholesale, a changed locale: field misroutes
# translation-sync, and a plural mapping Weblate edited collapses to one flat
# string (docs/devel/TRANSLATION_SYSTEM.md, "Weblate").
SECTION_START=$(date +%s)
echo -n "🌐 Checking locale files against Weblate's YAML shape..."

if "$TRANS_FMT_PY" scripts/check_translation_files.py >"${QC_TMP:-/tmp}/translation_files.out" 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ Locale files are in Weblate's shape, with their locale and plural entries intact"
else
  section_time $SECTION_START
  echo ""
  cat "${QC_TMP:-/tmp}/translation_files.out"
  echo "   Shape: $TRANS_FMT_PY scripts/check_translation_files.py --fix  (or: make translation-normalize)"
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_translation_files='^translations/|^scripts/translations/yaml_manager\.py$|^scripts/check_translation_files\.py$'
