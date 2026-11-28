# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Phase 1: Critical Checks
# ====================================================================
qc_phase1() {
  local EXIT_CODE=0

# Merge Conflict Markers Check
echo "⚠️  Checking for merge conflict markers..."
# Each file is read as the commit will hold it, at a commit (qc_read_path), and
# reported under its own name.
if [ -n "$FILES" ]; then
  CONFLICT_FILES=""
  for qc_f in $FILES; do
    qc_file="$(qc_read_path "$qc_f")"
    [ -f "$qc_file" ] || continue
    if grep -q "^<<<<<<< \|^=======$\|^>>>>>>> " "$qc_file" 2>/dev/null; then
      CONFLICT_FILES="$CONFLICT_FILES $qc_f"
    fi
  done
  if [ -n "$CONFLICT_FILES" ]; then
    echo "❌ Merge conflict markers found in:"
    echo "$CONFLICT_FILES" | tr ' ' '\n' | grep -v '^$' | sed 's/^/   /'
    EXIT_CODE=1
  else
    echo "✅ No merge conflict markers"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# Trailing Whitespace Check
echo "🧹 Checking for trailing whitespace..."
if [ -n "$FILES" ]; then
  TRAILING_WS=""
  for qc_f in $FILES; do
    qc_file="$(qc_read_path "$qc_f")"
    [ -f "$qc_file" ] || continue
    qc_hits="$(grep -n "[[:space:]]$" "$qc_file" 2>/dev/null | sed "s|^|$qc_f:|" || true)"
    if [ -n "$qc_hits" ]; then
      TRAILING_WS="$TRAILING_WS$qc_hits
"
    fi
  done
  if [ -n "$TRAILING_WS" ]; then
    echo "⚠️  Found trailing whitespace:"
    echo "$TRAILING_WS" | head -10 | sed 's/^/   /'
    if [ "$(echo "$TRAILING_WS" | wc -l)" -gt 10 ]; then
      echo "   ... and $(($(echo "$TRAILING_WS" | wc -l) - 10)) more"
    fi
    echo "ℹ️  Fix with: sed -i 's/[[:space:]]*$//' <file>"
  else
    echo "✅ No trailing whitespace"
  fi
else
  echo "ℹ️  No files to check"
fi

echo ""

# XML Validation
echo "📄 Validating XML files..."
if [ "$STAGED_ONLY" = true ]; then
  XML_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep "\.xml$" || true)
else
  XML_FILES=$(find ui_xml -name "*.xml" 2>/dev/null || true)
fi

if [ -n "$XML_FILES" ]; then
  if command -v xmllint >/dev/null 2>&1; then
    XML_ERRORS=0
    for xml in $XML_FILES; do
      if [ -f "$(qc_read_path "$xml")" ]; then
        # Use --recover to continue on namespace errors (LVGL uses colon syntax like
        # style_arc_color:indicator which xmllint interprets as namespace prefixes).
        # Filter: only keep error lines (file:line: format) and exclude namespace errors.
        XMLLINT_OUTPUT=$(qc_xmllint "$xml" | grep -E "^[^:]+:[0-9]+:" | grep -v "namespace error" || true)
        if [ -n "$XMLLINT_OUTPUT" ]; then
          echo "❌ Invalid XML: $xml"
          echo "$XMLLINT_OUTPUT"
          XML_ERRORS=$((XML_ERRORS + 1))
          EXIT_CODE=1
        fi
      fi
    done
    if [ $XML_ERRORS -eq 0 ]; then
      echo "✅ All XML files are valid"
    fi
  else
    echo "⚠️  xmllint not found - skipping XML validation"
    echo "   Install with: brew install libxml2 (macOS) or apt install libxml2-utils (Linux)"
  fi
else
  echo "ℹ️  No XML files to validate"
fi

echo ""

  return $EXIT_CODE
}
