#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for moonraker-plugin/install.sh phase-tracking setup.
#
# Guards the regression from 0d5bc370d: helix_phase_tracking.cfg was merged into
# helix_macros.cfg, but install.sh kept pointing at the old path. Because the
# only symptom was a warn() on a path nobody read, phase tracking silently
# stopped installing the macros it instruments PRINT_START to call
# (prestonbrown/helixscreen#1268).

load helpers

# Overridable so the guard itself can be checked against a known-bad copy of the
# installer (see the issue: the pre-fix script pointed at a deleted file).
SCRIPT="${HELIX_PLUGIN_INSTALL_SH:-moonraker-plugin/install.sh}"

setup() {
    CONFIG_DIR="$(mktemp -d)"
}

teardown() {
    rm -rf "$CONFIG_DIR"
}

# Extract the REAL find_helix_macros_cfg out of install.sh and run it with
# SCRIPT_DIR pointed where the test needs it. Sourcing the whole script would
# run main(); re-typing the candidate list here would mean the test passes
# against a lookup list that no longer matches the installer's.
run_find_helix_macros_cfg() {
    local script="${2:-$SCRIPT}"
    local fn
    fn="$(sed -n '/^find_helix_macros_cfg()/,/^}/p' "$script")"
    [ -n "$fn" ] || { echo "find_helix_macros_cfg not found in $script"; return 2; }

    sh -c "SCRIPT_DIR=\"\$1\"
$fn
find_helix_macros_cfg" _ "$1"
}

@test "install.sh exists and has valid POSIX sh syntax" {
    [ -f "$SCRIPT" ]
    sh -n "$SCRIPT"
}

@test "install.sh no longer references the deleted helix_phase_tracking.cfg as a source" {
    # The legacy name may only survive as an uninstall cleanup target, never as
    # a file the installer tries to copy FROM.
    ! grep -qE 'cp .*helix_phase_tracking\.cfg' "$SCRIPT"
}

@test "install.sh installs the include for helix_macros.cfg" {
    grep -q '\[include helix_macros.cfg\]' "$SCRIPT"
}

@test "the cfg install.sh looks for is actually shipped in the repo" {
    # The whole regression: a lookup path that resolves to nothing.
    run run_find_helix_macros_cfg "$(pwd)/moonraker-plugin"
    [ "$status" -eq 0 ]
    [ -f "$output" ]
}

@test "the shipped cfg defines every macro the plugin injects" {
    run run_find_helix_macros_cfg "$(pwd)/moonraker-plugin"
    [ "$status" -eq 0 ]
    cfg="$output"

    # Names come from PHASE_PATTERNS + the HELIX_READY terminator in
    # moonraker-plugin/helix_print.py. Instrumenting against a macro that is not
    # defined here aborts print start with "Unknown command".
    for macro in HELIX_READY \
                 HELIX_PHASE_HOMING \
                 HELIX_PHASE_QGL \
                 HELIX_PHASE_Z_TILT \
                 HELIX_PHASE_BED_MESH \
                 HELIX_PHASE_CLEANING \
                 HELIX_PHASE_PURGING \
                 HELIX_PHASE_HEATING_NOZZLE \
                 HELIX_PHASE_HEATING_BED
    do
        grep -q "^\[gcode_macro $macro\]" "$cfg" || {
            echo "missing [gcode_macro $macro] in $cfg"
            return 1
        }
    done
}

@test "every macro name in PHASE_PATTERNS is covered by the shipped cfg" {
    # Catches a new pattern added to helix_print.py without a matching macro.
    run run_find_helix_macros_cfg "$(pwd)/moonraker-plugin"
    [ "$status" -eq 0 ]
    cfg="$output"

    injected=$(grep -oE '"HELIX_PHASE_[A-Z_]+"' moonraker-plugin/helix_print.py | tr -d '"' | sort -u)
    [ -n "$injected" ]

    for macro in $injected; do
        grep -q "^\[gcode_macro $macro\]" "$cfg" || {
            echo "helix_print.py injects $macro but $cfg does not define it"
            return 1
        }
    done
}

@test "uninstall does not delete the shared helix_macros.cfg" {
    # helix_macros.cfg carries HELIX_START_PRINT / HELIX_CLEAN_NOZZLE and other
    # non-phase-tracking helpers. Removing it on plugin uninstall would break
    # features the user never uninstalled.
    ! grep -qE 'rm .*"?\$?\{?config_dir\}?/helix_macros\.cfg' "$SCRIPT"
}
