# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# XML Attribute Validation
# ====================================================================
qc_xml_attr() {
  local EXIT_CODE=0
echo "📄 Validating XML attributes..."

if [ -x "build/bin/validate-xml-attributes" ]; then
  if [ "$STAGED_ONLY" = true ]; then
    # Check only staged XML files in pre-commit mode
    STAGED_XML_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.xml$' || true)
    if [ -n "$STAGED_XML_FILES" ]; then
      # shellcheck disable=SC2086
      if ./build/bin/validate-xml-attributes --warn-only $STAGED_XML_FILES 2>/dev/null; then
        echo "✅ XML attribute validation passed"
      else
        echo "⚠️  Unknown XML attributes found (warnings only for now)"
        echo "   Run './build/bin/validate-xml-attributes' for details"
        # NOTE: Using --warn-only so this doesn't block commits during adoption
        # Remove --warn-only once all false positives are resolved
      fi
    else
      echo "ℹ️  No XML files staged for commit"
    fi
  else
    # CI mode: check all XML files with --warn-only
    if ./build/bin/validate-xml-attributes --warn-only 2>/dev/null; then
      echo "✅ XML attribute validation passed"
    else
      echo "⚠️  Unknown XML attributes found (warnings only for now)"
      echo "   Run './build/bin/validate-xml-attributes' for details"
    fi
  fi
else
  echo "⚠️  validate-xml-attributes not built - skipping"
  echo "   qc_xml_tools builds it when a staged .xml or the tool itself changes"
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_xml_attr="$QC_TRIGGER_XML"
