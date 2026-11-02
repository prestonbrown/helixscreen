# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# A staged src/ or include/ hunk can move a binding only by adding or removing a
# registration, or a preprocessor line that puts one in or out of a firmware
# branch. Anything else leaves the gate's answer unchanged.
QC_ESP32_BINDING_HUNK_RE='lv_xml_register_subject|lv_xml_register_event_cb|register_xml_callbacks|register_subject|publish[[:space:]]*\(|_SUBJECT_|SUBJECT_INIT|^[+-][[:space:]]*#[[:space:]]*(if|el|endif)'

# Paths that change the gate's answer whatever the hunk says.
QC_ESP32_BINDING_ALWAYS_RE='^ui_xml/.*\.xml$|^firmware/helixscreen-esp32/(components|ui_xml_overrides)/|^lib/helix-xml/src/xml/lv_xml\.c$|^scripts/(check_esp32_xml_bindings|check_orphan_callbacks|check_orphan_subjects|check_esp32_app_srcs)\.py$|^scripts/esp32_xml_binding_baseline\.txt$'

# Whether the staged change can alter which bindings the firmware registers.
qc_esp32_bindings_touched() {
  [ "$STAGED_ONLY" = true ] || return 0
  printf '%s\n' "$QC_STAGED_ALL" | grep -qE "$QC_ESP32_BINDING_ALWAYS_RE" && return 0
  git diff --cached -U0 -- src include 2>/dev/null | grep -E '^[+-]' | grep -v '^[+-][+-]' \
    | grep -qE "$QC_ESP32_BINDING_HUNK_RE"
}

# ====================================================================
# ESP32 firmware XML bindings
# ====================================================================
# Every subject and callback the desktop registers must be registered in a file
# and branch the firmware compiles, or the device shows a dead row.
qc_esp32_xml_bindings() {
  if ! qc_esp32_bindings_touched; then
    echo "⏭️  ESP32 XML bindings: no staged registration, #if or XML change"
    echo ""
    return 0
  fi
  local EXIT_CODE=0 out="$QC_TMP/esp32_xml_bindings.out"
  if python3 scripts/check_esp32_xml_bindings.py \
      --baseline scripts/esp32_xml_binding_baseline.txt >"$out" 2>&1; then
    cat "$out"
  else
    cat "$out"
    echo "   Run: python3 scripts/check_esp32_xml_bindings.py --list"
    EXIT_CODE=1
  fi
  echo ""
  return $EXIT_CODE
}

QC_TRIGGER_qc_esp32_xml_bindings="^src/|^include/|$QC_ESP32_BINDING_ALWAYS_RE"
