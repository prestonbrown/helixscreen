# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Design tokens + no private LVGL APIs
# ====================================================================
qc_design_tokens() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "🎨 Checking design tokens and LVGL API surface..."

TOKEN_EXIT=0

# Private LVGL internals. remote_control_server.cpp walks LVGL's XML subject
# linked list for `ctl list_subjects` — there is no public API for that.
PRIV=$(grep -rnoE '\b_lv_[a-z_]+\(' src include 2>/dev/null \
         | grep -v 'src/remote/remote_control_server.cpp' || true)
if [ -n "$PRIV" ]; then
  echo ""
  echo "$PRIV"
  echo "❌ Private LVGL API (_lv_*) — use the public API."
  TOKEN_EXIT=1
fi

# Hardcoded colors. Exempt: theme_manager and its src/ui/theme/ files (they parse hex into tokens, definitional),
# procedural canvas renderers, and helix-splash (a separate binary that does not
# link ThemeManager). Ratcheting baseline — port these to theme_manager_get_color().
HEX_ALLOW='theme_manager|src/ui/theme/|src/rendering/|canvas|confetti|glyph|src/helix_splash.cpp'
HEX_BASELINE=33
HEX_COUNT=$(grep -rn 'lv_color_hex(0x' src include 2>/dev/null | grep -vcE "$HEX_ALLOW" || true)
if [ "$HEX_COUNT" -gt "$HEX_BASELINE" ]; then
  echo ""
  grep -rn 'lv_color_hex(0x' src include 2>/dev/null | grep -vE "$HEX_ALLOW" || true
  echo "❌ Hardcoded colors: $HEX_COUNT exceeds baseline ($HEX_BASELINE)."
  echo "   Use theme_manager_get_color(\"token\") or an XML design token."
  TOKEN_EXIT=1
fi

# A spacing token C++ reads must be declared in a shipped XML file, not only
# in a dev panel's, or release builds read it as missing.
if ! python3 scripts/check_shipped_spacing_tokens.py >/tmp/shipped_spacing.out 2>&1; then
  echo ""
  cat /tmp/shipped_spacing.out
  TOKEN_EXIT=1
fi

section_time $SECTION_START
if [ "$TOKEN_EXIT" -eq 0 ]; then
  echo ""
  if [ "$HEX_COUNT" -lt "$HEX_BASELINE" ]; then
    echo "✅ Design tokens: $HEX_COUNT hardcoded colors (baseline $HEX_BASELINE — ratchet down)"
  else
    qc_count "✅ Design tokens: $HEX_COUNT == baseline ($HEX_BASELINE), no private LVGL APIs"
  fi
else
  echo ""
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_design_tokens='\.(cpp|h|xml)$'
