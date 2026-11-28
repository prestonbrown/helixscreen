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
  # At a commit (--staged-only) the gate judges the XML as the commit will hold
  # it: every *.xml under ui_xml/ and the codepoint header are written from the
  # index to a scratch tree, and the rules run on that. So they read the staged
  # content, and icon-names still sees every component: an icon name can arrive
  # through a prop another file's component declares. A deleted codepoint breaks
  # XML nowhere near the diff, which is why the header goes into the tree too.
  #
  # shipped-spacing-tokens does not run at a commit: its callers are every C++
  # file, so it is the full sweep's rule.
  #
  # The gate sets no QC_TRIGGER_qc_xml_rules, so every commit runs it.
  local rule
  XML_RULES_ARGS=()
  if [ "$STAGED_ONLY" = true ]; then
    XML_RULES_ROOT="$QC_TMP/xml_rules"
    qc_export_index_tree "$XML_RULES_ROOT" 'ui_xml/*.xml' include/ui_icon_codepoints.h
    XML_RULES_ARGS=(--root "$XML_RULES_ROOT")
    for rule in header-actions overlay-width icon-names responsive-token-scope modal-chrome-budget; do
      XML_RULES_ARGS+=(--rule "$rule")
    done
  fi
  if ! python3 scripts/check_xml_rules.py "${XML_RULES_ARGS[@]}" >"$QC_TMP/xml_rules.out" 2>&1; then
    EXIT_CODE=1
  fi
  cat "$QC_TMP/xml_rules.out"

  return $EXIT_CODE
}
