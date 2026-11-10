# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# CJK runtime font staleness + artifact coverage
# ====================================================================
qc_cjk_fonts() {
  local EXIT_CODE=0
# A translated string whose glyph never got baked renders as tofu in zh/ja
# with no build error and no runtime warning. Two halves, both needed:
# check_cjk_font_staleness.sh diffs the codepoints the translations need
# against the manifest the bake recorded; check_cjk_font_coverage.py parses
# the .bin cmap tables themselves so a stale bake cannot hide behind a
# freshly written manifest. Both inputs are git-tracked files, so this runs
# in every clone — no .otf source fonts required.
SECTION_START=$(date +%s)
echo -n "🌐 Checking CJK font bake..."

if bash scripts/check_cjk_font_staleness.sh >/tmp/cjk_stale.out 2>&1 \
   && python3 scripts/check_cjk_font_coverage.py >/tmp/cjk_cov.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ Every needed CJK codepoint is baked into the runtime fonts"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/cjk_stale.out /tmp/cjk_cov.out
  echo "   Fix: make regen-text-fonts, rebuild, commit assets/fonts/cjk/."
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

# Any locale's catalog can introduce a CJK codepoint, and so can a
# hardcoded string in src/, an XML layout or the printer database; the
# artifacts themselves live under assets/fonts/cjk/.
QC_TRIGGER_qc_cjk_fonts='^translations/|^src/|^include/|^ui_xml/|^assets/config/printer_database\.json$|^assets/fonts/cjk/|^scripts/(regen_text_fonts\.sh|check_cjk_font_staleness\.sh|check_cjk_font_coverage\.py|translations/(cjk_charset\.py|cjk_filename_fonts\.py|hanzi_level1\.txt))$'
