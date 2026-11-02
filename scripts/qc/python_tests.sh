# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Python script tests
# ====================================================================
qc_python_tests() {
  local EXIT_CODE=0
# CI's Code Quality job runs tests/python/ as its own step, outside this
# script, so without this gate nothing local ran it and a push could turn that
# job red. ~20s. Skipped on GitHub Actions, where that step already ran.
SECTION_START=$(date +%s)
echo -n "🐍 Running Python script tests..."

if [ -n "${GITHUB_ACTIONS:-}" ]; then
  section_time $SECTION_START
  echo ""
  echo "⏭️  CI runs tests/python/ as its own step"
elif [ -x "$VENV_PYTHON" ]; then
  # Git exports GIT_INDEX_FILE and friends to hooks; a test that builds a
  # throwaway repo would otherwise read this commit's index.
  # Each launch step is stamped, so a kept log (scripts/quality-checks.sh) shows
  # whether a slow run was spent before pytest's session, inside it, or after.
  if ( qc_stamp() { echo "[$(date -u +%H:%M:%S)] $*"; }
       qc_stamp "gate start"
       for v in $(compgen -e); do case "$v" in GIT_*) unset "$v" ;; esac; done
       qc_stamp "env cleared; starting $VENV_PYTHON"
       "$VENV_PYTHON" -c 'pass'
       qc_stamp "interpreter started and exited"
       "$VENV_PYTHON" -m pytest -q -p no:cacheprovider --durations=15 tests/python/
       rc=$?
       qc_stamp "pytest exited $rc"
       exit $rc ) \
     >"$QC_TMP/python_tests.log" 2>&1; then
    section_time $SECTION_START
    echo ""
    echo "✅ $(grep -E ' in [0-9.]+s' "$QC_TMP/python_tests.log" | tail -1)"
  else
    section_time $SECTION_START
    echo ""
    grep -E "^(FAILED|ERROR) " "$QC_TMP/python_tests.log" | head -20
    grep -E ' in [0-9.]+s' "$QC_TMP/python_tests.log" | tail -1
    echo "   Reproduce: .venv/bin/pytest tests/python/"
    EXIT_CODE=1
  fi
else
  section_time $SECTION_START
  echo ""
  echo "⚠️  .venv not set up - skipping (run 'make venv-setup')"
fi

echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_python_tests='\.py$|^tests/python/'
