# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Doc images: every image a user-facing page shows has a recipe in
# docs/images/doc-screenshots.yml, every entry's file exists, none is dead
# ====================================================================
qc_doc_images() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "🖼️  Checking doc images against their manifest..."

if python3 scripts/check_doc_images.py >"$QC_TMP/doc_images.out" 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ Every doc image has a manifest entry"
else
  section_time $SECTION_START
  echo ""
  cat "$QC_TMP/doc_images.out"
  echo "   Add an entry to docs/images/doc-screenshots.yml (schema: scripts/doc_screenshots.py)"
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_doc_images='\.md$|^docs/images/|^scripts/check_doc_images\.py$|^scripts/qc/doc_images\.sh$'
