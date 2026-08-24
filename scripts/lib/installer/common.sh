#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: common
# Core utilities: logging, colors, error handling, cleanup
#
# Reads: -
# Writes: RED, GREEN, YELLOW, CYAN, BOLD, NC, CLEANUP_TMP, BACKUP_CONFIG, INSTALL_DIR, GITHUB_REPO

# Source guard
[ -n "${_HELIX_COMMON_SOURCED:-}" ] && return 0
_HELIX_COMMON_SOURCED=1

# Default configuration (can be overridden before sourcing)
: "${GITHUB_REPO:=prestonbrown/helixscreen}"
: "${INSTALL_DIR:=/opt/helixscreen}"
: "${SERVICE_NAME:=helixscreen}"

# Well-known paths (used by uninstall, clean, stop_service)
# AD5M: /opt/helixscreen, /root/printer_software/helixscreen, /srv/helixscreen (ZMOD)
# K1: /usr/data/helixscreen
# Pi: /opt/helixscreen
# CC1 (COSMOS): /user-resource/helixscreen (/ is RO squashfs)
# Snapmaker U1: /userdata/helixscreen
# shellcheck disable=SC2034  # consumed by uninstall.sh (sweep of all known install locations)
HELIX_INSTALL_DIRS="/root/printer_software/helixscreen /opt/helixscreen /usr/data/helixscreen /srv/helixscreen /user-resource/helixscreen /userdata/helixscreen"

# Init script locations vary by platform/firmware
# AD5M Klipper Mod: S80, AD5M Forge-X: S90, K1: S99, CC1 (COSMOS): plain /etc/init.d/helixscreen
# shellcheck disable=SC2034  # consumed by service.sh and uninstall.sh
HELIX_INIT_SCRIPTS="/etc/init.d/S80helixscreen /etc/init.d/S90helixscreen /etc/init.d/S99helixscreen /etc/init.d/helixscreen"

# HelixScreen process names (order matters: watchdog first to prevent crash dialog)
# shellcheck disable=SC2034  # consumed by service.sh and uninstall.sh (kill_process_by_name)
HELIX_PROCESSES="helix-watchdog helix-screen helix-splash"

# Returns true when install.sh was spawned by helix-screen's in-app update.
# Used by multiple modules (service.sh, competing_uis.sh) to skip operations
# that are unnecessary or destructive during self-update.
# Set by update_checker.cpp before execv().
_is_self_update() {
    [ "${HELIX_SELF_UPDATE:-}" = "1" ]
}

# Probe for a usable Python interpreter with urllib (cached). Sets _PY_BIN to
# the first of python3/python that can import urllib.request — the baseline for
# downloading over plain HTTP. Used as a download/extraction fallback on
# platforms that lack curl/wget/unzip (notably recent Creality K2 Tina/OpenWrt
# firmware). HTTPS and zip support are probed separately via _py_has_module
# (ssl / zipfile) so an ssl-less or zlib-less python can still serve the
# HTTP-only mirror rather than being rejected outright. Returns 0 if a usable
# interpreter was found, non-zero otherwise.
_PY_BIN=""
_PY_PROBED=""
_has_python() {
    if [ -z "$_PY_PROBED" ]; then
        _PY_PROBED=1
        for _cand in python3 python; do
            if command -v "$_cand" >/dev/null 2>&1 && \
               "$_cand" -c 'import urllib.request' >/dev/null 2>&1; then
                _PY_BIN="$_cand"
                break
            fi
        done
    fi
    [ -n "$_PY_BIN" ]
}

# Check that the resolved python (_PY_BIN) can import the named module(s).
# Args: one or more module names (e.g. "ssl", or "zipfile zlib"). Returns
# non-zero if no python is available or any module fails to import. Modules are
# passed as argv (no external tr/echo dependency, so this works on a minimal
# PATH). Not cached — callers invoke it once per capability gate.
_py_has_module() {
    _has_python || return 1
    "$_PY_BIN" -c 'import sys
for m in sys.argv[1:]:
    __import__(m)' "$@" >/dev/null 2>&1
}

# Get sudo prefix needed for a file operation.
# Returns empty string if current user has write access, $SUDO otherwise.
# For existing files, checks file writability. For new files, checks parent dir.
# This avoids creating root-owned files in user-writable directories.
file_sudo() {
    local path="$1"
    if [ -e "$path" ]; then
        [ -w "$path" ] && echo "" || echo "$SUDO"
    else
        local dir
        dir="$(dirname "$path")"
        [ -w "$dir" ] && echo "" || echo "$SUDO"
    fi
}

# Get sudo prefix needed to RENAME or REMOVE a path (mv/rm/rmdir of the path
# itself, not of something inside it).
#
# Always checks the PARENT, existing target or not. rename(2) and unlink(2)
# mutate the parent directory's entries; the target's own mode has nothing to do
# with it. So a user-owned directory inside a root-owned parent is writable and
# still cannot be moved or deleted.
#
# That is not hypothetical, it is the /opt/helixscreen layout: helixscreen.service
# chowns the install dir to the service user via ExecStartPre while /opt stays
# root:root. file_sudo() answers "can I write INTO this", returns "" there, and the
# swap runs bare:
#
#   mv: cannot move '/opt/helixscreen' to '/opt/helixscreen.old': Permission denied
#
# Use file_sudo() when writing a file into a directory; use this when the path is
# the thing being moved or deleted.
path_sudo() {
    local dir
    dir="$(dirname "$1")"
    [ -w "$dir" ] && echo "" || echo "$SUDO"
}

# Resolve the directory holding the user's Klipper/Moonraker config files.
#
# Almost every Klipper install puts them in <klipper home>/printer_data/config,
# so that is the derived default and no platform needs to say anything. Vendor
# firmwares that do not use printer_data AT ALL set KLIPPER_CONFIG_DIR in
# set_install_paths() instead:
#
#   Elegoo Centauri Carbon / COSMOS (OpenCentauri) keeps everything in
#   /etc/klipper/config — moonraker.conf, printer.cfg, the *-readonly/ vendor
#   include dirs — and has no printer_data directory anywhere on the device
#   (verified over SSH; Moonraker's /server/files/roots reports the `config`
#   root as /etc/klipper/config, rw). Deriving from KLIPPER_HOME=/root there
#   yields /root/printer_data/config, which does not exist, so moonraker.conf
#   discovery, the config symlinks and the Klipper include all silently
#   skipped.
#
# Echoes the directory, or an empty string when neither is known.
klipper_config_dir() {
    if [ -n "${KLIPPER_CONFIG_DIR:-}" ]; then
        echo "$KLIPPER_CONFIG_DIR"
        return 0
    fi
    if [ -n "${KLIPPER_HOME:-}" ]; then
        echo "${KLIPPER_HOME}/printer_data/config"
        return 0
    fi
    echo ""
}

# Track what we've done for cleanup
CLEANUP_TMP=false
BACKUP_CONFIG=""
BACKUP_ENV=""
# shellcheck disable=SC2034  # consumed by release.sh (set true at the swap, read at config restore)
ORIGINAL_INSTALL_EXISTS=false

# Colors (if terminal supports it)
setup_colors() {
    if [ -t 1 ]; then
        RED='\033[0;31m'
        GREEN='\033[0;32m'
        YELLOW='\033[1;33m'
        CYAN='\033[0;36m'
        BOLD='\033[1m'
        NC='\033[0m'
    else
        RED=''
        GREEN=''
        YELLOW=''
        CYAN=''
        # shellcheck disable=SC2034  # consumed by main.sh (installer banner) and release.sh
        BOLD=''
        NC=''
    fi
}

# Initialize colors immediately
setup_colors

# Logging functions (printf %b interprets \033 escapes; BusyBox echo does not)
log_info() { printf '%b\n' "${CYAN}[INFO]${NC} $1" >&2; }
log_success() { printf '%b\n' "${GREEN}[OK]${NC} $1" >&2; }
log_warn() { printf '%b\n' "${YELLOW}[WARN]${NC} $1" >&2; }
log_error() { printf '%b\n' "${RED}[ERROR]${NC} $1" >&2; }

# Error handler - cleanup and report what went wrong
# Usage: trap 'error_handler $LINENO' ERR
error_handler() {
    local exit_code=$?
    local line_no=$1

    echo ""
    log_error "=========================================="
    log_error "Installation FAILED at line $line_no"
    log_error "Exit code: $exit_code"
    log_error "=========================================="
    echo ""

    # Restore backups BEFORE cleaning TMP_DIR — backup files live in TMP_DIR.
    # Try TMP_DIR backup first, then fall back to .old directory (survives PrivateTmp).
    $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config" 2>/dev/null || true

    if [ ! -f "${INSTALL_DIR}/config/settings.json" ]; then
        local _restored=false
        # Try TMP_DIR backup
        if [ -n "$BACKUP_CONFIG" ] && [ -f "$BACKUP_CONFIG" ]; then
            log_info "Restoring backed up configuration..."
            if $(file_sudo "${INSTALL_DIR}/config") cp "$BACKUP_CONFIG" "${INSTALL_DIR}/config/settings.json" 2>/dev/null; then
                log_success "Configuration restored from backup"
                _restored=true
            fi
        fi
        # Fallback: .old directory (try new name first, then legacy)
        if [ "$_restored" = false ] && [ -n "${INSTALL_BACKUP:-}" ]; then
            if [ -f "${INSTALL_BACKUP}/config/settings.json" ]; then
                if $(file_sudo "${INSTALL_DIR}/config") cp "${INSTALL_BACKUP}/config/settings.json" "${INSTALL_DIR}/config/settings.json" 2>/dev/null; then
                    log_success "Configuration restored from previous install"
                    _restored=true
                fi
            elif [ -f "${INSTALL_BACKUP}/config/helixconfig.json" ]; then
                if $(file_sudo "${INSTALL_DIR}/config") cp "${INSTALL_BACKUP}/config/helixconfig.json" "${INSTALL_DIR}/config/settings.json" 2>/dev/null; then
                    log_success "Configuration restored from previous install (migrated from helixconfig.json)"
                    _restored=true
                fi
            elif [ -f "${INSTALL_BACKUP}/helixconfig.json" ]; then
                if $(file_sudo "${INSTALL_DIR}/config") cp "${INSTALL_BACKUP}/helixconfig.json" "${INSTALL_DIR}/config/settings.json" 2>/dev/null; then
                    log_success "Configuration restored from previous install (legacy root location)"
                    _restored=true
                fi
            fi
        fi
        if [ "$_restored" = false ]; then
            log_warn "Could not restore config from any backup source"
        fi
    fi

    if [ ! -f "${INSTALL_DIR}/config/helixscreen.env" ]; then
        if [ -n "$BACKUP_ENV" ] && [ -f "$BACKUP_ENV" ]; then
            if $(file_sudo "${INSTALL_DIR}/config") cp "$BACKUP_ENV" "${INSTALL_DIR}/config/helixscreen.env" 2>/dev/null; then
                log_success "helixscreen.env restored"
            fi
        elif [ -n "${INSTALL_BACKUP:-}" ] && [ -f "${INSTALL_BACKUP}/config/helixscreen.env" ]; then
            if $(file_sudo "${INSTALL_DIR}/config") cp "${INSTALL_BACKUP}/config/helixscreen.env" "${INSTALL_DIR}/config/helixscreen.env" 2>/dev/null; then
                log_success "helixscreen.env restored from previous install"
            fi
        fi
    fi

    # Cleanup temporary files after restores are done
    if [ "$CLEANUP_TMP" = true ] && [ -d "$TMP_DIR" ]; then
        _safe_remove_tmp_dir "$TMP_DIR"
    fi

    echo ""
    log_error "Installation was NOT completed."
    log_error "Your system should be in its original state."
    echo ""
    log_info "For help, please:"
    log_info "  1. Check the error message above"
    log_info "  2. Verify network connectivity"
    log_info "  3. Report issues at: https://github.com/${GITHUB_REPO}/issues"
    echo ""

    exit $exit_code
}

# ---------------------------------------------------------------------------
# User-supplied path guards
#
# TMP_DIR and INSTALL_DIR are both documented, user-settable overrides — the
# installer itself prints "Try: TMP_DIR=/path/with/space sh install.sh" — and
# both feed destructive operations:
#
#   TMP_DIR      rm -rf "$TMP_DIR"                 (cleanup_on_success, error_handler)
#   INSTALL_DIR  mv "$INSTALL_DIR" "$INSTALL_BACKUP", rm -rf "$INSTALL_DIR",
#                and a sweep of every config/ sibling               (release.sh, uninstall.sh)
#
# Pointing either at an ordinary data directory therefore erases it.
# `TMP_DIR=/mnt/UDISK` once wiped a K2's whole user partition; the mountpoint
# check in _safe_remove_tmp_dir below catches only that exact shape, not
# `TMP_DIR=/home/pi`.
#
# The guard is the same one HELIX_OFFSITE_ROLLBACK_DIR already uses in
# release.sh: a `case` on the FINAL path component with an explicit refusal
# branch. If the last component isn't recognisably ours, we refuse loudly and
# the caller exits rather than silently falling back to a default.
# ---------------------------------------------------------------------------

# _user_dir_name_ok DIR PATTERN [PATTERN...]
# Returns 0 when DIR is absolute, traversal-free, and its final component
# matches one of the shell patterns. Patterns are intentionally unquoted in the
# `case` so globs apply.
_user_dir_name_ok() {
    local d="${1%/}"
    shift
    # Absolute only — a relative override resolves against an unknown $PWD.
    case "$d" in
        /*) ;;
        *) return 1 ;;
    esac
    # "/data/helixscreen-install/../.." is not the directory it claims to be.
    case "$d" in
        *..*) return 1 ;;
    esac
    local base="${d##*/}"
    # Empty base means d was "/" (or collapsed to it).
    [ -n "$base" ] || return 1
    local pat
    for pat in "$@"; do
        # shellcheck disable=SC2254  # $pat is a caller-supplied glob ('*helixscreen-install*'), not a literal
        case "$base" in
            $pat) return 0 ;;
        esac
    done
    return 1
}

# Accept only scratch directories the installer created, or the staging dir the
# in-app updater hands over via TMP_DIR (update_checker.cpp STAGING_NAME).
validate_tmp_dir() {
    local d="$1"
    if _user_dir_name_ok "$d" '*helixscreen-install*' '.helix-update-staging'; then
        return 0
    fi
    log_error "Refusing to use TMP_DIR='$d'"
    log_error "TMP_DIR is removed with 'rm -rf' when the installer finishes, so it"
    log_error "must be a scratch directory of ours — not an existing data directory."
    log_error "Its last path component must contain 'helixscreen-install'."
    log_error "Try: TMP_DIR=${d%/}/helixscreen-install sh install.sh"
    return 1
}

# Accept only install directories that name themselves after us. Every
# auto-detected value already does (/opt/helixscreen, $HOME/helixscreen,
# /usr/data/helixscreen, /srv/helixscreen, /user-resource/helixscreen, ...).
validate_install_dir() {
    local d="$1"
    if _user_dir_name_ok "$d" '*helixscreen*'; then
        return 0
    fi
    log_error "Refusing to use INSTALL_DIR='$d'"
    log_error "INSTALL_DIR is moved aside and 'rm -rf'd on update and uninstall, so"
    log_error "it must be a directory of ours — not an existing data directory."
    log_error "Its last path component must contain 'helixscreen'."
    log_error "Try: INSTALL_DIR=${d%/}/helixscreen sh install.sh"
    return 1
}

# Safely remove the installer's temp dir. REFUSES to delete the filesystem root,
# a mountpoint, or anything whose name isn't one of ours — a user-supplied
# TMP_DIR pointing at a mount root (e.g. `TMP_DIR=/mnt/UDISK`) once caused
# `rm -rf "$TMP_DIR"` to wipe a live data partition (printer_data + device
# userdata). Only ever removes a normal, non-mountpoint scratch directory.
_safe_remove_tmp_dir() {
    local d="$1"
    [ -n "$d" ] && [ -d "$d" ] || return 0
    if [ "$d" = "/" ]; then
        log_warn "Refusing to remove TMP_DIR='/'"
        return 0
    fi
    # Last line of defence: even if a caller skipped validate_tmp_dir, never
    # rm -rf a directory that isn't recognisably the installer's scratch space.
    if ! _user_dir_name_ok "$d" '*helixscreen-install*' '.helix-update-staging'; then
        log_warn "Refusing to rm -rf TMP_DIR='$d' (name is not an installer scratch dir); leaving it in place."
        return 0
    fi
    # Mountpoint detection: prefer mountpoint(1); else compare the device id of
    # the dir against its parent (differs at a mount boundary). BusyBox-safe.
    if command -v mountpoint >/dev/null 2>&1; then
        if mountpoint -q "$d"; then
            log_warn "Refusing to rm -rf mountpoint TMP_DIR='$d' (would wipe a live partition); leaving it in place."
            return 0
        fi
    else
        local _ddev _pdev
        _ddev=$(stat -c '%d' "$d" 2>/dev/null)
        _pdev=$(stat -c '%d' "$d/.." 2>/dev/null)
        if [ -n "$_ddev" ] && [ -n "$_pdev" ] && [ "$_ddev" != "$_pdev" ]; then
            log_warn "Refusing to rm -rf mountpoint TMP_DIR='$d' (would wipe a live partition); leaving it in place."
            return 0
        fi
    fi
    rm -rf "$d"
}

# Cleanup function for normal exit
cleanup_on_success() {
    if [ -d "$TMP_DIR" ]; then
        _safe_remove_tmp_dir "$TMP_DIR"
    fi
}

# Kill process(es) by name — SIGTERM first, then SIGKILL any survivors.
# helix-watchdog and helix-screen catch SIGTERM but don't always exit (e.g.
# during splash handoff or when blocked on I/O), so the installer must
# escalate or uninstall leaves zombie processes behind (#xxx observed on CC1).
# Works on both GNU systems and BusyBox (AD5M/K1/CC1).
# Args: process_name [process_name2 ...]
# Returns: 0 if any process was killed, 1 if none found
kill_process_by_name() {
    local killed_any=false
    local proc pids pid

    for proc in "$@"; do
        pids=$(pidof "$proc" 2>/dev/null || true)
        if [ -n "$pids" ]; then
            for pid in $pids; do
                $SUDO kill "$pid" 2>/dev/null || true
            done
            killed_any=true
        fi
    done

    [ "$killed_any" = true ] || return 1

    # Give caught-SIGTERM handlers a moment, then SIGKILL any survivors.
    sleep 1
    for proc in "$@"; do
        pids=$(pidof "$proc" 2>/dev/null || true)
        if [ -n "$pids" ]; then
            for pid in $pids; do
                $SUDO kill -9 "$pid" 2>/dev/null || true
            done
        fi
    done

    return 0
}

# Remove HelixScreen state directories that hold rolling config backups and
# update markers. These survive a normal install because they live OUTSIDE
# INSTALL_DIR by design — they need to survive Moonraker's rmtree of the
# install dir during in-app updates (see app_constants.h: Update namespace).
#
# On --uninstall and --clean the user has signaled they want a clean slate,
# so we sweep them here. Live config under printer_data/config/helixscreen/
# is handled separately by remove_config_symlink / clean_old_installation.
# The dot-prefix is the bright line: dotted = our state, undotted = live config.
#
# Paths swept (all are ours; we own these names):
#   /var/lib/helixscreen          (systemd StateDirectory=helixscreen)
#   $INSTALL_PARENT/.helixscreen  (self_restart_sentinel from helixscreen-update.service)
#   $KLIPPER_HOME/.helixscreen    (HOME-based fallback when no StateDirectory)
#   /root/.helixscreen            (always swept; service may have run as root
#                                  on a prior install regardless of current user)
#
# Reads: INSTALL_DIR, KLIPPER_HOME, SUDO,
#        HELIX_STATE_VAR_LIB (default /var/lib/helixscreen),
#        HELIX_STATE_ROOT_HOME (default /root/.helixscreen)
# Writes: (none)
clean_helix_state_dirs() {
    local install_parent
    # The two hardcoded paths are env-overrideable so the BATS suite can
    # redirect them at test-tmpdir paths instead of touching real /var/lib
    # and /root content. Production callers leave them unset.
    local state_var_lib="${HELIX_STATE_VAR_LIB:-/var/lib/helixscreen}"
    local state_root_home="${HELIX_STATE_ROOT_HOME:-/root/.helixscreen}"

    log_info "Removing HelixScreen state directories (rolling config backups)..."

    # systemd StateDirectory= target
    if [ -d "$state_var_lib" ]; then
        $SUDO rm -rf "$state_var_lib"
        log_success "Removed $state_var_lib"
    fi

    # $INSTALL_PARENT/.helixscreen — self_restart_sentinel (helixscreen-update.service).
    # Skip when INSTALL_DIR is unset or dirname would resolve to "/" or "." (defensive
    # against misuse from outside our normal install flow; current HELIX_INSTALL_DIRS
    # all have a real parent dir).
    if [ -n "${INSTALL_DIR:-}" ]; then
        install_parent=$(dirname "$INSTALL_DIR")
        case "$install_parent" in
            /|.|"") : ;;
            *)
                if [ -d "${install_parent}/.helixscreen" ]; then
                    $SUDO rm -rf "${install_parent}/.helixscreen"
                    log_success "Removed ${install_parent}/.helixscreen"
                fi
                ;;
        esac
    fi

    # Service user's $HOME/.helixscreen (fallback backup path)
    if [ -n "${KLIPPER_HOME:-}" ] && [ -d "${KLIPPER_HOME}/.helixscreen" ]; then
        $SUDO rm -rf "${KLIPPER_HOME}/.helixscreen"
        log_success "Removed ${KLIPPER_HOME}/.helixscreen"
    fi

    # /root/.helixscreen — always sweep. The directory name is ours (dot-prefix rule);
    # rm -rf on a missing dir is a no-op, and this catches platforms where the service
    # historically ran as root even if KLIPPER_HOME has since moved off /root.
    if [ -d "$state_root_home" ]; then
        $SUDO rm -rf "$state_root_home"
        log_success "Removed $state_root_home"
    fi
}

# Print post-install commands for the user
# Reads: INIT_SYSTEM, SERVICE_NAME, INIT_SCRIPT_DEST, INSTALL_DIR
print_post_install_commands() {
    echo "Useful commands:"
    if [ "$INIT_SYSTEM" = "systemd" ]; then
        # journalctl and restart need privilege: a service user outside adm/
        # systemd-journal gets "No journal files were found" on stderr and an
        # empty stdout, which reads as "there are no logs" when redirected.
        echo "  systemctl status ${SERVICE_NAME}         # Check status"
        echo "  sudo journalctl -u ${SERVICE_NAME} -f    # View logs"
        echo "  sudo systemctl restart ${SERVICE_NAME}   # Restart"
    else
        # helixscreen.init writes to /var/log/helixscreen/launcher.log when /var/log
        # is persistent, else ${INSTALL_DIR}/logs/launcher.log — show whichever exists.
        local log_path="/var/log/helixscreen/launcher.log"
        [ -f "$log_path" ] || log_path="${INSTALL_DIR}/logs/launcher.log"
        echo "  ${INIT_SCRIPT_DEST} status   # Check status"
        echo "  tail -f ${log_path}   # View logs"
        echo "  ${INIT_SCRIPT_DEST} restart  # Restart"
    fi
}
