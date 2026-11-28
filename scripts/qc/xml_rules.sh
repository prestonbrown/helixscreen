# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# The ui_xml rules: six properties of ui_xml/ that load without a word
# ====================================================================
qc_xml_rules() {
  local EXIT_CODE=0
  # One process lists ui_xml/ once, opens each file at most once and runs
  # every rule of scripts/check_xml_rules.py over it. A rule prints its own
  # section here, with the verdict line, and names itself in a Run: hint when
  # it fails.
  #
  # At a commit (--staged-only) overlay-width reads the staged XML blobs, for
  # what the commit will contain. The other rules stay whole-tree: where a file
  # sits, and the element order inside it, are not answered by the files a
  # commit stages.
  #
  # The gate sets no QC_TRIGGER_qc_xml_rules, so every commit runs it:
  # deleting a codepoint from include/ui_icon_codepoints.h breaks XML that is
  # nowhere near the diff, and one process runs every rule.
  if [ "$STAGED_ONLY" = true ]; then
    XML_RULES_ARGS="--staged-only"
  else
    XML_RULES_ARGS=""
  fi
  # shellcheck disable=SC2086
  if ! python3 scripts/check_xml_rules.py $XML_RULES_ARGS >"$QC_TMP/xml_rules.out" 2>&1; then
    EXIT_CODE=1
  fi
  cat "$QC_TMP/xml_rules.out"

  return $EXIT_CODE
}
