# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re. Sourced after xml_schema.sh, whose
# qc_xml_schema_refresh regenerates the snapshot here.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# helix-xml-linter (mirrors the "XML Lint" CI gate)
# ====================================================================
qc_xml_linter() {
  local EXIT_CODE=0
# The linter resolves every #const reference against a committed snapshot,
# tools/xml-linter/schema/schema.json. Adding a <px>/<color>/<string> to an XML
# file without regenerating that snapshot leaves the new name unknown, so every
# reference to it is an unknown-const-ref on CI while the commit sails through
# locally (prestonbrown/helixscreen#1204).
#
# This is the lint, and it reads all of ui_xml/, so a commit leaves it to the
# full sweep (QC_WHOLE_TREE); qc_xml_schema (xml_schema.sh) is what a commit
# runs, and refreshes the snapshot there. Where the lint does run at a commit
# (HELIX_QC_ALL_AT_COMMIT=1) it is kept cheap three ways:
#   - Triggers on staged paths for the XML half (ui_xml/, tools/xml-linter/)
#     and on staged diff CONTENT for the rest of the schema's inputs. The
#     extractor (mk/tools.mk regen-xml-schema) also reads src/ui/ widget
#     registrations, src/ lv_xml_register_const() calls, lib/helix-xml/src/xml
#     and the theme defaults, and none of those reach the linter except through
#     the schema. A path trigger on src/ would fire on nearly every commit and
#     pay make's startup for nothing, so the schema-input signals are the
#     added/removed lines instead, the predicates in xml_schema.sh.
#     CI's XML Lint job still covers the wider path set.
#   - Lints against the snapshot on disk first. That is what CI does with the
#     committed copy, so on a clean tree a pass here means CI passes and no
#     regen is owed. It also stops schema.json churning on commits where
#     staleness changes no outcome.
#   - Regenerates only on the paths where the snapshot can actually be at
#     fault: a failing lint, a staged edit that deletes a const definition
#     (which would otherwise leave dangling refs resolving against a stale
#     snapshot), or a staged change to a schema input the linter cannot see.
#     Only those rare paths pay the `make` startup.

# Mirrors `make lint-xml` (mk/tools.mk) but skips make's ~0.5s startup. Always
# lints all of ui_xml/, never just the staged files: the linter builds its
# cross-file component registry from the paths it is handed, so a staged-only
# run reports every component defined in an unstaged file as unknown.
run_xml_linter() {
  PYTHONPATH=tools/xml-linter/src python3 -m helix_xml_linter.cli \
    --schema "$XML_SCHEMA_PATH" --severity error ui_xml/
}

# Staged schema inputs the linter cannot see through the current snapshot, so
# they force a regen below rather than trusting the first lint: the paths and
# the C++ registrations qc_xml_schema names (xml_schema.sh).
SCHEMA_INPUT_STAGED=false

if [ "$STAGED_ONLY" = true ]; then
  XML_LINT_TRIGGERS=$(git diff --cached --name-only --diff-filter=ACM | \
    grep -E '^(ui_xml/.*\.xml$|tools/xml-linter/)' || true)
  if qc_xml_schema_path_input_staged || qc_xml_schema_cpp_input_changed; then
    SCHEMA_INPUT_STAGED=true
    XML_LINT_TRIGGERS="$XML_LINT_TRIGGERS schema-inputs"
  fi
else
  XML_LINT_TRIGGERS="all"
fi

# Nothing staged that the schema or the lint reads: say nothing at all, since
# the trigger admits every src/ .cpp commit for the content greps above.
[ -n "$XML_LINT_TRIGGERS" ] || return 0
echo "🧩 Running helix-xml-linter..."

if ! command -v python3 >/dev/null 2>&1; then
  echo "⚠️  python3 not found — skipping XML lint"
else
  # A deleted const can leave a dangling ref that still resolves against the
  # stale snapshot — the one staleness a passing lint cannot rule out.
  XML_CONST_DELETED=false
  if [ "$STAGED_ONLY" = true ] && qc_xml_schema_const_removed; then
    XML_CONST_DELETED=true
  fi

  if [ "$XML_CONST_DELETED" = false ] && [ "$SCHEMA_INPUT_STAGED" = false ] && \
     run_xml_linter >/tmp/lint_xml.out 2>&1; then
    qc_count "✅ helix-xml-linter passed ($(tail -1 /tmp/lint_xml.out))"
  else
    # The lint failed, a const was deleted, or a schema input the lint cannot
    # see is staged. Refresh the snapshot and retry (qc_xml_schema_refresh,
    # xml_schema.sh): one that the refresh could not stage fails the run, since
    # CI lints the committed copy.
    qc_xml_schema_refresh
    SCHEMA_REFRESH_RC=$?
    if run_xml_linter >/tmp/lint_xml.out 2>&1; then
      qc_count "✅ helix-xml-linter passed ($(tail -1 /tmp/lint_xml.out))"
      [ "$SCHEMA_REFRESH_RC" -eq 0 ] || EXIT_CODE=1
    else
      cat /tmp/lint_xml.out
      echo "   Run 'make lint-xml-all' to see warnings too"
      EXIT_CODE=1
    fi
  fi
fi

echo ""

  return $EXIT_CODE
}

# The XML half of the trigger plus every schema input SCHEMA_INPUT_STAGED reads:
# without them here the driver skips the gate before that test can see them.
# A src/ .cpp reaches the gate only for the two cheap content greps above.
QC_TRIGGER_qc_xml_linter="$QC_TRIGGER_XML"'|^src/.*\.cpp$|^assets/config/themes/defaults/|^lib/helix-xml$|^mk/tools\.mk$'
