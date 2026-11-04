# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# L081 Mechanism C anti-pattern (cluster:pstat-async-delete)
# ====================================================================
qc_l081() {
  local EXIT_CODE=0
# Background: bg-thread `tok.expired()` followed by `this->`/`api_->` member
# access races on owner destruction → UAF crashes that look unrelated in
# backtraces. The runtime detector emits `cluster:pstat-async-delete Mechanism C`
# warnings; this gate catches new instances at commit time.
#
# Scope: known bg-thread directories only (src/core, src/printer, src/calibration,
# src/led, src/print, src/system, src/sensors, src/api, src/network, src/platform, src/mock,
# src/bluetooth). src/ui/ is excluded — observer cbs there fire on main thread
# and would false-positive without AST-level lambda-context analysis.
SECTION_START=$(date +%s)
echo -n "🧵 Checking L081 bg-thread anti-pattern..."

if [ "$STAGED_ONLY" = true ]; then
  L081_ARGS="--staged-only"
else
  L081_ARGS=""
fi
if python3 scripts/check_l081_anti_pattern.py $L081_ARGS >/tmp/l081_check.out 2>&1; then
  section_time $SECTION_START
  echo ""
  echo "✅ No L081 anti-pattern sites found"
else
  section_time $SECTION_START
  echo ""
  cat /tmp/l081_check.out
  echo "   Run: python3 scripts/check_l081_anti_pattern.py"
  echo "   See include/async_lifetime_guard.h for the canonical fix."
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_l081="$QC_TRIGGER_NATIVE_SRC"
