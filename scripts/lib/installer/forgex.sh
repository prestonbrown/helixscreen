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

# Configure ForgeX display settings for HelixScreen
# We use GUPPY mode because ForgeX handles backlight properly in this mode.
# STOCK mode expects ffstartup-arm to manage display/backlight which doesn't work for us.
# We disable GuppyScreen's init scripts so HelixScreen takes over the display.
configure_forgex_display() {
    var_file="/opt/config/mod_data/variables.cfg"
    guppy_init="/opt/config/mod/.root/S80guppyscreen"
    tslib_init="/opt/config/mod/.root/S35tslib"
    changed=false

    # Set display mode to GUPPY (required for backlight to work)
    if [ -f "$var_file" ]; then
        if grep -q "display[[:space:]]*=[[:space:]]*'STOCK'" "$var_file"; then
            log_info "Setting ForgeX display mode to GUPPY..."
            $SUDO sed -i "s/display[[:space:]]*=[[:space:]]*'STOCK'/display = 'GUPPY'/" "$var_file"
            changed=true
        elif grep -q "display[[:space:]]*=[[:space:]]*'HEADLESS'" "$var_file"; then
            log_info "Setting ForgeX display mode to GUPPY..."
            $SUDO sed -i "s/display[[:space:]]*=[[:space:]]*'HEADLESS'/display = 'GUPPY'/" "$var_file"
            changed=true
        fi
    fi

    # Disable GuppyScreen init script (remove execute permission)
    if [ -x "$guppy_init" ]; then
        log_info "Disabling GuppyScreen init script..."
        $SUDO chmod a-x "$guppy_init"
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
        log_success "ForgeX configured for HelixScreen (GUPPY mode, GuppyScreen disabled)"
        return 0
    fi
    return 1
}

# Patch ForgeX screen.sh to skip non-100 backlight control when HelixScreen is active
# ForgeX's headless.cfg runs a delayed_gcode that dims the backlight 3 seconds after
# Klipper starts. This patch blocks dimming calls but allows the S99root 0→100 cycle.
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

# Patch ForgeX screen.sh to skip screen drawing when HelixScreen is active
# ForgeX's S99root calls draw_splash, draw_loading, and boot_message which write
# directly to the framebuffer, overwriting our splash screen during boot.
patch_forgex_screen_drawing() {
    screen_sh="/opt/config/mod/.shell/screen.sh"

    if [ ! -f "$screen_sh" ]; then
        log_info "ForgeX screen.sh not found, skipping screen drawing patch"
        return 1
    fi

    # Check if already patched (look for our signature in draw_splash)
    if grep -q 'draw_splash)' "$screen_sh" && \
       grep -A2 'draw_splash)' "$screen_sh" | grep -q 'helixscreen_active'; then
        log_info "ForgeX screen.sh already has screen drawing patches"
        return 0
    fi

    log_info "Patching ForgeX screen.sh to skip drawing when HelixScreen active..."

    # Patch draw_loading, draw_splash, and boot_message cases
    # Add helixscreen_active check after each case label
    tmp_file="${screen_sh}.tmp"
    awk '
    /^[[:space:]]*(draw_loading|draw_splash|boot_message)\)/ {
        print
        print "        # Skip when HelixScreen is controlling display"
        print "        if [ -f /tmp/helixscreen_active ]; then"
        print "            exit 0"
        print "        fi"
        next
    }
    { print }
    ' "$screen_sh" > "$tmp_file"

    if [ -s "$tmp_file" ] && grep -q 'helixscreen_active' "$tmp_file" 2>/dev/null; then
        $SUDO mv "$tmp_file" "$screen_sh"
        $SUDO chmod +x "$screen_sh"
        log_success "ForgeX screen.sh patched for screen drawing"
        return 0
    else
        rm -f "$tmp_file"
        log_warn "Failed to patch ForgeX screen.sh for screen drawing"
        return 1
    fi
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
    # Restore ForgeX display mode to GUPPY (from HEADLESS or STOCK)
    if [ -f "/opt/config/mod_data/variables.cfg" ]; then
        if grep -q "display[[:space:]]*=[[:space:]]*'HEADLESS'" "/opt/config/mod_data/variables.cfg"; then
            log_info "Restoring ForgeX display mode to GUPPY..."
            $SUDO sed -i "s/display[[:space:]]*=[[:space:]]*'HEADLESS'/display = 'GUPPY'/" "/opt/config/mod_data/variables.cfg"
        elif grep -q "display[[:space:]]*=[[:space:]]*'STOCK'" "/opt/config/mod_data/variables.cfg"; then
            log_info "Restoring ForgeX display mode to GUPPY..."
            $SUDO sed -i "s/display[[:space:]]*=[[:space:]]*'STOCK'/display = 'GUPPY'/" "/opt/config/mod_data/variables.cfg"
        fi
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

    # Clean up any leftover backup files from manual patches
    for backup_file in /opt/config/mod/.shell/*.helix-backup /opt/config/mod/.shell/*.bak; do
        if [ -f "$backup_file" ] 2>/dev/null; then
            log_info "Removing leftover backup: $backup_file"
            $SUDO rm -f "$backup_file"
        fi
    done
}
