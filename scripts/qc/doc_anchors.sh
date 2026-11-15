# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Citation anchors: every `path#scope` citation in the docs resolves in the
# code (scripts/doc_anchors.py --check, which itself always exits 0).
#
# Report-only by default: findings are printed and the gate passes. Set
# QC_DOC_ANCHORS_STRICT=1 to make any finding fail the commit; flip the
# default below to 1 once the tree reports 0 findings.
# QC_DOC_ANCHORS_PATHS (space-separated) narrows the check to those docs.
# ====================================================================
QC_DOC_ANCHORS_STRICT_DEFAULT=1

qc_doc_anchors() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "⚓ Checking doc citation anchors..."

local strict="${QC_DOC_ANCHORS_STRICT:-$QC_DOC_ANCHORS_STRICT_DEFAULT}"
# shellcheck disable=SC2086
python3 scripts/doc_anchors.py --check ${QC_DOC_ANCHORS_PATHS:-} >"$QC_TMP/doc_anchors.out" 2>&1
local findings
findings=$(grep -c -v -E '^(✅|⚠️)' "$QC_TMP/doc_anchors.out")

section_time $SECTION_START
echo ""
if [ "$findings" -eq 0 ]; then
  echo "✅ Every doc citation anchor resolves"
else
  cat "$QC_TMP/doc_anchors.out"
  if [ "$strict" = "1" ]; then
    echo "   Run: python3 scripts/doc_anchors.py --check"
    EXIT_CODE=1
  else
    echo "   Report only: $findings unresolved citation(s) (strict with QC_DOC_ANCHORS_STRICT=1)"
  fi
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_doc_anchors='\.md$|^scripts/doc_anchors\.py$|^scripts/qc/doc_anchors\.sh$|^(src|include|ui_xml|scripts|mk)/|^Makefile$'
