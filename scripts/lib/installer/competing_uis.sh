#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: competing_uis
# What the install's stop step stops, for its title: the vendor UIs detection
# found, else the HelixScreen an update replaces, else a neutral phrase (the
# step still runs the platform's screen setup).
# Reads: COMPETING_UIS_FOUND, update_mode, INSTALL_DIR
stop_step_noun() {
    if [ -n "${COMPETING_UIS_FOUND:-}" ]; then
        printf '%s' "$COMPETING_UIS_FOUND"
    elif [ "${update_mode:-}" = true ] && [ -d "${INSTALL_DIR:-}" ] && ! _is_self_update; then
        printf 'the previous HelixScreen'
    else
        printf 'the current screen'
    fi
}

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
# `makerbase-client` is the stock screen unit on QIDI firmware 1.1.x;
# the loop acts only on a unit that is running or enabled (_unit_is_competing),
# so it is a no-op on a host that has no such unit.
COMPETING_UIS="guppyscreen GuppyScreen grumpyscreen Grumpyscreen KlipperScreen klipperscreen featherscreen FeatherScreen mksclient qidi-client qidiclient makerbase-client"

# The stock screen unit is `makerbase-client` on firmware 1.1.x and
# `qidi-client` on 01.01.02+, and both are in COMPETING_UIS above. Two shapes
# stay out of a name list's reach:
#
#   - The unit runs /home/<klipper-user>/QD_Q2/bin/client, which outlives the
#     unit as a bare process. Its basename is `client`, so pidof would match
#     unrelated processes on a general-purpose SBC; it is matched by full path.
#   - A unit a later firmware renames still runs the client or its start.sh
#     launcher, so units are discovered by what their ExecStart runs rather
#     than by what they are called. Match those entry points, not the whole
#     bin directory: it also contains tuning.sh for makerbase-tuning, which
#     must remain enabled. Require a token boundary to spare similarly named
#     helpers and backup scripts too.
#     This arm has no state gate at all, so it also reaches a stock screen the
#     vendor ships neither running nor enabled, and it folds case because a
#     unit name's capitalisation is the vendor's to change.
#
# Nothing either arm matches can exist off a QIDI box, so the handler needs no
# hostname gate. Both arms are reversible and recorded, so uninstall's
# reenable_disabled_services() puts the stock screen back.
QIDI_STOCK_UI_BINS="/home/mks/QD_Q2/bin/client /home/qidi/QD_Q2/bin/client"
QIDI_STOCK_UI_EXEC_PATTERN='QD_Q2/bin/(client|start\.sh)([^[:alnum:]_.-]|$)|qidiclient|qidi-client|makerbase-client'

# Wayland compositors that hold the DRM/KMS master. On Armbian/Pi-class boards
# (e.g. BTT CB1) KlipperScreen commonly runs *inside* one of these; the
# compositor — not the UI process — owns /dev/dri/card0, so HelixScreen's direct
# DRM output gets EACCES ("not DRM master") until it is stopped. Matched by exact
# executable name via pidof. We deliberately do NOT touch getty/fbcon (they own
# the legacy console /dev/fb0, not the KMS master) or seatd (a seat broker, not a
# master holder) — stopping those carries risk without freeing card0.
WAYLAND_COMPOSITORS="cage weston labwc sway wayfire"

# Display managers start an X or Wayland session that takes the display, one
# layer above the compositors (#1693). The default target is left alone: with
# the DM held down, graphical.target has nothing left to start.
DISPLAY_MANAGERS="lightdm gdm3 gdm sddm xdm nodm slim"

# Record a disabled service for later re-enablement
# Args: $1 = type ("systemd", "systemd-dropin" or "sysv-chmod"), $2 = target (service name or script path)
record_disabled_service() {
    local type="$1"
    local target="$2"
    local entry="${type}:${target}"
    local state_file
    state_file="$(_ledger_path .disabled_services)"

    # Ensure config directory exists
    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    # Don't duplicate entries
    if [ -f "$state_file" ] && grep -qxF "$entry" "$state_file" 2>/dev/null; then
        return 0
    fi

    echo "$entry" | $(file_sudo "${INSTALL_DIR}/config") tee -a "$state_file" >/dev/null
}

# True when a systemd unit competes for the display: running right now, or
# enabled so it takes the display back at the next boot. An enabled-but-stopped
# unit is the case a bare is-active check misses - nothing stops it, nothing
# records it, and it returns after the reboot the installer asks for.
#
# is-enabled is matched on its exact word rather than its exit status, which is
# also 0 for `static`, `indirect` and `alias`. None of those name a unit
# `systemctl disable` can turn off by itself, so acting on the exit status would
# record a reversal uninstall's `systemctl enable` cannot perform.
# Args: $1 = unit name
_unit_is_competing() {
    systemctl is-active --quiet "$1" 2>/dev/null && return 0
    case "$(systemctl is-enabled "$1" 2>/dev/null)" in
        enabled|enabled-runtime) return 0 ;;
    esac
    return 1
}

# Drop-in that keeps a competing unit from starting while HelixScreen is
# installed (#1534). Shared by install and uninstall.
# Args: $1 = unit name
competing_ui_dropin() {
    echo "${HELIX_SYSTEMD_UNIT_DIR:-/etc/systemd/system}/$1.d/helixscreen-competing.conf"
}

# Units taken down in this run, by resolved Id: gdm3 is an alias of
# gdm.service on Debian, and both names must land on the one real unit.
_TAKEN_DOWN_UNITS=""

# Stop and disable a competing systemd unit, and add a drop-in that refuses
# to start it while HelixScreen is installed. disable only removes the wants
# symlink, so a vendor firmware update that re-enables the unit would hand it
# the display back; mask is refused for a unit whose file lives in
# /etc/systemd/system, where KIAUH and most vendors put theirs. The drop-in
# survives both a re-enable and a rewrite of the unit file, and fails safe:
# if the binary goes away, the unit starts again. reset-failed clears a unit
# that was failing on its own out of `systemctl --failed`, where it would
# read as our doing.
# Args: $1 = unit name, $2 = "nostop" to leave a running unit up until reboot
_take_down_unit() {
    local unit dropin record=systemd-dropin
    unit=$(systemctl show -p Id --value "$1" 2>/dev/null)
    [ -n "$unit" ] || unit="$1"
    # systemd ignores a drop-in dir named after a bare unit name.
    case "$unit" in *.*) ;; *) unit="$unit.service" ;; esac
    case " $_TAKEN_DOWN_UNITS " in *" $unit "*) return 0 ;; esac
    _TAKEN_DOWN_UNITS="$_TAKEN_DOWN_UNITS $unit"

    dropin=$(competing_ui_dropin "$unit")
    if $SUDO mkdir -p "$(dirname "$dropin")" 2>/dev/null \
        && printf '[Unit]\nConditionPathExists=!%s/bin/helix-screen\n' "$INSTALL_DIR" \
            | $SUDO tee "$dropin" >/dev/null 2>&1; then
        $SUDO systemctl daemon-reload 2>/dev/null || true
    else
        log_warn "Could not write $dropin: $unit is only disabled, so a firmware update may re-enable it."
        record=systemd
    fi
    [ "${2:-}" = "nostop" ] || $SUDO systemctl stop "$unit" 2>/dev/null || true
    $SUDO systemctl disable "$unit" 2>/dev/null || true
    record_disabled_service "$record" "$unit"
    $SUDO systemctl reset-failed "$unit" 2>/dev/null || true
}

# True when this installer runs inside a desktop session, where stopping the
# display manager or compositor takes down the terminal running it. `sudo sh`
# resets the environment, so the installer's own variables are not enough:
# walk the ancestors and look for a display in theirs. An unreadable /proc
# reads as not graphical.
_in_graphical_session() {
    local proc="${HELIX_PROC_ROOT:-/proc}" pid="$$" hops=0 stat
    case "${XDG_SESSION_TYPE:-}" in x11|wayland) return 0 ;; esac
    [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && return 0
    while [ "$pid" -gt 1 ] 2>/dev/null && [ "$hops" -lt 32 ]; do
        tr '\0' '\n' 2>/dev/null < "$proc/$pid/environ" \
            | grep -qE '^(DISPLAY|WAYLAND_DISPLAY)=.' && return 0
        read -r stat 2>/dev/null < "$proc/$pid/stat" || return 1
        # comm may hold spaces and ')': the fields resume after the last ')'.
        # shellcheck disable=SC2086
        set -- ${stat##*)}
        pid="${2:-0}"
        hops=$((hops + 1))
    done
    return 1
}

# Take down a unit that may be drawing the session this installer runs in:
# inside one, leave it up and let the reboot finish the switch.
# Args: $1 = unit name, $2 = what it is, for the log
_take_down_session_unit() {
    if _in_graphical_session; then
        log_info "Disabling $2 $1 (this session runs on it)..."
        _take_down_unit "$1" nostop
        log_warn "Reboot to finish switching from $1 to HelixScreen."
    else
        log_info "Stopping and disabling $2 $1..."
        _take_down_unit "$1"
    fi
}

# Stop enabled or running display managers (#1693). Only on hosts whose
# firmware ships a screen of its own: on a generic Pi or x86 box the DM is the
# user's desktop. Sets found_any in the caller's scope.
stop_display_managers() {
    local dm
    [ "$INIT_SYSTEM" = "systemd" ] || return 0
    _host_ships_a_stock_ui || return 0
    for dm in $DISPLAY_MANAGERS; do
        _unit_is_competing "$dm" || continue
        _take_down_session_unit "$dm" "display manager"
        found_any=true
    done
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
                if _unit_is_competing "$svc"; then
                    _take_down_session_unit "$svc" "Wayland compositor (DRM master)"
                    found_any=true
                fi
            done
        fi
        # Killing the compositor this session draws on kills the installer's
        # terminal; the disabled unit is gone after the reboot.
        _in_graphical_session && continue
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

# PIDs of python processes running KlipperScreen's screen.py, one per line.
# BusyBox ps doesn't support 'aux', so this uses the portable -ef.
_kmod_klipperscreen_pids() {
    # shellcheck disable=SC2009
    ps -ef 2>/dev/null | grep -E 'KlipperScreen.*screen\.py' | grep -v grep | awk '{print $2}'
}

# Framebuffer contenders on stock K1 firmware. Monitor is listed first: it is
# a watchdog that respawns display-server moments after a kill.
K1_STOCK_UI_PROCS="Monitor display-server"

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
    for pid in $(_kmod_klipperscreen_pids); do
        log_info "Killing KlipperScreen python process (PID $pid)..."
        $SUDO kill "$pid" 2>/dev/null || true
        found_any=true
    done
}

# Whether the stock Creality backend (master-server, app-server, web-server)
# is kept alive on K1 series printers (prestonbrown/helixscreen#1468).
# 1 = install /etc/init.d/S99creality-backend so the trio starts at boot and
#     Creality Print / Creality Cloud keep reaching the printer.
# 0 = leave the whole stock stack down (the backend ports stay closed).
# This is the one-line default the platform carries; flip it here if the
# on-device coexistence verification says otherwise.
K1_CREALITY_BACKEND_ENABLED="${K1_CREALITY_BACKEND_ENABLED:-1}"

# Where the backend init script is deployed. S99creality-backend sorts before
# S99helixscreen and S99start_app, so the backend is up before the UI takes
# the display and before any stock script a later restore re-enables.
K1_CREALITY_BACKEND_INIT="/etc/init.d/S99creality-backend"

# Stop the stock Creality display stack on K1 series. Monitor and
# display-server are the framebuffer contenders; the backend trio
# (master-server, app-server, web-server) is what Creality Print and the
# Creality Cloud app talk to and is deliberately left to
# /etc/init.d/S99creality-backend (prestonbrown/helixscreen#1468).
# Disable and record the stock K1 UI init script, adopting a disable that
# predates this run. Split out of stop_k1_stock_competing_uis so main.sh's
# post-extract K1 block can re-run it after extract_release replaces the
# payload: the ledger lives in INSTALL_DIR/config, which the fresh-install
# swap moves aside with the old payload.
record_k1_stock_ui_disable() {
    # The record must exist no matter which install did the chmod. An
    # S99start_app that is already de-executed is a disable an earlier
    # HelixScreen install left behind (or an operator following our docs),
    # and hooks-k1.sh de-executes it again at every launch while it is
    # executable, so for as long as HelixScreen is installed the disable is
    # ours to reverse. Uninstall chmod +x's recorded targets only, and no
    # scan fallback names S99start_app, so an unrecorded disable would leave
    # the stock UI dead after uninstall.
    [ -f /etc/init.d/S99start_app ] || return 0
    # Disable so it doesn't restart on reboot (reversible)
    chmod a-x /etc/init.d/S99start_app 2>/dev/null || true
    record_disabled_service "sysv-chmod" "/etc/init.d/S99start_app"
}

stop_k1_stock_competing_uis() {
    if [ -f /etc/init.d/S99start_app ]; then
        if [ -x /etc/init.d/S99start_app ]; then
            log_info "Stopping stock Creality UI (S99start_app)..."
            /etc/init.d/S99start_app stop 2>/dev/null || true
        fi
        record_k1_stock_ui_disable
        found_any=true

        if [ "$K1_CREALITY_BACKEND_ENABLED" = "1" ]; then
            log_warn "Stock Creality display UI disabled; the backend servers (master-server, app-server, web-server) are kept running so Creality Print and Creality Cloud can reach this printer."
        else
            log_warn "Stock Creality backend disabled; Creality Print and the Creality Cloud app will no longer reach this printer."
        fi
    fi

    # Kill any remaining framebuffer contenders, Monitor first.
    for proc in $K1_STOCK_UI_PROCS; do
        if kill_process_by_name "$proc"; then
            log_info "Killed remaining $proc process"
            found_any=true
        fi
    done

    # S99start_app also manages dropbear (SSH) on stock K1 firmware.
    # Disabling it kills SSH on next reboot (#535). Ensure SSH survives.
    ensure_k1_ssh
}

# Install the init script that starts the stock Creality backend trio at boot
# (prestonbrown/helixscreen#1468). Must run AFTER extract_release: the script
# ships in the release package as config/creality-backend.init. The installed
# path is recorded with type "sysv-created" so reenable_disabled_services
# removes it on uninstall, leaving S99start_app to bring the whole stock
# stack back. No-op on Simple AF hosts (they remove S99start_app and its
# stack themselves, so there is no stock backend of ours to spare) and on
# every non-K1 firmware.
install_k1_creality_backend() {
    if [ "$K1_CREALITY_BACKEND_ENABLED" != "1" ]; then
        return 0
    fi

    case "${K1_FIRMWARE:-}" in
        stock_klipper|guilouz) ;;
        *) return 0 ;;
    esac

    local src="${INSTALL_DIR}/config/creality-backend.init"
    if [ ! -f "$src" ]; then
        log_warn "creality-backend.init missing from ${INSTALL_DIR}/config (the payload being installed may predate prestonbrown/helixscreen#1468); the Creality backend will not start at boot"
        return 0
    fi

    # Plain copy first (installs run as root on the K1 family), sudo fallback
    # for a non-root caller, both non-fatal.
    mkdir -p "$(dirname "$K1_CREALITY_BACKEND_INIT")" 2>/dev/null \
        || $SUDO mkdir -p "$(dirname "$K1_CREALITY_BACKEND_INIT")" 2>/dev/null || true
    if cp "$src" "$K1_CREALITY_BACKEND_INIT" 2>/dev/null \
       || $SUDO cp "$src" "$K1_CREALITY_BACKEND_INIT" 2>/dev/null; then
        chmod +x "$K1_CREALITY_BACKEND_INIT" 2>/dev/null \
            || $SUDO chmod +x "$K1_CREALITY_BACKEND_INIT" 2>/dev/null || true
        record_disabled_service "sysv-created" "$K1_CREALITY_BACKEND_INIT"
        log_info "Installed Creality backend init script: $K1_CREALITY_BACKEND_INIT"
        # Bring the trio up now, so the backend survives this install session
        # without a reboot (S99start_app's own stop, above, took it down).
        "$K1_CREALITY_BACKEND_INIT" start 2>/dev/null || true
    else
        log_warn "Could not install $K1_CREALITY_BACKEND_INIT; the Creality backend will not start at boot"
    fi
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

# True when this device's firmware ships a screen UI of its own, so finding no
# competing UI is a sign we failed to recognise it rather than a clean host.
# pi/pi32 and x86 are excluded: a generic SBC with no stock UI is the normal
# case. QIDI-class boxes resolve to pi, so they are matched by fingerprint.
_host_ships_a_stock_ui() {
    case "${platform:-}" in
        ad5m|ad5x|k1|k2|cc1|m1|snapmaker-u1) return 0 ;;
    esac
    command -v _is_qidi_class_sbc >/dev/null 2>&1 && _is_qidi_class_sbc
}

# Units whose ExecStart runs the QIDI stock screen, one per line.
_qidi_stock_ui_units() {
    local unit_path unit
    for unit_path in /etc/systemd/system/*.service /lib/systemd/system/*.service; do
        [ -f "$unit_path" ] || continue
        unit=$(basename "$unit_path")
        # Our own unit runs out of an install directory that can sit under the
        # same home as the stock UI, so its ExecStart matches the pattern too.
        case "$unit" in ${SERVICE_NAME:-helixscreen}*) continue ;; esac
        grep -E '^ExecStart=' "$unit_path" 2>/dev/null \
            | grep -qiE "$QIDI_STOCK_UI_EXEC_PATTERN" || continue
        echo "$unit"
    done
}

# Stop the QIDI stock screen in the two shapes COMPETING_UIS cannot name.
# Sets found_any in the caller's scope, like the sibling handlers.
stop_qidi_competing_uis() {
    local bin unit stopped=false

    # Units before binaries: the stock screen unit sets Restart=always with
    # StartLimitIntervalSec=0, and its start script runs the client a second
    # time under taskset when the first exits. A process killed while its unit
    # is still live comes straight back, once a second, with nothing to
    # throttle it. Disabling the unit first removes both respawn paths.
    for unit in $(_qidi_stock_ui_units); do
        log_info "Stopping stock QIDI UI unit ($unit)..."
        _take_down_unit "$unit"
        found_any=true
        stopped=true
    done

    for bin in $QIDI_STOCK_UI_BINS; do
        [ -f "$bin" ] || continue
        log_info "Stopping stock QIDI UI ($bin)..."
        kill_process_by_path "$bin" || true
        # Persistent disable: without the execute bit whatever launches it at
        # boot fails to exec. Recorded so uninstall's chmod +x restores it.
        $SUDO chmod a-x "$bin" 2>/dev/null || true
        record_disabled_service "sysv-chmod" "$bin"
        found_any=true
        stopped=true
    done

    # The stock client also supplies the MQTT link credentials and the QIDI Box
    # filament state, so replacing it costs more than the screen.
    if [ "$stopped" = true ]; then
        log_warn "QIDI Studio box sync, QIDI cloud and QIDI Box filament edits depend on the stock QIDI client"
        log_warn "and will not work while HelixScreen replaces it. Uninstalling HelixScreen restores them."
    fi
}

# QIDI .3mf thumbnails (prestonbrown/helixscreen#1713): QIDI's customized
# Moonraker hardcodes every uploaded .3mf's thumbnail metadata at
# .thumbs/<subdir>/<stem>/plate_N.png and extracts no image itself; the stock
# screen client this installer stops is what wrote those files. The units,
# their gate and their refresh live in the shipped
# $INSTALL_DIR/config/qidi-3mf-thumbs-units.sh so the install path and the
# post-update refresh path (which has no sudo under NoNewPrivileges) run the
# same code. This step just invokes the installed copy with the resolved
# Klipper identity; it exits 0 with a logged reason wherever the capability
# gate does not hold. Runs post-extract, since the payload carries it.
install_qidi_3mf_thumbs() {
    local units_sh="${INSTALL_DIR}/config/qidi-3mf-thumbs-units.sh"

    if [ ! -f "$units_sh" ]; then
        log_warn "QIDI thumbnail units script missing under ${INSTALL_DIR}/config -- skipping"
        return 0
    fi
    run_logged env HELIX_QIDI_HOME="${HELIX_QIDI_HOME:-${KLIPPER_HOME:-}}" \
        $SUDO "$units_sh" "${KLIPPER_USER:-}" "${KLIPPER_GROUP:-}" || true
    return 0
}

# One install-log line saying whether SSH is up at a named point of the run:
# dropbear's pids and whether anything listens on port 22. The install log
# survives reboots, so a printer that loses SSH across an update shows which
# step it was lost at. assets/config/platform/hooks-k1.sh#_k1_ssh_state is
# the same probe for the launch-time log, which cannot source this module.
log_k1_ssh_state() { # phase
    _lks_pids=$(pidof dropbear 2>/dev/null || true)
    _lks_listen=unknown
    if command -v netstat >/dev/null 2>&1; then
        if netstat -tln 2>/dev/null | grep -q ':22 '; then
            _lks_listen=yes
        else
            _lks_listen=no
        fi
    fi
    log_info "SSH state ($1): dropbear=${_lks_pids:-none} port22=$_lks_listen"
}

# Ensure SSH (dropbear) is running and will start on boot.
# On stock K1 firmware, dropbear is managed by S99start_app which we disable.
# This creates an independent dropbear init script so SSH survives reboots.
ensure_k1_ssh() {
    log_k1_ssh_state "before ensure"
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
            log_k1_ssh_state "after ensure"
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
    log_k1_ssh_state "after ensure"
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

# Add a name to COMPETING_UIS_FOUND once.
_competing_ui_found() {
    case " $COMPETING_UIS_FOUND " in *" $1 "*) return 0 ;; esac
    COMPETING_UIS_FOUND="${COMPETING_UIS_FOUND:+$COMPETING_UIS_FOUND }$1"
}

# What stop_competing_uis would take down, for the plan. Read-only: the same
# platform gates, with every stop, disable, chmod and kill left out. A stock
# UI is listed only while it runs or would start at boot: the stop handlers
# also re-assert a disable an earlier install made, and that is not a UI the
# plan or the summary should claim to disable. Sets COMPETING_UIS_FOUND
# (space-separated names, empty if none).
detect_competing_uis() {
    local ui initscript bin unit comp dm current_ui proc
    COMPETING_UIS_FOUND=""
    _is_self_update && return 0
    # A desktop install opens a window beside the other UIs; it stops none.
    [ "${platform:-}" = "x86-desktop" ] && return 0
    [ "${HOST_OWNS_COMPETING_UIS:-}" = "1" ] && return 0
    [ "${AD5M_FIRMWARE:-}" = "zmod" ] && return 0

    if [ "${AD5M_FIRMWARE:-}" = "klipper_mod" ]; then
        [ -x /etc/init.d/S40xorg ] && _competing_ui_found Xorg
        [ -n "$(_kmod_klipperscreen_pids)" ] && _competing_ui_found KlipperScreen
    fi
    # disable_stock_firmware_ui comments the start line out of auto_run.sh.
    if grep -qs '^/opt/PROGRAM/ffstartup-arm' /opt/auto_run.sh \
        || pidof firmwareExe >/dev/null 2>&1 || pidof ffstartup-arm >/dev/null 2>&1; then
        _competing_ui_found FlashForge-UI
    fi
    case "${K1_FIRMWARE:-}" in
        stock_klipper|guilouz)
            # The disable is chmod a-x on the init script.
            [ -x /etc/init.d/S99start_app ] && _competing_ui_found Creality-UI
            for proc in $K1_STOCK_UI_PROCS; do
                pidof "$proc" >/dev/null 2>&1 && _competing_ui_found Creality-UI
            done
            ;;
    esac
    # Sovol and QIDI binaries are disabled by chmod a-x on the binary itself.
    for bin in /home/sovol/printer_data/build/mksclient /home/*/printer_data/build/mksclient; do
        [ -x "$bin" ] && _competing_ui_found mksclient
    done
    pidof mksclient >/dev/null 2>&1 && _competing_ui_found mksclient
    for unit in $(_qidi_stock_ui_units); do
        _unit_is_competing "$unit" && _competing_ui_found "${unit%.service}"
    done
    for bin in $QIDI_STOCK_UI_BINS; do
        [ -x "$bin" ] && _competing_ui_found QIDI-UI
    done

    if [ "${platform:-}" = "cc1" ]; then
        current_ui=$(config-manager ui screen_ui 2>/dev/null || echo "")
        if [ -n "$current_ui" ] && [ -x "/etc/init.d/${current_ui}" ]; then
            _competing_ui_found "$current_ui"
        fi
        return 0
    fi

    if [ -n "$PREVIOUS_UI_SCRIPT" ] && [ -x "$PREVIOUS_UI_SCRIPT" ] 2>/dev/null; then
        _competing_ui_found "$(basename "$PREVIOUS_UI_SCRIPT")"
    fi

    for ui in $COMPETING_UIS; do
        if [ "$INIT_SYSTEM" = "systemd" ] && _unit_is_competing "$ui"; then
            _competing_ui_found "$ui"
        fi
        for initscript in /etc/init.d/S*${ui}* /etc/init.d/${ui}* /opt/config/mod/.root/S*${ui}*; do
            [ -x "$initscript" ] && _competing_ui_found "$ui"
        done
        pidof "$ui" >/dev/null 2>&1 && _competing_ui_found "$ui"
    done

    for comp in $WAYLAND_COMPOSITORS; do
        if [ "$INIT_SYSTEM" = "systemd" ]; then
            for unit in "$comp" "${comp}@tty1"; do
                _unit_is_competing "$unit" && _competing_ui_found "$unit"
            done
        fi
        if ! _in_graphical_session && pidof "$comp" >/dev/null 2>&1; then
            _competing_ui_found "$comp"
        fi
    done

    if [ "$INIT_SYSTEM" = "systemd" ] && _host_ships_a_stock_ui; then
        for dm in $DISPLAY_MANAGERS; do
            _unit_is_competing "$dm" && _competing_ui_found "$dm"
        done
    fi
    return 0
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
        klipper_mod) stop_kmod_competing_uis ;;
        zmod)
            # ZMOD manages its own init scripts (S80guppyscreen etc.)
            # Do NOT fall through to the generic loop which would chmod -x them
            log_info "ZMOD platform: skipping generic UI disabling (ZMOD-managed)"
            return 0
            ;;
    esac

    # Stock FlashForge UI kill — keyed on the stock UI's own startup manager
    # (/opt/PROGRAM/ffstartup-arm), not the mod flavor: a mod-less AD5M
    # (flavor stock) ships it too, and the file guard makes every other host
    # a no-op. Unprobed ZMOD hosts still return at the arm above.
    stop_forgex_competing_uis

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

    # QIDI: the generic loop below covers the 01.01.02+ unit names; this reaches
    # the path-named 1.1.x binary and any unit a later firmware renames.
    stop_qidi_competing_uis

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
            if _unit_is_competing "$ui"; then
                log_info "Stopping and disabling $ui (systemd service)..."
                _take_down_unit "$ui"
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

    stop_display_managers

    if [ "$found_any" = true ]; then
        log_info "Waiting for competing UIs to stop..."
        sleep 2
    elif _host_ships_a_stock_ui; then
        # Finding nothing on a device whose firmware ships a screen means we did
        # not recognise it, not that there is none. /dev/fb0 is not exclusive and
        # the touchscreen is not grabbed, so the install completes, both UIs
        # share the display, and nothing downstream reports a problem.
        log_warn "No competing UIs found, but this device normally ships one."
        log_warn "If the stock screen is still running after install, please report it:"
        log_warn "  https://github.com/prestonbrown/helixscreen/issues"
    else
        log_info "No competing UIs found"
    fi
}
