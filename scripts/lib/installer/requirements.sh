#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: requirements
# Pre-flight checks: commands, dependencies, disk space, init system
#
# Reads: PLATFORM, SUDO
# Writes: INIT_SYSTEM

# Source guard
[ -n "${_HELIX_REQUIREMENTS_SOURCED:-}" ] && return 0
_HELIX_REQUIREMENTS_SOURCED=1

# Initialize INIT_SYSTEM (will be set by detect_init_system)
INIT_SYSTEM=""

# Run `apt-get update` at most once per installer run. The first call updates
# the package index; subsequent calls become no-ops. Avoids 5-15s of redundant
# index refreshes when both check_requirements and install_runtime_deps need apt.
_HELIX_APT_UPDATED=""
_apt_update_once() {
    [ -n "$_HELIX_APT_UPDATED" ] && return 0
    $SUDO apt-get update -qq 2>/dev/null || true
    _HELIX_APT_UPDATED=1
}

# Append a name to the global $missing list, comma-separated.
_helix_add_missing() { missing="${missing:+$missing, }$1"; }

# Check required commands exist. unzip is mandatory because release archives
# ship as .zip (Moonraker Update Manager contract); tar/gunzip remain required
# for bridge-release tar.gz fallback until 1.0 retires the format.
check_requirements() {
    local missing=""

    # Need curl, wget, or python3 (urllib) to download the release.
    if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1 && ! _has_python; then
        _helix_add_missing "curl, wget, or python3"
    fi
    command -v tar >/dev/null 2>&1 || _helix_add_missing "tar"
    # gunzip needed for AD5M BusyBox tar which doesn't support -z
    command -v gunzip >/dev/null 2>&1 || _helix_add_missing "gunzip"

    # Zip extraction: prefer unzip, but python's zipfile module is a built-in
    # fallback (used on platforms like recent Creality K2 firmware that lack
    # unzip but ship python3). The fallback needs zipfile + zlib (release zips
    # are DEFLATE-compressed), so gate on those modules rather than mere python
    # presence — otherwise a zlib-less python passes here and the install dies
    # in extract_release instead of failing fast with a clear message. Try a
    # transparent apt-install of unzip on Debian/Ubuntu images that lack it
    # (notably Snapmaker U1 extended firmware); only mark it missing when
    # there's no apt AND no usable python zipfile fallback.
    if ! command -v unzip >/dev/null 2>&1; then
        if command -v apt-get >/dev/null 2>&1 && ! _has_no_new_privs; then
            log_info "Installing missing dependency: unzip"
            _apt_update_once
            $SUDO apt-get install -y --no-install-recommends unzip >/dev/null 2>&1 || true
            if ! command -v unzip >/dev/null 2>&1 && ! _py_has_module zipfile zlib; then
                _helix_add_missing "unzip"
            fi
        elif ! _py_has_module zipfile zlib; then
            _helix_add_missing "unzip"
        fi
    fi

    if [ -n "$missing" ]; then
        log_error "Missing required commands: $missing"
        log_error "Please install them and try again."
        exit 1
    fi

    log_success "All required commands available"
}

# Install runtime dependencies for Pi platform
# Required for DRM display and evdev input handling
# AD5M uses framebuffer with static linking, no deps needed
install_runtime_deps() {
    local platform=$1

    # Only needed for Pi (32-bit and 64-bit) - AD5M uses framebuffer with static linking
    if [ "$platform" != "pi" ] && [ "$platform" != "pi32" ]; then
        return 0
    fi

    log_info "Checking runtime dependencies for display/input..."

    # Required libraries for DRM display, libinput, GPU rendering, and camera
    # GPU libs are needed for DRM+EGL hardware-accelerated rendering on Pi
    # libturbojpeg: SIMD-accelerated JPEG decode for camera MJPEG streams
    #   Debian names it libturbojpeg0, Ubuntu names it libturbojpeg
    # Note: OpenSSL is statically linked for Pi builds, no runtime libssl needed
    local deps="libdrm2 libinput10 libgbm1 libegl1 libgles2"
    local missing=""

    # turbojpeg: package name varies by distro (Debian=libturbojpeg0, Ubuntu=libturbojpeg)
    local turbo_pkg=""
    for candidate in libturbojpeg0 libturbojpeg; do
        if dpkg-query -W -f='${Status}' "$candidate" 2>/dev/null | grep -q "install ok installed" || \
           apt-cache show "$candidate" 2>/dev/null | grep -q "^Package:"; then
            turbo_pkg="$candidate"
            break
        fi
    done
    if [ -n "$turbo_pkg" ]; then
        deps="$deps $turbo_pkg"
    else
        log_warn "No turbojpeg package found (tried libturbojpeg0, libturbojpeg)"
        log_warn "JPEG thumbnail decoding may not work"
    fi

    for dep in $deps; do
        # Check if package is installed (dpkg-query returns 0 if installed)
        if ! dpkg-query -W -f='${Status}' "$dep" 2>/dev/null | grep -q "install ok installed"; then
            if [ -n "$missing" ]; then
                missing="$missing $dep"
            else
                missing="$dep"
            fi
        fi
    done

    if [ -n "$missing" ]; then
        # Under NoNewPrivileges (self-update from the running app), sudo is blocked.
        # Warn about missing deps but don't fail — the binary may still work, and
        # verify_binary_deps() will catch truly fatal missing libraries later.
        if _has_no_new_privs; then
            log_warn "Missing runtime libraries (cannot install under self-update): $missing"
            log_warn "Install manually after update: sudo apt-get install $missing"
            return 0
        fi
        log_info "Installing missing libraries: $missing"
        _apt_update_once
        # shellcheck disable=SC2086
        if ! $SUDO apt-get install -y --no-install-recommends $missing; then
            log_warn "Failed to install some runtime libraries: $missing"
            log_warn "The update will continue. Install manually: sudo apt-get install $missing"
        else
            log_success "Runtime libraries installed"
        fi
    else
        log_success "All runtime libraries already installed"
    fi
}

# Check available disk space
# Requires at least 50MB free on the install directory's filesystem, then hands
# off to check_service_dest_space for the filesystem that receives the service
# definition -- on the K1 those are two different partitions.
# Note: INSTALL_DIR and INIT_SCRIPT_DEST must be set before calling this function
check_disk_space() {
    local platform=$1
    local required_mb=50

    # Get the parent directory of install location (the filesystem to check)
    local check_dir
    check_dir=$(dirname "${INSTALL_DIR:-/opt/helixscreen}")
    # Walk up until we find an existing directory
    while [ ! -d "$check_dir" ] && [ "$check_dir" != "/" ]; do
        check_dir=$(dirname "$check_dir")
    done
    if [ "$check_dir" = "/" ]; then
        check_dir="/"
    fi

    # Get available space in MB
    local available_mb
    case "$platform" in
        ad5m|ad5x|k1|k2)
            # BusyBox df: blocks are in KB by default
            available_mb=$(df "$check_dir" 2>/dev/null | tail -1 | awk '{print int($4/1024)}')
            ;;
        *)
            # GNU df with -m flag outputs in MB
            available_mb=$(df -m "$check_dir" 2>/dev/null | tail -1 | awk '{print $4}')
            ;;
    esac

    if [ -n "$available_mb" ] && [ "$available_mb" -lt "$required_mb" ]; then
        log_error "Insufficient disk space on $check_dir"
        log_error "Required: ${required_mb}MB, Available: ${available_mb}MB"
        exit 1
    fi

    log_info "Disk space check: ${available_mb}MB available on $check_dir"

    # INSTALL_DIR is not the only filesystem this install writes to.
    check_service_dest_space
}

# Free space the service definition needs, in KB. The init script is ~15KB; the
# probe is sized well above it so a filesystem with only a few free blocks left
# fails here rather than at the real write.
SERVICE_DEST_PROBE_KB=64

# Echo the overlayfs upperdir backing `/`, if `/` is an overlay.
#
# On an overlay root the bytes live in the upperdir, so that is the only tree
# worth pointing a user's `du` at: `du /` descends into every larger partition
# mounted underneath (on the K1, the multi-gigabyte /usr/data) and reports a
# number with no relationship to the full filesystem.
# Reads /proc/mounts by default; the path is an argument so the parse can be
# driven against a fixture instead of the running system.
# shellcheck disable=SC2120  # the argument is a test seam; production passes none
_overlay_upperdir() {
    awk '$2 == "/" && $3 == "overlay" {
             n = split($4, opts, ",")
             for (i = 1; i <= n; i++)
                 if (opts[i] ~ /^upperdir=/) {
                     sub(/^upperdir=/, "", opts[i])
                     print opts[i]
                     exit
                 }
         }' "${1:-/proc/mounts}" 2>/dev/null
}

# Check the filesystem that will receive the init script or systemd unit.
#
# It is frequently NOT the filesystem holding INSTALL_DIR. On the K1 they are
# different partitions entirely: /usr/data is a multi-gigabyte ext4
# (mmcblk0p10) while /etc/init.d sits on the ~97MB root overlay (mmcblk0p9,
# upperdir for the overlayfs `/`). A K1 whose overlay is full sails through the
# INSTALL_DIR check with gigabytes reported free and then dies in
# install_service_sysv with
#     cp: write error: No space left on device
# -- by which point stop_competing_uis has already disabled the stock UI, so
# the printer is left with no screen at all, and the "3768MB available" line
# above sends the user off to delete print files on the partition that was
# never the problem.
#
# Probing with a real write rather than a free-space threshold: the operation
# under test is a 15KB copy, and a filesystem with 800KB free performs it just
# fine. Any MB-granularity floor either false-positives on a legitimately tight
# rootfs or fails to catch a genuinely full one. The probe also catches a
# read-only or unwritable destination, which is the same class of failure.
#
# Runs at pre-flight -- before any UI is stopped and before the ~58MB download
# -- so a refusal here leaves the printer exactly as it was found.
check_service_dest_space() {
    # set_install_paths() has already run, so INIT_SCRIPT_DEST is known. It is
    # empty on systemd platforms, where install_service_systemd writes the unit
    # to /etc/systemd/system instead.
    local dest_dir=""
    if [ -n "${INIT_SCRIPT_DEST:-}" ]; then
        dest_dir=$(dirname "$INIT_SCRIPT_DEST")
    elif [ -d /etc/systemd/system ]; then
        dest_dir="/etc/systemd/system"
    fi
    [ -n "$dest_dir" ] && [ -d "$dest_dir" ] || return 0

    # Resolve the same directory check_disk_space measured, so a destination on
    # that filesystem can be skipped -- it is already covered, and a second
    # line quoting the same number reads like a second measurement.
    local install_probe
    install_probe=$(dirname "${INSTALL_DIR:-/opt/helixscreen}")
    while [ ! -d "$install_probe" ] && [ "$install_probe" != "/" ]; do
        install_probe=$(dirname "$install_probe")
    done
    [ "$(_fs_id "$dest_dir")" != "$(_fs_id "$install_probe")" ] || return 0

    local probe="${dest_dir}/.helixscreen-space-probe.$$"
    if $SUDO dd if=/dev/zero of="$probe" bs=1024 count="$SERVICE_DEST_PROBE_KB" \
            >/dev/null 2>&1; then
        $SUDO rm -f "$probe" 2>/dev/null || true
        log_info "Service directory check: $(_fs_free_mb "$dest_dir")MB available on $dest_dir"
        return 0
    fi
    $SUDO rm -f "$probe" 2>/dev/null || true

    local upper
    upper=$(_overlay_upperdir)

    log_error "Cannot write the service definition to ${dest_dir}."
    log_error "Filesystem: $(_fs_id "$dest_dir") ($(_fs_free_mb "$dest_dir")MB free) -- full or read-only."
    log_error ""
    log_error "This is a DIFFERENT filesystem from ${install_probe}, which has room."
    log_error "Deleting print files or gcode will NOT help: they are on the other partition."
    log_error ""
    log_error "Find what is filling it:"
    log_error "  df -h ${dest_dir}"
    if [ -n "$upper" ] && [ -d "$upper" ]; then
        log_error "  du -k ${upper}/* 2>/dev/null | sort -n | tail -20"
    else
        log_error "  du -k ${dest_dir}/* 2>/dev/null | sort -n | tail -20"
    fi
    log_error ""
    log_error "Free a few MB there, then re-run this installer."
    exit 1
}

# Detect init system (systemd vs SysV)
# Sets: INIT_SYSTEM to "systemd" or "sysv"
detect_init_system() {
    # Check for systemd
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        INIT_SYSTEM="systemd"
        log_info "Init system: systemd"
        return
    fi

    # Check for SysV init (BusyBox or traditional)
    if [ -d /etc/init.d ]; then
        # shellcheck disable=SC2034  # consumed by service.sh, common.sh, competing_uis.sh, uninstall.sh
        INIT_SYSTEM="sysv"
        log_info "Init system: SysV (BusyBox/traditional)"
        return
    fi

    log_error "Could not detect init system."
    log_error "Neither systemd nor /etc/init.d found."
    exit 1
}

# Check that Klipper and Moonraker are running (AD5M + K1 only)
# These platforms require local Klipper/Moonraker; without them HelixScreen
# has nothing to connect to. Warns and prompts for confirmation.
check_klipper_ecosystem() {
    local platform=$1

    # Only relevant for embedded platforms with local Klipper
    case "$platform" in
        ad5m|ad5x|k1|k2) ;;
        *) return 0 ;;
    esac

    local warnings=""

    # Check Klipper process
    # Klipper runs as "klippy" (python module) or "klipper" (service name)
    if ! ps | grep -v grep | grep -q -e '[Kk]lipper' -e '[Kk]lippy'; then
        warnings="Klipper does not appear to be running."
    fi

    # Check Moonraker process
    if ! ps | grep -v grep | grep -q '[Mm]oonraker'; then
        if [ -n "$warnings" ]; then
            warnings="$warnings
Moonraker does not appear to be running."
        else
            warnings="Moonraker does not appear to be running."
        fi
    fi

    # If Moonraker process is up, verify it responds on the expected port
    if ps | grep -v grep | grep -q '[Mm]oonraker'; then
        if command -v wget >/dev/null 2>&1; then
            if ! wget -q -O /dev/null --timeout=5 "http://127.0.0.1:7125/server/info" 2>/dev/null; then
                if [ -n "$warnings" ]; then
                    warnings="$warnings
Moonraker is running but not responding on http://127.0.0.1:7125."
                else
                    warnings="Moonraker is running but not responding on http://127.0.0.1:7125."
                fi
            fi
        elif command -v curl >/dev/null 2>&1; then
            if ! curl -sf --connect-timeout 5 "http://127.0.0.1:7125/server/info" >/dev/null 2>&1; then
                if [ -n "$warnings" ]; then
                    warnings="$warnings
Moonraker is running but not responding on http://127.0.0.1:7125."
                else
                    warnings="Moonraker is running but not responding on http://127.0.0.1:7125."
                fi
            fi
        fi
    fi

    # Everything looks good
    if [ -z "$warnings" ]; then
        log_success "Klipper and Moonraker are running"
        return 0
    fi

    # Show warnings and prompt
    log_warn ""
    log_warn "WARNING: Klipper ecosystem check failed:"
    # Print each warning line separately
    echo "$warnings" | while IFS= read -r line; do
        log_warn "  - $line"
    done
    log_warn ""
    log_warn "HelixScreen requires Klipper and Moonraker to function."
    log_warn "It will install but won't work until these services are available."

    # Non-interactive mode: just warn and continue
    if [ ! -t 0 ]; then
        log_warn "Non-interactive mode: continuing anyway."
        return 0
    fi

    printf "Continue anyway? [y/N] "
    read -r answer
    case "$answer" in
        [Yy]|[Yy][Ee][Ss])
            log_info "Continuing installation..."
            return 0
            ;;
        *)
            log_error "Installation cancelled."
            exit 1
            ;;
    esac
}

# Verify the installed binary can find all shared libraries
# Runs ldd on the binary and checks for "not found" entries.
# If libssl.so.1.1 is missing (Bullseye→Bookworm upgrade), tries to install compat package.
# Called after extraction, before starting the service.
# Requires: INSTALL_DIR
verify_binary_deps() {
    local platform=$1
    local binary="${INSTALL_DIR}/bin/helix-screen"

    # Only relevant for platforms with dynamic linking and ldd
    if ! command -v ldd >/dev/null 2>&1; then
        return 0
    fi

    # Binary must exist
    if [ ! -f "$binary" ]; then
        log_warn "Binary not found at $binary, skipping dependency check"
        return 0
    fi

    # Check for missing shared libraries
    local missing_libs
    missing_libs=$(ldd "$binary" 2>/dev/null | grep "not found" || true)

    if [ -z "$missing_libs" ]; then
        log_success "All shared library dependencies satisfied"
        return 0
    fi

    log_warn "Missing shared libraries detected:"
    echo "$missing_libs" | while IFS= read -r line; do
        log_warn "  $line"
    done

    # Try to fix known issues on Pi
    case "$platform" in
        pi|pi32)
            # libssl.so.1.1 missing = Bookworm system with Bullseye-era binary
            if echo "$missing_libs" | grep -q "libssl.so.1.1"; then
                log_info "libssl.so.1.1 not found (common after Debian Bullseye→Bookworm upgrade)"
                # Try installing the compat package if available
                if apt-cache show libssl1.1 >/dev/null 2>&1; then
                    log_info "Installing libssl1.1 compatibility package..."
                    $SUDO apt-get install -y --no-install-recommends libssl1.1
                else
                    log_error "libssl1.1 package not available in your repositories."
                    log_error "This binary was built against OpenSSL 1.1 but your system has OpenSSL 3."
                    log_error "Please update HelixScreen to the latest version which includes OpenSSL statically."
                    exit 1
                fi
            fi

            # Re-check after attempted fixes
            missing_libs=$(ldd "$binary" 2>/dev/null | grep "not found" || true)
            if [ -n "$missing_libs" ]; then
                # Check if fbdev fallback binary exists and is loadable
                local fallback="${INSTALL_DIR}/bin/helix-screen-fbdev"
                if [ -x "$fallback" ]; then
                    local fb_missing
                    fb_missing=$(ldd "$fallback" 2>/dev/null | grep "not found" || true)
                    if [ -z "$fb_missing" ]; then
                        log_warn "DRM binary has missing GL libraries -- fbdev fallback will be used"
                        log_warn "Install GPU libraries for hardware acceleration: sudo apt install libgbm1 libegl1 libgles2"
                        return 0
                    fi
                fi
                # Under NoNewPrivileges (self-update), we can't install libs.
                # Warn but don't block — the binary may still start with reduced
                # functionality (e.g. no SIMD camera decode).
                if _has_no_new_privs; then
                    log_warn "Missing libraries (cannot install during self-update):"
                    echo "$missing_libs" | while IFS= read -r line; do
                        log_warn "  $line"
                    done
                    # Extract library basenames (e.g. "libturbojpeg.so.0") for user guidance
                    local lib_names
                    lib_names=$(echo "$missing_libs" | awk '{print $1}' | tr '\n' ' ')
                    log_warn "Install manually: sudo apt-get install packages providing: $lib_names"
                    return 0
                fi
                # No usable fallback — original error behavior
                log_error "Could not resolve all missing libraries:"
                echo "$missing_libs" | while IFS= read -r line; do
                    log_error "  $line"
                done
                log_error "The binary may not start correctly."
                log_error "Please report this issue at https://github.com/prestonbrown/helixscreen/issues"
                exit 1
            fi
            log_success "All shared library dependencies resolved"
            ;;
        *)
            # Non-Pi platforms: just warn, don't block
            log_warn "Some libraries are missing. The binary may not start correctly."
            ;;
    esac
}
