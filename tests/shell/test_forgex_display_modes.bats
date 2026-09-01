#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Executable tests for the ForgeX display-mode and screen-drawing patches
# across Forge-X 1.4.0 / 1.4.1 / 1.4.2.
#
# test_forgex_boot.bats greps forgex.sh as text. These tests actually RUN the
# functions against a mock mod tree, which is the only way to catch the
# failure modes that matter here:
#   - configure_forgex_display silently doing nothing on an unrecognised mode
#   - patch_forgex_screen_drawing reporting success after guarding only some
#     of the draw commands present in screen.sh
#   - a patch/unpatch SEQUENCE corrupting screen.sh (a 2026-08 review
#     reproduced both directions against real upstream files: the functions
#     were only ever tested in isolation)
#
# 1.4.0 and 1.4.1 are byte-identical across every file the installer touches,
# so the "1.4.0-shaped" fixture covers both. The fixtures mirror the real
# upstream screen.sh shapes (case labels, per-version draw command spellings,
# if/fi-bearing bodies like print_file) rather than a generic case statement:
# the corruption cases below eat specific `fi` lines, and a simplified
# fixture that has none cannot reproduce them.
#
# Paths run through the PROBE SEAM, not a sed rewrite of the module: the
# module derives its mod tree from HOST_MOD_ROOT (host_profile.sh). A sed
# rewrite of /opt/ masked exactly that derivation and let the AD5X host-side
# layout (/usr/data/config/mod) rot untested.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    MOCK_ROOT="$BATS_TEST_TMPDIR/root"

    # AD5X host-side layout by default, so every test in this file proves the
    # derivation. use_mod_root below switches the whole fixture onto another
    # layout (the AD5M/in-chroot /opt shape).
    MOD_ROOT="$MOCK_ROOT/usr/data/config/mod"
    use_mod_root "$MOD_ROOT"

    SUDO=""
    export SUDO

    source_forgex
    HOST_MOD_ROOT="$MOD_ROOT"
}

# Lay out a mod tree at $1 and point the path variables at it.
use_mod_root() {
    MOD_ROOT="$1"
    MOD_DATA="$(dirname "$MOD_ROOT")/mod_data"
    SCREEN_SH="$MOD_ROOT/.shell/screen.sh"
    VAR_CFG="$MOD_DATA/variables.cfg"
    PREV_DISPLAY_F="$MOD_DATA/helixscreen_prev_display"

    mkdir -p "$MOD_ROOT/.shell" \
             "$MOD_ROOT/.root" \
             "$MOD_ROOT/.bin/exec" \
             "$MOD_DATA"
    # The probe's marker (host_profile.sh looks for .shell/platform.sh).
    touch "$MOD_ROOT/.shell/platform.sh"
}

# Source the module UNMODIFIED plus the probe it derives its paths from.
source_forgex() {
    unset _HELIX_HOST_PROFILE_SOURCED _HELIX_FORGEX_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh"
}

write_variables_cfg() {
    cat > "$VAR_CFG" <<EOF
[Variables]
backlight = 100
display = '$1'
sound = 1
EOF
}

current_display_mode() {
    sed -n "s/^display[[:space:]]*=[[:space:]]*'\([A-Z]*\)'.*/\1/p" "$VAR_CFG"
}

# The .root init scripts/launcher configure_forgex_display de-execs.
write_guppy_executables() {
    printf '#!/bin/sh\nexec guppyscreen\n' > "$MOD_ROOT/.root/S80guppyscreen"
    printf '#!/bin/sh\nexec guppyscreen.bin\n' > "$MOD_ROOT/.root/guppyscreen"
    printf '#!/bin/sh\nexec tslib\n' > "$MOD_ROOT/.root/S35tslib"
    chmod +x "$MOD_ROOT/.root/S80guppyscreen" \
             "$MOD_ROOT/.root/guppyscreen" \
             "$MOD_ROOT/.root/S35tslib"
}

# Forge-X 1.4.0 / 1.4.1 screen.sh: draw_loading, draw_splash, boot_message.
# Bodies follow the real file: the draw cases carry if/fi pairs and print_file
# / backlight carry if + exit 1 + fi -- bare `fi` lines the runaway-unpatch
# corruption case eats.
write_screen_sh_140() {
    cat > "$SCREEN_SH" <<'EOF'
#!/bin/bash
source /opt/config/mod/.shell/common.sh

load_version() {
    export MOD_VERSION=$(cat /opt/config/mod/version.txt)
}

print_message() {
    local text="$1"
    "$BINS/typer" -db batch --batch text -t "$text"
}

case "$1" in
    draw_loading)
        load_version

        if [ "$2" != "--no-clear" ]; then
            xzcat "$LOAD_IMG_XZ" > /dev/fb0
        fi

        "$BINS/typer" -db batch --batch text -t "v$MOD_VERSION"
    ;;

    draw_splash)
        load_version
        if [ "$2" != "--no-clear" ]; then
            xzcat "$SPLASH_IMG_XZ" > /dev/fb0
        fi
    ;;

    draw_status_bar)
        icon_wifi=$(printf '\uE146')
    ;;

    boot_message)
        shift
        if [ -z "$1" ]; then
            echo "message text is missing"
            exit 1
        fi

        args=("$@")
        for str in "${args[@]}"; do
            level="${str%%;;*}"
            echo "$level"
        done
    ;;

    print_file)
        if [ -z "$2" ]; then
            echo "File name is missing"
            exit 1
        fi

        print_message "$2"
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
    chmod +x "$SCREEN_SH"
}

# Forge-X 1.4.2: draw_loading and boot_message are gone; a long-running splash
# process driven over a control FIFO replaces them. Real-file shapes: the
# splash_* functions above the case, and print_file/backlight bodies carrying
# if + exit 1 + fi.
write_screen_sh_142() {
    cat > "$SCREEN_SH" <<'EOF'
#!/bin/bash
source /opt/config/mod/.shell/common.sh

splash_running() {
    [ -p "$SPLASH_CONTROL_FIFO" ] || return 1
    pidof splash > /dev/null 2>&1
}

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
    if splash_running; then
        return 0
    fi

    rm -f "$SPLASH_CONTROL_FIFO"

    "$BINS/splash" --control-fifo "$SPLASH_CONTROL_FIFO" \
        </dev/null >/dev/null 2>&1 &

    local attempts=0
    while [ ! -p "$SPLASH_CONTROL_FIFO" ] && [ "$attempts" -lt 50 ]; do
        sleep 0.1
        attempts=$((attempts + 1))
    done
}

splash_stop() {
    splash_command stop || return 0
}

draw_splash() {
    "$BINS/splash" --subtitle "$(splash_subtitle)" --static
}

case "$1" in
    splash_start)
        splash_start
    ;;

    splash_version)
        splash_set_version "$2"
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
        icon_wifi=$(printf '\uE146')
    ;;

    print_file)
        if [ -z "$2" ]; then
            echo "File name is missing"
            exit 1
        fi

        print_message "$2"
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
    chmod +x "$SCREEN_SH"
}

# The backlight block a pre-smart-patch HelixScreen inserted (the spelling
# unpatch_forgex_screen_sh's old-style arm was written against).
apply_old_style_backlight_patch() {
    local tmp="$SCREEN_SH.oldstyle"
    awk '
    /^[[:space:]]*backlight\)/ {
        print
        print "        # Skip if HelixScreen is controlling the display"
        print "        if [ -f /tmp/helixscreen_active ]; then"
        print "            exit 0"
        print "        fi"
        next
    }
    { print }
    ' "$SCREEN_SH" > "$tmp"
    mv "$tmp" "$SCREEN_SH"
    chmod +x "$SCREEN_SH"
}

# A guard marker comment whose if/exit/fi was already eaten -- the mid-sequence
# state the review's uninstall corruption started from. Only the marker line
# is inserted.
apply_orphaned_guard_comment() {
    local label=$1
    local tmp="$SCREEN_SH.orphan"
    awk -v lbl="$label" '
    $0 ~ "^[[:space:]]*" lbl "\\)" {
        print
        print "        # Skip when HelixScreen is controlling display"
        next
    }
    { print }
    ' "$SCREEN_SH" > "$tmp"
    mv "$tmp" "$SCREEN_SH"
    chmod +x "$SCREEN_SH"
}

# Assert that the guard was inserted directly under a given case label.
assert_case_guarded() {
    local label=$1
    local body
    body=$(awk -v lbl="$label" '
        $0 ~ "^[[:space:]]*" lbl "\\)" { found=1; next }
        found { print; if (++n >= 5) exit }
    ' "$SCREEN_SH")
    printf '%s\n' "$body" | grep -q 'helixscreen_active' \
        || fail "case '${label})' is not guarded by /tmp/helixscreen_active"
}

assert_case_not_guarded() {
    local label=$1
    local body
    body=$(awk -v lbl="$label" '
        $0 ~ "^[[:space:]]*" lbl "\\)" { found=1; next }
        found { print; if (++n >= 5) exit }
    ' "$SCREEN_SH")
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
    [ "$(grep -c '^display' "$VAR_CFG")" -eq 1 ]
}

@test "configure_forgex_display records the mode it took over" {
    write_variables_cfg FEATHER
    configure_forgex_display
    [ "$(cat "$PREV_DISPLAY_F")" = "FEATHER" ]
}

@test "the previous-display record is written through the privileged seam" {
    # Regression: the record was written with a bare `printf >` redirect to a
    # .tmp, which fails on the root-owned mod_data of a real device -- the
    # record was silently lost and uninstall fell back to GUPPY. The write
    # must go through $SUDO (tee), like every other privileged file write.
    write_variables_cfg FEATHER
    local shim="$BATS_TEST_TMPDIR/sudo-shim"
    local shim_log="$BATS_TEST_TMPDIR/sudo.log"
    cat > "$shim" <<EOF
#!/bin/sh
echo "\$*" >> "$shim_log"
exec "\$@"
EOF
    chmod +x "$shim"
    SUDO="$shim"
    export SUDO

    configure_forgex_display

    grep -q "tee $PREV_DISPLAY_F" "$shim_log" \
        || fail "record write did not go through \$SUDO"
    [ "$(cat "$PREV_DISPLAY_F")" = "FEATHER" ]
}

@test "a printer that arrived on HEADLESS is recorded and stays HEADLESS" {
    # Regression: HEADLESS was not probed as an arrival state, so nothing was
    # recorded for it and uninstall "restored" it to GUPPY -- a mode that
    # printer never had, and one that starts a UI the operator had turned off.
    write_variables_cfg HEADLESS
    configure_forgex_display || true
    [ "$(cat "$PREV_DISPLAY_F")" = "HEADLESS" ]

    uninstall_forgex
    [ "$(current_display_mode)" = "HEADLESS" ]
}

@test "a re-run install does not overwrite the recorded pre-install mode" {
    # The record is "the mode the printer had before HelixScreen took over",
    # so an upgrade run (which finds HEADLESS, because we set it) must keep
    # the first run's record -- otherwise uninstall restores HEADLESS and
    # leaves an uninstalled printer with no UI at all.
    write_variables_cfg FEATHER
    configure_forgex_display
    configure_forgex_display || true
    [ "$(cat "$PREV_DISPLAY_F")" = "FEATHER" ]

    uninstall_forgex
    [ "$(current_display_mode)" = "FEATHER" ]
}

@test "an unmatched display spelling fails the takeover instead of returning success" {
    # Regression: with the init scripts present the chmod arms set changed=true
    # and the function returned 0 even though the display line itself was
    # never rewritten -- a future Forge-X mode string silently kept the vendor
    # UI drawing over HelixScreen while the install reported success.
    write_variables_cfg TFT
    write_guppy_executables

    run configure_forgex_display
    [ "$status" -ne 0 ] || fail "reported success without rewriting the display mode"
    [ "$(current_display_mode)" = "TFT" ]
}

# --- patch_forgex_screen_sh: the smart backlight guard ---

@test "backlight patch guards the backlight case and keeps screen.sh valid (1.4.2)" {
    write_screen_sh_142
    run patch_forgex_screen_sh
    [ "$status" -eq 0 ]
    assert_case_guarded backlight
    bash -n "$SCREEN_SH"
}

@test "backlight patch keeps screen.sh valid (1.4.0)" {
    write_screen_sh_140
    run patch_forgex_screen_sh
    [ "$status" -eq 0 ]
    assert_case_guarded backlight
    bash -n "$SCREEN_SH"
}

@test "backlight patch is idempotent" {
    write_screen_sh_142
    patch_forgex_screen_sh
    patch_forgex_screen_sh
    [ "$(grep -c 'helixscreen_active' "$SCREEN_SH")" -eq 1 ]
    bash -n "$SCREEN_SH"
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
    bash -n "$SCREEN_SH"
}

@test "screen drawing patch keeps screen.sh syntactically valid on 1.4.0" {
    write_screen_sh_140
    patch_forgex_screen_drawing
    bash -n "$SCREEN_SH"
}

@test "screen drawing patch is idempotent on 1.4.2" {
    write_screen_sh_142
    patch_forgex_screen_drawing
    patch_forgex_screen_drawing
    local n
    n=$(grep -c 'helixscreen_active' "$SCREEN_SH")
    [ "$n" -eq 2 ] || fail "expected 2 guards (draw_splash, splash_start), got $n"
}

@test "screen drawing patch fails when no known draw command is present" {
    # A future Forge-X that renames every draw command must not be reported as
    # successfully patched. This is the silent-half-application guard: the old
    # implementation grepped the whole file for helixscreen_active, so one
    # surviving match vouched for every label it had failed to find.
    cat > "$SCREEN_SH" <<'EOF'
#!/bin/bash
case "$1" in
    render_boot_ui)
        do_render
    ;;
esac
EOF
    chmod +x "$SCREEN_SH"
    run patch_forgex_screen_drawing
    [ "$status" -ne 0 ] || fail "reported success without guarding anything"
}

# --- sequences: the corruption the review reproduced on real upstream files ---
#
# Both reproduced paths share one root cause: text surgery on screen.sh with
# no post-condition syntax check, executed in orders the per-function tests
# never ran.

@test "install sequence over a legacy-patched screen.sh stays valid (1.4.0)" {
    # Install-time corruption: an older HelixScreen left drawing guards and an
    # old-style backlight block behind. patch_forgex_screen_sh's old-style
    # removal was a `grep -v helixscreen_active`, which drops the guards' if
    # lines but leaves their exit 0/fi behind -- unbalanced, and the corrupted
    # file was moved into place anyway.
    write_screen_sh_140
    patch_forgex_screen_drawing
    apply_old_style_backlight_patch
    bash -n "$SCREEN_SH"

    run patch_forgex_screen_sh
    [ "$status" -eq 0 ]
    bash -n "$SCREEN_SH"
    # The draw guards must survive the old-style removal...
    assert_case_guarded draw_loading
    assert_case_guarded boot_message
    # ...and the smart backlight patch must replace the old-style block.
    assert_case_guarded backlight
    refute_grep 'Skip if HelixScreen is controlling the display' "$SCREEN_SH"
}

@test "uninstall sequence restores a pristine 1.4.2 screen.sh byte-for-byte" {
    # Uninstall-time corruption: unpatch_forgex_screen_sh armed its skip state
    # machine on ANY `if [ -f /tmp/helixscreen_active ]` line -- including the
    # drawing guards', whose if-lines are byte-identical -- and ate their
    # if/exit/fi while leaving their marker comments. unpatch_forgex_screen_
    # drawing then armed on those orphaned comments and ran on, eating the
    # next bare fi/exit 0 lines it met (print_file's fi, the smart backlight
    # block's exit 0 and fi). The corrupted file was moved into place and the
    # function reported success.
    write_screen_sh_142
    cp "$SCREEN_SH" "$BATS_TEST_TMPDIR/pristine.sh"

    patch_forgex_screen_sh
    patch_forgex_screen_drawing
    unpatch_forgex_screen_sh
    unpatch_forgex_screen_drawing

    cmp "$BATS_TEST_TMPDIR/pristine.sh" "$SCREEN_SH" \
        || fail "uninstall sequence did not restore the original screen.sh"
}

@test "uninstall sequence restores a pristine 1.4.0 screen.sh byte-for-byte" {
    write_screen_sh_140
    cp "$SCREEN_SH" "$BATS_TEST_TMPDIR/pristine.sh"

    patch_forgex_screen_sh
    patch_forgex_screen_drawing
    unpatch_forgex_screen_sh
    unpatch_forgex_screen_drawing

    cmp "$BATS_TEST_TMPDIR/pristine.sh" "$SCREEN_SH" \
        || fail "uninstall sequence did not restore the original screen.sh"
}

@test "unpatch_forgex_screen_sh reports success after removing the backlight patch" {
    # With drawing guards also present, the old whole-file verify
    # (`grep helixscreen_active`) could never pass and every uninstall warned
    # "Could not fully remove patch". The verify must ask about the backlight
    # case, which is this function's patch.
    write_screen_sh_142
    patch_forgex_screen_sh
    patch_forgex_screen_drawing

    run unpatch_forgex_screen_sh
    [ "$status" -eq 0 ]
}

@test "an orphaned guard comment cannot corrupt screen.sh" {
    # Containment, independently of the arming fix above: whatever state a
    # previous (partial, older, hand-rolled) run left behind, an unpatch run
    # must never eat more than its own marker. The orphaned marker comment is
    # the exact trigger of the reproduced runaway, which consumed lines until
    # the next bare fi (print_file's) and shipped the result.
    write_screen_sh_142
    apply_orphaned_guard_comment splash_start
    local lines_before
    lines_before=$(wc -l < "$SCREEN_SH")

    run unpatch_forgex_screen_drawing
    bash -n "$SCREEN_SH" || fail "screen.sh corrupted"

    # At most the marker itself may disappear - a runaway strip shows up as a
    # multi-line gap.
    local lines_after
    lines_after=$(wc -l < "$SCREEN_SH")
    [ "$lines_after" -ge $((lines_before - 1)) ] \
        || fail "unpatch removed $((lines_before - lines_after)) lines from a one-marker file"
}

@test "forgex_apply_patch installs a valid candidate" {
    # Positive control for the rejection test below: without it, a helper
    # that rejects everything would pass that test.
    write_screen_sh_142
    cp "$SCREEN_SH" "$BATS_TEST_TMPDIR/before.sh"
    sed 's/draw_splash)/draw_splash_changed)/' "$SCREEN_SH" > "$SCREEN_SH.tmp"

    forgex_apply_patch "$SCREEN_SH.tmp" "$SCREEN_SH"
    grep -q 'draw_splash_changed)' "$SCREEN_SH"
    [ -x "$SCREEN_SH" ]
    [ ! -e "$SCREEN_SH.tmp" ]
}

@test "forgex_apply_patch rejects a broken candidate and leaves the original untouched" {
    # The containment the whole sequence family rests on: the candidate stays
    # a .tmp until it parses, so a rejected rewrite leaves the vendor's file
    # byte-identical -- the untouched original IS the backup.
    write_screen_sh_142
    cp "$SCREEN_SH" "$BATS_TEST_TMPDIR/before.sh"
    printf '#!/bin/bash\nif [ -f /tmp/helixscreen_active ]; then\n' > "$SCREEN_SH.tmp"

    run forgex_apply_patch "$SCREEN_SH.tmp" "$SCREEN_SH"
    [ "$status" -ne 0 ] || fail "installed a candidate that does not parse"
    cmp "$BATS_TEST_TMPDIR/before.sh" "$SCREEN_SH" \
        || fail "original screen.sh was modified by a rejected candidate"
    [ ! -e "$SCREEN_SH.tmp" ]
}

# --- unpatch must reverse the patch on both shapes ---

@test "unpatch removes every guard on 1.4.2" {
    write_screen_sh_142
    patch_forgex_screen_drawing
    unpatch_forgex_screen_drawing
    refute_grep 'helixscreen_active' "$SCREEN_SH"
    bash -n "$SCREEN_SH"
}

@test "unpatch removes every guard on 1.4.0" {
    write_screen_sh_140
    patch_forgex_screen_drawing
    unpatch_forgex_screen_drawing
    refute_grep 'helixscreen_active' "$SCREEN_SH"
    bash -n "$SCREEN_SH"
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

# --- the restored-UI report must name what was actually restored ---

@test "restored-ui report names Feather for a FEATHER printer, not GuppyScreen" {
    # Regression: uninstall_forgex claimed "GuppyScreen (...)" whenever the
    # init script existed, whatever mode it restored -- pointing the operator
    # at a UI that is not the one coming back.
    write_variables_cfg FEATHER
    write_guppy_executables
    configure_forgex_display
    unset restored_ui
    uninstall_forgex

    case "$restored_ui" in
        *Feather*) ;;
        *) fail "restored_ui does not name Feather: ${restored_ui:-<empty>}";;
    esac
    case "$restored_ui" in
        *GuppyScreen*) fail "restored_ui wrongly claims GuppyScreen: $restored_ui";;
    esac
}

@test "restored-ui report names GuppyScreen for the GUPPY fallback" {
    write_variables_cfg HEADLESS
    write_guppy_executables
    unset restored_ui
    uninstall_forgex

    [ "$(current_display_mode)" = "GUPPY" ]
    case "$restored_ui" in
        *GuppyScreen*) ;;
        *) fail "restored_ui does not name GuppyScreen: ${restored_ui:-<empty>}";;
    esac
}

@test "restored-ui report claims nothing for a HEADLESS arrival" {
    # A printer that was on HEADLESS before install had no vendor UI displaced,
    # so there is nothing to claim -- an empty report is the honest answer.
    write_variables_cfg HEADLESS
    write_guppy_executables
    configure_forgex_display || true
    unset restored_ui
    uninstall_forgex

    [ "$(current_display_mode)" = "HEADLESS" ]
    [ -z "${restored_ui:-}" ] \
        || fail "restored_ui claims a UI for a HEADLESS arrival: $restored_ui"
}

# --- paths derive from the probe, not the AD5M layout ---

@test "forgex paths follow the probed mod root" {
    [ "$(forgex_mod_root)" = "$MOD_ROOT" ]
    [ "$(forgex_mod_data)" = "$MOD_DATA" ]
    [ "$(forgex_prev_display_f)" = "$PREV_DISPLAY_F" ]
}

@test "host_profile_probe drives the forgex paths end to end" {
    # The real seam: the probe (env-overridable candidates) answers
    # HOST_MOD_ROOT and every forgex path follows it.
    unset HOST_MOD_ROOT
    export HELIX_MOD_TREE_CANDIDATES="$MOD_ROOT"
    host_profile_probe
    [ "$HOST_MOD_ROOT" = "$MOD_ROOT" ]

    write_variables_cfg FEATHER
    configure_forgex_display
    [ "$(current_display_mode)" = "HEADLESS" ]
}

@test "the in-chroot /opt layout works through the same seam" {
    # AD5M / Forge-X layout (also what the mod bind-mounts in its chroot):
    # only the probed root differs.
    local opt_root="$MOCK_ROOT/opt/config/mod"
    use_mod_root "$opt_root"
    HOST_MOD_ROOT="$opt_root"

    write_variables_cfg GUPPY
    configure_forgex_display
    [ "$(current_display_mode)" = "HEADLESS" ]
    [ "$(cat "$MOCK_ROOT/opt/config/mod_data/helixscreen_prev_display")" = "GUPPY" ]

    write_screen_sh_142   # under the new root now
    patch_forgex_screen_sh
    assert_case_guarded backlight
}

@test "without a probe the paths fall back to the AD5M in-chroot layout" {
    # String-level assertion only: an unprobed caller (nothing in this test
    # touches /opt) must keep the historical default.
    unset HOST_MOD_ROOT
    [ "$(forgex_mod_root)" = "/opt/config/mod" ]
    [ "$(forgex_mod_data)" = "/opt/config/mod_data" ]
    [ "$(forgex_prev_display_f)" = "/opt/config/mod_data/helixscreen_prev_display" ]
}
