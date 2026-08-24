#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate RLE-COMPRESSED, firmware-local twins of the 11 LVGL font faces
# used by the ESP32 build of HelixScreen.
#
# These .c files are the COMPRESSED counterparts of the (uncompressed on the
# desktop side) faces in assets/fonts/. They are regenerated from the EXACT
# same TTF/OTF sources, sizes, Unicode ranges, bpp, and format as
# scripts/regen_text_fonts.sh and scripts/regen_mdi_fonts.sh, with ONE
# difference: lv_font_conv's default RLE compression is left ENABLED (the
# desktop scripts pass --no-compress). Compression trades a small runtime
# decode cost for a large flash saving, which matters on the ESP32's limited
# flash budget but not on desktop.
#
# Output: firmware/helixscreen-esp32/components/helixcore/fonts/<face>.c
#   The firmware build points HELIX_FONT_SRCS
#   (firmware/helixscreen-esp32/components/helixcore/CMakeLists.txt) at these
#   files instead of the desktop assets/fonts/*.c originals.
#
# Symbol names, glyph sets, sizes, and bpp are IDENTICAL to the desktop
# originals so the firmware externs/aliases resolve unchanged. Const-ness of
# the top-level `lv_font_t <name>` symbol is also matched to the desktop
# originals AND to the firmware externs in
# firmware/helixscreen-esp32/components/helixcore/lv_conf.h. After Plan A all
# faces except noto_sans_18 are MOVED (runtime .bin + writable shim), so all of
# them are const-stripped:
#   - noto_sans_* faces  -> `lv_font_t <name>`  (const stripped, always were)
#   - source_code_pro_14 -> `lv_font_t <name>`  (const stripped: moved face)
#   - mdi_icons_*  faces -> `lv_font_t <name>`  (const stripped: moved faces)
# The .bin twins (frogfs faces loaded at boot) are emitted for the moved faces:
# noto_sans_26, noto_sans_bold_28, noto_sans_light_16, noto_sans_light_12,
# source_code_pro_14, and mdi_icons_16/24/32/48/64 (10 total). noto_sans_18 is
# the sole compiled-in anchor and has no .bin twin.
#
# This script does NOT touch anything under assets/fonts/.

set -euo pipefail
cd "$(dirname "$0")/.."

# --- lv_font_conv (v1.5.3) on PATH -------------------------------------------
# Use the project-local npm install, matching regen_text_fonts.sh /
# regen_mdi_fonts.sh — not a personal nvm path.
export PATH="$PWD/node_modules/.bin:$PATH"
if ! command -v lv_font_conv >/dev/null 2>&1; then
    echo "ERROR: lv_font_conv not found on PATH" >&2
    exit 1
fi

OUT_DIR="firmware/helixscreen-esp32/components/helixcore/fonts"
mkdir -p "$OUT_DIR"

# --- Font source files -------------------------------------------------------
FONT_REGULAR=assets/fonts/NotoSans-Regular.ttf
FONT_LIGHT=assets/fonts/NotoSans-Light.ttf
FONT_BOLD=assets/fonts/NotoSans-Bold.ttf
FONT_MONO=assets/fonts/SourceCodePro-Regular.ttf
FONT_CJK_SC=assets/fonts/NotoSansCJKsc-Regular.otf
FONT_CJK_JP=assets/fonts/NotoSansCJKjp-Regular.otf
FONT_MDI=assets/fonts/materialdesignicons-webfont.ttf

# CJK font download URLs (Google Noto CJK releases) - matches regen_text_fonts.sh
CJK_SC_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
CJK_JP_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf"

download_cjk_font() {
    local url="$1" dest="$2" name
    name=$(basename "$dest")
    [ -f "$dest" ] && return 0
    echo "Downloading $name..."
    if command -v curl >/dev/null 2>&1; then
        curl -fSL --progress-bar -o "$dest" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$dest" "$url"
    else
        echo "ERROR: Neither curl nor wget found - cannot download $name" >&2
        return 1
    fi
    [ -s "$dest" ] || { echo "ERROR: Download failed for $name" >&2; rm -f "$dest"; return 1; }
}

for FONT in "$FONT_REGULAR" "$FONT_LIGHT" "$FONT_BOLD" "$FONT_MONO" "$FONT_MDI"; do
    [ -f "$FONT" ] || { echo "ERROR: Font not found: $FONT" >&2; exit 1; }
done
if [ ! -f "$FONT_CJK_SC" ] || [ ! -f "$FONT_CJK_JP" ]; then
    echo "CJK fonts not found - downloading from GitHub notofonts/noto-cjk..."
    download_cjk_font "$CJK_SC_URL" "$FONT_CJK_SC" || { echo "ERROR: CJK SC font is REQUIRED." >&2; exit 1; }
    download_cjk_font "$CJK_JP_URL" "$FONT_CJK_JP" || { echo "ERROR: CJK JP font is REQUIRED." >&2; exit 1; }
fi

# --- Ranges (verbatim from regen_text_fonts.sh / regen_mdi_fonts.sh) ---------

# Wizard welcome page CJK codepoints - always compiled into the .c fonts.
# 欢迎！中文ようこそ！日本語
WIZARD_CJK="0x3046,0x3053,0x305d,0x3088,0x4e2d,0x6587,0x65e5,0x672c,0x6b22,0x8a9e,0x8fce,0xff01"

# Unicode ranges for Latin/Cyrillic (Noto text faces + Source Code Pro)
UNICODE_RANGES=""
UNICODE_RANGES+="0x20-0x7F"      # Basic Latin (ASCII)
UNICODE_RANGES+=",0xA0-0xFF"     # Latin-1 Supplement (Western European)
UNICODE_RANGES+=",0x100-0x17F"   # Latin Extended-A (Central European)
UNICODE_RANGES+=",0x400-0x4FF"   # Cyrillic (Russian, Ukrainian, etc.)
UNICODE_RANGES+=",0x2013-0x2014" # En/Em dashes
UNICODE_RANGES+=",0x2018-0x201D" # Smart quotes
UNICODE_RANGES+=",0x2022"        # Bullet
UNICODE_RANGES+=",0x2026"        # Ellipsis
UNICODE_RANGES+=",0x20AC"        # Euro sign
UNICODE_RANGES+=",0x2122"        # Trademark

# MDI icon codepoints (0xF0000 range) - verbatim from regen_mdi_fonts.sh
MDI_ICONS="0xF0026"      # alert (triangle-exclamation)
MDI_ICONS+=",0xF0028"    # alert-circle
MDI_ICONS+=",0xF0029"    # alert-octagon (emergency)
MDI_ICONS+=",0xF05D8"    # animation (stacked rectangles - for settings)
MDI_ICONS+=",0xF093A"    # animation-play (framerate/playback speed)
MDI_ICONS+=",0xF009A"    # bell (notifications)
MDI_ICONS+=",0xF00AD"    # block-helper (prohibited)
MDI_ICONS+=",0xF00E4"    # bug (debug bundle)
MDI_ICONS+=",0xF00AF"    # bluetooth
MDI_ICONS+=",0xF00B1"    # bluetooth-connect
MDI_ICONS+=",0xF0B5C"    # backspace-outline
MDI_ICONS+=",0xF05DA"    # book-open-page-variant (documentation)
MDI_ICONS+=",0xF0625"    # help-circle-outline (help & support)
MDI_ICONS+=",0xF0109"    # camera-timer (timelapse)
MDI_ICONS+=",0xF011D"    # tray-arrow-up (unload)
MDI_ICONS+=",0xF012A"    # chart-line
MDI_ICONS+=",0xF012C"    # check
MDI_ICONS+=",0xF0E1E"    # check-bold
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
MDI_ICONS+=",0xF01BC"    # database
MDI_ICONS+=",0xF01D9"    # dots-vertical (advanced)
MDI_ICONS+=",0xF01DA"    # download
MDI_ICONS+=",0xF01EA"    # eject (lane eject)
MDI_ICONS+=",0xF0E9F"    # electric-switch (switch sensors)
MDI_ICONS+=",0xF01FA"    # engine (motor)
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
MDI_ICONS+=",0xF19F0"    # folder-arrow-up (parent directory)
MDI_ICONS+=",0xF19F3"    # folder-arrow-up-outline
MDI_ICONS+=",0xF10B5"    # folder-home (install root in About)
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
MDI_ICONS+=",0xF01BE"    # cursor-move (4-way movement cross)
MDI_ICONS+=",0xF004C"    # arrow-expand-all (expand_all/move)
MDI_ICONS+=",0xF051F"    # timer-sand (hourglass)
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
MDI_ICONS+=",0xF04FE"    # target (touch calibration target)
MDI_ICONS+=",0xF030C"    # keyboard (keyboard layout settings)
MDI_ICONS+=",0xF030F"    # keyboard-close (dismiss keyboard)
MDI_ICONS+=",0xF0311"    # keyboard-return (enter key)
MDI_ICONS+=",0xF0632"    # apple-keyboard-caps (caps lock indicator)
MDI_ICONS+=",0xF0636"    # apple-keyboard-shift (shift key outline)

# --- Const-strip helper (Linux sed) ------------------------------------------
# Applied ONLY to the Noto text faces, matching regen_text_fonts.sh and the
# `extern lv_font_t noto_sans_*` (non-const) declarations in lv_conf.h. The
# CjkFontManager sets fallback pointers on these at runtime, so they must not
# be const. source_code_pro_14 and mdi_icons_* stay const (extern const in
# lv_conf.h; the desktop scripts do not strip them).
strip_const() { sed -i 's/^const lv_font_t /lv_font_t /' "$1"; }

echo "Generating COMPRESSED firmware-local font twins -> $OUT_DIR"
echo ""

# --- Noto text faces (Latin/Cyrillic + 12-codepoint CJK wizard subset) -------
# Same invocation as regen_text_fonts.sh minus --no-compress; then strip const.

# Passing "bin" as the 4th arg ALSO emits a runtime-loadable .bin twin (same
# glyphs/ranges/bpp, lv_font_conv --format bin) alongside the compiled .c. The
# .bin twins are the faces moved out of the app image into the frogfs `storage`
# partition (loaded at boot via lv_binfont_create); the .c stays generated so
# the face can be moved back into the compile without a regen.
gen_noto() { # <src_ttf> <size> <outname> [bin]
    local src="$1" size="$2" name="$3" emit_bin="${4:-}" out="$OUT_DIR/$3.c"
    echo "  $name (compressed) -> $out"
    lv_font_conv \
        --font "$src"        --size "$size" --range "$UNICODE_RANGES" \
        --font "$FONT_CJK_SC" --size "$size" --range "$WIZARD_CJK" \
        --font "$FONT_CJK_JP" --size "$size" --range "$WIZARD_CJK" \
        --bpp 4 --format lvgl \
        -o "$out"
    strip_const "$out"
    if [ "$emit_bin" = "bin" ]; then
        echo "  $name (compressed) -> $OUT_DIR/$name.bin  [frogfs twin]"
        lv_font_conv \
            --font "$src"        --size "$size" --range "$UNICODE_RANGES" \
            --font "$FONT_CJK_SC" --size "$size" --range "$WIZARD_CJK" \
            --font "$FONT_CJK_JP" --size "$size" --range "$WIZARD_CJK" \
            --bpp 4 --format bin \
            -o "$OUT_DIR/$name.bin"
    fi
}

gen_noto "$FONT_REGULAR" 26 noto_sans_26 bin
gen_noto "$FONT_BOLD"    28 noto_sans_bold_28 bin
gen_noto "$FONT_REGULAR" 18 noto_sans_18
gen_noto "$FONT_LIGHT"   16 noto_sans_light_16 bin
gen_noto "$FONT_LIGHT"   12 noto_sans_light_12 bin

# --- Source Code Pro monospace ----------------------------------------------
# regen_text_fonts.sh omits --no-compress for this face (so the desktop asset is
# itself compressed). source_code_pro_14 is now a MOVED face: its glyph data
# lives in a runtime .bin (frogfs twin) and its symbol is the writable shim in
# moved_fonts_shim.c, so ui_fonts.h / lv_conf.h declare it non-const. Strip
# const on the .c twin to match (so a move-back into the compile stays
# qualifier-clean) and emit the .bin twin alongside.
echo "  source_code_pro_14 (compressed) -> $OUT_DIR/source_code_pro_14.c"
lv_font_conv \
    --font "$FONT_MONO" --size 14 --bpp 4 --format lvgl \
    --range "$UNICODE_RANGES" \
    -o "$OUT_DIR/source_code_pro_14.c"
strip_const "$OUT_DIR/source_code_pro_14.c"
echo "  source_code_pro_14 (compressed) -> $OUT_DIR/source_code_pro_14.bin  [frogfs twin]"
lv_font_conv \
    --font "$FONT_MONO" --size 14 --bpp 4 --format bin \
    --range "$UNICODE_RANGES" \
    -o "$OUT_DIR/source_code_pro_14.bin"

# --- MDI icon faces ----------------------------------------------------------
# Same invocation as regen_mdi_fonts.sh minus --no-compress. All mdi sizes this
# script generates (16/24/32/48/64) are now MOVED faces: their symbols are
# non-const runtime-populated shims in ui_fonts.h / lv_conf.h (glyph data in a
# frogfs .bin), so every compiled twin must be de-const'd or moving the face
# back into the compile hits conflicting-qualifiers. Strip const on all of them.
gen_mdi() { # <size> [bin]
    local size="$1" emit_bin="${2:-}" out="$OUT_DIR/mdi_icons_$1.c"
    echo "  mdi_icons_$size (compressed) -> $out"
    lv_font_conv \
        --font "$FONT_MDI" --size "$size" --bpp 4 --format lvgl \
        --range "$MDI_ICONS" \
        -o "$out"
    sed -i 's/^const lv_font_t /lv_font_t /' "$out"
    if [ "$emit_bin" = "bin" ]; then
        echo "  mdi_icons_$size (compressed) -> $OUT_DIR/mdi_icons_$size.bin  [frogfs twin]"
        lv_font_conv \
            --font "$FONT_MDI" --size "$size" --bpp 4 --format bin \
            --range "$MDI_ICONS" \
            -o "$OUT_DIR/mdi_icons_$size.bin"
    fi
}

gen_mdi 16 bin
gen_mdi 24 bin
gen_mdi 32 bin
gen_mdi 48 bin
gen_mdi 64 bin

echo ""
echo "Done. 11 compressed .c font twins + 10 .bin frogfs twins written to $OUT_DIR"
