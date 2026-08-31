#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: competing_uis
# Stop competing screen UIs (GuppyScreen, KlipperScreen, Xorg, stock Creality/FlashForge/Sovol UI)
#
# Reads: AD5M_FIRMWARE, K1_FIRMWARE, INIT_SYSTEM, PREVIOUS_UI_SCRIPT, SUDO, INSTALL_DIR, HOST_OWNS_COMPETING_UIS

# Source guard
[ -n "${_HELIX_COMPETING_UIS_SOURCED:-}" ] && return 0
_HELIX_COMPETING_UIS_SOURCED=1

# Known competing screen UIs to stop
# Includes: GuppyScreen (AD5M/K1), Grumpyscreen (K1/Simple AF), KlipperScreen, FeatherScreen,
# mksclient (Sovol SV06 Ace stock touchscreen UI), QIDI stock screen (Q2/Plus 4/Max 4
# firmware 01.01.02+: systemd unit `qidi-client` runs the `qidiclient` binary with
# Restart=always, so the systemd stop+disable in the loop below is what actually keeps it
# down; the bare `qidiclient` entry is the process-kill backstop) (#1047).
COMPETING_UIS="guppyscreen GuppyScreen grumpyscreen Grumpyscreen KlipperScreen klipperscreen featherscreen FeatherScreen mksclient qidi-client qidiclient"

# Wayland compositors that hold the DRM/KMS master. On Armbian/Pi-class boards
# (e.g. BTT CB1) KlipperScreen commonly runs *inside* one of these; the
# compositor — not the UI process — owns /dev/dri/card0, so HelixScreen's direct
# DRM output gets EACCES ("not DRM master") until it is stopped. Matched by exact
# executable name via pidof. We deliberately do NOT touch getty/fbcon (they own
# the legacy console /dev/fb0, not the KMS master) or seatd (a seat broker, not a
# master holder) — stopping those carries risk without freeing card0.
WAYLAND_COMPOSITORS="cage weston labwc sway wayfire"

# Record a disabled service for later re-enablement
# Args: $1 = type ("systemd" or "sysv-chmod"), $2 = target (service name or script path)
record_disabled_service() {
    local type="$1"
    local target="$2"
    local entry="${type}:${target}"
    local state_file="${INSTALL_DIR}/config/.disabled_services"

    # Ensure config directory exists
    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    # Don't duplicate entries
    if [ -f "$state_file" ] && grep -qF "$entry" "$state_file" 2>/dev/null; then
        return 0
    fi

    echo "$entry" | $(file_sudo "${INSTALL_DIR}/config") tee -a "$state_file" >/dev/null
}

# Stop Wayland compositors holding the DRM master (cage/weston/labwc/sway/...).
# Run AFTER the named-UI loop so a compositor launched by a UI service (e.g.
# KlipperScreen.service ExecStart=cage -- screen.py) is already gone; this catches
# a standalone compositor service or one orphaned/launched from an autologin
# .profile that still pins /dev/dri/card0. Reversible: disabled services are
# recorded for re-enablement on uninstall. Sets found_any in the caller's scope.
stop_wayland_compositors() {
    local comp svc
    for comp in $WAYLAND_COMPOSITORS; do
        # Standalone systemd units (cage@tty1.service, weston.service, ...)
        if [ "$INIT_SYSTEM" = "systemd" ]; then
            for svc in "$comp" "${comp}@tty1"; do
                if systemctl is-active --quiet "$svc" 2>/dev/null; then
                    log_info "Stopping Wayland compositor service $svc (holds DRM master)..."
                    $SUDO systemctl stop "$svc" 2>/dev/null || true
                    $SUDO systemctl disable "$svc" 2>/dev/null || true
                    record_disabled_service "systemd" "$svc"
                    found_any=true
                fi
            done
        fi
        # Kill any lingering compositor process (exact basename via pidof)
        if kill_process_by_name "$comp"; then
            log_info "Killed lingering Wayland compositor: $comp (was holding /dev/dri/card0)"
            found_any=true
        fi
    done
}

# Stop ForgeX-specific competing UIs (stock FlashForge firmware UI)
stop_forgex_competing_uis() {
    # Stop stock FlashForge firmware UI (AD5M/Adventurer 5M)
    # ffstartup-arm is the startup manager that launches firmwareExe (the stock Qt UI)
    if [ -f /opt/PROGRAM/ffstartup-arm ]; then
        log_info "Stopping stock FlashForge UI..."
        kill_process_by_name firmwareExe ffstartup-arm || true
        found_any=true
    fi
}

# Stop Klipper Mod-specific competing UIs (Xorg, KlipperScreen)
stop_kmod_competing_uis() {
    # Stop Xorg first (required for framebuffer access)
    # Xorg takes over /dev/fb0 layer, preventing direct framebuffer rendering
    if [ -x "/etc/init.d/S40xorg" ]; then
        log_info "Stopping Xorg (Klipper Mod display server)..."
        $SUDO /etc/init.d/S40xorg stop 2>/dev/null || true
        # Disable Xorg init script (non-destructive, reversible)
        $SUDO chmod a-x /etc/init.d/S40xorg 2>/dev/null || true
        record_disabled_service "sysv-chmod" "/etc/init.d/S40xorg"
        # Kill any remaining Xorg processes
        kill_process_by_name Xorg X || true
        found_any=true
    fi

    # Kill python processes running KlipperScreen (common on Klipper Mod)
    # BusyBox ps doesn't support 'aux', use portable approach
    # shellcheck disable=SC2009
    for pid in $(ps -ef 2>/dev/null | grep -E 'KlipperScreen.*screen\.py' | grep -v grep | awk '{print $2}'); do
        log_info "Killing KlipperScreen python process (PID $pid)..."
        $SUDO kill "$pid" 2>/dev/null || true
        found_any=true
    done
}

# Stop stock Creality UI on K1 series (display-server, Monitor, master-server, etc.)
# S99start_app launches the entire stock Creality UI stack
stop_k1_stock_competing_uis() {
    if [ -x /etc/init.d/S99start_app ]; then
        log_info "Stopping stock Creality UI (S99start_app)..."
        /etc/init.d/S99start_app stop 2>/dev/null || true
        # Disable so it doesn't restart on reboot (reversible)
        chmod a-x /etc/init.d/S99start_app 2>/dev/null || true
        record_disabled_service "sysv-chmod" "/etc/init.d/S99start_app"
        found_any=true
    fi

    # Kill any remaining stock Creality UI processes
    for proc in display-server Monitor master-server audio-server wifi-server app-server upgrade-server web-server; do
        if kill_process_by_name "$proc"; then
            log_info "Killed remaining $proc process"
            found_any=true
        fi
    done

    # S99start_app also manages dropbear (SSH) on stock K1 firmware.
    # Disabling it kills SSH on next reboot (#535). Ensure SSH survives.
    ensure_k1_ssh
}

# Stop the Sovol SV06 Ace stock touchscreen UI (#986).
# Unlike the K1/CC1 stock UIs, mksclient is NOT a systemd/init.d service: it is a
# plain binary at /home/sovol/printer_data/build/mksclient run as user `sovol`.
# So there is no init script to chmod -x; we disable persistence by removing the
# execute bit on the binary itself (the existing re-enable path chmod +x restores
# it). mksclient also exports sysfs GPIO 67 at boot (the camera light), which pins
# it away from Klipper's [output_pin cameralight] — so after killing mksclient we
# unexport GPIO 67 to release it. Sets found_any in the caller's scope.
stop_sovol_competing_uis() {
    local bin
    for bin in /home/sovol/printer_data/build/mksclient /home/*/printer_data/build/mksclient; do
        # Skip if the glob didn't match (literal pattern returned) or not a file.
        [ -f "$bin" ] || continue
        log_info "Stopping stock Sovol UI (mksclient: $bin)..."
        kill_process_by_name mksclient || true
        # Persistent disable: drop the execute bit so it can't relaunch on boot.
        # Recorded as sysv-chmod so uninstall's chmod +x re-enables the binary.
        $SUDO chmod a-x "$bin" 2>/dev/null || true
        record_disabled_service "sysv-chmod" "$bin"
        found_any=true
    done

    # mksclient exports GPIO 67 (camera light) at boot; release it so Klipper's
    # [output_pin cameralight] can claim the line after the UI is gone.
    if [ -e /sys/class/gpio/gpio67 ]; then
        echo 67 > /sys/class/gpio/unexport 2>/dev/null || true
    fi
}

# Ensure SSH (dropbear) is running and will start on boot.
# On stock K1 firmware, dropbear is managed by S99start_app which we disable.
# This creates an independent dropbear init script so SSH survives reboots.
ensure_k1_ssh() {
    # Already running — nothing to do
    if pidof dropbear >/dev/null 2>&1; then
        # Make sure it has a boot-time init script
        _ensure_dropbear_init_script
        return 0
    fi

    # Try existing init script first
    for script in /etc/init.d/S50dropbear /etc/init.d/S*dropbear*; do
        [ -f "$script" ] || continue
        chmod +x "$script" 2>/dev/null || true
        "$script" start 2>/dev/null || true
        if pidof dropbear >/dev/null 2>&1; then
            log_info "SSH (dropbear) started via $script"
            return 0
        fi
    done

    # No init script or it failed — start dropbear directly
    local dropbear_bin=""
    for bin in /usr/sbin/dropbear /usr/bin/dropbear /sbin/dropbear; do
        if [ -x "$bin" ]; then
            dropbear_bin="$bin"
            break
        fi
    done

    if [ -n "$dropbear_bin" ]; then
        log_info "Starting dropbear directly ($dropbear_bin)..."
        "$dropbear_bin" -R 2>/dev/null || true
        _ensure_dropbear_init_script "$dropbear_bin"
        if pidof dropbear >/dev/null 2>&1; then
            log_info "SSH (dropbear) started successfully"
        else
            log_warn "Failed to start dropbear — SSH may not be available"
        fi
    else
        log_warn "dropbear not found — SSH may not be available"
    fi
}

# Create a minimal dropbear init script if one doesn't exist.
# Ensures SSH starts on boot independently of S99start_app.
# Args: $1 = dropbear binary path (optional, defaults to /usr/sbin/dropbear)
_ensure_dropbear_init_script() {
    local dropbear_path="${1:-/usr/sbin/dropbear}"

    # Check if any dropbear init script already exists
    for script in /etc/init.d/S50dropbear /etc/init.d/S*dropbear*; do
        if [ -f "$script" ] && [ -x "$script" ]; then
            return 0
        fi
    done

    # Create a minimal init script at S50 (before HelixScreen at S99)
    log_info "Creating dropbear init script (S50dropbear)..."
    cat > /etc/init.d/S50dropbear << INITEOF
#!/bin/sh
# Auto-created by HelixScreen to ensure SSH survives S99start_app being disabled
DROPBEAR="${dropbear_path}"
PIDFILE="/var/run/dropbear.pid"
case "\$1" in
    start)
        [ -x "\$DROPBEAR" ] || exit 0
        "\$DROPBEAR" -R -P "\$PIDFILE"
        ;;
    stop)
        [ -f "\$PIDFILE" ] && kill "\$(cat "\$PIDFILE")" 2>/dev/null
        killall dropbear 2>/dev/null || true
        rm -f "\$PIDFILE"
        ;;
    restart)
        \$0 stop
        sleep 1
        \$0 start
        ;;
    *)
        echo "Usage: \$0 {start|stop|restart}"
        exit 1
        ;;
esac
INITEOF
    chmod +x /etc/init.d/S50dropbear
}

# COSMOS sibling UIs that gui-switcher may launch. config-manager hardcodes this
# exact allowlist (['grumpyscreen','guppyscreen','atomscreen']) and rejects
# 'helixscreen', so whichever one COSMOS picks as screen_ui after an upgrade must
# delegate to us. We wrap ALL THREE so the handoff works regardless of selection.
CC1_SIBLING_UIS="grumpyscreen guppyscreen atomscreen"

# Install a delegating wrapper for ONE COSMOS sibling init script.
# Idempotent and backup-safe:
#   - Skip if the file already carries the HELIXSCREEN_WRAPPER marker.
#   - Back up the original to <s>.helix-bak ONLY if no backup exists yet — never
#     overwrite an existing backup (that would archive our own wrapper and make
#     uninstall restore the wrapper instead of the real UI).
#   - Replace with a wrapper that execs /etc/init.d/helixscreen, chmod +x.
# Args: $1 = sibling name (grumpyscreen|guppyscreen|atomscreen)
# Upstream fix tracked at OpenCentauri/cosmos#145.
_install_cc1_sibling_wrapper() {
    local s="$1"
    local target="/etc/init.d/${s}"
    local backup="/etc/init.d/${s}.helix-bak"

    # Nothing to wrap if the sibling init script isn't present on this device.
    [ -e "$target" ] || return 0

    # Already wrapped — idempotent no-op.
    if grep -q "HELIXSCREEN_WRAPPER" "$target" 2>/dev/null; then
        return 0
    fi

    # Preserve the real UI exactly once. NEVER overwrite an existing backup:
    # a second install pass must not archive a wrapper as if it were the original.
    if [ ! -e "$backup" ]; then
        if ! $SUDO cp -p "$target" "$backup"; then
            log_warn "Could not back up ${s} — skipping wrapper install"
            return 0
        fi
    fi

    log_info "Substituting /etc/init.d/${s} with helixscreen wrapper"
    if $SUDO tee "$target" >/dev/null <<'WRAPPER_EOF'
#!/bin/sh
# HELIXSCREEN_WRAPPER (do not remove this marker — used for idempotency)
# Workaround for OpenCentauri config-manager allowlist (cosmos#145):
# config-manager refuses to accept 'helixscreen' as screen_ui, so gui-switcher
# only ever launches one of grumpyscreen/guppyscreen/atomscreen. We delegate to
# HelixScreen's init script. The original UI is preserved at <name>.helix-bak
# and restored by HelixScreen's uninstaller.
exec /etc/init.d/helixscreen "$@"
WRAPPER_EOF
    then
        $SUDO chmod +x "$target" 2>/dev/null || true
    else
        log_warn "Failed to install ${s} wrapper — gui-switcher may not launch HelixScreen at boot"
    fi
}

# Register HelixScreen with COSMOS's gui-switcher and stop the currently active GUI.
# On Centauri Carbon / COSMOS, sibling UIs (grumpyscreen, guppyscreen, atomscreen) are
# peers managed by gui-switcher. config-manager rejects 'helixscreen' (allowlist), so
# rather than fight it we (1) point screen_ui at a sibling we wrap, and (2) replace each
# sibling init script with a wrapper that delegates to HelixScreen.
stop_cc1_competing_uis() {
    if ! command -v config-manager >/dev/null 2>&1; then
        log_warn "config-manager not found — cannot register with gui-switcher"
        return 0
    fi

    # Stop the currently active GUI before we hand the framebuffer to ourselves.
    local current_ui
    current_ui=$(config-manager ui screen_ui 2>/dev/null || echo "")
    if [ -n "$current_ui" ] && [ -x "/etc/init.d/${current_ui}" ]; then
        log_info "Stopping active COSMOS GUI: ${current_ui}"
        /etc/init.d/"${current_ui}" stop 2>/dev/null || true
        found_any=true
    fi

    # Ensure screen_ui points at a sibling we are going to wrap. We hijack all of
    # grumpyscreen/guppyscreen/atomscreen, so any of them is fine — but we
    # deliberately do NOT write 'helixscreen' (config-manager rejects it and
    # prints a scary "Invalid value 'helixscreen'" warning on every COSMOS
    # operation). Only change screen_ui if it's empty or set to something that
    # is NOT one of the siblings we wrap.
    local need_set=true
    case " ${CC1_SIBLING_UIS} " in
        *" ${current_ui} "*) need_set=false ;;  # already a wrapped sibling
    esac
    if [ "$need_set" = true ]; then
        if config-manager ui screen_ui grumpyscreen 2>/dev/null; then
            log_info "Set COSMOS screen_ui to grumpyscreen (delegates to HelixScreen)"
        elif [ -f /etc/klipper/config/cosmos.conf ]; then
            log_info "Updating /etc/klipper/config/cosmos.conf directly..."
            $SUDO sed -i "s|^screen_ui[[:space:]]*=.*|screen_ui = grumpyscreen|" \
                /etc/klipper/config/cosmos.conf 2>/dev/null || true
        fi
    fi

    # COSMOS gui-switcher (S96 init) reads `config-manager ui screen_ui` and
    # launches /etc/init.d/<value>. /usr/bin is read-only squashfs so we can't
    # patch the config-manager validator. /etc is a writable overlay, so we
    # substitute EACH sibling init script with a wrapper that delegates to
    # /etc/init.d/helixscreen. Whichever sibling COSMOS selects after an upgrade,
    # it now delegates to us. Originals preserved at <name>.helix-bak.
    local s
    for s in $CC1_SIBLING_UIS; do
        _install_cc1_sibling_wrapper "$s"
    done
}

# Stop competing screen UIs (GuppyScreen, KlipperScreen, Xorg, etc.)
# Dispatches platform-specific logic, then runs generic UI stopping
#
# Reads: AD5M_FIRMWARE/MOD_FLAVOR, K1_FIRMWARE, HOST_OWNS_COMPETING_UIS,
#        INIT_SYSTEM, PREVIOUS_UI_SCRIPT, SUDO, INSTALL_DIR
stop_competing_uis() {
    # During self-update, competing UIs were already disabled during initial install.
    # Re-running this would chmod -x init scripts that may have been restored or
    # customized by the platform (e.g. ZMOD manages S80guppyscreen) (#314).
    if _is_self_update; then
        log_info "Skipping competing UI check (self-update; already handled at install)"
        return 0
    fi

    log_info "Checking for competing screen UIs..."

    # Mod hosts the probe recognized: the mod owns its UI lifecycle. Stopping,
    # killing, and de-execing its init scripts is its business, not the
    # standalone installer's — the generic loop's chmod a-x on the mod's .root
    # scripts is a footprint on firmware we do not manage. Our display
    # takeover lives in configure_forgex_display instead.
    [ "$HOST_OWNS_COMPETING_UIS" = "1" ] && return 0

    found_any=false

    # Platform-specific competing UI handling
    case "${AD5M_FIRMWARE:-}" in
        forge_x)    stop_forgex_competing_uis ;;
        klipper_mod) stop_kmod_competing_uis ;;
        zmod)
            # ZMOD manages its own init scripts (S80guppyscreen etc.)
            # Do NOT fall through to the generic loop which would chmod -x them
            log_info "ZMOD platform: skipping generic UI disabling (ZMOD-managed)"
            return 0
            ;;
    esac

    # K1 platform: stop stock Creality UI
    case "${K1_FIRMWARE:-}" in
        stock_klipper|guilouz) stop_k1_stock_competing_uis ;;
    esac

    # Sovol SV06 Ace: stock UI is the plain `mksclient` binary (not a service).
    # Run the dedicated handler when the binary is present so it gets chmod-a-x'd
    # and GPIO 67 is released (the generic loop below only does a fallback kill).
    if [ -f /home/sovol/printer_data/build/mksclient ] || ls /home/*/printer_data/build/mksclient >/dev/null 2>&1; then
        stop_sovol_competing_uis
    fi

    # CC1 / COSMOS: use gui-switcher handoff instead of disabling peers.
    # Early-return so the generic loop below doesn't chmod out grumpyscreen/guppyscreen/atomscreen.
    if [ "${platform:-}" = "cc1" ]; then
        stop_cc1_competing_uis
        if [ "$found_any" = true ]; then
            log_info "Waiting for previous GUI to stop..."
            sleep 2
        fi
        return 0
    fi

    # Snapmaker U1: unisrv is the proprietary camera/MQTT daemon (handles
    # Klipper's TIMELAPSE_START), NOT a UI. The real U1 stock UI is /usr/bin/gui
    # via /etc/init.d/S99screen, handled by snapmaker-u1-setup-autostart.sh.
    # Killing unisrv here breaks timelapse on every deploy.

    # Handle the specific previous UI if we know it (for clean reversibility)
    if [ -n "$PREVIOUS_UI_SCRIPT" ] && [ -x "$PREVIOUS_UI_SCRIPT" ] 2>/dev/null; then
        log_info "Stopping previous UI: $PREVIOUS_UI_SCRIPT"
        $SUDO "$PREVIOUS_UI_SCRIPT" stop 2>/dev/null || true
        # Disable by removing execute permission (non-destructive, reversible)
        $SUDO chmod a-x "$PREVIOUS_UI_SCRIPT" 2>/dev/null || true
        record_disabled_service "sysv-chmod" "$PREVIOUS_UI_SCRIPT"
        found_any=true
    fi

    for ui in $COMPETING_UIS; do
        # Check systemd services
        if [ "$INIT_SYSTEM" = "systemd" ]; then
            if systemctl is-active --quiet "$ui" 2>/dev/null; then
                log_info "Stopping $ui (systemd service)..."
                $SUDO systemctl stop "$ui" 2>/dev/null || true
                $SUDO systemctl disable "$ui" 2>/dev/null || true
                record_disabled_service "systemd" "$ui"
                found_any=true
            fi
        fi

        # Check SysV init scripts (various locations)
        for initscript in /etc/init.d/S*${ui}* /etc/init.d/${ui}* /opt/config/mod/.root/S*${ui}*; do
            # Skip if glob didn't match any files (literal pattern returned)
            [ -e "$initscript" ] || continue
            # Skip if this is the PREVIOUS_UI_SCRIPT we already handled
            if [ "$initscript" = "$PREVIOUS_UI_SCRIPT" ]; then
                continue
            fi
            if [ -x "$initscript" ]; then
                log_info "Stopping $ui ($initscript)..."
                $SUDO "$initscript" stop 2>/dev/null || true
                # Disable by removing execute permission (non-destructive)
                $SUDO chmod a-x "$initscript" 2>/dev/null || true
                record_disabled_service "sysv-chmod" "$initscript"
                found_any=true
            fi
        done

        # Kill any remaining processes by name
        if kill_process_by_name "$ui"; then
            log_info "Killed remaining $ui processes"
            found_any=true
        fi
    done

    # Free the DRM master from any Wayland compositor (KlipperScreen-under-cage etc.)
    stop_wayland_compositors

    if [ "$found_any" = true ]; then
        log_info "Waiting for competing UIs to stop..."
        sleep 2
    else
        log_info "No competing UIs found"
    fi
}
