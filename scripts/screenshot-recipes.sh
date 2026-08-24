#!/bin/bash

# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later

# Navigation recipes for the screenshot pipeline.
#
# Each token maps to a sequence of `helix-screen ctl` commands (semicolon-separated) that
# brings the UI to the screen we want to capture, run from a clean base (no open
# overlays). Base panels are a single `navigate`; overlays are a `navigate` into
# a base panel followed by one or more `click`s that fire the real open handler
# (real widget lifecycle — init_subjects/create/on_activate — not an empty shell).
#
# A handful of screens only appear in response to a real printer event (pre-print
# check, filament runout, an active print) or configured state (a lock PIN), so
# they can't be reached by navigation in mock mode. Those use `demo <name>`, which
# constructs representative sample data and shows the overlay with real lifecycle.
#
# This table is the single source of truth, sourced by both screenshot.sh (single
# shot) and screenshot-all.sh (long-lived batch). Discover a new recipe by driving
# the app: launch `helix-screen --test --skip-wizard --remote`, then
# `helix-screen ctl navigate <panel>`, `helix-screen ctl ls`, find the trigger widget.

# One record per line: "<token>  <recipe>", first whitespace run separating
# them. A plain string rather than a bash associative array because macOS
# ships bash 3.2, which supports neither `declare -A` nor `declare -g` — and
# fails *silently* on them (the shell reports an error but still exits 0), so
# every macOS caller read this table as empty. That took out screenshot.sh,
# screenshot-all.sh and tests/ui/test_screens.py, the last of which surfaced
# it only as a KeyError a hundred lines away from the cause.
#
# Comment lines and blank lines are ignored by both accessors below.
SCREENSHOT_RECIPES="
# Base panels
home               navigate home
controls           navigate controls
filament           navigate filament
settings           navigate settings
advanced           navigate advanced
print-select       navigate print-select

# Control overlays
motion             navigate controls; click btn_motion
nozzle-temp        navigate controls; click btn_nozzle_temp
temperature        navigate controls; click btn_nozzle_temp
bed-temp           navigate controls; click btn_bed_temp
extrusion          navigate filament
fan                navigate controls; click card_cooling
bed-mesh           navigate controls; click btn_bed_mesh
pid                navigate advanced; click row_pid_tuning

# Filament / AMS (the filament panel's AMS row no-ops without a configured
# backend, so the dedicated management panel is reached via demo)
ams                demo ams

# Settings overlays (settings panel groups leaves under category rows)
display            navigate settings; click row_display_sound
theme              navigate settings; click row_display_sound; click row_theme_settings
sensors            navigate settings; click row_hardware; click row_filament_sensors
network            navigate settings; click row_system; click row_network
hardware-health    navigate settings; click row_hardware; click row_hardware_health
fan-settings       navigate settings; click row_hardware; click row_fan_settings
barcode-scanner    navigate settings; click row_hardware; click row_spoolman_settings; click row_barcode_scanner
label-printer      navigate settings; click row_hardware; click row_spoolman_settings; click row_label_printer
security           navigate settings; click row_system; click row_security
safety             navigate settings; click row_safety

# Advanced overlays
input-shaper       navigate advanced; click row_input_shaping
screws             navigate advanced; click row_bed_mesh
zoffset            navigate advanced; click row_z_offset
spoolman           navigate advanced; click row_spoolman
history-dashboard  navigate advanced; click row_print_history
macros             navigate advanced; click row_macros
console            navigate advanced; click row_console

# Sample-data / event-only screens (need programmatic setup — see demo command)
lock-screen        demo lock-screen
print-status       demo print-status
print-tune         demo print-tune
preflight-check    demo preflight-check
color-mismatch     demo color-mismatch
runout-modal       demo runout-modal
camera             demo camera
"

# Resolve a token to its recipe. Falls back to `navigate <token>` for anything
# not in the table (helix-screen ctl resolves a bare panel or on-screen widget name),
# so ad-hoc single-panel captures keep working without a table entry.
screenshot_recipe_for() {
    local token="$1"
    local recipe
    recipe="$(printf '%s\n' "$SCREENSHOT_RECIPES" |
        awk -v want="$token" '$1 == want { $1 = ""; sub(/^[ \t]+/, ""); print; exit }')"
    if [ -n "$recipe" ]; then
        printf '%s' "$recipe"
    else
        printf 'navigate %s' "$token"
    fi
}

# Every token in the table, one per line, in file order.
screenshot_recipe_tokens() {
    printf '%s\n' "$SCREENSHOT_RECIPES" | awk 'NF && $1 !~ /^#/ { print $1 }'
}
