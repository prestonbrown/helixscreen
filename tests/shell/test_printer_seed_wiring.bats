#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Integration test for the generic per-printer install-time layer wiring (#986).
# Real model detection is STUBBED/reporter-blocked, so this drives the two
# mechanisms directly with the forced id "sovol_sv06_ace" against the REAL
# runtime preset in assets/config/presets/ and the include payload in
# config/klipper_includes/. This proves the seed extracts ONLY the install-time
# device-level blocks (top-level input + display) from the preset, that it does
# NOT seed the preset's printer block or top-level "preset" key, that the
# include payload is well-formed, and that the mechanisms apply them end-to-end
# even though auto-detection is best-guess.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export SETTINGS_FILE="$INSTALL_DIR/config/settings.json"

    export KLIPPER_HOME="$BATS_TEST_TMPDIR/home/sovol"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    export PRINTER_CFG="$KLIPPER_HOME/printer_data/config/printer.cfg"
    printf '%s\n' '[mcu]' 'serial: /dev/ttyS0' > "$PRINTER_CFG"

    SUDO=""
    export SUDO

    # Point the mechanisms at the REAL shipped payload dirs in the repo so the
    # actual sovol_sv06_ace preset + include are exercised end-to-end (not test
    # fixtures). The seed source is now the runtime preset.
    export HELIX_PRESET_DIR="$WORKTREE_ROOT/assets/config/presets"
    export HELIX_KLIPPER_CFG_DIR="$WORKTREE_ROOT/config/klipper_includes"
    unset _HELIX_PRINTER_SEED_SOURCED _HELIX_KLIPPER_INCLUDE_SOURCED

    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/printer_seed.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/klipper_include.sh"
}

@test "wiring: real sovol seed lands the device-level input block from the preset" {
    rm -f "$SETTINGS_FILE"

    run seed_settings_for_printer "sovol_sv06_ace"
    [ "$status" -eq 0 ]
    [ -f "$SETTINGS_FILE" ]

    # The shipped matrix from issue #123 (a=1.66, e=1.76, valid:true) lands...
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));c=d['input']['calibration'];assert c['valid'] is True;assert c['a']==1.66;assert c['e']==1.76"

    # ...but the preset's printer block (hardware mappings) does NOT, and neither
    # does the top-level "preset" key — the app's PrinterDetector applies the
    # full preset on first Moonraker connect, so only the device-level blocks are
    # baked into settings.json here.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert 'printer' not in d, d;assert 'preset' not in d, d"

    # And the preset's nested provenance _comment never leaked into calibration.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert '_comment' not in d['input']['calibration'], d"
}

@test "wiring: real sovol seed does not clobber an existing user calibration" {
    printf '%s\n' '{"input":{"calibration":{"valid":true,"a":2.5}}}' > "$SETTINGS_FILE"

    run seed_settings_for_printer "sovol_sv06_ace"
    [ "$status" -eq 0 ]

    # User's a=2.5 must survive (existing wins over the seed's 1.66).
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['a']==2.5"
}

@test "wiring: real sovol Klipper include lands the cameralight output_pin" {
    run install_klipper_include_for_printer "sovol_sv06_ace"
    [ "$status" -eq 0 ]

    local dest="$KLIPPER_HOME/printer_data/config/helixscreen/sovol_sv06_ace.cfg"
    [ -f "$dest" ]
    grep -q "output_pin cameralight" "$dest"
    grep -q "rpi:gpiochip2/gpio3" "$dest"
    grep -qF "[include helixscreen/sovol_sv06_ace.cfg]" "$PRINTER_CFG"
}

@test "wiring: detect_printer_model fires when the mksclient binary exists" {
    # CONFIRMED fingerprint (#986): the stock Sovol UI binary present at its
    # build path. HELIX_SOVOL_MKSCLIENT redirects the lookup at a temp file so
    # the test exercises the real file-based detection (not a PATH command).
    local bin="$BATS_TEST_TMPDIR/home/sovol/printer_data/build/mksclient"
    mkdir -p "$(dirname "$bin")"
    printf '%s\n' '#!/bin/sh' 'echo stock ui' > "$bin"
    chmod +x "$bin"

    HELIX_SOVOL_MKSCLIENT="$bin" run detect_printer_model
    [ "$status" -eq 0 ]
    [ "$output" = "sovol_sv06_ace" ]
}

@test "wiring: detect_printer_model stays empty without the mksclient binary" {
    # No binary at the (redirected) path → no detection. A sovol hostname alone
    # is NOT a signal anymore: detection keys solely on the binary path.
    local missing="$BATS_TEST_TMPDIR/home/sovol/printer_data/build/mksclient"
    [ ! -e "$missing" ]

    HELIX_SOVOL_MKSCLIENT="$missing" run detect_printer_model
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}
