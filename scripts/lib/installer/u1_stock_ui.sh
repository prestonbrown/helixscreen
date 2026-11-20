#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Snapmaker U1: give the display back to the stock screen.
#
# One copy of the rule, used by every door: the uninstaller (bundled into
# install.sh and uninstall.sh), and the platform hook's restore trigger, which
# sources the copy the U1 package ships as scripts/u1_stock_ui.sh. Plain POSIX
# sh with no installer dependencies, because the hook runs it at boot.

[ -n "${_HELIX_U1_STOCK_UI_SOURCED:-}" ] && return 0
_HELIX_U1_STOCK_UI_SOURCED=1

# Log through the installer's log_info/log_warn when they exist, else plain
# echo (the hook's output goes to the launcher log).
_u1_stock_log() {
    _u1_level="$1"
    shift
    if [ "$_u1_level" = warn ] && command -v log_warn >/dev/null 2>&1; then
        log_warn "$*"
    elif [ "$_u1_level" = info ] && command -v log_info >/dev/null 2>&1; then
        log_info "$*"
    else
        echo "$*"
    fi
}

# Undo what snapmaker-u1-setup-autostart.sh did to the firmware:
#   1. Re-enable /usr/bin/gui (chmod +x) so a stock launcher can exec it.
#   2. Restore the launcher(s): from S99screen.stock on PAXX 1.3, by removing
#      our HelixScreen-marked S99screen on PAXX 1.4, from S99fb-http.stock on
#      PAXX 1.4, and from S99input-event-daemon.stock on stock firmware.
#   3. Drop the /oem/.debug flag, which stops /etc/init.d/S01aoverlayfs from
#      wiping /oem/overlay/* on boot, but only when our installer created it
#      (autostart step 1 leaves /oem/.debug.helixscreen beside it). Removing it
#      restores stock overlay-wipe-on-boot: the next boot discards the
#      overlay-upper and runs the pristine squashfs-lower rootfs. A flag another
#      mod created (multiACE creates it too) stays, or that mod's overlay files
#      go at the next boot.
# HELIX_U1_ROOT prefixes every path (empty on the device; a mock rootfs in
# tests). $SUDO is empty when run as root. Never fails the caller.
u1_restore_stock_ui() {
    _r="${HELIX_U1_ROOT:-}"
    if [ -f "$_r/usr/bin/gui" ] && [ ! -x "$_r/usr/bin/gui" ]; then
        _u1_stock_log info "Re-enabling stock UI binary (/usr/bin/gui)"
        ${SUDO:-} chmod +x "$_r/usr/bin/gui" 2>/dev/null \
            || _u1_stock_log warn "Could not re-enable /usr/bin/gui — stock UI may not start on next boot"
    fi
    if [ -f "$_r/etc/init.d/S99screen.stock" ]; then
        _u1_stock_log info "Restoring stock /etc/init.d/S99screen (firmware 1.3)"
        ${SUDO:-} mv "$_r/etc/init.d/S99screen.stock" "$_r/etc/init.d/S99screen" \
            || _u1_stock_log warn "Could not restore /etc/init.d/S99screen — stock UI launcher may be missing"
    elif [ -f "$_r/etc/init.d/S99screen" ] && \
         grep -q HelixScreen "$_r/etc/init.d/S99screen" 2>/dev/null; then
        _u1_stock_log info "Removing HelixScreen-created /etc/init.d/S99screen (firmware 1.4)"
        ${SUDO:-} rm -f "$_r/etc/init.d/S99screen" 2>/dev/null || true
    fi
    # Firmware 1.4: the framebuffer-HTTP screen launcher (1.2/1.3 have no .stock).
    if [ -f "$_r/etc/init.d/S99fb-http.stock" ]; then
        _u1_stock_log info "Restoring stock /etc/init.d/S99fb-http (firmware 1.4)"
        ${SUDO:-} mv "$_r/etc/init.d/S99fb-http.stock" "$_r/etc/init.d/S99fb-http" \
            || _u1_stock_log warn "Could not restore /etc/init.d/S99fb-http — stock screen launcher may be missing"
    fi
    # Stock firmware: the input-event-daemon launcher. .stock is present only if
    # we patched it.
    if [ -f "$_r/etc/init.d/S99input-event-daemon.stock" ]; then
        _u1_stock_log info "Restoring stock /etc/init.d/S99input-event-daemon"
        ${SUDO:-} mv "$_r/etc/init.d/S99input-event-daemon.stock" "$_r/etc/init.d/S99input-event-daemon" \
            || _u1_stock_log warn "Could not restore /etc/init.d/S99input-event-daemon"
    fi
    if [ -f "$_r/oem/.debug.helixscreen" ]; then
        if [ -f "$_r/oem/.debug" ]; then
            _u1_stock_log info "Removing /oem/.debug (restores stock overlay-wipe-on-boot)"
            ${SUDO:-} rm -f "$_r/oem/.debug" 2>/dev/null || true
        fi
        ${SUDO:-} rm -f "$_r/oem/.debug.helixscreen" 2>/dev/null || true
    elif [ -f "$_r/oem/.debug" ]; then
        _u1_stock_log info "Keeping /oem/.debug: HelixScreen did not create it"
    fi
    unset _r _u1_level
    return 0
}
