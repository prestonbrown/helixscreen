#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: uninstall
# Uninstall and clean installation functions
#
# Reads: All paths, INIT_SYSTEM, SUDO, AD5M_FIRMWARE, SERVICE_NAME, INSTALL_DIR
# Writes: (none)

# Source guard
[ -n "${_HELIX_UNINSTALL_SOURCED:-}" ] && return 0
_HELIX_UNINSTALL_SOURCED=1

# Sentinel paths checked by helixscreen-update.service so it refuses to fire
# while an uninstall is in progress (closes a race where the update path unit
# could trigger a restart between stop_service and rm -rf $INSTALL_DIR).
# Two locations cover both systemd-with-StateDirectory and the bare fallback;
# both live outside $INSTALL_DIR so they survive its removal.  Both are swept
# by clean_helix_state_dirs at the end of uninstall and by the EXIT/INT/TERM
# trap if an uninstall aborts before reaching the sweep (otherwise a stuck
# sentinel silently blocks every future update.service firing).
#
# Honors $HELIX_STATE_VAR_LIB so this stays consistent with the env-override
# the BATS suite uses to redirect clean_helix_state_dirs away from real
# /var/lib.  Production callers leave it unset; the systemd unit hardcodes
# /var/lib/helixscreen/.uninstalling because @@HELIX_STATE_VAR_LIB@@ isn't a
# template var.
_uninstalling_sentinel_paths() {
    echo "${HELIX_STATE_VAR_LIB:-/var/lib/helixscreen}/.uninstalling"
    if [ -n "${INSTALL_DIR:-}" ]; then
        local _parent
        _parent=$(dirname "$INSTALL_DIR" 2>/dev/null || true)
        case "$_parent" in
            /|.|"") : ;;
            *) echo "${_parent}/.helixscreen/.uninstalling" ;;
        esac
    fi
}

_drop_uninstalling_sentinel() {
    local _p _dir
    _uninstalling_sentinel_paths | while IFS= read -r _p; do
        [ -z "$_p" ] && continue
        _dir=$(dirname "$_p")
        $SUDO mkdir -p "$_dir" 2>/dev/null || true
        $SUDO touch "$_p" 2>/dev/null || true
    done
}

# Sweep sentinel files.  Wired to EXIT/INT/TERM in uninstall() so an aborted
# run doesn't leave a stuck sentinel blocking helixscreen-update.service.
# On a successful uninstall this is a no-op — clean_helix_state_dirs already
# removed them — but the trap fires either way.
_sweep_uninstalling_sentinel() {
    local _p
    _uninstalling_sentinel_paths 2>/dev/null | while IFS= read -r _p; do
        [ -z "$_p" ] && continue
        $SUDO rm -f "$_p" 2>/dev/null || true
    done
}

# Re-enable services that were disabled during installation
# Reads the state file and reverses each recorded disable action
reenable_disabled_services() {
    local state_file="${INSTALL_DIR}/config/.disabled_services"
    [ -f "$state_file" ] || return 0

    log_info "Re-enabling previously disabled services..."
    while IFS= read -r entry; do
        # Skip empty lines and comments
        case "$entry" in ""|\#*) continue ;; esac

        local type="${entry%%:*}"
        local target="${entry#*:}"

        case "$type" in
            systemd)
                log_info "Re-enabling systemd service: $target"
                $SUDO systemctl enable "$target" 2>/dev/null || true
                ;;
            sysv-chmod)
                if [ -f "$target" ]; then
                    log_info "Re-enabling init script: $target"
                    $SUDO chmod +x "$target" 2>/dev/null || true
                fi
                ;;
        esac
    done < "$state_file"
}

# Undo per-printer Klipper includes recorded at install time (#986).
# Reverses each entry in ${INSTALL_DIR}/config/.klipper_includes:
#   cfg:<path>                      → remove the copied snippet
#   include:<printer.cfg>:<relpath> → strip the [include <relpath>] line (and
#                                     the installer's marker comment above it)
# Must run BEFORE $INSTALL_DIR is removed (the state file lives in it) and
# touches printer_data files that live outside $INSTALL_DIR.
undo_klipper_includes() {
    local state_file="${INSTALL_DIR}/config/.klipper_includes"
    [ -f "$state_file" ] || return 0

    log_info "Reverting HelixScreen Klipper config includes..."
    while IFS= read -r entry; do
        case "$entry" in ""|\#*) continue ;; esac

        local type="${entry%%:*}"
        local rest="${entry#*:}"

        case "$type" in
            cfg)
                if [ -f "$rest" ]; then
                    log_info "Removing Klipper snippet: $rest"
                    $(path_sudo "$rest") rm -f "$rest" 2>/dev/null || true
                fi
                ;;
            include)
                # rest = "<printer.cfg path>:<relpath>"
                local pcfg="${rest%%:*}"
                local relpath="${rest#*:}"
                if [ -f "$pcfg" ]; then
                    log_info "Removing [include $relpath] from $pcfg"
                    local include_line="[include ${relpath}]"
                    local marker="# Added by HelixScreen installer (#986) -- ${relpath}"
                    local tmp="${pcfg}.helix-uninstall.$$"
                    # Drop the marker comment line and the include line. Anchored
                    # full-line matches via awk for portability (no sed -i).
                    if awk -v inc="$include_line" -v mark="$marker" \
                        '$0 == inc { next } $0 == mark { next } { print }' \
                        "$pcfg" > "$tmp" 2>/dev/null; then
                        $(file_sudo "$pcfg") mv "$tmp" "$pcfg" 2>/dev/null \
                            || rm -f "$tmp" 2>/dev/null || true
                    else
                        rm -f "$tmp" 2>/dev/null || true
                    fi
                fi
                ;;
        esac
    done < "$state_file"
}

# Undo per-printer settings seeding recorded at install time (#986).
# Reads ${INSTALL_DIR}/config/.seeded_settings (written by
# _record_seeded_settings() in printer_seed.sh as "settings:<printer_id>" lines)
# and logs which printer's defaults were seeded. We deliberately do NOT attempt
# to un-merge those values out of settings.json: the seed was deep-merged into
# whatever the user already had, so seeded keys can't be safely separated from
# the user's own choices (the user may have since edited them, too). The seeded
# defaults therefore REMAIN in settings.json; we only remove the marker file.
# Must run BEFORE $INSTALL_DIR is removed (the state file lives in it).
undo_seeded_settings() {
    local state_file="${INSTALL_DIR}/config/.seeded_settings"
    [ -f "$state_file" ] || return 0

    log_info "Reviewing HelixScreen settings seeds..."
    while IFS= read -r entry; do
        case "$entry" in ""|\#*) continue ;; esac

        local type="${entry%%:*}"
        local printer_id="${entry#*:}"

        case "$type" in
            settings)
                log_info "Seeded defaults for ${printer_id} remain in settings.json (not un-merged: seeded and user values can't be safely separated)"
                ;;
        esac
    done < "$state_file"

    # Remove only the marker; settings.json is left untouched on purpose.
    $(path_sudo "$state_file") rm -f "$state_file" 2>/dev/null || true
}

# Uninstall HelixScreen
# Args: platform (optional)
# Restore whatever screen UI HelixScreen displaced at install time, for the
# platform passed in $1. Split out of uninstall() so the STANDALONE uninstaller can
# reach it too. `install.sh --uninstall` calls uninstall() and always could;
# bundle-uninstaller.sh builds its own main() around reenable_previous_ui() instead,
# so every platform branch below — COSMOS, Snapmaker U1, AD5M zmod, Creality app —
# was unreachable from the uninstall.sh that ships into the install dir. On a U1 that
# left /usr/bin/gui non-executable and /oem/.debug set: no bootable stock UI and the
# firmware's overlay-wipe disabled for good.
#
# Communicates results through HELIX_RESTORED_UI / HELIX_RESTORED_XORG rather
# than a return value, because callers need both.
restore_previous_ui_platform() {
    local platform="${1:-}"
    local restored_ui=""
    local restored_xorg=""

    if [ "$AD5M_FIRMWARE" = "klipper_mod" ] || [ -f "/etc/init.d/S80klipperscreen" ]; then
        # Klipper Mod - restore Xorg and KlipperScreen
        if [ -f "/etc/init.d/S40xorg" ]; then
            $SUDO chmod +x "/etc/init.d/S40xorg" 2>/dev/null || true
            restored_xorg="Xorg (/etc/init.d/S40xorg)"
        fi
        if [ -f "/etc/init.d/S80klipperscreen" ]; then
            $SUDO chmod +x "/etc/init.d/S80klipperscreen" 2>/dev/null || true
            restored_ui="KlipperScreen (/etc/init.d/S80klipperscreen)"
        fi
    fi

    # ZMOD - no UI restore needed; S80guppyscreen is managed by ZMOD
    if [ "$AD5M_FIRMWARE" = "zmod" ]; then
        log_info "ZMOD firmware: no previous UI to restore (managed by ZMOD)"
    fi

    # K2 series (Creality K2 Plus / K2 Pro): re-enable the stock procd-managed UI.
    # hooks-k2.sh runs `/etc/init.d/app disable` on every launch (which removes
    # the procd rc.d symlink), so without this step the device boots into the
    # Creality logo with no UI after uninstall.
    if [ -z "$restored_ui" ] && [ -f /etc/init.d/app ] && \
       { [ "$platform" = "k2" ] || [ -f /mnt/UDISK/printer_data/config/printer.cfg ]; }; then
        log_info "Re-enabling Creality stock UI (/etc/init.d/app)..."
        $SUDO /etc/init.d/app enable 2>/dev/null || true
        $SUDO /etc/init.d/app start 2>/dev/null || true
        restored_ui="Creality stock UI (/etc/init.d/app)"
    fi

    # Check for K1/Simple AF GuppyScreen
    if [ -z "$restored_ui" ] && [ "$AD5M_FIRMWARE" != "zmod" ] && [ -f "/etc/init.d/S99guppyscreen" ]; then
        $SUDO chmod +x "/etc/init.d/S99guppyscreen" 2>/dev/null || true
        restored_ui="GuppyScreen (/etc/init.d/S99guppyscreen)"
    fi

    # ForgeX - restore GuppyScreen and stock UI settings
    if [ -z "$restored_ui" ] && [ "$AD5M_FIRMWARE" = "forge_x" ]; then
        uninstall_forgex
    fi

    # COSMOS (Centauri Carbon): restore the sibling gui-switcher UIs from the
    # backups the installer made when it substituted in helixscreen-wrapper init
    # scripts (see competing_uis.sh stop_cc1_competing_uis). We wrap ALL THREE
    # siblings (grumpyscreen/guppyscreen/atomscreen), so restore each one — but
    # ONLY when the live file is actually our wrapper (carries the
    # HELIXSCREEN_WRAPPER marker) AND a .helix-bak backup exists. This guards
    # against clobbering a stock file that was restored by a COSMOS upgrade, and
    # against restoring a wrapper-over-wrapper. Also revert cosmos.conf in case
    # the upstream config-manager allowlist fix lands — we want to be a clean
    # citizen on uninstall regardless.
    if [ -z "$restored_ui" ] && [ -x "/usr/bin/update-cosmos" ]; then
        for _sib in grumpyscreen guppyscreen atomscreen; do
            _target="/etc/init.d/${_sib}"
            _backup="/etc/init.d/${_sib}.helix-bak"
            if [ -f "$_backup" ] && \
               grep -q "HELIXSCREEN_WRAPPER" "$_target" 2>/dev/null; then
                log_info "Restoring original /etc/init.d/${_sib}"
                $SUDO mv "$_backup" "$_target" \
                    || log_warn "Could not restore /etc/init.d/${_sib} — gui-switcher may not launch a UI on next boot"
            elif [ -f "$_backup" ]; then
                # Backup exists but the live file is no longer our wrapper (e.g. a
                # COSMOS upgrade already restored the stock file). Drop the now-
                # redundant backup so it doesn't masquerade as a future original.
                $SUDO rm -f "$_backup" 2>/dev/null || true
            fi
        done
        unset _sib _target _backup
        # Revert screen_ui ONLY if it holds the legacy invalid 'helixscreen'
        # value (written by pre-3-sibling installs). We no longer write that —
        # we point screen_ui at a real sibling — and after this uninstall all
        # three siblings are restored to stock, so any sibling value is valid
        # and we must respect the operator's choice rather than clobber it.
        if [ -f /etc/klipper/config/cosmos.conf ] && \
           grep -q "^screen_ui[[:space:]]*=[[:space:]]*helixscreen" \
               /etc/klipper/config/cosmos.conf 2>/dev/null; then
            log_info "Reverting cosmos.conf screen_ui to grumpyscreen"
            $SUDO sed -i "s|^screen_ui[[:space:]]*=.*|screen_ui = grumpyscreen|" \
                /etc/klipper/config/cosmos.conf 2>/dev/null || true
        fi
        if [ -x /etc/init.d/grumpyscreen ]; then
            log_info "Starting grumpyscreen"
            $SUDO /etc/init.d/grumpyscreen start 2>/dev/null || true
            restored_ui="grumpyscreen (COSMOS stock UI)"
        fi
    fi

    # Snapmaker U1: re-enable the stock UI we neutralized at install time.
    # snapmaker-u1-setup-autostart.sh disables the stock UI binary
    # (chmod a-x /usr/bin/gui) so no launcher (PAXX's S99screen / 1.4's relocated
    # launcher, or stock's supervisor) can start it; it also patches the boot
    # launchers to start HelixScreen. Reverse all of it:
    #   1. Re-enable /usr/bin/gui (chmod +x) so a stock launcher can exec it.
    #   2. Restore the launcher(s) — from S99screen.stock on PAXX 1.3, by
    #      removing our HelixScreen-marked S99screen on PAXX 1.4, from
    #      S99fb-http.stock on PAXX 1.4, and from S99input-event-daemon.stock on
    #      stock firmware (the stock-safe boot hook).
    if [ -z "$restored_ui" ] && [ "$platform" = "snapmaker-u1" ]; then
        if [ -f /usr/bin/gui ] && [ ! -x /usr/bin/gui ]; then
            log_info "Re-enabling stock UI binary (/usr/bin/gui)"
            $SUDO chmod +x /usr/bin/gui 2>/dev/null \
                || log_warn "Could not re-enable /usr/bin/gui — stock UI may not start on next boot"
        fi
        if [ -f /etc/init.d/S99screen.stock ]; then
            log_info "Restoring stock /etc/init.d/S99screen (firmware 1.3)"
            $SUDO mv /etc/init.d/S99screen.stock /etc/init.d/S99screen \
                || log_warn "Could not restore /etc/init.d/S99screen — stock UI launcher may be missing"
        elif [ -f /etc/init.d/S99screen ] && \
             grep -q HelixScreen /etc/init.d/S99screen 2>/dev/null; then
            log_info "Removing HelixScreen-created /etc/init.d/S99screen (firmware 1.4)"
            $SUDO rm -f /etc/init.d/S99screen 2>/dev/null || true
        fi
        # Firmware 1.4: restore the stock framebuffer-HTTP screen launcher we
        # took over for boot-time autostart (1.2/1.3 have no .stock — no-op).
        if [ -f /etc/init.d/S99fb-http.stock ]; then
            log_info "Restoring stock /etc/init.d/S99fb-http (firmware 1.4)"
            $SUDO mv /etc/init.d/S99fb-http.stock /etc/init.d/S99fb-http \
                || log_warn "Could not restore /etc/init.d/S99fb-http — stock screen launcher may be missing"
        fi
        # Stock firmware: restore the input-event-daemon launcher we took over for
        # boot-time autostart. .stock is present only if we patched it, so this is
        # a no-op where it was never hooked.
        if [ -f /etc/init.d/S99input-event-daemon.stock ]; then
            log_info "Restoring stock /etc/init.d/S99input-event-daemon"
            $SUDO mv /etc/init.d/S99input-event-daemon.stock /etc/init.d/S99input-event-daemon \
                || log_warn "Could not restore /etc/init.d/S99input-event-daemon"
        fi
        restored_ui="Snapmaker stock UI (/usr/bin/gui re-enabled)"
        # Final step: drop the /oem/.debug firmware debug-mode flag the installer
        # created (snapmaker-u1-setup-autostart.sh step 1) to stop
        # /etc/init.d/S01aoverlayfs from wiping /oem/overlay/* on boot. Removing it
        # restores stock overlay-wipe-on-boot behavior: on the next boot the
        # overlay-upper (our patched init scripts) is discarded and the pristine
        # squashfs-lower stock rootfs is what runs — the cleanest possible
        # uninstall, leaving no HelixScreen state change behind. No-op if the flag
        # was never created (guarded by -f).
        if [ -f /oem/.debug ]; then
            log_info "Removing /oem/.debug (restores stock overlay-wipe-on-boot)"
            $SUDO rm -f /oem/.debug 2>/dev/null || true
        fi
    fi

    HELIX_RESTORED_UI="$restored_ui"
    HELIX_RESTORED_XORG="$restored_xorg"
}

uninstall() {
    local platform=${1:-}

    log_info "Uninstalling HelixScreen..."

    # Drop sentinel BEFORE any destructive work.  helixscreen-update.service
    # checks for it and refuses to fire while uninstall is running, closing
    # the race where Moonraker's path unit could re-trigger a restart between
    # stop_service and rm -rf.  Swept at the end by clean_helix_state_dirs;
    # the trap covers the abort case so a stuck sentinel can't silently block
    # future update.service firings.
    # Chained with the scratch-dir cleanup main.sh arms: a trap REPLACES the
    # previous handler for a signal, and install.sh bundles both modules, so
    # setting only the sweep here would disarm cleanup on the --uninstall path.
    trap '_sweep_uninstalling_sentinel; type cleanup_on_success >/dev/null 2>&1 && cleanup_on_success' EXIT INT TERM
    _drop_uninstalling_sentinel

    # Remove the [update_manager helixscreen] section FIRST, before any files
    # disappear.  If Moonraker auto-refreshes (or someone clicks "Update" in
    # Mainsail mid-uninstall), having the section gone before we start
    # dismantling files prevents a re-extract from racing us.  Moonraker's
    # in-memory updater object survives until Moonraker is reloaded, but
    # type:web only extracts on explicit user trigger so the on-disk edit is
    # the effective fix; no moonraker restart needed.
    if type remove_update_manager_section >/dev/null 2>&1; then
        remove_update_manager_section || true
    fi

    # Drop the service-allowlist entry the install added. Nothing else prunes
    # moonraker.asvc, so skipping this leaves helixscreen listed forever.
    if type remove_moonraker_asvc >/dev/null 2>&1; then
        local _asvc_conf
        _asvc_conf=$(find_moonraker_conf 2>/dev/null || true)
        [ -n "$_asvc_conf" ] && remove_moonraker_asvc "$_asvc_conf" || true
    fi

    # Detect init system first
    detect_init_system

    if [ "$INIT_SYSTEM" = "systemd" ]; then
        # Stop and disable systemd service
        $SUDO systemctl stop "$SERVICE_NAME" 2>/dev/null || true
        $SUDO systemctl disable "$SERVICE_NAME" 2>/dev/null || true
        $SUDO rm -f "/etc/systemd/system/${SERVICE_NAME}.service"
        # Remove update watcher units (mainsail#2444 workaround)
        $SUDO systemctl stop helixscreen-update.path 2>/dev/null || true
        $SUDO systemctl disable helixscreen-update.path 2>/dev/null || true
        $SUDO rm -f /etc/systemd/system/helixscreen-update.path
        $SUDO rm -f /etc/systemd/system/helixscreen-update.service
        # Remove permission rules (udev, polkit)
        $SUDO rm -f /etc/udev/rules.d/99-helixscreen-backlight.rules
        $SUDO rm -f /etc/polkit-1/localauthority/50-local.d/helixscreen-network.pkla
        $SUDO rm -f /etc/polkit-1/rules.d/49-helixscreen-network.rules
        $SUDO rm -f /etc/polkit-1/rules.d/50-helixscreen-network.rules
        $SUDO systemctl daemon-reload
    else
        # Stop and remove SysV init scripts (check all possible locations)
        local removed_procd_shim=false
        for init_script in $HELIX_INIT_SCRIPTS; do
            if [ -f "$init_script" ]; then
                log_info "Stopping and removing $init_script..."
                $SUDO "$init_script" stop 2>/dev/null || true
                # K2 procd shim: only call disable if this is actually a
                # rc.common-style script. CC1 installs a plain SysV script
                # at the same /etc/init.d/helixscreen path, and CC1's BusyBox
                # rejects `head -1` (only supports `head -n 1`), so we use
                # awk for the shebang check (portable across all BusyBox
                # variants we ship to). Also CC1 has no /etc/rc.common, so
                # the first guard short-circuits anyway.
                if [ "$init_script" = "/etc/init.d/helixscreen" ] && \
                   [ -x /etc/rc.common ] && \
                   awk 'NR==1 {exit !/\/etc\/rc\.common/}' "$init_script" 2>/dev/null; then
                    $SUDO "$init_script" disable 2>/dev/null || true
                    removed_procd_shim=true
                fi
                $SUDO rm -f "$init_script"
            fi
        done
        # Belt-and-suspenders cleanup of rc.d symlinks, but only if we actually
        # removed a procd shim (avoid touching /etc/rc.d on platforms that
        # don't use the procd boot iterator).
        if [ "$removed_procd_shim" = "true" ]; then
            $SUDO rm -f /etc/rc.d/S99helixscreen /etc/rc.d/K01helixscreen 2>/dev/null || true
        fi
    fi

    # Kill any remaining processes (watchdog first to prevent crash dialog flash)
    # shellcheck disable=SC2086
    kill_process_by_name $HELIX_PROCESSES || true

    # Clean up PID files and log file
    $SUDO rm -f /var/run/helixscreen.pid 2>/dev/null || true
    $SUDO rm -f /var/run/helix-splash.pid 2>/dev/null || true
    rm -f /tmp/helixscreen.log 2>/dev/null || true

    # K2 ustreamer camera teardown (#camera): stop/disable/remove the ustreamer
    # service + binary and restore Moonraker's stock webcam list. Must run BEFORE
    # reenable_disabled_services (which chmod +x's the stock WebRTC init scripts
    # back) and BEFORE $INSTALL_DIR is removed (the .webcams_backup.json and
    # .camera_migrated marker live in $INSTALL_DIR/config). No-op off K2.
    if type uninstall_camera_k2 >/dev/null 2>&1; then
        uninstall_camera_k2 "$platform" || true
    fi

    # Re-enable services from state file (before removing install dir)
    reenable_disabled_services

    # Revert per-printer Klipper includes (#986) — strip the [include] line from
    # printer.cfg and remove the copied snippet. Must run before $INSTALL_DIR
    # (which holds the .klipper_includes state file) is removed.
    undo_klipper_includes

    # Acknowledge per-printer settings seeding (#986) — log which defaults were
    # seeded (they remain in settings.json by design) and remove the marker.
    # Must run before $INSTALL_DIR (which holds the .seeded_settings state file)
    # is removed.
    undo_seeded_settings

    # --mod-payload: this uninstall's one install target is the mod's payload
    # root (HELIX_INSTALL_DIRS gains it via helix_install_dirs_for_run below).
    # Restore the mod's display mode FIRST - while the payload is still in
    # place, the rig is never left with neither UI nor a restore record.
    if [ "${HELIX_MOD_PAYLOAD:-}" = "1" ]; then
        if type uninstall_forgex >/dev/null 2>&1; then
            uninstall_forgex || true
        fi
    fi

    # Remove installation (check all possible locations)
    local removed_dir=""
    for install_dir in $(helix_install_dirs_for_run); do
        if [ -d "$install_dir" ]; then
            # A mod-owned entry belongs to the firmware mod, not to this
            # uninstall — skip it (a hard exit here would strand the rest of
            # the uninstall on one unremovable directory).
            if host_mod_destruct_blocked "$install_dir"; then
                log_warn "Skipping mod-owned ${install_dir} (managed by the firmware mod)"
                continue
            fi
            $SUDO rm -rf "$install_dir"
            log_success "Removed ${install_dir}"
            removed_dir="$install_dir"
            # Also remove the updater repo clone if present
            if [ -d "${install_dir}-repo" ]; then
                $SUDO rm -rf "${install_dir}-repo"
                log_success "Removed ${install_dir}-repo"
            fi
        fi
    done

    if [ -z "$removed_dir" ]; then
        log_warn "No HelixScreen installation found"
    fi

    # Re-enable the previous UI based on firmware
    log_info "Re-enabling previous screen UI..."
    restore_previous_ui_platform "$platform"
    local restored_ui="$HELIX_RESTORED_UI"
    local restored_xorg="$HELIX_RESTORED_XORG"

    # Clean up helixscreen cache directories
    for cache_dir in /root/.cache/helix /tmp/helix_thumbs /.cache/helix /data/helixscreen/cache /usr/data/helixscreen/cache; do
        if [ -d "$cache_dir" ] 2>/dev/null; then
            log_info "Removing cache: $cache_dir"
            $SUDO rm -rf "$cache_dir"
        fi
    done
    # Clean up /var/tmp helix files
    for tmp_pattern in /var/tmp/helix_*; do
        if [ -e "$tmp_pattern" ] 2>/dev/null; then
            log_info "Removing cache: $tmp_pattern"
            $SUDO rm -rf "$tmp_pattern"
        fi
    done

    # Clean up active flag file
    rm -f /tmp/helixscreen_active 2>/dev/null || true

    # Clean up macOS resource fork files (created by scp from Mac)
    for pattern in /opt/._helixscreen /root/._helixscreen; do
        $(path_sudo "$pattern") rm -f "$pattern" 2>/dev/null || true
    done

    # Remove config symlinks (preserves user files in printer_data)
    if type remove_config_symlink >/dev/null 2>&1; then
        remove_config_symlink || true
    fi

    # Sweep state dirs holding rolling config backups (out-of-INSTALL_DIR by
    # design).  Also sweeps the .uninstalling sentinel dropped at the top.
    clean_helix_state_dirs

    # Strip the legacy [shell_command helix_recover] block from moonraker.conf
    # (dead since v0.99.61 — kept around on already-installed K2s until they
    # next upgrade or, as here, uninstall).
    if type remove_legacy_moonraker_block >/dev/null 2>&1; then
        local _mr_conf
        _mr_conf=$(find_moonraker_conf 2>/dev/null || true)
        if [ -n "$_mr_conf" ] && [ -f "$_mr_conf" ]; then
            remove_legacy_moonraker_block "$_mr_conf" "$(file_sudo "$_mr_conf")" || true
        fi
    fi

    log_success "HelixScreen uninstalled"
    if [ -n "$restored_xorg" ]; then
        log_info "Re-enabled: $restored_xorg"
    fi
    if [ -n "$restored_ui" ]; then
        log_info "Re-enabled: $restored_ui"
        log_info "Reboot to start the previous UI"
    else
        log_info "Note: No previous UI found to restore"
    fi
}

# Gate --clean's irreversible sweep on explicit consent.
#
# "stdin is not a terminal" is NOT consent. The documented invocation is
# `curl … | sh -s -- --clean`, where stdin is the pipe carrying the script, so
# a bare `[ -t 0 ]` guard skipped the "PERMANENTLY DELETE your configuration"
# prompt on exactly the path users actually take. Non-interactive runs must opt
# in with --yes (ASSUME_YES, set by main.sh's argument parser).
#
# Returns 0 to proceed; otherwise exits (0 = user declined, 1 = no consent).
confirm_clean_install() {
    if [ "${ASSUME_YES:-false}" = true ]; then
        log_warn "--yes given: proceeding without confirmation."
        return 0
    fi

    if [ -t 0 ]; then
        printf "Are you sure? [y/N] "
        read -r response
        case "$response" in
            [yY][eE][sS]|[yY])
                return 0
                ;;
            *)
                log_info "Clean install cancelled."
                exit 0
                ;;
        esac
    fi

    log_error "Refusing to run --clean without confirmation."
    log_error "stdin is not a terminal (a piped 'curl ... | sh' has the script on"
    log_error "stdin), so the y/N prompt cannot be answered."
    log_error "Re-run with --yes to confirm the deletions listed above:"
    log_error "  curl -sSL https://releases.helixscreen.org/install.sh | sh -s -- --clean --yes"
    exit 1
}

# Clean up old installation completely (for --clean flag)
# Removes all files, config, and caches without backup
# Args: platform
clean_old_installation() {
    local platform=$1

    log_warn "=========================================="
    log_warn "  CLEAN INSTALL MODE"
    log_warn "=========================================="
    log_warn ""
    log_warn "This will PERMANENTLY DELETE:"
    log_warn "  - All HelixScreen files in ${INSTALL_DIR}"
    log_warn "  - Your configuration (settings.json)"
    log_warn "  - Rolling config backups (/var/lib/helixscreen + .helixscreen under the service user's home)"
    log_warn "  - Thumbnail cache files"
    log_warn ""

    confirm_clean_install

    log_info "Cleaning old installation..."

    # Stop any running services
    stop_service

    # Remove installation directories (check all possible locations). The
    # payload-mode list adds the mod's payload root via
    # helix_install_dirs_for_run; the mod-owned skip below exempts only the
    # flag-armed run, so --clean without --mod-payload cannot touch it.
    for install_dir in $(helix_install_dirs_for_run); do
        if [ -d "$install_dir" ]; then
            # Same ownership rule as uninstall()'s sweep: never rm -rf a
            # mod-owned directory out from under the firmware mod.
            if host_mod_destruct_blocked "$install_dir"; then
                log_warn "Skipping mod-owned ${install_dir} (managed by the firmware mod)"
                continue
            fi
            log_info "Removing $install_dir..."
            $SUDO rm -rf "$install_dir"
        fi
    done

    # Remove thumbnail caches (POSIX-compatible: no arrays)
    for cache_pattern in \
        "/root/.cache/helix/helix_thumbs" \
        "/home/*/.cache/helix/helix_thumbs" \
        "/tmp/helix_thumbs" \
        "/var/tmp/helix_thumbs" \
        "/var/tmp/helix_*" \
        "/data/helixscreen/cache" \
        "/usr/data/helixscreen/cache"
    do
        for cache_dir in $cache_pattern; do
            if [ -d "$cache_dir" ] 2>/dev/null; then
                log_info "Removing cache: $cache_dir"
                $SUDO rm -rf "$cache_dir"
            fi
        done
    done

    # Remove init scripts (check all possible locations)
    for init_script in $HELIX_INIT_SCRIPTS; do
        if [ -f "$init_script" ]; then
            log_info "Removing init script: $init_script"
            $SUDO rm -f "$init_script"
        fi
    done

    # Remove systemd service if present
    if [ -f "/etc/systemd/system/${SERVICE_NAME}.service" ]; then
        log_info "Removing systemd service..."
        $SUDO systemctl disable "$SERVICE_NAME" 2>/dev/null || true
        $SUDO rm -f "/etc/systemd/system/${SERVICE_NAME}.service"
    fi
    # Remove update watcher units
    $SUDO systemctl stop helixscreen-update.path 2>/dev/null || true
    $SUDO systemctl disable helixscreen-update.path 2>/dev/null || true
    $SUDO rm -f /etc/systemd/system/helixscreen-update.path
    $SUDO rm -f /etc/systemd/system/helixscreen-update.service
    # Remove permission rules (udev, polkit)
    $SUDO rm -f /etc/udev/rules.d/99-helixscreen-backlight.rules
    $SUDO rm -f /etc/polkit-1/localauthority/50-local.d/helixscreen-network.pkla
    $SUDO rm -f /etc/polkit-1/rules.d/49-helixscreen-network.rules
    $SUDO rm -f /etc/polkit-1/rules.d/50-helixscreen-network.rules
    $SUDO systemctl daemon-reload 2>/dev/null || true

    # Remove <klipper config dir>/helixscreen/ (user config) in clean mode
    local pd_config
    pd_config="$(klipper_config_dir)"
    if [ -n "$pd_config" ]; then
        local pd_helix="${pd_config}/helixscreen"
        if [ -d "$pd_helix" ] || [ -L "$pd_helix" ]; then
            log_info "Removing user config: $pd_helix"
            $SUDO rm -rf "$pd_helix"
        fi
    fi

    # Sweep state dirs holding rolling config backups
    clean_helix_state_dirs

    log_success "Old installation cleaned"
    echo ""
}
