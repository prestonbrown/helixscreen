#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: forgex
# ForgeX firmware-specific: display config, screen.sh patching, stock UI handling
#
# Reads: SUDO, AD5M_FIRMWARE
# Writes: restored_ui (via dynamic scoping in uninstall_forgex)

# Source guard
[ -n "${_HELIX_FORGEX_SOURCED:-}" ] && return 0
_HELIX_FORGEX_SOURCED=1

# Display modes we take over from, in the order they are probed.
FORGEX_DISPLAY_MODES="STOCK FEATHER GUPPY"

# Where the pre-install display mode is recorded so uninstall can restore it.
FORGEX_PREV_DISPLAY_F="/opt/config/mod_data/helixscreen_prev_display"

# Configure ForgeX display settings for HelixScreen.
#
# HEADLESS is the slot DrA1ex asked custom screens to occupy (DrA1ex/ff5m#74).
# Any other mode risks failed OTA updates and repeated Moonraker recovery
# prompts. It is also the quietest: under HEADLESS, start.sh starts neither
# tslib nor GuppyScreen on 1.4.0, 1.4.1 or 1.4.2.
#
# All three other modes have to be handled. 1.4.2 moved the stock default from
# STOCK to FEATHER, and Feather cannot be stopped as a process - it is Klipper
# macros in config/feather.cfg driving screen.sh - so leaving it selected means
# it keeps drawing over HelixScreen.
#
# The GuppyScreen init scripts and launcher are de-execed regardless: a SET_MOD
# display change can reach .root/guppyscreen through zdisplay.sh without going
# through start.sh at all, whatever mode variables.cfg names.
configure_forgex_display() {
    var_file="/opt/config/mod_data/variables.cfg"
    guppy_init="/opt/config/mod/.root/S80guppyscreen"
    guppy_bin="/opt/config/mod/.root/guppyscreen"
    tslib_init="/opt/config/mod/.root/S35tslib"
    changed=false

    if [ -f "$var_file" ]; then
        for mode in $FORGEX_DISPLAY_MODES; do
            grep -q "display[[:space:]]*=[[:space:]]*'$mode'" "$var_file" || continue

            log_info "Setting ForgeX display mode to HEADLESS (was $mode)..."

            # Remember where we found it so uninstall can put it back. 1.4.0
            # and 1.4.1 default to STOCK, 1.4.2 to FEATHER, so a fixed restore
            # target would strand one of them on a mode it never had.
            if printf '%s\n' "$mode" > "${FORGEX_PREV_DISPLAY_F}.tmp" 2>/dev/null; then
                $SUDO mv "${FORGEX_PREV_DISPLAY_F}.tmp" "$FORGEX_PREV_DISPLAY_F" 2>/dev/null || \
                    rm -f "${FORGEX_PREV_DISPLAY_F}.tmp"
            fi

            $SUDO sed -i "s/display[[:space:]]*=[[:space:]]*'$mode'/display = 'HEADLESS'/" "$var_file"
            changed=true
            break
        done
    fi

    # Disable GuppyScreen init script (remove execute permission). HEADLESS
    # never invokes it via start.sh, but belt-and-braces: nothing may be able
    # to relaunch GuppyScreen while HelixScreen owns the framebuffer.
    if [ -x "$guppy_init" ]; then
        log_info "Disabling GuppyScreen init script..."
        $SUDO chmod a-x "$guppy_init"
        changed=true
    fi

    # Disable the GuppyScreen launcher too. start.sh runs S80guppyscreen at
    # boot, but zdisplay.sh's apply_display_off() calls .root/guppyscreen
    # directly, so disabling only the init script leaves GuppyScreen reachable
    # on every SET_MOD display change and the framebuffer collision returns.
    # That path is independent of the selected display mode, so it stays
    # closed under HEADLESS as well.
    if [ -x "$guppy_bin" ]; then
        log_info "Disabling GuppyScreen launcher..."
        $SUDO chmod a-x "$guppy_bin"
        changed=true
    fi

    # Disable tslib init script (GuppyScreen's touch input layer)
    # HelixScreen uses its own input handling
    if [ -x "$tslib_init" ]; then
        log_info "Disabling tslib init script..."
        $SUDO chmod a-x "$tslib_init"
        changed=true
    fi

    if [ "$changed" = true ]; then
        log_success "ForgeX configured for HelixScreen (HEADLESS mode, GuppyScreen disabled)"
        return 0
    fi
    return 1
}

# Pre-dismiss ForgeX's "Try the new Feather screen" offer.
#
# ForgeX ships config/display_offer.cfg, a [delayed_gcode] that fires a few
# seconds after every Klipper start and raises an action:prompt offering to
# switch the display to Feather. Accepting it takes the screen away from
# HelixScreen, and the prompt returns on each startup until answered.
#
# The macro is gated on the mod_params variable show_feather_promo, and its
# "Never show again" button does nothing but set that variable to 0. Seeding
# the variable is therefore exactly equivalent to the user having dismissed
# it, and the prompt is never composed at all.
#
# Only touched when the installed ForgeX actually ships the offer, so older
# releases never gain a variable their mod_params does not define. Only a
# pending (non-zero) value is rewritten, so a user who already dismissed it
# by hand is left alone -- and for the same reason uninstall does not restore
# it, since we cannot tell our 0 from theirs.
dismiss_forgex_feather_promo() {
    var_file="${FORGEX_VAR_FILE:-/opt/config/mod_data/variables.cfg}"
    offer_cfg="${FORGEX_OFFER_CFG:-/opt/config/mod/config/display_offer.cfg}"

    if [ ! -f "$offer_cfg" ]; then
        log_info "ForgeX has no Feather display offer, nothing to dismiss"
        return 0
    fi

    if [ ! -f "$var_file" ]; then
        log_info "ForgeX variables.cfg not found, cannot dismiss Feather offer"
        return 1
    fi

    if grep -qE "^[[:space:]]*show_feather_promo[[:space:]]*=[[:space:]]*0[[:space:]]*$" "$var_file"; then
        log_info "ForgeX Feather offer already dismissed"
        return 0
    fi

    log_info "Dismissing ForgeX Feather display offer..."
    tmp_file="${var_file}.tmp"

    if grep -qE "^[[:space:]]*show_feather_promo[[:space:]]*=" "$var_file"; then
        sed "s/^[[:space:]]*show_feather_promo[[:space:]]*=.*/show_feather_promo = 0/" \
            "$var_file" > "$tmp_file"
    else
        # Klipper's [Variables] block is the only valid home for the key.
        awk '
        /^\[Variables\]/ && !done { print; print "show_feather_promo = 0"; done = 1; next }
        { print }
        ' "$var_file" > "$tmp_file"
    fi

    if [ -s "$tmp_file" ] && \
       grep -qE "^show_feather_promo = 0$" "$tmp_file" 2>/dev/null; then
        $SUDO mv "$tmp_file" "$var_file"
        log_success "ForgeX Feather display offer dismissed"
        return 0
    fi

    rm -f "$tmp_file"
    log_warn "Failed to dismiss ForgeX Feather display offer"
    return 1
}

# Patch ForgeX screen.sh to skip non-100 backlight control when HelixScreen is active
#
# A `reset_screen` delayed_gcode dims the backlight 3 seconds after Klipper
# starts. Which config carries it moved: in 1.4.0/1.4.1 it is guppy.cfg only,
# and 1.4.2 added it to headless.cfg as well. Since the backlight case in
# screen.sh is identical across all three, patch it unconditionally rather than
# reasoning about which mode is selected.
#
# This blocks dimming calls but allows the S99root 0->100 cycle.
#
# The smart patch:
# - Allows "backlight 100" (needed for S99root initialization cycle)
# - Blocks other values (10, 0, etc.) when helixscreen_active flag exists
patch_forgex_screen_sh() {
    screen_sh="/opt/config/mod/.shell/screen.sh"

    if [ ! -f "$screen_sh" ]; then
        log_info "ForgeX screen.sh not found, skipping patch"
        return 1
    fi

    # Check if already patched (look for the smart patch signature)
    if grep -q 'helixscreen_active.*!=.*100' "$screen_sh" 2>/dev/null; then
        log_info "ForgeX screen.sh already has smart patch"
        return 0
    fi

    # Remove old-style patch if present (blocks ALL backlight when flag exists)
    if grep -q "helixscreen_active" "$screen_sh" 2>/dev/null; then
        log_info "Removing old-style patch from screen.sh..."
        tmp_file="${screen_sh}.tmp"
        grep -v "helixscreen_active\|# Skip if HelixScreen" "$screen_sh" > "$tmp_file"
        $SUDO mv "$tmp_file" "$screen_sh"
        $SUDO chmod +x "$screen_sh"
    fi

    # Find the backlight) case and add our guard
    if ! grep -q "^[[:space:]]*backlight)" "$screen_sh"; then
        log_info "Could not find backlight case in screen.sh"
        return 1
    fi

    log_info "Patching ForgeX screen.sh with smart backlight control..."

    # Use awk to insert our check after "backlight)" line (BusyBox compatible)
    # Smart patch: only block non-100 values, allowing S99root's 0→100 init cycle
    tmp_file="${screen_sh}.tmp"
    awk '
    /^[[:space:]]*backlight\)/ {
        print
        print "        # Skip non-100 backlight changes when HelixScreen is controlling display"
        print "        # Allows S99root 0->100 init cycle but blocks Klipper eco dimming"
        print "        if [ -f /tmp/helixscreen_active ] && [ \"$2\" != \"100\" ]; then"
        print "            exit 0"
        print "        fi"
        next
    }
    { print }
    ' "$screen_sh" > "$tmp_file"

    if [ -s "$tmp_file" ] && grep -q 'helixscreen_active.*!=.*100' "$tmp_file" 2>/dev/null; then
        $SUDO mv "$tmp_file" "$screen_sh"
        $SUDO chmod +x "$screen_sh"
        log_success "ForgeX screen.sh patched with smart backlight control"
        return 0
    else
        rm -f "$tmp_file"
        log_warn "Failed to patch ForgeX screen.sh"
        return 1
    fi
}

# Remove HelixScreen patch from ForgeX screen.sh (for uninstall)
unpatch_forgex_screen_sh() {
    screen_sh="/opt/config/mod/.shell/screen.sh"

    if [ ! -f "$screen_sh" ]; then
        return 1
    fi

    # Check if patched
    if ! grep -q "helixscreen_active" "$screen_sh" 2>/dev/null; then
        log_info "ForgeX screen.sh not patched, nothing to remove"
        return 0
    fi

    log_info "Removing HelixScreen patch from ForgeX screen.sh..."

    # Use awk to remove only our specific block (BusyBox compatible)
    # Match and skip: comment line, if line with helixscreen_active, exit 0, fi
    tmp_file="${screen_sh}.tmp"
    awk '
    /# Skip if HelixScreen is controlling the display/ { skip=1; next }
    /if \[ -f \/tmp\/helixscreen_active \]; then/ { skip=1; next }
    skip && /^[[:space:]]*exit 0[[:space:]]*$/ { next }
    skip && /^[[:space:]]*fi[[:space:]]*$/ { skip=0; next }
    { print }
    ' "$screen_sh" > "$tmp_file"

    if [ -s "$tmp_file" ]; then
        $SUDO mv "$tmp_file" "$screen_sh"
        $SUDO chmod +x "$screen_sh"
    else
        rm -f "$tmp_file"
        log_warn "Failed to unpatch ForgeX screen.sh"
        return 1
    fi

    # Verify removal
    if grep -q "helixscreen_active" "$screen_sh" 2>/dev/null; then
        log_warn "Could not fully remove patch from screen.sh"
        return 1
    fi

    log_success "ForgeX screen.sh patch removed"
    return 0
}

# Disable stock FlashForge UI in auto_run.sh
# The stock firmware UI (ffstartup-arm/firmwareExe) is started by /opt/auto_run.sh
# which runs AFTER init scripts. We comment out the line to prevent it starting.
disable_stock_firmware_ui() {
    auto_run="/opt/auto_run.sh"
    if [ -f "$auto_run" ]; then
        # Check if ffstartup-arm line exists and is not already commented
        if grep -q "^/opt/PROGRAM/ffstartup-arm" "$auto_run"; then
            log_info "Disabling stock FlashForge UI in auto_run.sh..."
            # Comment out the ffstartup-arm line
            $SUDO sed -i 's|^/opt/PROGRAM/ffstartup-arm|# Disabled by HelixScreen: /opt/PROGRAM/ffstartup-arm|' "$auto_run"
            log_success "Stock FlashForge UI disabled"
            return 0
        fi
    fi
    return 1
}

# Re-enable stock FlashForge UI in auto_run.sh (for uninstall)
restore_stock_firmware_ui() {
    auto_run="/opt/auto_run.sh"
    if [ -f "$auto_run" ]; then
        # Check if our disabled line exists
        if grep -q "^# Disabled by HelixScreen: /opt/PROGRAM/ffstartup-arm" "$auto_run"; then
            log_info "Re-enabling stock FlashForge UI in auto_run.sh..."
            # Uncomment the ffstartup-arm line
            $SUDO sed -i 's|^# Disabled by HelixScreen: /opt/PROGRAM/ffstartup-arm|/opt/PROGRAM/ffstartup-arm|' "$auto_run"
            log_success "Stock FlashForge UI re-enabled"
            return 0
        fi
    fi
    return 1
}

# screen.sh commands that draw to the framebuffer and must stand down while
# HelixScreen owns it. Unguarded, these overwrite our splash during boot:
# S99root and S00init both drive them.
#
# Forge-X 1.4.0 and 1.4.1 ship draw_loading, draw_splash and boot_message.
# 1.4.2 drops the first and third, and adds splash_start, which launches a
# long-running splash process over a control FIFO. Which ones exist is decided
# per firmware at install time rather than by version number.
#
# splash_stop is deliberately absent: blocking it would strand that splash
# process on screen for the rest of the boot.
FORGEX_DRAW_COMMANDS="draw_loading draw_splash boot_message splash_start"

# Is the case label for $2 in screen.sh $1 already followed by our guard?
# Looks only at the lines immediately under the label, so an unrelated guard
# elsewhere in the file cannot vouch for this one.
forgex_case_is_guarded() {
    awk -v lbl="$2" '
        $0 ~ "^[[:space:]]*" lbl "\\)" { found = 1; next }
        found {
            if ($0 ~ /helixscreen_active/) { hit = 1; exit }
            if (++n >= 5) exit
        }
        END { exit !hit }
    ' "$1"
}

# Guard every draw command this firmware has, and prove each one took.
# Reports failure rather than success when a command it found could not be
# guarded, so a future Forge-X that reshapes screen.sh is loud instead of
# quietly leaving the framebuffer contended.
patch_forgex_screen_drawing() {
    screen_sh="/opt/config/mod/.shell/screen.sh"

    if [ ! -f "$screen_sh" ]; then
        log_info "ForgeX screen.sh not found, skipping screen drawing patch"
        return 1
    fi

    # Which draw commands this firmware actually has, and which of those still
    # need a guard. Re-running only patches what is missing, so the function is
    # idempotent and also repairs a partially patched screen.sh.
    present=""
    unguarded=""
    for cmd in $FORGEX_DRAW_COMMANDS; do
        grep -q "^[[:space:]]*${cmd})" "$screen_sh" || continue
        present="$present $cmd"
        forgex_case_is_guarded "$screen_sh" "$cmd" || unguarded="$unguarded $cmd"
    done

    if [ -z "$present" ]; then
        log_warn "ForgeX screen.sh has no known draw commands - not patching"
        return 1
    fi

    if [ -z "$unguarded" ]; then
        log_info "ForgeX screen.sh already has screen drawing patches"
        return 0
    fi

    log_info "Patching ForgeX screen.sh to skip drawing when HelixScreen active..."

    tmp_file="${screen_sh}.tmp"
    awk -v cmds="$unguarded" '
    BEGIN { n = split(cmds, want, " ") }
    {
        print
        for (i = 1; i <= n; i++) {
            if ($0 ~ "^[[:space:]]*" want[i] "\\)") {
                print "        # Skip when HelixScreen is controlling display"
                print "        if [ -f /tmp/helixscreen_active ]; then"
                print "            exit 0"
                print "        fi"
                break
            }
        }
    }
    ' "$screen_sh" > "$tmp_file"

    if [ ! -s "$tmp_file" ]; then
        rm -f "$tmp_file"
        log_warn "Failed to patch ForgeX screen.sh for screen drawing"
        return 1
    fi

    # Verify every command we set out to guard actually got one, on the
    # candidate file, before it replaces the original. A whole-file grep for
    # helixscreen_active cannot do this: one successful insertion would vouch
    # for every label that silently failed to match.
    still_unguarded=""
    for cmd in $unguarded; do
        forgex_case_is_guarded "$tmp_file" "$cmd" || still_unguarded="$still_unguarded $cmd"
    done

    if [ -n "$still_unguarded" ]; then
        rm -f "$tmp_file"
        log_warn "Failed to guard ForgeX draw commands:${still_unguarded}"
        return 1
    fi

    $SUDO mv "$tmp_file" "$screen_sh"
    $SUDO chmod +x "$screen_sh"
    log_success "ForgeX screen.sh patched for screen drawing (${unguarded# })"
    return 0
}

# Remove screen drawing patches from ForgeX screen.sh (for uninstall)
unpatch_forgex_screen_drawing() {
    screen_sh="/opt/config/mod/.shell/screen.sh"

    if [ ! -f "$screen_sh" ]; then
        return 1
    fi

    # Check if our drawing patches are present
    if ! grep -q '# Skip when HelixScreen is controlling display' "$screen_sh" 2>/dev/null; then
        log_info "ForgeX screen.sh has no drawing patches, nothing to remove"
        return 0
    fi

    log_info "Removing HelixScreen drawing patches from ForgeX screen.sh..."

    # Remove our 4-line block: comment + if + exit 0 + fi
    tmp_file="${screen_sh}.tmp"
    awk '
    /# Skip when HelixScreen is controlling display/ { skip=1; next }
    skip && /if \[ -f \/tmp\/helixscreen_active \]; then/ { next }
    skip && /^[[:space:]]*exit 0[[:space:]]*$/ { next }
    skip && /^[[:space:]]*fi[[:space:]]*$/ { skip=0; next }
    { print }
    ' "$screen_sh" > "$tmp_file"

    if [ -s "$tmp_file" ]; then
        $SUDO mv "$tmp_file" "$screen_sh"
        $SUDO chmod +x "$screen_sh"
    else
        rm -f "$tmp_file"
        log_warn "Failed to unpatch ForgeX screen.sh drawing patches"
        return 1
    fi

    # Verify removal
    if grep -q '# Skip when HelixScreen is controlling display' "$screen_sh" 2>/dev/null; then
        log_warn "Could not fully remove drawing patches from screen.sh"
        return 1
    fi

    log_success "ForgeX screen.sh drawing patches removed"
    return 0
}

# Install logged wrapper to prevent direct framebuffer writes during boot
# ForgeX's 'logged' binary writes directly to /dev/fb0 when --send-to-screen is used,
# bypassing our screen.sh patches. This wrapper strips that flag when HelixScreen is active.
install_forgex_logged_wrapper() {
    logged_bin="/opt/config/mod/.bin/exec/logged"
    logged_real="/opt/config/mod/.bin/exec/logged-real"
    logged_wrapper="/opt/config/mod/.bin/exec/logged-wrapper"

    if [ ! -f "$logged_bin" ]; then
        log_info "ForgeX logged binary not found, skipping wrapper"
        return 1
    fi

    # Always re-write the wrapper to pick up fixes on upgrade
    if [ -L "$logged_bin" ] && [ -f "$logged_real" ]; then
        log_info "Updating ForgeX logged wrapper..."
    elif [ -L "$logged_bin" ]; then
        # Don't wrap if it's already a symlink to something else
        log_warn "ForgeX logged is already a symlink, skipping wrapper"
        return 1
    else
        log_info "Installing ForgeX logged wrapper..."
    fi

    # Create the wrapper script
    cat > "$logged_wrapper" << 'WRAPPER_EOF'
#!/bin/sh
# Wrapper for logged that strips --send-to-screen when HelixScreen is active
# The logged binary writes directly to /dev/fb0, bypassing screen.sh patches

if [ -f /tmp/helixscreen_active ]; then
    # Remove --send-to-screen and related args, keep everything else
    args=""
    skip_next=0
    for arg in "$@"; do
        if [ $skip_next -eq 1 ]; then
            skip_next=0
            continue
        fi
        case "$arg" in
            --send-to-screen) continue ;;
            --screen-no-followup) continue ;;
            --screen-level) skip_next=1; continue ;;
            --screen-queue) skip_next=1; continue ;;
            *) args="$args $arg" ;;
        esac
    done
    exec /opt/config/mod/.bin/exec/logged-real $args
else
    exec /opt/config/mod/.bin/exec/logged-real "$@"
fi
WRAPPER_EOF

    $SUDO chmod +x "$logged_wrapper"

    # Move original to logged-real and symlink logged to wrapper (skip if already done)
    if [ ! -L "$logged_bin" ]; then
        $SUDO mv "$logged_bin" "$logged_real"
        $SUDO ln -s "$logged_wrapper" "$logged_bin"
    fi

    if [ -L "$logged_bin" ] && [ -f "$logged_real" ]; then
        log_success "ForgeX logged wrapper installed"
        return 0
    else
        # Rollback on failure
        [ -f "$logged_real" ] && $SUDO mv "$logged_real" "$logged_bin"
        rm -f "$logged_wrapper"
        log_warn "Failed to install ForgeX logged wrapper"
        return 1
    fi
}

# Remove logged wrapper (for uninstall)
uninstall_forgex_logged_wrapper() {
    logged_bin="/opt/config/mod/.bin/exec/logged"
    logged_real="/opt/config/mod/.bin/exec/logged-real"
    logged_wrapper="/opt/config/mod/.bin/exec/logged-wrapper"

    if [ ! -f "$logged_real" ]; then
        return 0  # Not installed
    fi

    log_info "Removing ForgeX logged wrapper..."

    $SUDO rm -f "$logged_bin"
    $SUDO mv "$logged_real" "$logged_bin"
    $SUDO rm -f "$logged_wrapper"

    log_success "ForgeX logged wrapper removed"
    return 0
}

# Uninstall ForgeX-specific configuration (for uninstall)
# Restores display mode, stock UI, screen.sh, GuppyScreen/tslib init scripts,
# and cleans up backup files from manual patches.
# Note: Sets caller's `restored_ui` variable via dynamic scoping.
uninstall_forgex() {
    # Put the display mode back where install found it. 1.4.0/1.4.1 default to
    # STOCK and 1.4.2 to FEATHER, so a hardcoded restore target would leave one
    # of them on a mode the printer never had. GUPPY is the fallback for
    # installs predating the recorded value; it exists in every supported
    # Forge-X.
    if [ -f "/opt/config/mod_data/variables.cfg" ]; then
        restore_mode="GUPPY"
        if [ -r "$FORGEX_PREV_DISPLAY_F" ]; then
            saved_mode=$(cat "$FORGEX_PREV_DISPLAY_F" 2>/dev/null)
            case "$saved_mode" in
                STOCK|FEATHER|GUPPY|HEADLESS) restore_mode="$saved_mode" ;;
            esac
        fi

        if grep -q "display[[:space:]]*=[[:space:]]*'HEADLESS'" "/opt/config/mod_data/variables.cfg"; then
            log_info "Restoring ForgeX display mode to ${restore_mode}..."
            $SUDO sed -i "s/display[[:space:]]*=[[:space:]]*'HEADLESS'/display = '${restore_mode}'/" "/opt/config/mod_data/variables.cfg"
        fi
        $SUDO rm -f "$FORGEX_PREV_DISPLAY_F"
    fi

    # Restore stock FlashForge UI in auto_run.sh
    restore_stock_firmware_ui || true

    # Remove HelixScreen patches from screen.sh
    unpatch_forgex_screen_sh || true
    unpatch_forgex_screen_drawing || true

    # Remove logged wrapper
    uninstall_forgex_logged_wrapper || true

    # Re-enable GuppyScreen and tslib init scripts
    if [ -f "/opt/config/mod/.root/S80guppyscreen" ]; then
        $SUDO chmod +x "/opt/config/mod/.root/S80guppyscreen" 2>/dev/null || true
        # shellcheck disable=SC2034  # consumed by uninstall.sh (previous-UI restore chain) and the uninstaller bundle
        restored_ui="GuppyScreen (/opt/config/mod/.root/S80guppyscreen)"
    fi
    if [ -f "/opt/config/mod/.root/S35tslib" ]; then
        $SUDO chmod +x "/opt/config/mod/.root/S35tslib" 2>/dev/null || true
    fi
    # install_forgex de-execs the launcher as well as the init script, so the
    # GUPPY fallback below must restore both or the restored UI never starts.
    if [ -f "/opt/config/mod/.root/guppyscreen" ]; then
        $SUDO chmod +x "/opt/config/mod/.root/guppyscreen" 2>/dev/null || true
    fi

    # Clean up any leftover backup files from manual patches
    for backup_file in /opt/config/mod/.shell/*.helix-backup /opt/config/mod/.shell/*.bak; do
        if [ -f "$backup_file" ] 2>/dev/null; then
            log_info "Removing leftover backup: $backup_file"
            $SUDO rm -f "$backup_file"
        fi
    done
}
