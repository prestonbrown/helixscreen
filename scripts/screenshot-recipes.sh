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

# Printer image callouts (home widget, prestonbrown/helixscreen#1397).
# callouts-2x2 and callouts-untagged just need the widget's natural default
# span, so --printer selects a tagged vs untagged printer image. callouts-4x2 needs
# a 4x2-cell span. On the default mock printer at 800x480 the cells it grows
# into hold temperature and bed_temperature beside the image and ams below
# them. ctl home remove frees those cells by widget id and ctl home resize
# widens the image (docs/devel/HELIXCTL.md, section Home-grid layout without
# gestures), so the recipe carries no pixel coordinates; a default that puts
# other widgets in those cells needs their ids here instead.
# tests/ui/test_callouts_recipe.py runs it against the default.
callouts-2x2       navigate home
callouts-4x2       navigate home; home remove temperature; home remove bed_temperature; home remove ams; home resize printer_image 4 2; wait_idle
callouts-untagged  navigate home

# Home widget catalog (docs/user/guide/home-panel.md). A long press on the grid
# enters Edit Mode, then the nav bar plus button (nav_btn_edit_add) opens the
# catalog; a second long press would grab the selected tile instead
# (docs/devel/HELIXCTL.md, section Home-grid Edit Mode and the widget catalog).
# The category rows share the name action_row, so the Filament category is
# picked by its index, which follows the WidgetCategory order in
# include/panel_widget_registry.h. The default 800x480 layout leaves no free
# cell, which dims every row with a needs-space note, so the category shot
# removes fan_stack first. The long press lands inside printer_image, which every
# default places at the grid origin and spans past x=200,y=111 at 800x480.
widget-catalog           navigate home; long_press 200 111; click nav_btn_edit_add
widget-catalog-category  navigate home; home remove fan_stack; wait_idle; long_press 200 111; click nav_btn_edit_add; wait_idle; click action_row[2]

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
# The multi-unit overview (any backend with two or more units; the 12-unit
# HELIX_MOCK_OPENAMS_UNITS=fleet mock fills the cards row and shows both stubs and the drying
# glyph in the unit view). Tapping a unit card zooms into the unit view on that unit's page:
# ams-pages taps the third card, ams-pages-next steps one page forward with the right arrow,
# ams-pages-last pages to the end, where the right arrow is gone.
ams-pages          demo ams; wait_idle; click ams_unit_card[2]
ams-pages-next     demo ams; wait_idle; click ams_unit_card[2]; wait_idle; click page_next_button
ams-pages-last     demo ams; wait_idle; click ams_unit_card[2]; wait_idle; click page_next_button; wait_idle; click page_next_button; wait_idle; click page_next_button; wait_idle; click page_next_button; wait_idle; click page_next_button; wait_idle; click page_next_button; wait_idle; click page_next_button; wait_idle; click page_next_button; wait_idle; click page_next_button
# ams-cycle loads lane 2 from its context menu and unloads it from the sidebar,
# waiting out each operation, so --repeat drives the AMS sync paths under a
# sanitizer (make tsan-app RECIPE=ams-cycle). reset first: a second demo ams
# would stack a new panel over the old one.
ams-cycle          reset; demo ams; wait_idle; click s/ams_panel/overlay_content/left_column/ams_unit_card/slot_area/slots_wrapper/unit_detail/slot_container/slot_grid/ams_slot_view[1]; click s/context_backdrop/context_menu/menu_columns/col_filament/btn_load; wait_for ams_filament_loaded 1 --timeout 240; wait_for ams_action 0 --timeout 240; click s/ams_panel/right_column/ams_operation_sidebar/action_buttons_container/btn_unload; wait_for ams_filament_loaded 0 --timeout 240; wait_for ams_action 0 --timeout 240

# Settings overlays (settings panel groups leaves under category rows).
# A -2/-3 token shows the part of a long page a 480-tall screen cannot. The
# wait_idle lets the queued overlay push finish first, since activating the
# page resets its scroll position. ctl scroll moves only the target's direct
# scroll parent and leaves a group taller than the viewport where it is, so
# most of these scroll overlay_content by a distance measured at 800x480;
# appearance-2 brings its short last group into view instead.
settings-printer   navigate settings; scroll group_printer
settings-helixscreen navigate settings; scroll group_helixscreen
display            navigate settings; click row_display
display-2          navigate settings; click row_display; wait_idle; scroll overlay_content 0 -274
appearance         navigate settings; click row_appearance
appearance-2       navigate settings; click row_appearance; wait_idle; scroll group_printer_visuals
theme              navigate settings; click row_appearance; click row_theme_settings
touch-input        navigate settings; click row_touch_input
touch-input-2      navigate settings; click row_touch_input; wait_idle; scroll overlay_content 0 -333
sound              navigate settings; click row_sound
sound-2            navigate settings; click row_sound; wait_idle; scroll overlay_content 0 -190
printing           navigate settings; click row_printing
printing-2         navigate settings; click row_printing; wait_idle; scroll overlay_content 0 -406
printing-3         navigate settings; click row_printing; wait_idle; scroll overlay_content 0 -832
devices            navigate settings; click row_devices
devices-2          navigate settings; click row_devices; wait_idle; scroll overlay_content 0 -209
sensors            navigate settings; click row_devices; click row_filament_sensors
hardware-health    navigate settings; click row_devices; click row_hardware_health
fan-settings       navigate settings; click row_devices; click row_fan_settings
barcode-scanner    navigate settings; click row_devices; click row_spoolman_settings; click row_barcode_scanner
label-printer      navigate settings; click row_devices; click row_spoolman_settings; click row_label_printer
safety             navigate settings; click row_safety
connection         navigate settings; click row_connection
network            navigate settings; click row_connection; click row_network
printers           navigate settings; click row_connection; click row_printers
language-time      navigate settings; click row_language_time
system             navigate settings; click row_system
system-2           navigate settings; click row_system; wait_idle; scroll overlay_content 0 -124
security           navigate settings; click row_system; click row_security
updates            navigate settings; click row_updates
help-about         navigate settings; click row_help
about              navigate settings; click row_help; click row_about
help-qr            navigate settings; click row_help; click row_discord; click btn_ok

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
preflight-material demo preflight-material
color-mismatch     demo color-mismatch
runout-modal       demo runout-modal
leds               demo leds
camera             demo camera
action-prompt-zmod demo action-prompt-zmod
action-prompt-afc-cal demo action-prompt-afc-cal
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
