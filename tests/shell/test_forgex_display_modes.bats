#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Executable tests for the ForgeX display-mode and screen-drawing patches
# across Forge-X 1.4.0 / 1.4.1 / 1.4.2.
#
# test_forgex_boot.bats greps forgex.sh as text. These tests actually RUN the
# functions against a mock /opt tree, which is the only way to catch the two
# failure modes that matter here:
#   - configure_forgex_display silently doing nothing on an unrecognised mode
#   - patch_forgex_screen_drawing reporting success after guarding only some
#     of the draw commands present in screen.sh
#
# 1.4.0 and 1.4.1 are byte-identical across every file the installer touches,
# so the "1.4.0-shaped" fixture covers both.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    MOCK_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$MOCK_ROOT/opt/config/mod/.shell" \
             "$MOCK_ROOT/opt/config/mod/.root" \
             "$MOCK_ROOT/opt/config/mod/.bin/exec" \
             "$MOCK_ROOT/opt/config/mod_data"

    SUDO=""
    export SUDO

    source_forgex_against_mock_root
}

# Rewrite the module's hardcoded /opt paths to point at MOCK_ROOT, then source
# the copy. Mirrors the pattern in test_cc1_competing_uis.bats.
source_forgex_against_mock_root() {
    local patched="$BATS_TEST_TMPDIR/forgex.sh"
    sed -e "s|/opt/|$MOCK_ROOT/opt/|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh" > "$patched"

    # Safety net: every surviving /opt/ must be under MOCK_ROOT. A line
    # mentioning /opt/ without the mock prefix would reach the real tree.
    refute_sh "grep -v '$MOCK_ROOT' '$patched' | grep -q '/opt/'"

    # /tmp/helixscreen_active is a runtime flag inside the generated patch text
    # and must survive the rewrite untouched.
    grep -q '/tmp/helixscreen_active' "$patched"

    unset _HELIX_FORGEX_SOURCED
    # shellcheck disable=SC1090
    . "$patched"
}

# Minimal logging shims — forgex.sh calls these but they live in common.sh.
log_info()    { :; }
log_warn()    { :; }
log_error()   { :; }
log_success() { :; }

write_variables_cfg() {
    cat > "$MOCK_ROOT/opt/config/mod_data/variables.cfg" <<EOF
[Variables]
backlight = 100
display = '$1'
sound = 1
EOF
}

current_display_mode() {
    sed -n "s/^display[[:space:]]*=[[:space:]]*'\([A-Z]*\)'.*/\1/p" \
        "$MOCK_ROOT/opt/config/mod_data/variables.cfg"
}

# Forge-X 1.4.0 / 1.4.1 screen.sh: draw_loading, draw_splash, boot_message.
write_screen_sh_140() {
    cat > "$MOCK_ROOT/opt/config/mod/.shell/screen.sh" <<'EOF'
#!/bin/bash
source /opt/config/mod/.shell/common.sh

case "$1" in
    draw_loading)
        do_draw_loading "$2"
    ;;

    draw_splash)
        do_draw_splash
    ;;

    draw_status_bar)
        do_status_bar "$2"
    ;;

    boot_message)
        do_boot_message "$2"
    ;;

    backlight)
        value=$2
        if [ -z "$2" ]; then
            echo "Backlight value is missing"
            exit 1
        fi

        chroot "$MOD" /root/printer_data/py/backlight.py $value
    ;;
    *)
        echo "Usage: $0 <command> [args...]"
        exit 1
esac
EOF
    chmod +x "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
}

# Forge-X 1.4.2: draw_loading and boot_message are gone; a long-running splash
# process driven over a control FIFO replaces them.
write_screen_sh_142() {
    cat > "$MOCK_ROOT/opt/config/mod/.shell/screen.sh" <<'EOF'
#!/bin/bash
source /opt/config/mod/.shell/common.sh

splash_command() {
    local command="$1"
    shift

    case "$command" in
        subtitle)
            { printf 'subtitle %s\n' "$*" >&3; } 3<> "$SPLASH_CONTROL_FIFO"
        ;;
        stop)
            { printf 'stop\n' >&3; } 3<> "$SPLASH_CONTROL_FIFO"
        ;;
    esac
}

splash_start() {
    "$BINS/splash" --control-fifo "$SPLASH_CONTROL_FIFO" &
}

splash_stop() {
    splash_command stop
}

case "$1" in
    splash_start)
        splash_start
    ;;

    splash_version)
        splash_set_subtitle "$2"
    ;;

    splash_subtitle)
        splash_set_subtitle "$2"
    ;;

    splash_stop)
        splash_stop
    ;;

    draw_splash)
        draw_splash
    ;;

    draw_status_bar)
        do_status_bar "$2"
    ;;

    backlight)
        value=$2
        if [ -z "$2" ]; then
            echo "Backlight value is missing"
            exit 1
        fi

        chroot "$MOD" /root/printer_data/py/backlight.py $value
    ;;
    *)
        echo "Usage: $0 splash_start|splash_version|splash_subtitle|splash_stop|draw_splash|<screen command> [args...]"
        exit 1
esac
EOF
    chmod +x "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
}

# Assert that the guard was inserted directly under a given case label.
assert_case_guarded() {
    local label=$1
    local body
    body=$(awk -v lbl="$label" '
        $0 ~ "^[[:space:]]*" lbl "\\)" { found=1; next }
        found { print; if (++n >= 5) exit }
    ' "$MOCK_ROOT/opt/config/mod/.shell/screen.sh")
    printf '%s\n' "$body" | grep -q 'helixscreen_active' \
        || fail "case '${label})' is not guarded by /tmp/helixscreen_active"
}

assert_case_not_guarded() {
    local label=$1
    local body
    body=$(awk -v lbl="$label" '
        $0 ~ "^[[:space:]]*" lbl "\\)" { found=1; next }
        found { print; if (++n >= 5) exit }
    ' "$MOCK_ROOT/opt/config/mod/.shell/screen.sh")
    if printf '%s\n' "$body" | grep -q 'helixscreen_active'; then
        fail "case '${label})' must NOT be guarded"
        return 1
    fi
    return 0
}

# --- configure_forgex_display: every mode must land on HEADLESS ---

@test "configure_forgex_display moves STOCK to HEADLESS" {
    write_variables_cfg STOCK
    configure_forgex_display
    [ "$(current_display_mode)" = "HEADLESS" ]
}

@test "configure_forgex_display moves FEATHER to HEADLESS (1.4.2 default)" {
    # Regression: 1.4.2 changed the mod_params.json default from STOCK to
    # FEATHER. Without a FEATHER branch this function is a silent no-op and
    # Feather's Klipper macros keep drawing over HelixScreen.
    write_variables_cfg FEATHER
    configure_forgex_display
    [ "$(current_display_mode)" = "HEADLESS" ]
}

@test "configure_forgex_display moves GUPPY to HEADLESS" {
    # Upgrade path from HelixScreen installs that selected GUPPY themselves.
    write_variables_cfg GUPPY
    configure_forgex_display
    [ "$(current_display_mode)" = "HEADLESS" ]
}

@test "configure_forgex_display leaves an existing HEADLESS alone" {
    write_variables_cfg HEADLESS
    configure_forgex_display || true
    [ "$(current_display_mode)" = "HEADLESS" ]
}

@test "configure_forgex_display never leaves the printer on GUPPY" {
    # DrA1ex: any slot other than HEADLESS risks failed OTA updates and
    # repeated Moonraker recovery prompts (DrA1ex/ff5m#74).
    for mode in STOCK FEATHER GUPPY HEADLESS; do
        write_variables_cfg "$mode"
        configure_forgex_display || true
        [ "$(current_display_mode)" != "GUPPY" ] \
            || fail "display left on GUPPY after starting from $mode"
    done
}

@test "configure_forgex_display is idempotent" {
    write_variables_cfg FEATHER
    configure_forgex_display
    configure_forgex_display || true
    [ "$(current_display_mode)" = "HEADLESS" ]
    [ "$(grep -c '^display' "$MOCK_ROOT/opt/config/mod_data/variables.cfg")" -eq 1 ]
}

# --- patch_forgex_screen_drawing: guard what is actually present ---

@test "screen drawing patch guards all three 1.4.0 draw commands" {
    write_screen_sh_140
    patch_forgex_screen_drawing
    assert_case_guarded draw_loading
    assert_case_guarded draw_splash
    assert_case_guarded boot_message
}

@test "screen drawing patch guards draw_splash on 1.4.2" {
    write_screen_sh_142
    patch_forgex_screen_drawing
    assert_case_guarded draw_splash
}

@test "screen drawing patch guards splash_start on 1.4.2" {
    # 1.4.2's S00init runs `screen.sh splash_start`, which launches a
    # long-running splash process that owns /dev/fb0. Unguarded, it draws over
    # HelixScreen for the whole boot.
    write_screen_sh_142
    patch_forgex_screen_drawing
    assert_case_guarded splash_start
}

@test "screen drawing patch does NOT guard splash_stop" {
    # Blocking splash_stop would strand the splash process on screen forever.
    write_screen_sh_142
    patch_forgex_screen_drawing
    assert_case_not_guarded splash_stop
}

@test "screen drawing patch leaves the backlight case to the backlight patch" {
    write_screen_sh_142
    patch_forgex_screen_drawing
    assert_case_not_guarded backlight
}

@test "screen drawing patch keeps screen.sh syntactically valid on 1.4.2" {
    write_screen_sh_142
    patch_forgex_screen_drawing
    bash -n "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
}

@test "screen drawing patch keeps screen.sh syntactically valid on 1.4.0" {
    write_screen_sh_140
    patch_forgex_screen_drawing
    bash -n "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
}

@test "screen drawing patch is idempotent on 1.4.2" {
    write_screen_sh_142
    patch_forgex_screen_drawing
    patch_forgex_screen_drawing
    local n
    n=$(grep -c 'helixscreen_active' "$MOCK_ROOT/opt/config/mod/.shell/screen.sh")
    [ "$n" -eq 2 ] || fail "expected 2 guards (draw_splash, splash_start), got $n"
}

@test "screen drawing patch fails when no known draw command is present" {
    # A future Forge-X that renames every draw command must not be reported as
    # successfully patched. This is the silent-half-application guard: the old
    # implementation grepped the whole file for helixscreen_active, so one
    # surviving match vouched for every label it had failed to find.
    cat > "$MOCK_ROOT/opt/config/mod/.shell/screen.sh" <<'EOF'
#!/bin/bash
case "$1" in
    render_boot_ui)
        do_render
    ;;
esac
EOF
    chmod +x "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
    run patch_forgex_screen_drawing
    [ "$status" -ne 0 ] || fail "reported success without guarding anything"
}

# --- unpatch must reverse the patch on both shapes ---

@test "unpatch removes every guard on 1.4.2" {
    write_screen_sh_142
    patch_forgex_screen_drawing
    unpatch_forgex_screen_drawing
    refute_grep 'helixscreen_active' "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
    bash -n "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
}

@test "unpatch removes every guard on 1.4.0" {
    write_screen_sh_140
    patch_forgex_screen_drawing
    unpatch_forgex_screen_drawing
    refute_grep 'helixscreen_active' "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
    bash -n "$MOCK_ROOT/opt/config/mod/.shell/screen.sh"
}

# --- uninstall must put the display mode back where it was ---

@test "uninstall restores the display mode the printer arrived with (FEATHER)" {
    # Regression guard: uninstall_forgex used to hardcode a restore to GUPPY.
    # Now that 1.4.2 defaults to FEATHER, that would leave an uninstalled
    # printer on a mode it never had.
    write_variables_cfg FEATHER
    configure_forgex_display
    [ "$(current_display_mode)" = "HEADLESS" ]

    uninstall_forgex
    [ "$(current_display_mode)" = "FEATHER" ]
}

@test "uninstall restores STOCK for a 1.4.0-era printer" {
    write_variables_cfg STOCK
    configure_forgex_display
    uninstall_forgex
    [ "$(current_display_mode)" = "STOCK" ]
}

@test "uninstall falls back to GUPPY when no prior mode was recorded" {
    # Older HelixScreen installs left no record. GUPPY is the historical
    # restore target and still exists in every supported Forge-X.
    write_variables_cfg HEADLESS
    uninstall_forgex
    [ "$(current_display_mode)" = "GUPPY" ]
}
