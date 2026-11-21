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

# A Creality K1-series box: HOME is /root with no Moonraker in it, the Moonraker
# repo is nested inside /usr/data/moonraker, and printer_data is under
# /usr/data. Only the running process says where any of it is.
setup_creality_k1_layout() {
    local root="$1"
    local pkg="$root/usr/data/moonraker/moonraker/moonraker"
    local data="$root/usr/data/printer_data"
    mkdir -p "$pkg/components" "$data/config" "$root/home" "$root/proc/1444"
    : > "$pkg/moonraker.py"
    printf '[server]\nhost: 0.0.0.0\n' > "$data/config/moonraker.conf"
    printf '%s\0' "$root/usr/data/moonraker/moonraker-env/bin/python" "$pkg/moonraker.py" \
        -d "$data" > "$root/proc/1444/cmdline"
    for cmd in sudo service curl; do
        mock_command_script "$cmd" 'exit 0'
    done
}

@test "auto install finds a Creality K1 Moonraker from its running process" {
    local root="$BATS_TEST_TMPDIR/k1"
    setup_creality_k1_layout "$root"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" sh "$SCRIPT" --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    [ -L "$root/usr/data/moonraker/moonraker/moonraker/components/helix_print.py" ]
    grep -q '^\[helix_print\]' "$root/usr/data/printer_data/config/moonraker.conf"
}

@test "a plugin installed through a package's latest link stays linked through latest" {
    # PAXX's extended-pkg replaces the version dir on every upgrade and re-points
    # latest at the new one; a link through latest survives that (#2136).
    local root="$BATS_TEST_TMPDIR/k1"
    setup_creality_k1_layout "$root"
    local apps="$root/oem/apps/helixscreen"
    local repo="$BATS_TEST_DIRNAME/../.."
    mkdir -p "$apps/helixscreen-1.0.5/moonraker-plugin"
    cp "$repo"/moonraker-plugin/*.sh "$repo"/moonraker-plugin/*.py \
        "$apps/helixscreen-1.0.5/moonraker-plugin/"
    ln -s helixscreen-1.0.5 "$apps/latest"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" \
        sh "$apps/latest/moonraker-plugin/install.sh" --auto
    [ "$status" -eq 0 ] || fail "install failed: $output"
    local link="$root/usr/data/moonraker/moonraker/moonraker/components/helix_print.py"
    [ "$(readlink "$link")" = "$apps/latest/moonraker-plugin/helix_print.py" ] \
        || fail "link names $(readlink "$link"), not the path through latest"
}

@test "auto uninstall finds a Creality K1 Moonraker from its running process" {
    local root="$BATS_TEST_TMPDIR/k1"
    setup_creality_k1_layout "$root"
    local target="$root/usr/data/moonraker/moonraker/moonraker/components/helix_print.py"
    ln -s "$PWD/moonraker-plugin/helix_print.py" "$target"

    run env HOME="$root/home" HELIX_PROC_ROOT="$root/proc" sh "$SCRIPT" --uninstall-auto
    [ "$status" -eq 0 ] || fail "uninstall failed: $output"
    [ ! -e "$target" ]
}

@test "plugin installer searches every Moonraker root the main installer knows" {
    local roots
    roots=$(sed -n '/^MOONRAKER_SRC_PATHS="/,/^"/p' scripts/lib/installer/moonraker.sh | grep '^/')
    [ -n "$roots" ] || fail "no roots parsed from scripts/lib/installer/moonraker.sh"
    local plugin_roots
    plugin_roots=$(sed -n '/^MOONRAKER_SRC_PATHS="/,/^"/p' "$SCRIPT" | grep '^/')
    local r
    for r in $roots; do
        printf '%s\n' "$plugin_roots" | grep -qx "$r" || fail "plugin install.sh misses Moonraker root $r"
    done
}
