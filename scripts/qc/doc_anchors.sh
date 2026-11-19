# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Citation anchors: every `path#scope` citation in the docs a change touches
# resolves in the code (scripts/doc_anchors.py --check on those docs).
#
# Scoped to the change so it stays fast and never fails a commit for a doc it
# did not touch: at commit, the staged markdown; on a full run (pre-push,
# land-gate, CI), the markdown changed since the fork point. One doc checks in
# about 0.2 s; the whole tree takes about a minute, so the whole-tree sweep is
# `make check-doc-anchors`, run by hand. QC_DOC_ANCHORS_PATHS (space-separated)
# overrides the selection; QC_DOC_ANCHORS_STRICT=0 reports without failing.
# ====================================================================
QC_DOC_ANCHORS_STRICT_DEFAULT=1

qc_doc_anchors_paths() {
  if [ -n "${QC_DOC_ANCHORS_PATHS:-}" ]; then
    printf '%s\n' $QC_DOC_ANCHORS_PATHS
  elif [ "${STAGED_ONLY:-false}" = true ]; then
    printf '%s\n' "${QC_STAGED_ALL:-}" | grep -E '\.md$' || true
  else
    local base
    base=$(python3 scripts/diff_base.py 2>/dev/null) || base=""
    [ -n "$base" ] && git diff --name-only --diff-filter=ACMR "$base"...HEAD -- '*.md' 2>/dev/null
  fi | while read -r f; do [ -n "$f" ] && [ -f "$f" ] && printf '%s\n' "$f"; done
}

qc_doc_anchors() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "⚓ Checking doc citation anchors..."

local strict="${QC_DOC_ANCHORS_STRICT:-$QC_DOC_ANCHORS_STRICT_DEFAULT}"
local paths
paths=$(qc_doc_anchors_paths)
if [ -z "$paths" ]; then
  section_time $SECTION_START
  echo ""
  echo "✅ Doc citation anchors: no changed docs to check"
  echo ""
  return 0
fi
# shellcheck disable=SC2086
python3 scripts/doc_anchors.py --check $paths >"$QC_TMP/doc_anchors.out" 2>&1
local findings
findings=$(grep -c -v -E '^(✅|⚠️)' "$QC_TMP/doc_anchors.out")

section_time $SECTION_START
echo ""
if [ "$findings" -eq 0 ]; then
  echo "✅ Every doc citation anchor resolves"
else
  cat "$QC_TMP/doc_anchors.out"
  if [ "$strict" = "1" ]; then
    echo "   Point each citation at the symbol's current name (path#scope), or drop it if the code is gone."
    echo "   Check one doc: python3 scripts/doc_anchors.py --check <doc.md>"
    EXIT_CODE=1
  else
    echo "   Report only: $findings unresolved citation(s) (strict with QC_DOC_ANCHORS_STRICT=1)"
  fi
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_doc_anchors='\.md$|^scripts/doc_anchors\.py$|^scripts/qc/doc_anchors\.sh$|^(src|include|ui_xml|scripts|mk)/|^Makefile$'
