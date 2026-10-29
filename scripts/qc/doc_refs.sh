# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Agent-facing docs: references resolve, doc index is complete, and the
# public docs state shipped counts truthfully
# ====================================================================
qc_doc_refs() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "📚 Checking doc references and index..."

if python3 scripts/check_doc_refs.py >/tmp/doc_refs.out 2>&1; then
  section_time $SECTION_START
  echo ""
  cat /tmp/doc_refs.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/doc_refs.out
  echo "   Run: python3 scripts/check_doc_refs.py"
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_doc_refs='\.md$|^scripts/check_doc_refs\.py$|^include/ams_types\.h$|^assets/config/printer_database\.json$|^assets/config/themes/defaults/'
