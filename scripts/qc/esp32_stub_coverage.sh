# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# ESP32 firmware stubs cover what the firmware's vtables name
# ====================================================================
# A virtual added to a class the firmware stubs compiles everywhere and fails
# the firmware's ELF link, where the stub's vtable names it. At commit the gate
# runs when a staged path can change that (a header, an excluded source, a
# firmware-tree file); the full run always checks.
qc_esp32_stub_coverage() {
  local EXIT_CODE=0 out="$QC_TMP/esp32_stub_coverage.out"
  local -a touched=()
  if [ "$STAGED_ONLY" = true ]; then
    local IFS=$'\n'
    # shellcheck disable=SC2206
    touched=(--if-touched $QC_STAGED_ALL)
  fi
  if python3 scripts/check_esp32_stub_coverage.py "${touched[@]}" >"$out" 2>&1; then
    echo "✅ ESP32 stubs: $(tail -1 "$out")"
  else
    echo "❌ ESP32 stubs:"
    cat "$out"
    EXIT_CODE=1
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_esp32_stub_coverage='^(src|include)/.*\.(h|hpp|cpp)$|^firmware/helixscreen-esp32/|^scripts/check_esp32_(stub_coverage|app_srcs)\.py$'
