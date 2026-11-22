# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# A head is named and mounted through the head identity, never a tool number
# ====================================================================
qc_head_labels() {
  local EXIT_CODE=0 out="$QC_TMP/head_labels.out" args=""
  SECTION_START=$(date +%s)
  echo -n "🧭 Checking head labels and head commands..."
  # A commit scans the staged blobs of the sources it changed; pre-push and
  # land-gate scan the whole tree.
  [ "$STAGED_ONLY" = true ] && args="--staged-only"
  # shellcheck disable=SC2086
  if python3 scripts/check_head_labels.py $args >"$out" 2>&1; then
    section_time "$SECTION_START"
    echo ""
    echo "✅ $(tail -1 "$out")"
  else
    section_time "$SECTION_START"
    echo ""
    cat "$out"
    EXIT_CODE=1
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_head_labels='^src/(ui|printer)/.*\.(cpp|h)$|^scripts/(check_head_labels|source_strip|staged_content)\.py$'
