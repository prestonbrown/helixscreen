# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, QC_TMP, ...) and reads the QC_TRIGGER_<gate> assignment through
# qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Layering bans: an include a refactor emptied stays gone (#1946)
# ====================================================================
# CLEARED_PAIRS in scripts/check_layering.py names (includer directory, header)
# pairs a refactor removed everywhere; one coming back anywhere at HEAD fails.
# `--base HEAD` asks only that. A new upward include in general is the layering
# design ratchet's (qc_design_ratchets), which warns.
qc_layering() {
  local EXIT_CODE=0 out="$QC_TMP/layering.out"
  SECTION_START=$(date +%s)
  echo -n "🧱 Checking the layering bans (cleared include pairs)..."
  if python3 scripts/check_layering.py --base HEAD >"$out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    echo "✅ Layering: no cleared include pair is back"
  else
    section_time "$SECTION_START"
    echo ""
    cat "$out"
    EXIT_CODE=1
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_layering='^(src|include)/.*\.(cpp|h)$|^scripts/check_layering\.py$'
