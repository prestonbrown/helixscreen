# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# XML Validator Tool Build
# ====================================================================
# qc_xml_attr passes vacuously when its binary is missing, and nothing in a
# normal build produces it — `make` builds only the app. This step builds it
# ahead of that gate, serially and before the parallel batch, so two gates
# never run make against one tree at once. Failing to build is a red gate, not
# a skip: a validator that silently doesn't exist is a validator that silently
# passes.
#
# validate-xml-constants is not built here: it links the whole app, which on
# a cold CI runner is a full build that cannot finish inside the step's time
# limit. Its check runs in the unit suite instead (see qc_xml_const).
qc_xml_tools() {
  local EXIT_CODE=0
  # Same bounded -j the build-verification phase uses (qc_build_jobs).
  local TOOL_JOBS
  TOOL_JOBS=$(qc_build_jobs)
echo "🔧 Building XML validator tools..."

if "$(qc_make)" SKIP_COMPILE_COMMANDS=1 -j"$TOOL_JOBS" validate-xml-attrs >/tmp/qc_xml_tools.out 2>&1; then
  echo "✅ XML validator tools ready"
else
  echo ""
  echo "❌ XML validator tools failed to build"
  sed -n '1,40p' /tmp/qc_xml_tools.out
  EXIT_CODE=1
fi

echo ""
  return $EXIT_CODE
}

# Builds the validator binary the XML attribute gate runs. A commit runs that gate
# on staged XML only, so a staged .cpp has no use for the tool; wakes on the
# validators' own sources so an edit to a tool rebuilds it.
QC_TRIGGER_qc_xml_tools='\.xml$|^tools/validate_xml|^tools/xml-linter/'
