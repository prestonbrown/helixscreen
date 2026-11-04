# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# ESP32 firmware app_srcs manifest and link boundary
# ====================================================================
qc_esp32_app_srcs() {
  local EXIT_CODE=0
# ESP32 firmware app_srcs manifest drift. The manifest is a hand-maintained
# subset of src/ (v1 Core+AMS cut); a new src/ file that misses it breaks the
# firmware link ~25 min into esp32-build CI. This makes the drift loud here.
if python3 scripts/check_esp32_app_srcs.py >"$QC_TMP/esp32_app_srcs.out" 2>&1; then
  echo "✅ ESP32 app_srcs manifest covers src/ (no drift)"
else
  cat "$QC_TMP/esp32_app_srcs.out"
  EXIT_CODE=1
fi

# The same boundary at link level: a listed file calling a symbol that only an
# excluded file defines compiles everywhere and fails the firmware link. The verdict
# reads the native build's objects, so a tree whose objects are absent or older than
# their sources has none, and the land gate, which has just built, must reach one
# (HELIX_LINK_BOUNDARY_REQUIRED). The baseline is drawn from Linux objects; a Mac
# compiles different backends, so it never has a verdict to give.
if [ "$(uname -s)" = Darwin ]; then
  echo "ℹ️  ESP32 link boundary: no verdict on macOS, the land gate decides"
else
  python3 scripts/check_esp32_app_srcs.py --link >"$QC_TMP/esp32_app_srcs_link.out" 2>&1
  case $? in
    0) echo "✅ ESP32 link boundary: no listed file needs an excluded file's symbols" ;;
    2) if [ "${HELIX_LINK_BOUNDARY_REQUIRED:-0}" = 1 ]; then
         echo "❌ ESP32 link boundary: no verdict in a tree that must have one:"
         cat "$QC_TMP/esp32_app_srcs_link.out"
         EXIT_CODE=1
       else
         echo "ℹ️  ESP32 link boundary: no verdict: $(cat "$QC_TMP/esp32_app_srcs_link.out")"
       fi ;;
    *) echo "❌ ESP32 link boundary:"
       cat "$QC_TMP/esp32_app_srcs_link.out"
       EXIT_CODE=1 ;;
  esac
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_esp32_app_srcs='^src/|^firmware/helixscreen-esp32/components/helixapp/|^scripts/check_esp32_app_srcs\.py$|^scripts/esp32_link_baseline\.txt$'
