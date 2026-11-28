# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Every component src/ creates by name through lv_xml_create is registered
# ====================================================================
qc_xml_create_registered() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "🧩 Checking lv_xml_create component registration..."

if python3 scripts/check_source_rules.py --rule xml-create-registered >/tmp/xml_create_registered.out 2>&1; then
  section_time $SECTION_START
  echo ""
  cat /tmp/xml_create_registered.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/xml_create_registered.out
  echo "   Run: python3 scripts/check_source_rules.py --rule xml-create-registered"
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_xml_create_registered='^src/|^ui_xml/|^scripts/check_source_rules\.py$'
