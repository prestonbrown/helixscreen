#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: requirements
# Pre-flight checks: commands, dependencies, disk space, init system
#
# Reads: PLATFORM, SUDO
# Writes: INIT_SYSTEM, MISSING_UNZIP_PKG, MISSING_RUNTIME_DEPS, DISK_CHECK_DEFERRED

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
    if ! _has_real_curl && ! command -v wget >/dev/null 2>&1 && ! _has_python; then
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
    # in extract_release instead of failing fast with a clear message. Where
    # apt can install unzip (detect_missing_unzip), install_missing_unzip does
    # that after the confirm point; only mark it missing when there's no apt
    # AND no usable python zipfile fallback.
    if ! command -v unzip >/dev/null 2>&1 && [ -z "${MISSING_UNZIP_PKG:-}" ] \
        && ! _py_has_module zipfile zlib; then
        _helix_add_missing "unzip"
    fi

    if [ -n "$missing" ]; then
        log_error "Missing required commands: $missing"
        log_error "Please install them and try again."
        exit 1
    fi

    log_success "All required commands available"
}

# unzip is missing and apt can install it (notably Snapmaker U1 extended
# firmware). Read-only: sets MISSING_UNZIP_PKG to "unzip" or empty.
detect_missing_unzip() {
    MISSING_UNZIP_PKG=""
    command -v unzip >/dev/null 2>&1 && return 0
    # A desktop install runs without root, so apt is not an option there.
    [ "${platform:-}" = "x86-desktop" ] && return 0
    if command -v apt-get >/dev/null 2>&1 && ! _has_no_new_privs; then
        MISSING_UNZIP_PKG=unzip
    fi
    return 0
}

# Install what detect_missing_unzip found. Exits when zip extraction is still
# impossible afterwards.
install_missing_unzip() {
    [ -n "${MISSING_UNZIP_PKG:-}" ] || return 0
    local rc=0
    log_info "Installing missing dependency: $MISSING_UNZIP_PKG"
    _apt_update_once
    run_logged $SUDO apt-get install -y --no-install-recommends "$MISSING_UNZIP_PKG" || rc=1
    if ! command -v unzip >/dev/null 2>&1 && ! _py_has_module zipfile zlib; then
        log_error "Missing required commands: unzip"
        log_error "Please install them and try again."
        exit 1
    fi
    return "$rc"
}

# The Debian packages the Pi build cannot start without: DRM display, libinput,
# and the GPU libraries behind DRM+EGL rendering. OpenSSL is statically linked,
# so no libssl. An image install checks the same list against the image's own
# package database (image.sh).
HELIX_PI_RUNTIME_DEPS="libdrm2 libinput10 libgbm1 libegl1 libgles2"

# Runtime libraries the Pi build needs that are not installed yet. Read-only:
# sets MISSING_RUNTIME_DEPS (space-separated package names, empty if none).
# Required for DRM display and evdev input handling; AD5M uses framebuffer
# with static linking, no deps needed.
detect_missing_runtime_deps() {
    local platform=$1
    MISSING_RUNTIME_DEPS=""

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
    local deps="$HELIX_PI_RUNTIME_DEPS"

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
        log_info "No turbojpeg package found (tried libturbojpeg0, libturbojpeg)"
        log_info "JPEG thumbnail decoding may not work"
    fi

    for dep in $deps; do
        # Check if package is installed (dpkg-query returns 0 if installed)
        if ! dpkg-query -W -f='${Status}' "$dep" 2>/dev/null | grep -q "install ok installed"; then
            MISSING_RUNTIME_DEPS="${MISSING_RUNTIME_DEPS:+$MISSING_RUNTIME_DEPS }$dep"
        fi
    done

    # Under NoNewPrivileges (self-update from the running app) sudo is
    # blocked, so the install cannot add them. The binary may still run, and
    # verify_binary_deps stops the install if a library it needs is missing.
    if [ -n "$MISSING_RUNTIME_DEPS" ] && _has_no_new_privs; then
        log_warn "Missing runtime libraries (cannot install under self-update): $MISSING_RUNTIME_DEPS"
        log_warn "Install them manually: sudo apt-get install $MISSING_RUNTIME_DEPS"
        MISSING_RUNTIME_DEPS=""
    fi
    return 0
}

# Install what detect_missing_runtime_deps found, detecting first when it has
# not run. Returns non-zero when apt could not install them.
install_runtime_deps() {
    [ -n "${MISSING_RUNTIME_DEPS+x}" ] || detect_missing_runtime_deps "$1"
    local missing="${MISSING_RUNTIME_DEPS:-}"

    # Only needed for Pi (32-bit and 64-bit) - AD5M uses framebuffer with static linking
    if [ "$1" != "pi" ] && [ "$1" != "pi32" ]; then
        return 0
    fi

    if [ -n "$missing" ]; then
        log_info "Installing missing libraries: $missing"
        _apt_update_once
        # shellcheck disable=SC2086
        if ! run_logged $SUDO apt-get install -y --no-install-recommends $missing; then
            log_warn "Failed to install some runtime libraries: $missing"
            log_warn "The install will continue. Install manually: sudo apt-get install $missing"
            return 1
        else
            log_success "Runtime libraries installed"
        fi
    else
        log_success "All runtime libraries already installed"
    fi
}

# Real-write space probe: can <dir> take a <kb>-KB file? Leaves nothing
# behind. Free-space numbers cannot see a read-only filesystem (df happily
# reports room on a squashfs), so where a wrong answer costs a mid-install
# ENOSPC the filesystem is asked directly instead. Shared by
# check_service_dest_space and check_disk_space's no-df-target fallback so
# both ask the same way.
#
# Before the confirm point sudo must not prompt, so a probe that would need a
# password returns 2 (undetermined) and DISK_CHECK_DEFERRED asks main() to
# repeat the check once confirm_point has been granted sudo.
_fs_probe_write_kb() {
    local dir="$1" kb="$2"
    local probe="${dir%/}/.helixscreen-space-probe.$$"
    if [ -n "$SUDO" ] && [ "${HELIX_CONFIRMED:-}" != 1 ] && ! $SUDO -n true 2>/dev/null; then
        # shellcheck disable=SC2034  # consumed by main.sh and plan.sh (confirm_point)
        DISK_CHECK_DEFERRED=1
        return 2
    fi
    if $SUDO dd if=/dev/zero of="$probe" bs=1024 count="$kb" \
            >/dev/null 2>&1; then
        $SUDO rm -f "$probe" 2>/dev/null || true
        return 0
    fi
    $SUDO rm -f "$probe" 2>/dev/null || true
    return 1
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

    # The walk ran out at "/": no ancestor of INSTALL_DIR exists. Pointing df
    # at "/" would measure the read-only squashfs the embedded hosts boot
    # from -- on a stock AD5X "/" is a 12.5M squashfs with no /srv on it,
    # while the install's bytes actually land on the writable data partition
    # (4.7G free on the same box). Measure the first conventional data mount
    # instead. With none present, leave check_dir at "/" and fall through to
    # the real-write probe below, which also refuses a read-only root that df
    # would report as roomy.
    if [ "$check_dir" = "/" ]; then
        local cand data_mount=""
        # shellcheck disable=SC2086  # word splitting is the point: a candidate path list
        for cand in ${HELIX_DATA_MOUNT_CANDIDATES:-/usr/data /mnt/UDISK /data}; do
            if [ -d "$cand" ]; then
                data_mount="$cand"
                break
            fi
        done
        [ -n "$data_mount" ] && check_dir="$data_mount"
    fi

    # Get available space in MB (skipped when there is nothing trustworthy to
    # point df at)
    local available_mb=""
    if [ "$check_dir" != "/" ]; then
        available_mb=$(_fs_free_mb "$check_dir")
    fi

    if [ -z "$available_mb" ]; then
        # df could not answer (no directory to point it at, or df itself
        # failed). Ask the filesystem with a real write instead.
        local probe_rc=0
        _fs_probe_write_kb "$check_dir" "$SERVICE_DEST_PROBE_KB" || probe_rc=$?
        if [ "$probe_rc" -ne 1 ]; then
            [ "$probe_rc" -eq 0 ] && log_info "Disk space check: $check_dir accepts a real write"
            # INSTALL_DIR is not the only filesystem this install writes to.
            check_service_dest_space
            return 0
        fi
        log_error "Cannot write to $check_dir -- full or read-only."
        log_error "Install target: ${INSTALL_DIR:-/opt/helixscreen}"
        exit 1
    fi

    if [ "$available_mb" -lt "$required_mb" ]; then
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
    # A desktop install writes no service definition.
    [ "${platform:-}" = "x86-desktop" ] && return 0
    # Nor does a self-update under systemd: it runs inside the unit's
    # ProtectSystem=strict sandbox, where /etc is read-only even for root, and
    # the unit's root ExecStartPre refreshes the service files on restart.
    if _is_self_update && _systemd_is_init; then
        return 0
    fi
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

    local probe_rc=0
    _fs_probe_write_kb "$dest_dir" "$SERVICE_DEST_PROBE_KB" || probe_rc=$?
    case "$probe_rc" in
        0) log_info "Service directory check: $(_fs_free_mb "$dest_dir")MB available on $dest_dir"
           return 0 ;;
        2) return 0 ;;
    esac

    # The K2's procd shim and its rc.d links also live here. A self-update
    # leaves each untouched when already current, and a failed write keeps
    # the installed copy, so neither makes the write required.
    if ! _self_update_needs_service_write; then
        log_warn "Cannot write to ${dest_dir} (full or read-only); continuing, the self-update leaves the installed service definition as it is."
        return 0
    fi

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
# True when systemd is this host's running init.
_systemd_is_init() {
    command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]
}

detect_init_system() {
    # Check for systemd
    if _systemd_is_init; then
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

# Print every process's command line, one per line, through the widest form
# the host's ps takes. Bare `ps` lists every process only on BusyBox: procps-ng
# ps lists the caller's session alone, which under `curl | sh` holds neither
# Klipper nor Moonraker. Some BusyBox builds reject -e/-o, and their plain ps
# cuts each line at the terminal width unless given w.
_ps_all_args() {
    ps -e -o args 2>/dev/null || ps w 2>/dev/null || ps 2>/dev/null
}

# True when systemd has service $1 active: $1.service or a numbered instance
# ($1-1.service, ...). Only digits may follow the dash, because klipper-mcu and
# moonraker-obico are other programs. Never true off systemd (INIT_SYSTEM).
_systemd_service_active() {
    [ "$INIT_SYSTEM" = systemd ] || return 1
    systemctl is-active --quiet "$1.service" 2>/dev/null && return 0
    systemctl list-units --type=service --state=active --no-legend --plain "$1-*.service" 2>/dev/null |
        awk -v svc="$1" '$1 ~ ("^" svc "-[0-9]+\\.service$") { found = 1 } END { exit !found }'
}

# True when HELIX_PROC_ROOT (else /proc) shows any process's argv. When it
# does, /proc holds every process whichever ps the host ships, and ps has
# nothing to add.
_proc_lists_processes() {
    local f
    for f in "${HELIX_PROC_ROOT:-/proc}"/[0-9]*/cmdline; do
        [ -r "$f" ] && return 0
    done
    return 1
}

# True when a ps line (_ps_all_args) runs the python program $1 the way
# running_python_module recognizes it: $1.py, or the module $1 or $1.$1.
# A ps line's columns differ between builds, so argv[0] is not checked here.
_ps_runs_python() {
    _ps_all_args | grep -v grep |
        grep -Eq "(^|[ /])$1\.py( |\$)|-m ?$1(\.$1)?( |\$)"
}

# True when the python program $1 runs here: in /proc when it lists any
# process, else in the ps listing, or on systemd as an active $1 unit. A log
# viewer, an editor, KlipperScreen or moonraker-obico only names the program.
_python_service_running() {
    if _proc_lists_processes; then
        [ -n "$(running_python_module "$1")" ] && return 0
    else
        _ps_runs_python "$1" && return 0
    fi
    _systemd_service_active "$2"
}

# True when Klipper (klippy.py, service klipper) runs here.
installer_klipper_running() {
    _python_service_running klippy klipper
}

# True when Moonraker runs here.
installer_moonraker_running() {
    _python_service_running moonraker moonraker
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

    local warnings="" moonraker_up=false

    if ! installer_klipper_running; then
        warnings="Klipper does not appear to be running."
    fi

    installer_moonraker_running && moonraker_up=true
    if [ "$moonraker_up" = false ]; then
        if [ -n "$warnings" ]; then
            warnings="$warnings
Moonraker does not appear to be running."
        else
            warnings="Moonraker does not appear to be running."
        fi
    fi

    # If Moonraker process is up, verify it responds where it serves
    if [ "$moonraker_up" = true ]; then
        local mr_url
        mr_url=$(installer_moonraker_url)
        # 5s: a loaded K1 or AD5M can take that long to answer.
        if ! moonraker_answers "${mr_url}/server/info" 5; then
            if [ -n "$warnings" ]; then
                warnings="$warnings
Moonraker is running but not responding on ${mr_url}."
            else
                warnings="Moonraker is running but not responding on ${mr_url}."
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

    # A dry run and --update never ask; tty_confirm answers yes itself under
    # --yes.
    if [ "${DRY_RUN:-false}" = true ] || [ "${update_mode:-false}" = true ]; then
        log_warn "Continuing without them."
        return 0
    fi

    if tty_confirm "Continue anyway?" y; then
        log_info "Continuing installation..."
        return 0
    fi
    log_error "Installation cancelled."
    exit 1
}

# Verify a binary that was built for the mod's chroot, from outside it.
#
# The binary's interpreter and libraries live under the chroot rootfs, so a
# host-side ldd resolves against the HOST's libc and reports the chroot's
# glibc as "not found" -- false errors for a binary that runs fine where the
# mod actually runs it (audit item 8). Ask the chroot's own ldd via chroot(1)
# instead, trying the in-chroot spellings of the install tree the chroot can
# expose: the same host path (the chroot may mount the data partition at the
# same node), plus each host->chroot prefix bind (the Forge-X chroot binds
# /usr/data at /opt). When no spelling exists there is no way to verify from
# outside: say so and carry on. Never fails the install.
#
# The bind list is env-overridable so the BATS suite can point the mapping at
# a sandbox tree (same convention as HELIX_MOD_TREE_CANDIDATES in
# host_profile.sh). Production leaves it unset.
_verify_binary_deps_via_chroot() {
    local chroot_dir="$1"
    local binary="$2"

    local candidates="$binary"
    local bind
    # shellcheck disable=SC2086  # word splitting is the point: a bind list
    for bind in ${HELIX_CHROOT_BINDS:-/usr/data=/opt}; do
        candidates="$candidates ${bind#*=}${binary#"${bind%%=*}"}"
    done

    local ldd_out=""
    local cand
    # shellcheck disable=SC2086  # word splitting is the point: a candidate list
    for cand in $candidates; do
        [ -f "${chroot_dir}${cand}" ] || continue
        ldd_out=$($SUDO chroot "$chroot_dir" ldd "$cand" 2>/dev/null || true)
        [ -n "$ldd_out" ] && break
    done

    if [ -n "$ldd_out" ]; then
        local missing
        missing=$(echo "$ldd_out" | grep "not found" || true)
        if [ -z "$missing" ]; then
            log_success "All shared library dependencies satisfied (verified inside the mod chroot)"
            return 0
        fi
        log_warn "Missing shared libraries inside the mod chroot:"
        echo "$missing" | while IFS= read -r line; do
            log_warn "  $line"
        done
        return 0
    fi

    log_warn "HelixScreen's binary is built for the firmware mod's chroot"
    log_warn "($chroot_dir) and cannot be dependency-checked from the host rootfs."
    log_warn "Host-side ldd would only report the chroot's libc as missing."
    log_warn "To check it by hand, run inside the chroot:"
    log_warn "  chroot $chroot_dir ldd <install root as seen inside the chroot>/bin/helix-screen"
}

# Verify the installed binary can find all shared libraries
# Runs ldd on the binary and checks for "not found" entries.
# If libssl.so.1.1 is missing (Bullseye→Bookworm upgrade), tries to install compat package.
# Called after extraction, before starting the service.
# Requires: INSTALL_DIR
verify_binary_deps() {
    local platform=$1
    local binary="${INSTALL_DIR}/bin/helix-screen"

    # Binary must exist
    if [ ! -f "$binary" ]; then
        log_warn "Binary not found at $binary, skipping dependency check"
        return 0
    fi

    # Mod chroot host: the binary is linked against the chroot's libc, not
    # the host's, so host-side ldd's answers there are false (see
    # _verify_binary_deps_via_chroot). Verify against the chroot and only
    # ever WARN. Inside the chroot ("/" IS the chroot root) the host-side
    # check below is already the right environment.
    case "${HOST_CHROOT_STATE:-}" in
        outside:*)
            _verify_binary_deps_via_chroot "${HOST_CHROOT_STATE#outside:}" "$binary"
            return 0
            ;;
    esac

    # Only relevant for platforms with dynamic linking and ldd
    if ! command -v ldd >/dev/null 2>&1; then
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
                    run_logged $SUDO apt-get install -y --no-install-recommends libssl1.1
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
