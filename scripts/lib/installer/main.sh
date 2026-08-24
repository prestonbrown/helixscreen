#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: main
# Top-level installer orchestration: argument parsing, platform detection,
# preflight, download, install, post-install. Single source of truth shared
# by install-dev.sh and the bundled install.sh.
#
# Reads: every other lib/installer module
# Writes: nothing; orchestrates the install flow

# Source guard
[ -n "${_HELIX_MAIN_SOURCED:-}" ] && return 0
_HELIX_MAIN_SOURCED=1

# Set up error trap (ERR is bash-specific, skip on POSIX shells like dash/ash)
# shellcheck disable=SC3047
trap 'error_handler $LINENO' ERR 2>/dev/null || true

# Remove the scratch dir however the installer ends.
#
# The ERR trap above is a bash extension and is silently discarded on the
# ash/dash shells every embedded platform runs, so an interrupted or failing
# install used to leak the whole download: a K2 was found holding a 60MB
# helixscreen.zip from four months earlier, on a 240MB overlay partition.
#
# cleanup_on_success is idempotent (it tests for the directory first) so the
# explicit call on the success path is unaffected, and it routes through
# _safe_remove_tmp_dir, which is what refuses to rm -rf a mountpoint.
trap 'cleanup_on_success' EXIT INT TERM

# Print usage
usage() {
    echo "HelixScreen Installer"
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --update       Update existing installation (preserves config)"
    echo "  --uninstall    Remove HelixScreen"
    echo "  --clean        Clean install: remove old installation completely,"
    echo "                 including config and caches (asks for confirmation)"
    echo "  --yes, -y      Confirm destructive prompts non-interactively."
    echo "                 Required for --clean when stdin is not a terminal"
    echo "                 (e.g. curl ... | sh -s -- --clean --yes)"
    echo "  --version VER  Install specific version (default: latest)"
    echo "  --local FILE   Install from local archive (.zip or .tar.gz, skip download)"
    echo "  --skip-kiauh-registration"
    echo "                 Skip KIAUH extension registration (default: install if KIAUH detected)"
    echo "  --help         Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                    # Fresh install, latest version"
    echo "  $0 --update           # Update existing installation"
    echo "  $0 --clean            # Remove old install completely, then install"
    echo "  $0 --clean --yes      # Same, without the interactive confirmation"
    echo "  $0 --version v1.1.0   # Install specific version"
    echo "  $0 --local /tmp/helixscreen-ad5m.tar.gz  # Install from local file"
}

# Configure platform-specific settings before stopping competing UIs
# (ForgeX display mode, stock UI disable, screen.sh patching)
configure_platform() {
    case "${AD5M_FIRMWARE:-}" in
        forge_x)
            configure_forgex_display || true
            disable_stock_firmware_ui || true
            patch_forgex_screen_sh || true
            patch_forgex_screen_drawing || true
            install_forgex_logged_wrapper || true
            ;;
        klipper_mod)
            # Klipper Mod-specific install-time configuration (if any)
            ;;
    esac
}

# Deploy platform-specific hooks for the init script
# Must be called after extract_release (hooks are in the release package)
install_platform_hooks() {
    local platform_hook=""
    case "${AD5M_FIRMWARE:-}" in
        forge_x)     platform_hook="ad5m-forgex" ;;
        klipper_mod) platform_hook="ad5m-kmod" ;;
        zmod)        platform_hook="ad5m-zmod" ;;
    esac

    # Platform hooks (pi32 shares Pi hooks; AD5X shares the ad5m-zmod hook
    # because both run ZMOD firmware with the same /data layout).
    case "$platform" in
        pi|pi32)       platform_hook="pi" ;;
        k1)            platform_hook="k1" ;;
        k2)            platform_hook="k2" ;;
        cc1)           platform_hook="cc1" ;;
        m1)            platform_hook="m1" ;;
        ad5x)          platform_hook="ad5m-zmod" ;;
        snapmaker-u1)  platform_hook="snapmaker-u1" ;;
    esac

    if [ -n "$platform_hook" ]; then
        deploy_platform_hooks "$INSTALL_DIR" "$platform_hook"
    fi
}

# Print the post-detection platform banner.
#
# For Pi-class SBCs (platform=pi/pi32 — which covers a long tail of ARM Linux
# boxes including QIDI Q2/Plus, BTT CB1, MKS-Pi, generic Armbian) we lead with
# the friendly hardware label and reframe "pi" as the install package. Plain
# "Detected platform: pi" reads as wrong to anyone whose printer says QIDI on
# the lid — they see "pi" first and assume we mis-identified their device.
# Actual Raspberry Pi owners keep the original ordering.
#
# All other platforms (k1, k2, ad5m, snapmaker-u1, x86, …) get the single
# "Detected platform: X" line — there's no device-name ambiguity to clear up.
print_platform_banner() {
    local platform="$1"
    local _hw_label

    if [ "$platform" != "pi" ] && [ "$platform" != "pi32" ]; then
        log_info "Detected platform: ${BOLD}${platform}${NC}"
        return 0
    fi

    _hw_label=$(describe_hardware)
    case "$_hw_label" in
        "Raspberry Pi"*)
            log_info "Detected platform: ${BOLD}${platform}${NC}"
            log_info "Hardware: ${_hw_label}"
            ;;
        *)
            log_info "Detected hardware: ${BOLD}${_hw_label}${NC}"
            log_info "Install package: ${BOLD}${platform}${NC} (generic ARM Linux build, compatible with your SBC)"
            ;;
    esac
}

# Refuse to run --uninstall from a script sitting inside the dir we're about to
# delete. The release tarball ships scripts/install.sh into $INSTALL_DIR for
# offline --local updates; users sometimes invoke that copy with --uninstall,
# which "works" on Linux only because the kernel keeps the inode open after rm.
# Force the user to copy out. No-op (returns 0) when INSTALL_DIR isn't known
# yet or $0 lives elsewhere, so it's safe to call more than once.
_refuse_uninstall_from_install_dir() {
    local _script_dir _script_abs _install_norm
    _script_dir="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || _script_dir=""
    [ -n "$_script_dir" ] || return 0
    [ -n "${INSTALL_DIR:-}" ] || return 0
    _script_abs="${_script_dir}/$(basename "$0")"
    _install_norm="${INSTALL_DIR%/}"
    case "$_script_abs" in
        "$_install_norm"/* | "$_install_norm")
            log_error "Refusing to run --uninstall from inside \$INSTALL_DIR"
            log_error "  script:      $_script_abs"
            log_error "  INSTALL_DIR: $INSTALL_DIR"
            log_error ""
            log_error "Copy the script out first, then re-run:"
            log_error "  cp '$_script_abs' /tmp/install.sh"
            log_error "  sh /tmp/install.sh --uninstall"
            exit 1
            ;;
    esac
}

# Refuse to run on a device where HelixScreen is managed by the firmware
# (e.g. PAXX's Snapmaker U1 firmware, which bind-mounts our binary over
# /usr/bin/gui, supervises it via lmd, and owns updates through a pinned
# helixscreen-pkg). Running the standalone installer there would trample the
# firmware-managed setup. Detection: either marker present ⇒ firmware-managed.
#
# Advanced users can force past the guard with HELIX_IGNORE_FIRMWARE_MANAGED=1.
# Tests set HELIX_FIRMWARE_MANAGED_MARKER to a temp root to exercise the guard
# without touching the real /oem or /etc trees.
_refuse_if_firmware_managed() {
    if [ "${HELIX_IGNORE_FIRMWARE_MANAGED:-0}" = "1" ]; then
        log_warn "HELIX_IGNORE_FIRMWARE_MANAGED=1 set — skipping firmware-managed guard"
        return 0
    fi

    _fw_root="${HELIX_FIRMWARE_MANAGED_MARKER:-}"
    _fw_dir_marker="${_fw_root}/oem/apps/helixscreen"
    _fw_hook_marker="${_fw_root}/etc/hooks/lmd.d/30-helixscreen.sh"

    if [ -d "$_fw_dir_marker" ] || [ -f "$_fw_hook_marker" ]; then
        if [ -d "$_fw_dir_marker" ]; then
            _fw_detected="$_fw_dir_marker"
        else
            _fw_detected="$_fw_hook_marker"
        fi
        log_error "=========================================="
        log_error "HelixScreen is managed by your firmware on this device."
        log_error "Detected: $_fw_detected"
        log_error "=========================================="
        log_error ""
        log_error "Use the firmware configuration to enable, disable, or update"
        log_error "HelixScreen (Snapmaker Components > Touchscreen GUI)."
        log_error "Do NOT run this installer — it would overwrite the"
        log_error "firmware-managed setup."
        log_error ""
        log_error "To override this check anyway (advanced users), re-run with:"
        log_error "  HELIX_IGNORE_FIRMWARE_MANAGED=1 sh install.sh"
        exit 1
    fi
}

# Main installation flow
main() {
    update_mode=false
    uninstall_mode=false
    clean_mode=false
    ASSUME_YES=false
    version=""
    local_tarball=""
    skip_kiauh_registration=false

    # Parse arguments
    while [ $# -gt 0 ]; do
        case $1 in
            --update)
                update_mode=true
                shift
                ;;
            --uninstall)
                uninstall_mode=true
                shift
                ;;
            --clean)
                clean_mode=true
                shift
                ;;
            --yes|-y|--force)
                # Explicit non-interactive consent for destructive prompts.
                # ASSUME_YES is read by clean_old_installation (uninstall.sh);
                # it is deliberately NOT inferred from a non-TTY stdin, since
                # the documented `curl ... | sh` invocation always has one.
                # shellcheck disable=SC2034  # consumed by uninstall.sh (clean_old_installation)
                ASSUME_YES=true
                shift
                ;;
            --version)
                if [ -z "${2:-}" ]; then
                    log_error "--version requires a version argument"
                    exit 1
                fi
                version="$2"
                shift 2
                ;;
            --local)
                if [ -z "${2:-}" ]; then
                    log_error "--local requires a file path argument"
                    exit 1
                fi
                local_tarball="$2"
                shift 2
                ;;
            --skip-kiauh-registration)
                skip_kiauh_registration=true
                shift
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done

    # Self-delete safety guard runs as early as possible — before platform
    # detection, which exits on "unsupported" hardware and would otherwise
    # preempt the guard. Only fires when INSTALL_DIR is already known (env);
    # the post-set_install_paths call below covers the normal runtime case.
    if [ "$uninstall_mode" = true ]; then
        _refuse_uninstall_from_install_dir
    fi

    # Refuse to install/update over a firmware-managed HelixScreen. Runs before
    # platform detection and any install work so we never trample the firmware's
    # setup. Uninstall is exempt — leave the firmware's own teardown to it.
    if [ "$uninstall_mode" != true ]; then
        _refuse_if_firmware_managed
    fi

    printf '\n'
    printf '%b\n' "${BOLD}========================================${NC}"
    printf '%b\n' "${BOLD}       HelixScreen Installer${NC}"
    printf '%b\n' "${BOLD}========================================${NC}"
    printf '\n'

    # Detect platform
    platform=$(detect_platform)
    # For platforms that share a binary with pi/pi32 (e.g. m1), the download
    # URL uses the donor platform key while $platform stays put so hooks,
    # banner, and competing-UI shutdown still target the real device.
    download_platform=$(get_download_platform "$platform")
    print_platform_banner "$platform"

    # AD5X: refuse to run outside the ZMOD chroot — applies to fresh install,
    # --update, --uninstall, and --local. Inside the chroot the check is a no-op.
    if [ "$platform" = "ad5x" ]; then
        ad5x_check_chroot_context
    fi

    if [ "$platform" = "unsupported" ]; then
        log_error "Unsupported platform: $(uname -m)"
        log_error "HelixScreen supports:"
        log_error "  - Raspberry Pi (aarch64/armv7l)"
        log_error "  - FlashForge Adventurer 5M (armv7l)"
        log_error "  - Creality K1 series with Simple AF"
        log_error "  - Creality K2 series (K2/K2 Pro/K2 Plus)"
        log_error "  - x86_64 Debian/Ubuntu (x86_64)"
        exit 1
    fi

    # For AD5M/K1, detect firmware variant and set appropriate paths
    local firmware=""
    if [ "$platform" = "ad5m" ]; then
        AD5M_FIRMWARE=$(detect_ad5m_firmware)
        firmware="$AD5M_FIRMWARE"
    elif [ "$platform" = "k1" ]; then
        K1_FIRMWARE=$(detect_k1_firmware)
        firmware="$K1_FIRMWARE"
    fi
    set_install_paths "$platform" "$firmware"

    # Check permissions
    check_permissions "$platform"

    # Handle uninstall (doesn't need all checks). The self-delete guard also
    # ran early (before platform detection); this second call covers the normal
    # case where INSTALL_DIR was computed by set_install_paths just above.
    if [ "$uninstall_mode" = true ]; then
        _refuse_uninstall_from_install_dir
        uninstall "$platform"
        exit 0
    fi

    # Defensive: if uninstall_mode is still true at this point, the early
    # exit above is broken — fail loudly rather than running the install
    # path, which is the failure mode that caused user reports of
    # "--uninstall reinstalled HelixScreen".
    if [ "$uninstall_mode" = true ]; then
        log_error "internal error: install path entered with uninstall_mode=true"
        log_error "please report at https://github.com/prestonbrown/helixscreen/issues"
        exit 99
    fi

    # Pre-flight checks
    log_info "Running pre-flight checks..."
    check_requirements
    install_runtime_deps "$platform"
    check_disk_space "$platform"
    detect_init_system
    check_klipper_ecosystem "$platform"

    # Get version (skip if using local archive)
    if [ -n "$local_tarball" ]; then
        # Validate local file exists
        if [ ! -f "$local_tarball" ]; then
            log_error "Local archive not found: $local_tarball"
            exit 1
        fi
        # Extract version from filename if possible. Only the tar.gz layout
        # carries a version in the name (helixscreen-<plat>-v1.2.3.tar.gz).
        # The unversioned helixscreen-<plat>.zip gets "local" as a placeholder.
        version=$(echo "$local_tarball" | sed -n 's/.*helixscreen-[^-]*-\(v[0-9.]*\)\.tar\.gz/\1/p')
        if [ -z "$version" ]; then
            version="local"
        fi
        log_info "Installing from local file: ${BOLD}${local_tarball}${NC}"
    else
        if [ -z "$version" ]; then
            version=$(get_latest_version "$download_platform")
        fi
    fi
    log_info "Target version: ${BOLD}${version}${NC}"

    # Configure platform-specific settings before stopping UIs
    configure_platform

    # Stop competing UIs
    stop_competing_uis

    # Clean old installation if requested
    if [ "$clean_mode" = true ]; then
        clean_old_installation "$platform"
    fi

    # Download/stage the release archive BEFORE stopping the service.
    # Stopping helixscreen first can disrupt the network on some platforms
    # (e.g. Snapmaker U1 where platform_post_stop restarts the stock GUI which
    # owns wpa_supplicant and drops WiFi/SSH mid-update). Staging first also
    # means a failed download leaves the running service untouched.
    if [ -n "$local_tarball" ]; then
        use_local_tarball "$local_tarball"
    else
        download_release "$version" "$download_platform"
    fi

    if [ "$update_mode" = true ]; then
        if [ ! -d "$INSTALL_DIR" ]; then
            log_warn "No existing installation found. Performing fresh install."
        fi
        stop_service "$platform"
    fi

    extract_release "$platform"
    fix_install_ownership
    install_service "$platform"
    install_platform_hooks

    # Install KIAUH extension if KIAUH is detected
    install_kiauh_extension "$skip_kiauh_registration" || true

    # K1: ensure SSH (dropbear) is running — recovers from #535 where disabling
    # S99start_app also killed SSH. Runs on both fresh install and self-update.
    if [ "$platform" = "k1" ]; then
        ensure_k1_ssh
    fi

    # Verify all shared library dependencies are satisfied before starting
    verify_binary_deps "$platform"

    # Create platform cache directory
    case "$platform" in
        ad5m)
            $SUDO mkdir -p /data/helixscreen/cache
            ;;
        k1)
            $SUDO mkdir -p /usr/data/helixscreen/cache
            ;;
    esac

    # Symlink config into printer_data (Pi/Klipper only - enables web UI editing)
    setup_config_symlink

    # Configure Moonraker update_manager (Pi only - enables web UI updates)
    configure_moonraker_updates "$platform"

    # K2: replace the stock proprietary WebRTC camera (which HelixScreen and
    # fluidd can't consume) with a static ustreamer MJPEG server and point both
    # UIs at it. No-op on every other platform. Runs after the binary is
    # extracted and Moonraker is reachable; safe (detect-first) and reversible
    # (stock WebRTC disable is recorded for re-enable on uninstall).
    install_camera_k2 "$platform" || true

    # Install platform-specific helix-recover.sh used by PrinterRecoveryService
    # when klippy_uds is dead and firmware_restart can't proxy. No-op on stock
    # systemd platforms (pi/pi32/x86) where services.restart handles it.
    configure_local_recovery "$platform"

    # Fix known Klipper config issues (AD5M screw_thread, etc.)
    fix_ad5m_klipper_config || true

    # Generic per-printer install-time layer (#986): detect a known printer
    # model and apply its bundled settings seed + Klipper include, if any.
    # detect_printer_model() is conservative and returns empty for unknown
    # hardware, so this is a no-op on every platform without a registered id.
    local seed_pid
    seed_pid=$(detect_printer_model)
    if [ -n "$seed_pid" ]; then
        log_info "Recognized printer model: ${seed_pid} -- applying install-time defaults"
        seed_settings_for_printer "$seed_pid" || true
        install_klipper_include_for_printer "$seed_pid" || true
    else
        seed_from_moonraker_detection || true
    fi

    # Configure ALSA "default" when the board has no card 0 (e.g. Pi + HDMI-audio
    # screens like the BTT HDMI5, whose only outputs are vc4hdmi0/vc4hdmi1 at
    # indices 1/2). Safe no-op when "default" already works or /etc/asound.conf
    # already exists.
    configure_alsa_default || true

    # Start service
    start_service "$platform"
    cleanup_old_install
    cleanup_stale_cache_dirs

    # Cleanup on success
    cleanup_on_success

    printf '\n'
    printf '%b\n' "${GREEN}${BOLD}========================================${NC}"
    printf '%b\n' "${GREEN}${BOLD}    Installation Complete!${NC}"
    printf '%b\n' "${GREEN}${BOLD}========================================${NC}"
    printf '\n'
    echo "HelixScreen ${version} installed to ${INSTALL_DIR}"
    echo ""
    print_post_install_commands
    echo ""

    if [ "$platform" = "ad5m" ] || [ "$platform" = "k1" ] || [ "$platform" = "k2" ]; then
        echo "Note: You may need to reboot for the display to update."
    fi
}
