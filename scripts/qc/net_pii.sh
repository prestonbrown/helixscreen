# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Network PII: no SSID/BSSID/MAC logged above trace level
# ====================================================================
qc_net_pii() {
  local EXIT_CODE=0
# Background: the in-memory log ring is captured at debug regardless of the
# user's configured verbosity, and leaves the machine three ways — the debug
# bundle, the crash reporter's automatic upload, and the `ctl log` RPC. A set
# of nearby SSIDs with signal strengths is a geolocation fingerprint, and a
# scan enumerates the neighbours' networks too. No downstream regex can catch
# an SSID, so the control has to be at the log call site (#1191).
SECTION_START=$(date +%s)
echo -n "🔒 Checking network PII in log calls..."

if [ "$STAGED_ONLY" = true ]; then
  PII_ARGS="--staged-only"
else
  PII_ARGS=""
fi
if python3 scripts/check_wifi_pii_logging.py $PII_ARGS >/tmp/wifi_pii_check.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ No network identifiers logged above trace"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/wifi_pii_check.out
  echo "   Run: python3 scripts/check_wifi_pii_logging.py"
  echo "   See include/core/log_redact.h for the redaction helpers."
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_net_pii="$QC_TRIGGER_NATIVE_SRC"
