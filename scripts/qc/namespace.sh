# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/quality-checks.sh, which owns the run state this reads
# (STAGED_ONLY, AUTO_FIX, FILES, QC_TMP, ...) and reads the QC_TRIGGER_<gate>
# assignment through qc_trigger_re.
# shellcheck disable=SC2034,SC2154

# ====================================================================
# Namespace: HelixScreen declarations live under helix::
# ====================================================================
qc_namespace() {
  local EXIT_CODE=0
SECTION_START=$(date +%s)
echo -n "📛 Checking namespace compliance (declarations outside helix::)..."

# Ratcheting baseline. docs/devel/DEVELOPMENT.md § Namespace organization says all
# HelixScreen code lives under helix::. The rule was written, never gated, and a
# third of the tree drifted out from under it (#1370). The drift runs along
# subsystem lines rather than by age — every ams_backend_*, every ui_panel_*,
# every display/wifi/usb/sound backend is global — so those areas keep taking new
# global-scope declarations by local precedent unless something says no.
# The number may go DOWN (move a declaration under helix::, then lower this
# baseline) but must never go up. extern "C", file-local statics in .cpp, and
# forward declarations of third-party types are structural and never counted.
# The one exception is a sync merge from main, which has neither this gate nor
# its script and so imports code written without it: 2296 -> 2304 covers the
# eight such sites the 2026-08-28 sync brought over, each following its file's
# dominant convention (the inline predicates beside the global AmsAction enum,
# two more ui_gcode_viewer_* C-style entry points, and a custom XML widget
# module registered by C-string name). 2304 -> 2305 is the next sync's single
# site: RecoverySuppression::RESTART_FLAG_TIMEOUT joins an existing global
# namespace whose other constants are already counted here. 2305 -> 2325 is
# the 2026-08-31 sync's twenty: main's netd backends (EthernetBackendNetd,
# WifiBackendNetd, and ethernet_backend.h's foreign-ns sysfs helpers beside
# the ones already counted), the Spoolman searchable-text/filter free
# functions, backend_owns_runout_during_job, ModalCloseReason and
# for_each_in_tree from the modal teardown rework, ui_button_owns_user_data
# and ContainerDeleteNet from the widget-pool fix,
# wifi_signal_percent_from_dbm, and an AmsBackend forward declaration - each
# beside global-scope siblings in its own file. +2 for the main-side sync:
# ui_gcode_viewer_set_thumbnail_parity (declaration + definition), another
# member of the global ui_gcode_viewer_* C-API family. 2328 -> 2334 is the
# 2026-09-02 sync's six: display_backend.h's display_is_rotated and
# display_rotation_degrees, beside the global inline rotation helpers already
# counted there, and ui_gcode_viewer_clear_tool_colors and
# ui_gcode_viewer_get_tool_colors (declaration + definition each), two more of
# the same global ui_gcode_viewer_* C-API family. 2334 -> 2336 is
# theme_manager_get_readable_on (declaration + definition), a new member of
# the global theme_manager_* family it sits in - every accessor in that header
# is global scope, so putting this one alone in helix:: would make its call
# sites the odd ones out. 2336 -> 2337 was filament_op_execute.h forward-
# declaring the then-global AmsBackend and AmsError; that entry retires with
# the AMS layer's move under helix:: (#1370). 2337 -> 2298 is the AMS backend
# class layer: AmsBackend, AmsSubscriptionBackend, the concrete
# backends and the mock with their per-backend value structs and test-access
# forward declarations, AmsState, and the headers that forward-declared
# AmsBackend at global scope (filament_sensor_manager.h's test-access
# forward declarations went with the RunoutScopeTestAccess it shares with
# the Snapmaker backend). 2298 -> 2290 is ams_error.h (AmsResult, AmsError,
# AmsErrorHelper and the result-to-string helper) and ams_step_operation.h
# following the backends into helix::. 2290 -> 2242 is ams_types.h: every
# AMS value type, enum, constant and inline helper it declared at global
# scope, plus the SlotInfo and DryingPreset forward declarations that
# followed them. 2242 -> 2239 is main's own slack, picked up by the merge:
# main dropped the Plugins overlay and retired three globals without
# ratcheting, so the merge collects that slack too. 2239 -> 2238 is
# ResolvedMacroScript and resolve_macro_script moving into helix::.
# 2215 -> 2233 is FOREIGN_PREFIXES anchored to real foreign spellings
# (#1586). Every entry there is matched with startswith, so a prefix this
# tree also spells - bare 'G', 'Display', 'Window', 'z_' - exempts our own
# declarations from the gate rather than a library's. The list carries only
# spellings a third-party API actually uses, and the 18 symbols that covers
# are counted here, as are the three spellings of ui_gcode_viewer_pump_offscreen_2d:
# its declaration, its definition, and the stub for builds without the renderer.
# It joins the ui_gcode_viewer_* C API, which is global by design because it
# is the widget's LVGL-facing surface; scoping this one call into helix::
# would make it the only member of that family that is. 2215 -> 2214 is
# plugin_api.h's file-scope `class IMoonrakerAPI;` forward declaration.
# 2193 -> 2194 nets three sites: ui_panel_controls.cpp's redundant
# redeclaration of get_global_motion_panel() leaves (its header defines it
# inline), and the bed mesh and spoolman accessors are counted as .cpp
# definitions, kept out of line because the ESP32 build supplies its own.
#
# tests/shell/test_namespace_gate.bats carries this same number and fails if
# the two disagree or if the tree drifts under it.
if python3 scripts/check_namespace_compliance.py --max-allowed 2101 --summary >/tmp/namespace_check.out 2>&1; then
  section_time $SECTION_START
  echo ""
  tail -1 /tmp/namespace_check.out
else
  section_time $SECTION_START
  echo ""
  cat /tmp/namespace_check.out
  echo "   Declare new types under helix:: (or a helix:: sub-namespace)."
  echo "   Genuinely-global sites take \`// NAMESPACE_OK: <reason>\`."
  EXIT_CODE=1
fi

echo ""

  return $EXIT_CODE
}

QC_TRIGGER_qc_namespace="$QC_TRIGGER_NATIVE_SRC"
