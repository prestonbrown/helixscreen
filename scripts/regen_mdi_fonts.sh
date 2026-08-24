#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate Material Design Icons fonts for LVGL
#
# This script generates LVGL font files using MDI (Pictogrammers) icons.
# MDI provides 7200+ icons in a single unified font, eliminating the need
# for FontAwesome's split Solid/Brands/Regular fonts.
#
# MDI uses Unicode Plane 15 Private Use Area (0xF0000+)
# UTF-8 encoding: 4 bytes per codepoint (vs FontAwesome's 3-byte 0xF000 range)
#
# Source: https://pictogrammers.com/library/mdi/
# Font:   https://github.com/Templarian/MaterialDesign-Webfont

set -e
cd "$(dirname "$0")/.."

# Add node_modules/.bin to PATH so lv_font_conv is available
# (same as how npm scripts work)
export PATH="$PWD/node_modules/.bin:$PATH"

FONT=assets/fonts/materialdesignicons-webfont.ttf

# Check font exists
if [ ! -f "$FONT" ]; then
    echo "ERROR: MDI font not found at $FONT"
    echo "Download from: https://github.com/Templarian/MaterialDesign-Webfont/raw/master/fonts/materialdesignicons-webfont.ttf"
    exit 1
fi

# Icon categories and their MDI codepoints
# All codepoints verified from https://pictogrammers.com/library/mdi/
#
# Common icon categories used in this project:
# - Navigation: home, cog, chevron-*, back
# - Status: thermometer, wifi-*, alert, check, close
# - Actions: play, pause, stop, delete, pencil
# - Motion: arrow-*, axis-*-arrow, cursor-move
# - Hardware: fan, printer-3d, engine, power

# MDI icon codepoints (0xF0000 range)
# All icons we need, sorted by codepoint for lv_font_conv efficiency
# Format: 0xFxxxx (MDI uses 5 hex digits in the Private Use Area Plane 15)
#
# Note: MDI is a pure icon font - no ASCII characters included.
# lv_font_conv handles sparse high codepoints fine without ASCII workaround.
# =============================================================================
# Icon codepoints organized by category
# All icons needed for the font-based icon system
# =============================================================================

MDI_ICONS="0xF0026"      # alert (triangle-exclamation)
MDI_ICONS+=",0xF0028"    # alert-circle
MDI_ICONS+=",0xF0029"    # alert-octagon (emergency)
MDI_ICONS+=",0xF05D8"    # animation (stacked rectangles - for settings)
MDI_ICONS+=",0xF093A"    # animation-play (framerate/playback speed)
MDI_ICONS+=",0xF009A"    # bell (notifications)
MDI_ICONS+=",0xF00AD"    # block-helper (prohibited)
MDI_ICONS+=",0xF00E4"    # bug (debug bundle)
MDI_ICONS+=",0xF0232"    # filter (funnel)
MDI_ICONS+=",0xF029A"    # gauge (clog/flow meter tile)
MDI_ICONS+=",0xF00AF"    # bluetooth
MDI_ICONS+=",0xF00B1"    # bluetooth-connect
MDI_ICONS+=",0xF0B5C"    # backspace-outline
MDI_ICONS+=",0xF05DA"    # book-open-page-variant (documentation)
MDI_ICONS+=",0xF0625"    # help-circle-outline (help & support)

# Check/Close/UI
MDI_ICONS+=",0xF0109"    # camera-timer (timelapse)
MDI_ICONS+=",0xF011D"    # tray-arrow-up (unload)
MDI_ICONS+=",0xF012A"    # chart-line
MDI_ICONS+=",0xF012C"    # check
MDI_ICONS+=",0xF0E1E"    # check-bold
MDI_ICONS+=",0xF0132"    # checkbox-marked (filled/checked)
MDI_ICONS+=",0xF0131"    # checkbox-blank-outline (empty/unchecked)
MDI_ICONS+=",0xF0140"    # chevron-down
MDI_ICONS+=",0xF0141"    # chevron-left
MDI_ICONS+=",0xF0142"    # chevron-right
MDI_ICONS+=",0xF0143"    # chevron-up
MDI_ICONS+=",0xF0150"    # clock-outline
MDI_ICONS+=",0xF0156"    # close (xmark)
MDI_ICONS+=",0xF0159"    # close-circle (xmark-circle)
MDI_ICONS+=",0xF0169"    # code-braces
MDI_ICONS+=",0xF0174"    # code-tags
MDI_ICONS+=",0xF018D"    # console
MDI_ICONS+=",0xF018F"    # content-copy (duplicate)
MDI_ICONS+=",0xF01A4"    # crosshairs-gps (probe)
MDI_ICONS+=",0xF01B4"    # delete (trash)
MDI_ICONS+=",0xF01B7"    # debug-step-over (skip objects)
MDI_ICONS+=",0xF01BC"    # database
MDI_ICONS+=",0xF01D9"    # dots-vertical (advanced)
MDI_ICONS+=",0xF01DA"    # download
MDI_ICONS+=",0xF01EA"    # eject (lane eject)
MDI_ICONS+=",0xF0E9F"    # electric-switch (switch sensors)
MDI_ICONS+=",0xF01FA"    # engine (motor)

# Ethernet/Network/Eye
MDI_ICONS+=",0xF0200"    # ethernet
MDI_ICONS+=",0xF0208"    # eye (password visibility toggle)
MDI_ICONS+=",0xF0209"    # eye-off (password visibility toggle)
MDI_ICONS+=",0xF0210"    # fan
MDI_ICONS+=",0xF0238"    # fire
MDI_ICONS+=",0xF0241"    # flash (lightning bolt - heating indicator)
MDI_ICONS+=",0xF024B"    # folder
MDI_ICONS+=",0xF0256"    # folder-outline
MDI_ICONS+=",0xF0259"    # folder-upload
MDI_ICONS+=",0xF0279"    # format-list-bulleted (list)
MDI_ICONS+=",0xF02D7"    # help-circle (question-circle)
MDI_ICONS+=",0xF02DC"    # home
MDI_ICONS+=",0xF02E3"    # bed
MDI_ICONS+=",0xF02FC"    # information (info-circle)
MDI_ICONS+=",0xF02FD"    # information-outline

# Layers/Light
MDI_ICONS+=",0xF0317"    # lan
MDI_ICONS+=",0xF0328"    # layers
MDI_ICONS+=",0xF0335"    # lightbulb
MDI_ICONS+=",0xF0339"    # link (tool mapping)
MDI_ICONS+=",0xF033E"    # lock
MDI_ICONS+=",0xF0FC6"    # lock-open-variant
MDI_ICONS+=",0xF0349"    # magnify (search/discovery)
MDI_ICONS+=",0xF1276"    # magnify-scan (BT discovery)
MDI_ICONS+=",0xF0347"    # magnet (Klicky probe)
MDI_ICONS+=",0xF0369"    # message-text (discord/chat)
MDI_ICONS+=",0xF0374"    # minus
MDI_ICONS+=",0xF0F1B"    # play-outline
MDI_ICONS+=",0xF0415"    # plus
MDI_ICONS+=",0xF046D"    # ruler (print height)
MDI_ICONS+=",0xF03D6"    # package-variant (inventory)
MDI_ICONS+=",0xF03D8"    # palette (color sensors)
MDI_ICONS+=",0xF03E4"    # pause
MDI_ICONS+=",0xF03EB"    # pencil (edit)
MDI_ICONS+=",0xF040A"    # play
MDI_ICONS+=",0xF040C"    # play-circle (resume)
MDI_ICONS+=",0xF0425"    # power (power-off)
MDI_ICONS+=",0xF042A"    # printer
MDI_ICONS+=",0xF042B"    # printer-3d
MDI_ICONS+=",0xF0433"    # qrcode-scan
MDI_ICONS+=",0xF032A"    # leaf
MDI_ICONS+=",0xF0438"    # radiator (heater alternative)
MDI_ICONS+=",0xF044E"    # redo (clockwise arrow - for tighten)
MDI_ICONS+=",0xF0450"    # refresh
MDI_ICONS+=",0xF0465"    # rotate-left (CCW rotation)
MDI_ICONS+=",0xF0467"    # rotate-right (CW rotation)
MDI_ICONS+=",0xF0469"    # router-wireless
MDI_ICONS+=",0xF06A9"    # robot (auto-controlled fan indicator)
MDI_ICONS+=",0xF0479"    # sd (SD card)
MDI_ICONS+=",0xF048A"    # send (for console input)
MDI_ICONS+=",0xF0493"    # cog (settings)
MDI_ICONS+=",0xF04B2"    # sleep (moon/zzz)
MDI_ICONS+=",0xF04C5"    # speedometer
MDI_ICONS+=",0xF04DB"    # stop
MDI_ICONS+=",0xF14F1"    # spirit-level (QGL/Z-Tilt)
MDI_ICONS+=",0xF04E2"    # swap-vertical
MDI_ICONS+=",0xF06E4"    # infinity (endless spool)
MDI_ICONS+=",0xF04E6"    # sync (auto-detect)
MDI_ICONS+=",0xF03C8"    # coolant-temperature
MDI_ICONS+=",0xF050F"    # thermometer
MDI_ICONS+=",0xF0510"    # thermometer-lines
MDI_ICONS+=",0xF054C"    # undo (counter-clockwise arrow - for loosen)
MDI_ICONS+=",0xF0E04"    # thermometer-minus
MDI_ICONS+=",0xF0E05"    # thermometer-plus
MDI_ICONS+=",0xF0F54"    # home-thermometer
MDI_ICONS+=",0xF1531"    # thermometer-off
MDI_ICONS+=",0xF1B2B"    # thermometer-probe

# View/Dashboard
MDI_ICONS+=",0xF0566"    # vibrate (input shaper)
MDI_ICONS+=",0xF0567"    # video (timelapse)
MDI_ICONS+=",0xF0568"    # video-off (timelapse disabled)
MDI_ICONS+=",0xF056E"    # view-dashboard
MDI_ICONS+=",0xF0570"    # view-grid
MDI_ICONS+=",0xF0572"    # view-list
MDI_ICONS+=",0xF057E"    # volume-high
MDI_ICONS+=",0xF0580"    # volume-medium
MDI_ICONS+=",0xF0581"    # volume-off
MDI_ICONS+=",0xF058C"    # water (droplet)
MDI_ICONS+=",0xF05A1"    # weight
MDI_ICONS+=",0xF05A9"    # wifi
MDI_ICONS+=",0xF05AA"    # wifi-off (wifi-slash)
MDI_ICONS+=",0xF05CA"    # translate (language selection)
MDI_ICONS+=",0xF0553"    # usb
MDI_ICONS+=",0xF05E1"    # check-circle-outline (check-circle)
MDI_ICONS+=",0xF062C"    # source-branch (bypass)

# Toolbox/Tune/Update
MDI_ICONS+=",0xF09AD"    # toolbox-outline (tools section header)
MDI_ICONS+=",0xF062E"    # tune
MDI_ICONS+=",0xF06A5"    # power-plug
MDI_ICONS+=",0xF06B0"    # update
MDI_ICONS+=",0xF0709"    # restart (restart HelixScreen)
MDI_ICONS+=",0xF0717"    # snowflake (cooldown)
MDI_ICONS+=",0xF072E"    # arrow-down-bold (z-closer)
MDI_ICONS+=",0xF0731"    # arrow-left-bold
MDI_ICONS+=",0xF0734"    # arrow-right-bold
MDI_ICONS+=",0xF0737"    # arrow-up-bold (z-farther)
MDI_ICONS+=",0xF17BF"    # arrow-up-right (retraction)
MDI_ICONS+=",0xF073A"    # cancel
MDI_ICONS+=",0xF0758"    # grid-large
MDI_ICONS+=",0xF02C2"    # grid-off (no mesh empty state)
MDI_ICONS+=",0xF0770"    # folder-open
MDI_ICONS+=",0xF0792"    # arrow-collapse-down (flow-down)
MDI_ICONS+=",0xF0CE1"    # arrow-up-circle (load/activate)
MDI_ICONS+=",0xF0901"    # power-cycle
MDI_ICONS+=",0xF0902"    # power-off
MDI_ICONS+=",0xF0903"    # power-on
MDI_ICONS+=",0xF0906"    # power-standby
MDI_ICONS+=",0xF0907"    # rabbit (Happy Hare logo)
MDI_ICONS+=",0xF0427"    # power-socket
MDI_ICONS+=",0xF0905"    # power-socket-au
MDI_ICONS+=",0xF0FB3"    # power-socket-ch
MDI_ICONS+=",0xF1107"    # power-socket-de
MDI_ICONS+=",0xF07E7"    # power-socket-eu
MDI_ICONS+=",0xF1108"    # power-socket-fr
MDI_ICONS+=",0xF14FF"    # power-socket-it
MDI_ICONS+=",0xF1109"    # power-socket-jp
MDI_ICONS+=",0xF07E8"    # power-socket-uk
MDI_ICONS+=",0xF07E9"    # power-socket-us
MDI_ICONS+=",0xF06A6"    # power-plug-off
MDI_ICONS+=",0xF1425"    # power-plug-outline
MDI_ICONS+=",0xF1C3B"    # power-plug-battery
MDI_ICONS+=",0xF0796"    # arrow-expand-down (bed drops - CoreXY Z closer)
MDI_ICONS+=",0xF0795"    # arrow-collapse-up (flow-up)
MDI_ICONS+=",0xF0799"    # arrow-expand-up (bed rises - CoreXY Z farther)
MDI_ICONS+=",0xF081D"    # fan-off
MDI_ICONS+=",0xF1542"    # tune-variant (controls)
MDI_ICONS+=",0xF1543"    # tune-vertical-variant (scroll momentum)

# WiFi strength icons
MDI_ICONS+=",0xF091F"    # wifi-strength-1
MDI_ICONS+=",0xF0920"    # wifi-strength-1-alert
MDI_ICONS+=",0xF0921"    # wifi-strength-1-lock
MDI_ICONS+=",0xF0922"    # wifi-strength-2
MDI_ICONS+=",0xF0924"    # wifi-strength-2-lock
MDI_ICONS+=",0xF0925"    # wifi-strength-3
MDI_ICONS+=",0xF0927"    # wifi-strength-3-lock
MDI_ICONS+=",0xF0928"    # wifi-strength-4
MDI_ICONS+=",0xF092A"    # wifi-strength-4-lock
MDI_ICONS+=",0xF095B"    # sine-wave (input shaper)
MDI_ICONS+=",0xF0996"    # progress-clock (phase tracking)
MDI_ICONS+=",0xF022F"    # film (filament - film reel icon)
MDI_ICONS+=",0xF0A46"    # engine-off (motor-off)
MDI_ICONS+=",0xF0A66"    # puzzle-outline (plugin)
MDI_ICONS+=",0xF0A7A"    # trash-can-outline (delete)
MDI_ICONS+=",0xF0BEC"    # alpha-a-circle (auto indicator)
MDI_ICONS+=",0xF0BC2"    # script-text (macro buttons)
MDI_ICONS+=",0xF0D3B"    # tortoise (AFC/Box Turtle logo)
MDI_ICONS+=",0xF0D49"    # axis-arrow (all 3 axes)
MDI_ICONS+=",0xF07F4"    # television-classic (screensaver)
MDI_ICONS+=",0xF0D91"    # motion-sensor (sensor placeholder)
MDI_ICONS+=",0xF0D4C"    # axis-x-arrow
MDI_ICONS+=",0xF0D51"    # axis-y-arrow
MDI_ICONS+=",0xF0D55"    # axis-z-arrow
MDI_ICONS+=",0xF0E4F"    # lightbulb-off

# Folder navigation icons (for file browser)
MDI_ICONS+=",0xF19F0"    # folder-arrow-up (parent directory)
MDI_ICONS+=",0xF19F3"    # folder-arrow-up-outline
MDI_ICONS+=",0xF10B5"    # folder-home (install root in About)

# Graduated lightbulb icons (brightness levels)
MDI_ICONS+=",0xF0336"    # lightbulb-outline (OFF state)
MDI_ICONS+=",0xF1A4E"    # lightbulb-on-10
MDI_ICONS+=",0xF1A4F"    # lightbulb-on-20
MDI_ICONS+=",0xF1A50"    # lightbulb-on-30
MDI_ICONS+=",0xF1A51"    # lightbulb-on-40
MDI_ICONS+=",0xF1A52"    # lightbulb-on-50
MDI_ICONS+=",0xF1A53"    # lightbulb-on-60
MDI_ICONS+=",0xF1A54"    # lightbulb-on-70
MDI_ICONS+=",0xF1A55"    # lightbulb-on-80
MDI_ICONS+=",0xF1A56"    # lightbulb-on-90
MDI_ICONS+=",0xF06E8"    # lightbulb-on (100%)
MDI_ICONS+=",0xF07D6"    # led-strip (LED controls widget)

MDI_ICONS+=",0xF0E5B"    # printer-3d-nozzle (extruder)
MDI_ICONS+=",0xF11C0"    # printer-3d-nozzle-alert (filament sensor empty)
MDI_ICONS+=",0xF0EC7"    # rotate-3d (orbit/3D view rotation)
MDI_ICONS+=",0xF0F85"    # speedometer-medium (speed-up)
MDI_ICONS+=",0xF0F86"    # speedometer-slow (speed-down)
MDI_ICONS+=",0xF0F9C"    # home-import-outline (home-z)
MDI_ICONS+=",0xF128D"    # toy-brick-outline (building block)
MDI_ICONS+=",0xF1323"    # hammer-wrench (tools/build)
MDI_ICONS+=",0xF147D"    # waveform (accelerometers)
MDI_ICONS+=",0xF16B5"    # wifi-alert
MDI_ICONS+=",0xF16BD"    # wifi-check
MDI_ICONS+=",0xF16BF"    # wifi-lock
MDI_ICONS+=",0xF18B8"    # printer-3d-nozzle-heat (heater)
MDI_ICONS+=",0xF02EB"    # image-area (photo/picture)
MDI_ICONS+=",0xF02EE"    # image-broken-variant (missing/broken image fallback)
MDI_ICONS+=",0xF1274"    # inbox-outline
MDI_ICONS+=",0xF01A7"    # cube-outline
MDI_ICONS+=",0xF04CE"    # star (favorites/recent)
MDI_ICONS+=",0xF1A45"    # heat-wave (for heated bed icon)
MDI_ICONS+=",0xF15EE"    # fridge-industrial (chamber temperature)
MDI_ICONS+=",0xF1B35"    # train-car-flatbed (print bed base)

# Custom icons
MDI_ICONS+=",0xF01BE"    # cursor-move (4-way movement cross)
MDI_ICONS+=",0xF004C"    # arrow-expand-all (expand_all/move)
MDI_ICONS+=",0xF051F"    # timer-sand (hourglass)

# Arrow icons for motion control (using MDI arrows)
MDI_ICONS+=",0xF005D"    # arrow-up
MDI_ICONS+=",0xF0045"    # arrow-down
MDI_ICONS+=",0xF004D"    # arrow-left
MDI_ICONS+=",0xF0054"    # arrow-right
MDI_ICONS+=",0xF0042"    # arrow-bottom-left
MDI_ICONS+=",0xF0043"    # arrow-bottom-right
MDI_ICONS+=",0xF005B"    # arrow-top-left
MDI_ICONS+=",0xF005C"    # arrow-top-right
MDI_ICONS+=",0xF004E"    # arrow-left-thick
MDI_ICONS+=",0xF005F"    # arrow-up-bold-circle
MDI_ICONS+=",0xF0E73"    # arrow-left-right (bidirectional horizontal)
MDI_ICONS+=",0xF0E79"    # arrow-up-down (bidirectional vertical)

# Touch calibration
MDI_ICONS+=",0xF04FE"    # target (touch calibration target)

# Keyboard icons (for on-screen keyboard special keys)
MDI_ICONS+=",0xF030C"    # keyboard (keyboard layout settings)
MDI_ICONS+=",0xF030F"    # keyboard-close (dismiss keyboard)
MDI_ICONS+=",0xF0311"    # keyboard-return (enter key)
MDI_ICONS+=",0xF0632"    # apple-keyboard-caps (caps lock indicator)
MDI_ICONS+=",0xF0636"    # apple-keyboard-shift (shift key outline)

# Sizes to generate - matching text font sizes for consistent UI
SIZES="14 16 24 32 48 64 80 96 128"

echo "Generating Material Design Icons fonts for LVGL..."
echo "  Font: $FONT"
echo ""

for SIZE in $SIZES; do
    OUTPUT="assets/fonts/mdi_icons_${SIZE}.c"
    echo "  Generating mdi_icons_${SIZE} -> $OUTPUT"

    lv_font_conv \
        --font "$FONT" --size "$SIZE" --bpp 4 --format lvgl \
        --range "$MDI_ICONS" \
        --no-compress \
        -o "$OUTPUT"

    # De-const mdi_icons_16/24/32/48/64: these five faces are runtime-populated
    # on embedded targets (ESP32 loads their glyph data from a .bin and
    # struct-copies into the symbol at boot; Plan A fonts->frogfs move), so
    # ui_fonts.h declares them non-const. The .c definition must match or gcc
    # errors with "conflicting type qualifiers". The remaining sizes (14/80/96/
    # 128) are NOT moved and stay const. Mirrors the strip in
    # scripts/regen_text_fonts.sh.
    if [ "$SIZE" = "16" ] || [ "$SIZE" = "24" ] || [ "$SIZE" = "32" ] || \
       [ "$SIZE" = "48" ] || [ "$SIZE" = "64" ]; then
        if [[ "$OSTYPE" == "darwin"* ]]; then
            sed -i '' 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
        else
            sed -i 's/^const lv_font_t /lv_font_t /' "$OUTPUT"
        fi
    fi
done

echo ""
echo "Done! Generated fonts for sizes: $SIZES"
echo ""
echo "Fonts are ready. If you added new icons, remember to:"
echo "  1. Add mapping to include/ui_icon_codepoints.h"
echo "  2. Rebuild the project: make -j"
