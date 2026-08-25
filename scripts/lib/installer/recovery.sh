#!/bin/sh
# scripts/lib/installer/recovery.sh
# Per-platform deep-recovery script installation.
#
# When the user clicks "Firmware Restart" on a HelixScreen co-hosted with
# Klipper, PrinterRecoveryService::recover() tries (in order):
#   1. printer.firmware_restart — works when klippy_uds is alive
#   2. $INSTALL_DIR/bin/helix-recover.sh — local exec, works even when klippy is dead
#   3. machine.services.restart klipper — last resort on systemd hosts
#
# Step 2 is what this file installs. We can't use Moonraker's `[shell_command
# helix_recover]` config block — upstream Moonraker doesn't actually parse such
# sections (shell_command.py is a library used by machine/power/webcam, not a
# user-config consumer). The early v0.99.5x K2 snippet was silently dead code;
# remove_legacy_moonraker_block() strips it from already-installed K2 confs on
# next upgrade.
#
# Platforms that don't need this (stock Pi/RatOS, x86 dev) get nothing — the
# frontend's step 1 or 3 already covers them via systemd_dbus/systemd_cli.

# Re-source guard
[ -n "${_HELIX_RECOVERY_SOURCED:-}" ] && return 0
_HELIX_RECOVERY_SOURCED=1

# Per-platform `helix-recover.sh` body. Each function emits a stand-alone
# POSIX-sh script that, when invoked by helix-screen, fully restarts the
# platform's klippy + any host-side MCU daemons. Exit 0 = recovery launched
# successfully (the platform's own init script handles the rest). Non-zero =
# helix-screen surfaces a "Firmware restart failed" toast with stderr.

# Common header — used at the top of every emitted script.
_helix_recover_script_header() {
    cat <<'EOF'
#!/bin/sh
# HelixScreen deep-recovery (managed). See:
# https://github.com/prestonbrown/helixscreen/blob/main/scripts/lib/installer/recovery.sh
#
# Invoked by PrinterRecoveryService::run_local_recovery() when klippy_uds is
# unreachable and printer.firmware_restart can't proxy a recovery. Do not
# edit by hand — re-install HelixScreen to regenerate.
set -u
EOF
}

# Creality K2 series (K2 Plus / K2 Pro / K2 Max). Two host-side processes:
# klippy.py and klipper_mcu (rpi virtual MCU bridging the RS-485 to CFS).
# Bouncing klipper_mcu first clears the rpi-MCU-shutdown that traps
# FIRMWARE_RESTART after a CFS fault (key298).
helix_recover_script_k2() {
    _helix_recover_script_header
    cat <<'EOF'
/etc/init.d/klipper_mcu restart
sleep 2
exec /etc/init.d/klipper restart
EOF
}

# Snapmaker U1 (single-extruder, aarch64 Debian, BusyBox init). S60klipper
# bounces both klippy_mcu (/home/lava/firmware_MCU/klippy_mcu) and the python
# klippy host, and uses lava_io to power-cycle the MAIN_MCU and HEAD_MCU rails.
# Verified live 2026-05-13.
helix_recover_script_snapmaker_u1() {
    _helix_recover_script_header
    cat <<'EOF'
exec /etc/init.d/S60klipper restart
EOF
}

# Creality K1 / K1C / K1 Max (stock + Guilouz). Same klippy_mcu + klipper_service
# split as K2 but with the SXX prefixes that Creality's BusyBox init expects.
helix_recover_script_k1() {
    _helix_recover_script_header
    cat <<'EOF'
/etc/init.d/S57klipper_mcu restart
sleep 2
exec /etc/init.d/S55klipper_service restart
EOF
}

# Elegoo Centauri Carbon (COSMOS firmware, armv7l). Single klippy process,
# no host-side mcu daemon. Init script handles a 10s SIGKILL fallback itself.
helix_recover_script_cc1() {
    _helix_recover_script_header
    cat <<'EOF'
exec /etc/init.d/klipper restart
EOF
}

# FlashForge AD5M family (klipper_mod / ZMOD / ForgeX). Three community
# firmwares with different klipper-start mechanisms — probe at runtime since
# the script is generated once per install but the user can switch firmwares
# without re-running helix-screen's installer.
helix_recover_script_ad5m() {
    _helix_recover_script_header
    cat <<'EOF'
if [ -x /opt/config/mod/.shell/restart_klipper.sh ]; then
    exec /opt/config/mod/.shell/restart_klipper.sh
fi
if [ -x /etc/init.d/S55klipper_service ]; then
    exec /etc/init.d/S55klipper_service restart
fi
if [ -x /root/printer_software/klipper/scripts/klipper-restart.sh ]; then
    exec /root/printer_software/klipper/scripts/klipper-restart.sh
fi
echo "helix-recover: no known klipper restart mechanism on this AD5M firmware" >&2
exit 1
EOF
}

# Return the script body for the detected platform, or empty when this
# platform doesn't need a local helper (firmware_restart is sufficient).
helix_recover_script_for_platform() {
    local platform="$1"
    case "$platform" in
        k2)           helix_recover_script_k2 ;;
        k1)           helix_recover_script_k1 ;;
        cc1)          helix_recover_script_cc1 ;;
        ad5m|ad5x)    helix_recover_script_ad5m ;;
        snapmaker-u1) helix_recover_script_snapmaker_u1 ;;
        # pi / pi32 / x86: stock systemd Klipper — `machine.services.restart`
        # works because moonraker.conf has `provider: systemd_*`. Frontend
        # never reaches the local-recovery branch.
        *) : ;;
    esac
}

# Write $install_dir/bin/helix-recover.sh for the platform. Idempotent: a
# re-run with the same platform produces byte-identical output and overwrites
# the existing file (so a recovery.sh edit on the dev side propagates on the
# next upgrade).
install_recovery_script() {
    local install_dir="$1"
    local platform="$2"
    local fs="${3:-}"   # `sudo` prefix if needed, "" otherwise — match moonraker.sh idiom

    [ -z "$install_dir" ] && return 1
    [ -z "$platform" ] && return 1

    local body
    body=$(helix_recover_script_for_platform "$platform")
    if [ -z "$body" ]; then
        log_info "No local recovery script needed for platform: $platform"
        return 0
    fi

    local script="$install_dir/bin/helix-recover.sh"
    $fs mkdir -p "$install_dir/bin"
    printf '%s\n' "$body" | $fs tee "$script" >/dev/null
    $fs chmod +x "$script"
    log_success "Installed helix-recover.sh for $platform → $script"
}

# Remove the local recovery script on uninstall. No-op when absent.
# UNCALLED_OK: the uninstall path deletes the whole install tree
# (remove_installation does `rm -rf "$INSTALL_DIR"`), which takes
# bin/helix-recover.sh with it. Kept as the targeted removal for a caller that
# wants to drop only the recovery script; covered by test_recovery_script.bats.
remove_recovery_script() {
    local install_dir="$1"
    local fs="${2:-}"
    local script="$install_dir/bin/helix-recover.sh"
    [ -f "$script" ] || return 0
    $fs rm -f "$script"
    log_success "Removed $script"
}

# Strip the legacy [shell_command helix_recover] block from moonraker.conf.
# Early v0.99.5x K2 installs got this block; it was always silently dead since
# Moonraker upstream doesn't parse [shell_command name] sections. Sweeping it
# on every install keeps existing K2 confs clean as users upgrade.
remove_legacy_moonraker_block() {
    local conf="$1"
    local fs="${2:-}"
    [ -f "$conf" ] || return 0
    grep -q '^\[shell_command helix_recover\]' "$conf" 2>/dev/null || return 0

    log_info "Removing legacy [shell_command helix_recover] from $conf (dead block)"
    local tmp
    tmp=$(mktemp)
    awk '
        /^\[shell_command helix_recover\]/ { skip=1; next }
        skip && /^\[/ { skip=0 }
        !skip { print }
    ' "$conf" > "$tmp"
    $fs cp "$tmp" "$conf"
    rm -f "$tmp"
}

# High-level installer entry point — drop-in companion to
# configure_moonraker_updates() in moonraker.sh. Call from main install flow
# AFTER install_files (so $INSTALL_DIR/bin/ exists) and AFTER
# configure_moonraker_updates (so moonraker.conf exists for the legacy sweep).
#
# Reads: INSTALL_DIR, SUDO. Calls find_moonraker_conf() / file_sudo().
configure_local_recovery() {
    local platform="$1"

    local body
    body=$(helix_recover_script_for_platform "$platform")
    if [ -z "$body" ]; then
        # Stock systemd platform (pi/pi32/x86). Still sweep the legacy block
        # in case the user switched platforms post-install — cheap, idempotent.
        :
    else
        log_info "Configuring local recovery script..."
        install_recovery_script "${INSTALL_DIR}" "$platform" "${SUDO:-}"
    fi

    # Migration sweep: strip the dead [shell_command helix_recover] block from
    # any existing moonraker.conf. Runs on every platform so K2 users upgrading
    # past v0.99.61 lose the stale block on the next install pass.
    local conf
    conf=$(find_moonraker_conf 2>/dev/null)
    if [ -n "$conf" ]; then
        local fs
        fs=$(file_sudo "$conf")
        remove_legacy_moonraker_block "$conf" "$fs"
    fi
}

# Backward-compat shim — main.sh historically called this name. Keep it
# until the next bundle so callers that pulled an older install.sh keep
# working through the upgrade.
# UNCALLED_OK: deliberate compatibility alias for configure_local_recovery(),
# which is what main.sh calls now.
configure_moonraker_recovery() {
    configure_local_recovery "$@"
}
