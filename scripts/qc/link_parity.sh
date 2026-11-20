# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

qc_link_parity() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "🔗 Checking fbdev/EGL links against the main link..."

# The variant binaries link hand-written prerequisite lists, and an object the
# main link gains but a variant lacks fails only in the configuration that
# compiles it in - often a release or cache-warm job, never a local build.
if python3 scripts/check_link_parity.py >/tmp/link_parity.out 2>&1; then
  section_time $SECTION_START
  echo ""
  head -1 /tmp/link_parity.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/link_parity.out
  EXIT_CODE=1
fi

echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_link_parity='^Makefile$|^mk/.*\.mk$|^scripts/check_link_parity\.py$|^scripts/qc/link_parity\.sh$'
