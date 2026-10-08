#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# helix-launcher.sh - Launch HelixScreen with watchdog supervision
#
# When watchdog is available (embedded targets), it manages the splash screen
# lifecycle and provides crash recovery. Otherwise, launches helix-screen directly.
#
# NOTE: Written for POSIX sh compatibility (no bash arrays) to work on AD5M BusyBox.
#
# Usage:
#   ./helix-launcher.sh [options]
#
# Launcher-specific options:
#   --debug              Enable debug-level logging (-vv)
#   --log-level=<level>  Log level: trace, debug, info, warn, error, critical, off
#   --log-dest=<dest>    Log destination: auto, journal, syslog, file, console
#   --log-file=<path>    Log file path (when --log-dest=file)
#   --print-env NAME     Print NAME as the env-file read resolves it and exit
#                       (query mode for callers that launch this script later;
#                       no display side effects, no daemon)
#
# Environment variables:
#   HELIX_DATA_DIR=<d>   Override asset directory (ui_xml/, assets/, config/)
#   HELIX_LOG_LEVEL=<l>  Log level (preferred over HELIX_DEBUG)
#   HELIX_DEBUG=1        Same as --debug (legacy, use HELIX_LOG_LEVEL instead)
#   HELIX_LOG_DEST=<d>   Same as --log-dest (auto|journal|syslog|file|console)
#   HELIX_LOG_FILE=<f>   Same as --log-file
#
# Exported to helix-screen, never read from the environment
# (prestonbrown/helixscreen#1712): HELIX_ENV_FILE_REFUSED (whole-file
# refusal: "kind|detail|expected|path") and HELIX_ENV_LINES_SKIPPED
# ("label:reason" entries joined by '|', capped). The app turns them into
# startup notifications; see docs/devel/ENVIRONMENT_VARIABLES.md.
#
# All other options are passed through to helix-screen.
#
# Logging behavior:
#   - Destination defaults to "auto", which helix-screen resolves with
#     detect_best_target(): systemd journal when /run/systemd/journal/socket
#     exists, otherwise syslog on Linux, console only on macOS. Auto NEVER
#     resolves to a file.
#   - Most embedded targets therefore do not use auto: their platform hook
#     exports HELIX_LOG_DEST=file + HELIX_LOG_FILE from platform_pre_start,
#     which this script reads (after sourcing the hooks) and forwards.
#   - An interactive run additionally gets a stdout console sink on top of the
#     resolved target, because stdout is a TTY — the target itself is unchanged.
#   - Use --log-dest=file --log-file=/path for explicit file logging
#
# Installation:
#   Copy to /opt/helixscreen/bin/ or similar
#   Make executable: chmod +x helix-launcher.sh
#   Use with systemd service: config/helixscreen.service

set -e

# Co-hosted-with-Klipper detection — used below (just before exec) to decide
# whether to nice the UI down and hand helix-screen a higher OOM score. Defined
# here, called late, so platform hooks and the init script's
# platform_wait_for_services have had time to bring Klipper / Moonraker up
# before we look for them.
#
# Reads /proc/<pid>/cmdline directly instead of shelling out to pgrep. pgrep is
# absent entirely on some BusyBox rootfs — Forge-X on the AD5M ships none — and
# where BusyBox does provide it, `-f` matching is unreliable. A socket probe
# alone is not enough either: Forge-X's klippy socket is /tmp/uds and its
# Moonraker has none, so the socket checks below are only a fallback.
#
# Reading the files also avoids the pgrep self-match trap: nothing is spawned
# with the search pattern on its own command line.
#
# HELIX_PROC_ROOT exists so the tests can point the scan at a fixture tree.
: "${HELIX_PROC_ROOT:=/proc}"
helix_klipper_co_hosted() {
    # Patterns are deliberately narrow. A bare *moonraker* would also match a
    # standalone kiosk started as `helix-screen --moonraker ws://host:7125`,
    # which is precisely the not-co-hosted case that must stay at nice 0.
    for _hkc_f in "$HELIX_PROC_ROOT"/[0-9]*/cmdline; do
        [ -r "$_hkc_f" ] || continue
        # argv is NUL-separated; fold to spaces for substring matching. A
        # process that exits mid-scan makes the open fail — that is not an
        # error. The stderr redirect must precede `<`: redirections apply left
        # to right, so the shell's own "cannot open" would otherwise leak.
        _hkc_cmd=$(tr '\0' ' ' 2>/dev/null < "$_hkc_f") || _hkc_cmd=""
        case " ${_hkc_cmd} " in
            *klippy.py*|*moonraker.py*|*moonraker-env*|*" -m moonraker"*)
                unset _hkc_f _hkc_cmd
                return 0
                ;;
        esac
    done
    unset _hkc_f _hkc_cmd

    # Fallback for hosts where /proc is unreadable or Klipper lives in another
    # PID namespace. /tmp/uds is Forge-X's klippy socket on the AD5M.
    [ -S /tmp/klippy_uds ]      && return 0
    [ -S /tmp/moonraker.sock ]  && return 0
    [ -S /tmp/uds ]             && return 0
    return 1
}

# Log function. Defined before the first log site so every launcher line,
# including the env-file parse warnings below, gets the same treatment.
#
# Uses stderr to avoid polluting stdout which could be captured unexpectedly.
#
# Every line is stamped with wall-clock time. launcher.log is the only record
# of the wrapper's own output and of crash stderr (glibc aborts,
# std::terminate); spdlog is already dead by then, so none of that reaches the
# app log. Without a timestamp its lines cannot be correlated to anything else
# on the machine (a Klipper macro, a print, a calibration run). The format is
# deliberately plain
# `date` with %Y-%m-%d %H:%M:%S — the BusyBox date on AD5M/K1/CC1/SonicPad has
# no -I / --rfc-3339 / %N. If date is missing entirely, log without the stamp
# rather than aborting the launcher under `set -e`.
log() {
    _log_ts=$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || true)
    if [ -n "$_log_ts" ]; then
        echo "[$_log_ts] [helix-launcher] $*" >&2
    else
        echo "[helix-launcher] $*" >&2
    fi
}

# Determine script and binary locations
# Use $0 instead of BASH_SOURCE for POSIX compatibility
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Support installed and development layouts
# Installed: launcher is in bin/ alongside binaries
# Development: launcher in scripts/, binaries in build/bin/
if [ -x "${SCRIPT_DIR}/helix-screen" ]; then
    # Installed: binaries in same directory as launcher (bin/)
    BIN_DIR="${SCRIPT_DIR}"
elif [ -x "${SCRIPT_DIR}/../build/bin/helix-screen" ]; then
    # Development: launcher in scripts/, binaries in build/bin/
    BIN_DIR="${SCRIPT_DIR}/../build/bin"
else
    echo "Error: Cannot find helix-screen binary" >&2
    echo "Looked in: ${SCRIPT_DIR} and ${SCRIPT_DIR}/../build/bin" >&2
    exit 1
fi

# Derive the install root (parent of bin/)
INSTALL_DIR="$(cd "${BIN_DIR}/.." && pwd)"

# Source environment configuration file if present.
# Supports both installed (/etc/helixscreen/) and deployed (config/) locations.
# Variables already set in the environment take precedence — the env file only
# provides defaults for unset variables.
#
# One implementation, called by this launcher's own startup below and by the
# --print-env query above it: any caller that needs the launcher's resolution
# for a variable before exec'ing this script asks for it here rather than
# forking a second parser of helixscreen.env (prestonbrown/helixscreen#1634).
#
# Values are literal text, never shell code: one pair of matching surrounding
# quotes is stripped, and $VAR, $(...) and backticks stay as typed. Only the
# keys helix_env_key_allowed accepts are exported, because this file reaches
# the environment of root on every SysV firmware device and a key like
# LD_PRELOAD or PATH, or a HELIX_* key a platform hook runs as a program,
# would still be code execution.
#
# Trust gate, as defence in depth under that: the file is read only when it is
# owned by root or by this launcher's own user, with no group or world write
# bit. One more owner is trusted: the owner of the directory a symlinked env
# file resolves into, when that directory has no group or world write bit, the
# link sits in a directory owned by root or this user with none either, and
# the link points straight at the file rather than through another link. That
# is the printer_data layout, where the file must stay editable from Mainsail
# and Fluidd: Moonraker saves an edit as its own user (lava on the Snapmaker
# U1) while the launcher runs as root, and only root or this user could have
# aimed the link there. A file with a trusted owner whose only fault is a
# write bit is repaired to 0644 in place and loaded: web updates and deploys
# ship the file without pinning it, and refusing there would blank settings
# on every update. A refused file is skipped with a warning, and startup
# continues on built-in defaults: the env file only ever supplies defaults,
# and a display that must come up beats a config file. A file whose owner or
# mode cannot be read is refused for the same reason - unverifiable is not
# trusted.
#
# Both stat spellings dereference symlinks (-L): on the per-file installs the
# env file is a symlink into printer_data/config, and stat without -L reports
# the link itself, always mode 777, which the write-bit mask below would refuse
# on every boot. -c is the GNU/BusyBox spelling (the same form every installer
# platform probe uses on the devices we ship to); BSD stat (macOS dev runs) is
# tried second, %p because the file type bits it includes do not intersect the
# write-bit mask below.
helix_env_stat() {
    stat -L -c '%u %a' "$1" 2>/dev/null && return 0
    stat -L -f '%u %p' "$1" 2>/dev/null && return 0
    return 0
}

# Hard-link count (st_nlink) of an existing path, for the log-file gate.
helix_env_nlink() {
    stat -L -c '%h' "$1" 2>/dev/null && return 0
    stat -L -f '%l' "$1" 2>/dev/null && return 0
    return 0
}

# Keys the env file may set: exactly the settings it is meant to carry - every
# key config/helixscreen.env names (a bats lint keeps the two in step), the
# env-file settings the user docs list, the keys deploys and the init script
# write or query, and the glibc knobs the heap-diagnostic and arena blocks
# document as overrides. A positive list on purpose: directory keys such as
# HELIX_DATA_DIR (the app chdirs there and dlopens plugins from it) and
# HELIX_CONFIG_DIR stay off it, as does every other name (LD_*, PATH, IFS,
# HOME, SHELL, ENV, BASH_ENV, PYTHON*, ...).
helix_env_key_allowed() {
    case "$1" in
        HELIX_ALSA_DEVICE | HELIX_AUTO_QUIT_MS | HELIX_AUTO_SCREENSHOT | \
            HELIX_BACKLIGHT_DEVICE | HELIX_COLOR_SWAP_RB | HELIX_DEBUG | \
            HELIX_DEBUG_TOUCH | HELIX_DIAGNOSTIC_UPLOADS | \
            HELIX_DISABLE_AUTO_UPDATES | HELIX_DISPLAY_BACKEND | \
            HELIX_DISPLAY_ROTATION | HELIX_DPI | HELIX_DRM_DEVICE | \
            HELIX_FB_DEVICE | HELIX_GCODE_MODE | \
            HELIX_GCODE_STREAMING | HELIX_KEYBOARD_DEVICE | HELIX_LOG_DEST | \
            HELIX_LOG_FILE | HELIX_LOG_LEVEL | HELIX_MOUSE_DEVICE | \
            HELIX_NICE | HELIX_NO_SPLASH | HELIX_PWM_SOUND | HELIX_REMOTE_CONTROL | \
            HELIX_REMOTE_HTTP_TOKEN | HELIX_REMOTE_SOCKET | \
            HELIX_REQUIRE_POINTER | HELIX_SCREEN_SIZE | HELIX_SCROLL_GUARD | \
            HELIX_SCROLL_GUARD_COOLDOWN_MS | HELIX_SKIP_SPLASH | HELIX_SSAO | \
            HELIX_THEME | HELIX_TOUCH_CALIBRATE | HELIX_TOUCH_DEVICE | \
            HELIX_TOUCH_SWAP_AXES | HELIX_USB_AUTOMOUNT | \
            MALLOC_ARENA_MAX | MALLOC_CHECK_ | MALLOC_PERTURB_ | \
            MOONRAKER_HOST | MOONRAKER_PORT)
            return 0
            ;;
    esac
    return 1
}

# Why KEY's literal VALUE is refused, printed; nothing (and status 1) when it
# is fine. Shell syntax is refused for every key: the file does not run it,
# and exporting it as text would turn a line like `$(openssl rand -hex 16)`
# into a secret anyone can read here. The per-key rules cover values root acts
# on: the launcher splices HELIX_DPI, HELIX_LOG_*, and HELIX_REMOTE_SOCKET
# unquoted into the app's command line, the app writes the log file as root
# (and echoes other settings into it, so a log aimed at a script is code), and
# ALSA's file plugin popen()s a `|cmd` target.
helix_env_value_refusal() {
    case "$2" in
        *'$('* | *'`'* | *'${'* | *'$'[A-Za-z_]*)
            echo "holds shell syntax (\$VAR, \$(...), \${...} or a backtick), which this file does not run - write the final value itself"
            return 0
            ;;
    esac
    case "$1" in
        HELIX_DPI | HELIX_LOG_DEST | HELIX_LOG_FILE | HELIX_LOG_LEVEL | HELIX_REMOTE_SOCKET)
            case "$2" in
                *[' 	*?[']*)
                    echo "may not contain whitespace or * ? ["
                    return 0
                    ;;
            esac
            ;;
    esac
    case "$1" in
        HELIX_LOG_FILE)
            helix_env_log_file_ok "$2" && return 1
            echo "must be a *.log file under /tmp/, /var/log/ or ${INSTALL_DIR:-the install dir}/, with no .. and not a symlink"
            return 0
            ;;
        HELIX_REMOTE_SOCKET)
            case "$2" in
                */../* | */.. | */./*) ;;
                /tmp/?* | /run/?*) return 1 ;;
            esac
            echo "must be a path under /tmp/ or /run/ with no .."
            return 0
            ;;
        HELIX_NICE)
            case "$2" in
                [0-9] | 1[0-9]) return 1 ;;
            esac
            echo "must be 0-19 (a negative nice would let the UI starve Klipper)"
            return 0
            ;;
        HELIX_PWM_SOUND)
            case "$2" in
                *[!0-9:]* | *:*:* | :* | *:) ;;
                *:*) return 1 ;;
            esac
            echo "must be <chip>:<channel>, e.g. 0:0"
            return 0
            ;;
        HELIX_ALSA_DEVICE)
            case "$2" in
                *'|'* | *file* | *tee*) ;;
                default | sysdefault | sysdefault:* | hw:* | plughw:* | dmix:*) return 1 ;;
            esac
            echo "must be default, sysdefault[:...], hw:..., plughw:... or dmix:..."
            return 0
            ;;
    esac
    return 1
}

# The only log files the env file may aim the app at: an absolute *.log path
# whose parent resolves to /tmp, /var/log, or a directory under those or the
# install dir that root or this user owns with no group/world write bit, with
# no dot segments, no symlink at the file itself, and - when the file exists -
# a single-link regular file owned by root or this user. Platform hooks pick
# their own firmware log directories after this file loads, so nothing else
# is needed.
# A link planted in /tmp after this check still races the app's
# open; fs.protected_symlinks closes that on the kernels we ship to.
helix_env_log_file_ok() {
    case "$1" in
        /*.log) ;;
        *) return 1 ;;
    esac
    case "$1/" in
        */../* | */./*) return 1 ;;
    esac
    [ -L "$1" ] && return 1
    # An existing file must be a regular file with no other hard link, owned
    # by root or this user: a planted hard link has the app append to an
    # arbitrary file as root where fs.protected_hardlinks=0.
    if [ -e "$1" ]; then
        [ -f "$1" ] || return 1
        _helf_nlink=$(helix_env_nlink "$1")
        case "$_helf_nlink" in
            *[!0-9]* | '') return 1 ;;
            *) [ "$_helf_nlink" -gt 1 ] && return 1 ;;
        esac
        _helf_uid=$(id -u 2>/dev/null) || _helf_uid=""
        [ -n "$_helf_uid" ] || return 1
        _helf_fst=$(helix_env_stat "$1")
        case "${_helf_fst%% *}" in
            0 | "$_helf_uid") ;;
            *) return 1 ;;
        esac
    fi
    _helf_dir=$(readlink -f "${1%/*}" 2>/dev/null) || _helf_dir=""
    _helf_inst=$(readlink -f "${INSTALL_DIR:-/nonexistent}" 2>/dev/null) || _helf_inst=""
    _helf_ok=1
    case "$_helf_dir/" in
        /tmp/ | /var/log/) _helf_ok=0 ;;
        /tmp/* | /var/log/*) _helf_ok=2 ;;
        "$_helf_inst"/*) [ -n "$_helf_inst" ] && [ -n "$_helf_dir" ] && _helf_ok=2 ;;
    esac
    # A subdirectory must be one nobody else can swap for a symlink after
    # this check: owned by root or this user, with no group/world write bit.
    if [ "$_helf_ok" = "2" ]; then
        _helf_ok=1
        _helf_st=$(helix_env_stat "$_helf_dir")
        _helf_uid=$(id -u 2>/dev/null) || _helf_uid=""
        case "${_helf_st##* }" in
            '' | *[!0-9]*) ;;
            *)
                case "${_helf_st%% *}" in
                    0 | "$_helf_uid")
                        [ -n "$_helf_uid" ] && [ "$((0${_helf_st##* } & 022))" = "0" ] && _helf_ok=0
                        ;;
                esac
                ;;
        esac
    fi
    unset _helf_dir _helf_inst _helf_st _helf_uid _helf_nlink _helf_fst
    return $_helf_ok
}

# A POSIX identifier: the one rule for names from the file and --print-env.
helix_env_is_name() {
    case "$1" in
        '' | [!A-Za-z_]* | *[!A-Za-z0-9_]*) return 1 ;;
    esac
    return 0
}

# Owner uid of REAL_DIR when it may hold the env file: REAL_DIR has no group
# or world write bit, and LINK_DIR (where the symlink lives) is owned by root
# or UID with none either. Prints 0 (root, trusted anyway) otherwise.
# Judges the two directories only, not every ancestor; walk the
# ancestors if the env file ever becomes code again.
helix_env_dir_owner() {
    _hed_link=$(helix_env_stat "$2")
    _hed_real=$(helix_env_stat "$1")
    _hed_out=0
    case "${_hed_link%% *}" in
        0 | "$3")
            case "${_hed_link##* }${_hed_real##* }" in
                '' | *[!0-9]*) ;;
                *)
                    if [ -n "$_hed_real" ] &&
                        [ "$((0${_hed_link##* } & 022))" = "0" ] &&
                        [ "$((0${_hed_real##* } & 022))" = "0" ]; then
                        _hed_out="${_hed_real%% *}"
                    fi
                    ;;
            esac
            ;;
    esac
    echo "$_hed_out"
    unset _hed_link _hed_real _hed_out
}

# Refuse the env file: log the detailed reason with the full fix, and hand a
# structured record to the app (prestonbrown/helixscreen#1712) so the app
# words the on-screen warning; the launcher's longer hint stays in the log.
# Callers set _hef_kind (mode|owner|chain|other), _hef_detail (the offending
# uid for owner, a short reason for other) and _hef_file (the path as
# configured). The kind decides which fields the export carries, normalized
# here so no refusal site has to clear a field: mode keeps neither middle,
# owner keeps both (detail uid + _hef_expected chown target), other and chain
# keep only the detail. kind|detail|expected|path, every field always present.
helix_env_refuse() {
    log "warning: $1 - env file skipped ($_hef_fix)"
    case "$_hef_kind" in
        owner) ;;
        mode) _hef_detail=""; _hef_expected="" ;;
        *) _hef_expected="" ;;
    esac
    export HELIX_ENV_FILE_REFUSED="$_hef_kind|$_hef_detail|$_hef_expected|$_hef_file"
    helix_env_trust_cleanup
}

# Record one skipped line for the app (prestonbrown/helixscreen#1712):
# append "label:reason" to _helix_skipped, which helix_load_env_file exports
# as HELIX_ENV_LINES_SKIPPED once the parse finishes. Labels are the variable
# name, or "line N" for a line with no parsable key; reasons are fixed
# sentences that never contain the '|' separator. Past the cap one sentinel
# entry ("more skipped") closes the list so the app can say "at least N"
# instead of naming N as the whole story; its label has a space, so no
# variable name or "line N" label can collide with it.
HELIX_ENV_SKIP_CAP=12
helix_env_note_skip() {
    if [ "$_hes_count" -ge "$HELIX_ENV_SKIP_CAP" ]; then
        if [ "$_hes_more" != 1 ]; then
            _helix_skipped="${_helix_skipped}|more skipped:not every skipped line is listed"
            _hes_more=1
        fi
        return 0
    fi
    _helix_skipped="${_helix_skipped}${_helix_skipped:+|}$1"
    _hes_count=$((_hes_count + 1))
}

helix_env_file_trusted() {
    _hef_file="$1"
    _hef_kind=other
    _hef_detail=""
    _hef_expected=""
    _hef_real=$(readlink -f "$1" 2>/dev/null) || _hef_real="$1"
    [ -n "$_hef_real" ] || _hef_real="$1"
    _hef_uid=$(id -u 2>/dev/null) || _hef_uid=""
    _hef_dir_owner=0
    if [ -L "$1" ] && [ -n "$_hef_uid" ]; then
        # Only a link aimed straight at the file: a chain would let whoever
        # owns the middle link choose which file this is.
        _hef_hop=$(readlink "$1" 2>/dev/null) || _hef_hop=""
        case "$_hef_hop" in
            '') ;;
            /*) ;;
            *) _hef_hop="${1%/*}/$_hef_hop" ;;
        esac
        if [ -n "$_hef_hop" ] && [ ! -L "$_hef_hop" ]; then
            _hef_dir_owner=$(helix_env_dir_owner "${_hef_real%/*}" "${1%/*}" "$_hef_uid")
        fi
    fi
    if [ "$_hef_dir_owner" = "0" ]; then
        _hef_expected="root:root"
    else
        _hef_expected="$_hef_dir_owner"
    fi
    _hef_fix="fix: chown $_hef_expected $_hef_real && chmod 644 $_hef_real"
    _hef_stat=$(helix_env_stat "$1")
    if [ -z "$_hef_stat" ] || [ -z "$_hef_uid" ]; then
        _hef_detail="its owner or permissions could not be read"
        helix_env_refuse "cannot determine owner/mode of $1"
        return 1
    fi
    _hef_owner="${_hef_stat%% *}"
    _hef_mode="${_hef_stat##* }"
    if [ "$_hef_owner" != "0" ] && [ "$_hef_owner" != "$_hef_uid" ] &&
        [ "$_hef_owner" != "$_hef_dir_owner" ]; then
        _hef_kind=owner
        _hef_detail="$_hef_owner"
        helix_env_refuse "$1 is owned by uid $_hef_owner, not root, this user (uid $_hef_uid) or the owner of its directory"
        return 1
    fi
    case "$_hef_mode" in
        '' | *[!0-9]*)
            _hef_detail="its permissions could not be read"
            helix_env_refuse "unreadable mode '$_hef_mode' on $1"
            return 1
            ;;
    esac
    # 0-prefixed so the digits read as the octal stat reported them; 022 is
    # the group+world write bits. Setuid/setgid digits are unaffected by the
    # mask, so a 4755 file is judged on its 755 alone.
    if [ "$((0$_hef_mode & 022))" != "0" ]; then
        # The owner already passed the check above, so the write bits are a
        # shipping fault, not an attack: tighten the mode on the symlink's
        # target and load the file if that settles it. The re-stat stays
        # fail-closed - a chmod that reports success without taking effect
        # still refuses the file.
        if chmod 0644 "$_hef_real" 2>/dev/null; then
            _hef_again=$(helix_env_stat "$_hef_real")
            _hef_again_mode="${_hef_again##* }"
            case "$_hef_again_mode" in
                '' | *[!0-9]*) ;;
                *)
                    if [ "$((0$_hef_again_mode & 022))" = "0" ]; then
                        log "repaired $_hef_real from mode $_hef_mode to 0644 - env file loaded"
                        helix_env_trust_cleanup
                        return 0
                    fi
                    ;;
            esac
        fi
        _hef_kind=mode
        helix_env_refuse "$1 (uid $_hef_owner, mode $_hef_mode) is group- or world-writable"
        return 1
    fi
    helix_env_trust_cleanup
    return 0
}

helix_env_trust_cleanup() {
    unset _hef_stat _hef_uid _hef_owner _hef_mode _hef_real _hef_dir_owner \
        _hef_hop _hef_fix _hef_again _hef_again_mode _hef_kind _hef_detail \
        _hef_expected _hef_file
}

# Literal value of a `KEY=value` line's right-hand side: one pair of matching
# surrounding quotes stripped, anything after a closing quote allowed only as
# a `# comment`, and an unquoted value cut at the first whitespace-led `#`.
# Fails (prints nothing) on an unterminated quote or text after one.
helix_env_value() {
    _hev_v="$1"
    case "$_hev_v" in
        \"*) _hev_q='"' ;;
        \'*) _hev_q="'" ;;
        *)
            _hev_v="${_hev_v%%[ 	]#*}"
            _hev_v="${_hev_v%"${_hev_v##*[! 	]}"}"
            printf '%s' "$_hev_v"
            unset _hev_v
            return 0
            ;;
    esac
    _hev_rest="${_hev_v#?}"
    _hev_ok=0
    case "$_hev_rest" in
        *"$_hev_q"*)
            _hev_after="${_hev_rest#*"$_hev_q"}"
            _hev_trim="${_hev_after#"${_hev_after%%[! 	]*}"}"
            case "$_hev_after" in
                '') _hev_ok=1 ;;
                [' 	']*)
                    case "$_hev_trim" in
                        '' | '#'*) _hev_ok=1 ;;
                    esac
                    ;;
            esac
            ;;
    esac
    [ "$_hev_ok" = "1" ] && printf '%s' "${_hev_rest%%"$_hev_q"*}"
    unset _hev_v _hev_q _hev_rest _hev_after _hev_trim
    [ "$_hev_ok" = "1" ]
    _hev_rc=$?
    unset _hev_ok
    return $_hev_rc
}

helix_load_env_file() {
    # The two handoff variables are launcher-authoritative: an ambient value
    # (systemd Environment=, an operator shell) must not fabricate a refusal
    # the launcher never made.
    unset HELIX_ENV_FILE_REFUSED HELIX_ENV_LINES_SKIPPED
    _helix_env_file=""
    for _env_path in \
        "${INSTALL_DIR}/config/helixscreen.env" \
        /etc/helixscreen/helixscreen.env; do
        if [ -f "$_env_path" ]; then
            _helix_env_file="$_env_path"
            break
        fi
    done
    unset _env_path

    [ -n "$_helix_env_file" ] || return 0

    if ! helix_env_file_trusted "$_helix_env_file"; then
        unset _helix_env_file
        return 0
    fi

    # Read each VAR=value line; only export if not already set.
    # Tolerant of common typos so users don't get a silent no-op:
    #   - CRLF line endings (env file edited on Windows)
    #   - Leading/trailing whitespace
    #   - `export VAR=value` (bash habit)
    #   - `VAR = value` (spaces around the equals sign)
    # Malformed lines emit a stderr warning instead of being dropped silently.
    _lineno=0
    _helix_file_set=""
    _helix_refused=""
    _helix_skipped=""
    _hes_count=0
    _hes_more=0
    while IFS= read -r _line || [ -n "$_line" ]; do
        _lineno=$((_lineno + 1))
        # Normalize: strip CR, trim whitespace, drop optional `export ` prefix.
        # Literal spaces+tabs in the bracket classes are deliberate (POSIX
        # `[:space:]` is unreliable in busybox sed shipped on AD5X/K1/SonicPad).
        _line=$(printf '%s' "$_line" | sed -e 's/\r$//' \
                                            -e 's/^[ 	]*//' \
                                            -e 's/[ 	]*$//' \
                                            -e 's/^export[ 	][ 	]*//')
        case "$_line" in
            '#'*|'') continue ;;
        esac
        # Require KEY=value with a valid POSIX identifier on the LHS.
        case "$_line" in
            [A-Za-z_]*=*) ;;
            *)
                log "warning: ${_helix_env_file}:${_lineno}: ignored malformed line: $_line"
                helix_env_note_skip "line ${_lineno}:malformed line"
                continue
                ;;
        esac
        _var="${_line%%=*}"
        if ! helix_env_is_name "$_var"; then
            log "warning: ${_helix_env_file}:${_lineno}: invalid variable name '$_var'"
            helix_env_note_skip "line ${_lineno}:invalid variable name"
            continue
        fi
        if ! helix_env_key_allowed "$_var"; then
            case " ${_helix_refused} " in
                *" $_var "*) ;;
                *)
                    log "warning: ${_helix_env_file}:${_lineno}: $_var is not a setting this file may change - ignored"
                    _helix_refused="${_helix_refused}${_helix_refused:+ }$_var"
                    helix_env_note_skip "${_var}:not a setting this file may change"
                    ;;
            esac
            continue
        fi
        if ! _val=$(helix_env_value "${_line#*=}"); then
            log "warning: ${_helix_env_file}:${_lineno}: unterminated quote or text after the closing quote: $_line"
            helix_env_note_skip "${_var}:unterminated quote"
            continue
        fi
        if _why=$(helix_env_value_refusal "$_var" "$_val"); then
            log "warning: ${_helix_env_file}:${_lineno}: $_var $_why; ignored"
            helix_env_note_skip "${_var}:${_why}"
            continue
        fi
        # Only set if not already in environment (systemd Environment= /
        # exported parent shell vars win over the file). The eval splices in
        # only the NAME, already checked as an identifier above, never the
        # value; it reads the shell's own variable without forking a helper.
        eval "_existing=\"\${${_var}:-}\""
        if [ -z "$_existing" ]; then
            if ! export "$_var=$_val" 2>/dev/null; then
                log "warning: ${_helix_env_file}:${_lineno}: failed to export: $_line"
                helix_env_note_skip "${_var}:could not be exported"
            else
                case " ${_helix_file_set} " in
                    *" $_var "*) ;;
                    *) _helix_file_set="${_helix_file_set}${_helix_file_set:+ }$_var" ;;
                esac
            fi
        elif [ "${HELIX_DEBUG:-0}" = "1" ]; then
            # DEBUG_MODE is derived later in this script; the raw variable
            # is all that exists at parse time. The note names the winning
            # source: a duplicate key earlier in this file, or the parent
            # environment.
            case " ${_helix_file_set} " in
                *" $_var "*)
                    log "note: ${_helix_env_file}:${_lineno}: $_var already set by an earlier line of this file; this value ignored"
                    ;;
                *)
                    log "note: ${_helix_env_file}:${_lineno}: $_var already set in environment; file value ignored"
                    ;;
            esac
        fi
    done < "$_helix_env_file"
    if [ -n "$_helix_skipped" ]; then
        export HELIX_ENV_LINES_SKIPPED="$_helix_skipped"
    fi
    unset _line _var _val _why _existing _lineno _helix_file_set _helix_refused \
        _helix_skipped _hes_count _hes_more _helix_env_file
}

# --print-env NAME: resolve NAME exactly as the env-file read resolves it
# (shell environment first, then the file, first definition wins) and print
# the value, exiting before any other launcher work — no display side
# effects, no daemon. The init script's early-splash gate uses this so one
# parser serves every reader of helixscreen.env. NAME must be a plain
# identifier, because the lookup below splices it into an eval.
if [ "${1:-}" = "--print-env" ]; then
    if [ "$#" -ne 2 ]; then
        echo "usage: $0 --print-env NAME" >&2
        exit 2
    fi
    if ! helix_env_is_name "$2"; then
        echo "$0: --print-env: not a variable name: $2" >&2
        exit 2
    fi
    helix_load_env_file
    eval "printf '%s\n' \"\${$2:-}\""
    exit 0
fi

# Stop firmware display-management services that conflict with HelixScreen.
# Creality SonicPad/Nebula Pad ships display-sleep.sh which polls X11 DPMS via
# xset. When X isn't running (fbdev mode), xset fails and the script interprets
# the empty response as "monitor Off", killing the backlight every 2 seconds.
# Only a host where the unit is running is asked to stop it: is-active is a
# read, while a stop sent to a host without it still asks polkit for rights.
if command -v systemctl >/dev/null 2>&1 &&
    systemctl is-active -q display-sleep.service 2>/dev/null; then
    systemctl stop display-sleep.service 2>/dev/null || true
fi
killall display-sleep.sh 2>/dev/null || true

# Hide the Linux console text cursor (visible as a blinking block on fbdev)
# The redirect target may exist but be unwritable, and the shell reports that
# failure on whatever stderr is current when it *opens* the target — a trailing
# 2>/dev/null is applied too late to catch it. Wrap in a group so stderr is
# already silenced before the inner redirect is attempted.
{ setterm --cursor off || printf '\033[?25l' > /dev/tty1; } 2>/dev/null || true

# Unbind the kernel console from the framebuffer so it doesn't paint text
# over the UI. This affects vtcon1 (the fbcon driver); vtcon0 is the dummy.
for vtcon in /sys/class/vtconsole/vtcon*/bind; do
    { [ -f "$vtcon" ] && echo 0 > "$vtcon"; } 2>/dev/null || true
done

# Parse launcher-specific arguments (POSIX-compatible, no arrays)
# Passthrough args stored as space-separated string
# CLI flags take priority over env vars; env vars are applied after env file sourcing below
PASSTHROUGH_ARGS=""
CLI_DEBUG=""
CLI_LOG_DEST=""
CLI_LOG_FILE=""
CLI_LOG_LEVEL=""
for arg in "$@"; do
    case "$arg" in
        --debug)
            CLI_DEBUG=1
            ;;
        --log-dest=*)
            CLI_LOG_DEST="${arg#--log-dest=}"
            ;;
        --log-file=*)
            CLI_LOG_FILE="${arg#--log-file=}"
            ;;
        --log-level=*)
            CLI_LOG_LEVEL="${arg#--log-level=}"
            ;;
        *)
            PASSTHROUGH_ARGS="${PASSTHROUGH_ARGS} ${arg}"
            ;;
    esac
done

# True when every shared library a binary needs resolves on this system.
# A GPU-linked binary on a board whose userspace has no Mesa presents exactly
# this way, and it is cheaper to detect than a failed exec.
libs_resolve() {
    command -v ldd >/dev/null 2>&1 || return 0
    ! ldd "$1" 2>/dev/null | grep -q "not found"
}

# Ask a binary whether EGL comes up on a real GPU here. Exit 0 means yes.
# Run as a separate process so a GPU bring-up that aborts cannot take the UI
# down with it, and under a deadline so a wedged driver cannot hold up boot.
probe_egl() {
    if command -v timeout >/dev/null 2>&1; then
        _pe_out=$(timeout 10 "$1" --probe-egl 2>&1)
    else
        _pe_out=$("$1" --probe-egl 2>&1)
    fi
    _pe_rc=$?
    if [ -n "$_pe_out" ]; then
        log "EGL probe: $_pe_out"
    fi
    return $_pe_rc
}

# Select the display binary: EGL (GPU presentation), DRM dumb buffers, fbdev.
# Checks: env override → shared lib resolution → EGL probe → default to DRM.
#
# The EGL rung is taken only when the probe reports a hardware renderer.
# Succeeding into a software rasterizer such as llvmpipe is worse than failing,
# because it spends CPU to save CPU.
select_binary() {
    _sb_bin_dir=$1
    _sb_primary="${_sb_bin_dir}/helix-screen"
    _sb_fallback="${_sb_bin_dir}/helix-screen-fbdev"
    _sb_egl="${_sb_bin_dir}/helix-screen-egl"

    # A forced backend short-circuits every check below. Naming a rung whose
    # binary this install does not carry falls through to normal selection.
    case "${HELIX_DISPLAY_BACKEND:-}" in
        fbdev)
            if [ -x "$_sb_fallback" ]; then
                echo "$_sb_fallback"
                return
            fi
            ;;
        egl)
            if [ -x "$_sb_egl" ]; then
                echo "$_sb_egl"
                return
            fi
            log "HELIX_DISPLAY_BACKEND=egl but no helix-screen-egl is installed"
            ;;
        drm)
            echo "$_sb_primary"
            return
            ;;
    esac

    # Top rung: GPU presentation, gated on probing the very binary we would run.
    if [ -x "$_sb_egl" ] && libs_resolve "$_sb_egl"; then
        if probe_egl "$_sb_egl"; then
            echo "$_sb_egl"
            return
        fi
        log "EGL unavailable here — using DRM dumb buffers"
    fi

    # No fallback available (non-Pi, dev builds)
    if [ ! -x "$_sb_fallback" ]; then
        echo "$_sb_primary"
        return
    fi

    if ! libs_resolve "$_sb_primary"; then
        echo "$_sb_fallback"
        return
    fi

    echo "$_sb_primary"
}

# The display backend to hand the app for the binary select_binary() chose;
# prints nothing when the app should auto-detect. `egl` names a binary, not a
# backend: the app knows sdl, drm and fbdev, and the EGL binary presents
# through LVGL's DRM driver. Which of the two DRM drivers a binary carries is
# compiled in, not selected here. Only dual-binary installs (Pi with DRM+fbdev)
# get one set explicitly; single-binary platforms (AD5M, K1, etc.) auto-detect.
app_display_backend() {
    _ab_bin=$1
    _ab_fallback=$2
    case "${HELIX_DISPLAY_BACKEND:-}" in
        "" | egl) ;;
        *)
            echo "$HELIX_DISPLAY_BACKEND"
            return
            ;;
    esac
    case "$(basename "$_ab_bin")" in
        helix-screen-fbdev)
            echo fbdev
            ;;
        helix-screen-egl)
            echo drm
            ;;
        *)
            if [ -x "$_ab_fallback" ]; then
                echo drm
            fi
            ;;
    esac
}

SPLASH_BIN="${BIN_DIR}/helix-splash"
WATCHDOG_BIN="${BIN_DIR}/helix-watchdog"
FALLBACK_BIN="${BIN_DIR}/helix-screen-fbdev"

# Point OpenSSL (the app's own, libhv's default context, child processes) at a CA
# bundle. The shipped bundle comes first, matching find_ca_store() in
# src/system/tls_trust.cpp: stock printer stores are years old and the bundle is
# refreshed every release. Static glibc builds embed compiled-in cert paths from
# the Docker build container, which don't exist on the device. A user-set
# SSL_CERT_FILE wins.
if [ -z "${SSL_CERT_FILE:-}" ]; then
    if [ -s "${INSTALL_DIR}/certs/ca-certificates.crt" ]; then
        export SSL_CERT_FILE="${INSTALL_DIR}/certs/ca-certificates.crt"
    else
        for _cert_path in \
            /etc/ssl/certs/ca-certificates.crt \
            /etc/pki/tls/certs/ca-bundle.crt \
            /etc/ssl/cert.pem; do
            if [ -f "$_cert_path" ]; then
                export SSL_CERT_FILE="$_cert_path"
                break
            fi
        done
        unset _cert_path
    fi
fi

helix_load_env_file

# Heap-corruption diagnostics on constrained embedded glibc platforms where
# ASAN is not feasible and crash reports otherwise show only libc frames.
# MALLOC_CHECK_=3 aborts on malloc metadata corruption and prints a glibc
# diagnostic line to stderr/syslog BEFORE the SIGABRT — pinpoints the
# corruption class (double free, invalid pointer, corrupted size) that would
# otherwise be invisible. MALLOC_PERTURB_=165 poisons freed memory so
# use-after-free surfaces at the actual read, not six allocations later.
# Applied ONLY on AD5M/AD5X (Flashforge ZMOD/Forge-X/KlipperMod) where we
# have no test hardware and AD5X crash bundles have been unactionable.
# Set MALLOC_CHECK_=0 in helixscreen.env to disable.
_arch=$(uname -m)
_kernel=$(uname -r)
_enable_heap_diag=0

# AD5M / AD5M Pro (armv7l, Flashforge firmware — kernel 5.4.61).
# Exclude CC1 (OpenCentauri COSMOS) and K2 (Tina Linux) which share armv7l.
if [ "$_arch" = "armv7l" ] && echo "$_kernel" | grep -q "ad5m\|5.4.61"; then
    if [ ! -x /usr/bin/update-cosmos ] && [ ! -d /mnt/UDISK ]; then
        _enable_heap_diag=1
    fi
fi

# AD5X (MIPS — ZMOD or Forge-X mod tree on FlashForge AD5X, Ingenic X2600).
# ZMOD hosts carry the /ZMOD marker or FlashForge's own /usr/prog dir; a
# Forge-X chroot has neither — and no /usr/data either, since the chroot binds
# /usr/data at /opt — but the mod's git tree stays reachable and
# .shell/platform.sh in it is the evidence. K1 shares mips and carries none of
# the four markers, so the arch alone never arms this. Same rule as
# helix::ad5x_mod_layout_present(). Probes resolve under
# HELIX_AD5X_PROBE_ROOT (default /) so the bats suite can point the predicate
# at a sandbox root instead of touching the real filesystem.
_ad5x_root="${HELIX_AD5X_PROBE_ROOT:-/}"
_ad5x_root="${_ad5x_root%/}"
if [ "$_arch" = "mips" ] && {
     [ -f "${_ad5x_root}/ZMOD" ] ||
     [ -d "${_ad5x_root}/usr/prog" ] ||
     [ -f "${_ad5x_root}/opt/config/mod/.shell/platform.sh" ] ||
     [ -f "${_ad5x_root}/usr/data/config/mod/.shell/platform.sh" ]
   }; then
    _enable_heap_diag=1
fi
unset _ad5x_root

if [ "$_enable_heap_diag" = "1" ]; then
    [ -z "${MALLOC_CHECK_:-}" ] && export MALLOC_CHECK_=3
    [ -z "${MALLOC_PERTURB_:-}" ] && export MALLOC_PERTURB_=165
fi
unset _arch _kernel _enable_heap_diag

# Cap glibc's per-thread malloc arenas on memory-constrained boards.
#
# glibc spawns up to 8*ncores secondary arenas on demand. Each reserves
# HEAP_MAX_SIZE (1 MB on 32-bit ARM) of address space and commits a head.
# helix-screen runs ~12 threads, and they are IO-bound (websocket, HTTP,
# thumbnail workers), not allocation-bound, so the extra arenas buy no
# measurable contention relief and cost real memory.
#
# Measured A/B on a CC1 (114 MB RAM), two fresh processes at the same age:
#   default          18,360 kB RSS / 7,352 kB anon / 4 secondary arenas
#   MALLOC_ARENA_MAX=2  17,060 kB RSS / 6,180 kB anon / 1 secondary arena
# 1.3 MB RSS back, 1.17 MB of it anonymous, plus 24 MB of 32-bit address space.
#
# Anonymous is the expensive kind on these boards: file pages can be dropped,
# anonymous pages can only be swapped. helix-screen's working set already keeps
# these devices in continuous reclaim, and the pages the kernel steals belong to
# Klipper and Moonraker (measured on a CC1: 3675 and 5526 major faults against
# helix-screen's 68). A Klipper stalled on flash IO is a "Timer too close".
#
# Gated on total RAM rather than a platform list, because a list is the thing
# that has to be remembered for every new board and silently isn't. Measured
# fleet: AD5M 110 MB, CC1 112 MB, K1C 209 MB, K2 Plus 488 MB, Snapmaker U1
# 962 MB, CB1 987 MB — the 512 MB line sits inside a 2x gap, not on an edge.
# A value already set (helixscreen.env or the environment) always wins.
: "${HELIX_MEMINFO_FILE:=/proc/meminfo}"
HELIX_CONSTRAINED_MEM_KB=${HELIX_CONSTRAINED_MEM_KB:-524288}
if [ -z "${MALLOC_ARENA_MAX:-}" ] && [ -r "$HELIX_MEMINFO_FILE" ]; then
    _mem_total_kb=$(awk '/^MemTotal:/ { print $2; exit }' "$HELIX_MEMINFO_FILE" 2>/dev/null || echo "")
    # Ignore an unreadable or non-numeric MemTotal rather than guessing.
    case "$_mem_total_kb" in
        '' | *[!0-9]*) ;;
        *)
            if [ "$_mem_total_kb" -lt "$HELIX_CONSTRAINED_MEM_KB" ]; then
                export MALLOC_ARENA_MAX=2
            fi
            ;;
    esac
    unset _mem_total_kb
fi

# Select binary AFTER env file is sourced so HELIX_DISPLAY_BACKEND=fbdev in env file works
MAIN_BIN=$(select_binary "${BIN_DIR}")

_app_backend=$(app_display_backend "${MAIN_BIN}" "${FALLBACK_BIN}")
if [ -n "$_app_backend" ]; then
    export HELIX_DISPLAY_BACKEND="$_app_backend"
else
    unset HELIX_DISPLAY_BACKEND
fi
unset _app_backend

# Verify main binary exists
if [ ! -x "${MAIN_BIN}" ]; then
    echo "Error: Cannot find helix-screen binary at ${MAIN_BIN}" >&2
    exit 1
fi
log "Selected binary: $(basename "${MAIN_BIN}")"

# Resolve a writable path for the boot-splash heartbeat file and export it so
# the splash binary (via the watchdog), platform hooks, and helix-screen all
# resolve the SAME path. The default /tmp is READ-ONLY inside the service mount
# namespace on ProtectSystem=strict installs (Creality Sonic Pad, OrangePi
# Zero3), where the heartbeat write below silently fails and the splash can
# blank on a slow boot. Prefer /tmp so behavior and any external readers are
# unchanged on normal devices; fall back to the (writable) install dir
# otherwise. A user override from helixscreen.env is respected (already set).
if [ -z "${HELIX_SPLASH_STATUS_FILE:-}" ]; then
    for _sp_dir in /tmp /var/tmp "${INSTALL_DIR}"; do
        if ( : > "${_sp_dir}/.helix-splash-probe.$$" ) 2>/dev/null; then
            rm -f "${_sp_dir}/.helix-splash-probe.$$" 2>/dev/null || true
            HELIX_SPLASH_STATUS_FILE="${_sp_dir}/helix-splash-status"
            export HELIX_SPLASH_STATUS_FILE
            log "Splash status file: ${HELIX_SPLASH_STATUS_FILE}"
            break
        fi
    done
    unset _sp_dir
fi

# Source platform hooks if present and run platform_pre_start. The init
# script (S90helixscreen) also runs this on production boot — as a subshell,
# for side effects only — but dev deploys (`make deploy-*` → restart
# launcher directly) bypass init.d, so the launcher needs to fire it too —
# otherwise platform-specific setup like stopping the stock UI or loading
# the WiFi driver never happens during iterative testing. Calls are
# idempotent (active flag is just a touch, load functions check before
# acting), so production boot stays correct.
#
# Firing the hook HERE is what holds the precedence rule: the env file above
# and the parent shell's own variables are already in place, so a hook's
# guarded default fills only what nothing else set —
#   shell environment > helixscreen.env > platform hook > built-in.
# That rule governs this launcher's environment. A caller that must resolve
# one of these variables before exec'ing this script (the init script's
# early-splash HELIX_NO_SPLASH gate) asks this launcher via
# `helix-launcher.sh --print-env NAME` — helix_load_env_file above answers
# the query — so every reader of helixscreen.env shares one parser and one
# resolution order. Any caller that runs platform_pre_start before exec'ing
# this script must confine its exports the way the init script does, or its
# hook defaults arrive as "already set" and outrank the operator's env file.
PLATFORM_HOOKS="${INSTALL_DIR}/platform/hooks.sh"
if [ -f "${PLATFORM_HOOKS}" ]; then
    # shellcheck disable=SC1090  # path depends on INSTALL_DIR
    . "${PLATFORM_HOOKS}"
    if command -v platform_pre_start >/dev/null 2>&1; then
        platform_pre_start || true
    fi
fi

# Resolve debug/logging settings: CLI flags > env vars (incl. env file) > defaults.
#
# MUST come after the platform-hooks block above. The platform hooks export
# HELIX_LOG_DEST / HELIX_LOG_FILE from platform_pre_start (every AD5M, AD5X,
# K1/K2 and CC1 hook; the Pi, QIDI, M1 and Snapmaker U1 targets have no
# firmware log archiver to feed) to steer the app log onto a partition that
# is persistent AND captured by that firmware's log archiver.
# Resolved any earlier, those variables are unset: --log-dest/--log-file never
# reach the binary and the app log falls back to auto-detection (syslog on
# Linux). That binds every launch path the same way: the init script runs
# platform_pre_start in a subshell for side effects only, so the exports
# these lines see come from the hook call above, whether we were started by
# init, a `make deploy-*` restart, or by hand.
DEBUG_MODE="${CLI_DEBUG:-${HELIX_DEBUG:-0}}"
LOG_DEST="${CLI_LOG_DEST:-${HELIX_LOG_DEST:-auto}}"
LOG_FILE="${CLI_LOG_FILE:-${HELIX_LOG_FILE:-}}"
LOG_LEVEL="${CLI_LOG_LEVEL:-${HELIX_LOG_LEVEL:-}}"

# Check if watchdog is available (embedded targets only, provides crash recovery)
USE_WATCHDOG=0
if [ -x "${WATCHDOG_BIN}" ]; then
    USE_WATCHDOG=1
    log "Watchdog available: crash recovery enabled"
fi

# Check if splash is already running (started by init script for earlier visibility)
# If so, pass the PID to helix-screen for cleanup, and don't start another
# HELIX_NO_SPLASH=1 disables splash entirely (for debugging)
SPLASH_ARGS=""
if [ "${HELIX_NO_SPLASH:-0}" = "1" ]; then
    log "Splash disabled (HELIX_NO_SPLASH=1)"
elif [ -n "${HELIX_SPLASH_PID}" ]; then
    # Splash was pre-started by init script, pass PID to watchdog (before --)
    # so watchdog can forward it to helix-screen on first launch
    SPLASH_ARGS="--splash-pid=${HELIX_SPLASH_PID}"
    log "Using pre-started splash (PID ${HELIX_SPLASH_PID})"
elif [ -x "${SPLASH_BIN}" ]; then
    # No pre-started splash, let watchdog manage it
    SPLASH_ARGS="--splash-bin=${SPLASH_BIN}"
    log "Splash binary: ${SPLASH_BIN}"
fi

# Set when we are intentionally tearing down (a caught signal or normal exit),
# so the boot-time respawn loop below never fights a deliberate `init stop`.
HELIX_SHUTTING_DOWN=0

# Cleanup function for signal handling
cleanup() {
    HELIX_SHUTTING_DOWN=1
    log "Shutting down..."
    # Kill watchdog/helix-screen if we started them
    killall helix-watchdog helix-screen helix-screen-fbdev helix-screen-egl helix-splash 2>/dev/null || true
    # Remove the GUI pidfile we advertised (CC1 gui-switcher) so a stale PID
    # can't be signalled after we exit. HELIX_GUI_PIDFILE is exported by the
    # platform hook; unset elsewhere this is a harmless no-op.
    [ -n "${HELIX_GUI_PIDFILE:-}" ] && rm -f "${HELIX_GUI_PIDFILE}" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

log "Starting main application"

# Build command flags
EXTRA_FLAGS=""

# Log level: named level takes priority over HELIX_DEBUG
if [ -n "${LOG_LEVEL}" ]; then
    EXTRA_FLAGS="--log-level=${LOG_LEVEL}"
    log "Log level: ${LOG_LEVEL}"
elif [ "${DEBUG_MODE}" = "1" ]; then
    EXTRA_FLAGS="-vv"
    log "Debug mode enabled (debug-level logging)"
fi

# Logging destination
if [ "${LOG_DEST}" != "auto" ]; then
    EXTRA_FLAGS="${EXTRA_FLAGS} --log-dest=${LOG_DEST}"
    log "Log destination: ${LOG_DEST}"
fi

# Explicit log file path (only meaningful with --log-dest=file)
if [ -n "${LOG_FILE}" ]; then
    EXTRA_FLAGS="${EXTRA_FLAGS} --log-file=${LOG_FILE}"
    log "Log file: ${LOG_FILE}"
fi

# DPI override (env var only — CLI passthrough handles --dpi directly)
if [ -n "${HELIX_DPI:-}" ]; then
    EXTRA_FLAGS="${EXTRA_FLAGS} --dpi ${HELIX_DPI}"
    log "DPI override: ${HELIX_DPI}"
fi

# Skip internal splash screen
if [ "${HELIX_SKIP_SPLASH:-0}" = "1" ]; then
    EXTRA_FLAGS="${EXTRA_FLAGS} --skip-splash"
    log "Splash screen disabled (HELIX_SKIP_SPLASH=1)"
fi

# Remote control server (helix-screen ctl). The server only listens when the app
# is started with --remote or --test, and the SysV/systemd units exec this
# launcher with no arguments — so without an env var there is no way to reach a
# deployed device with `ctl` at all, and diagnosing a display problem means
# physically walking to the printer. Off by default: it opens a control socket
# that can drive the entire UI.
if [ "${HELIX_REMOTE_CONTROL:-0}" = "1" ]; then
    EXTRA_FLAGS="${EXTRA_FLAGS} --remote"
    # "requested", not "enabled": the bind happens later, inside the app, and
    # can still fail. Whether it listens is the app's [RemoteControl] line.
    log "Remote control requested (HELIX_REMOTE_CONTROL=1)"
    if [ -n "${HELIX_REMOTE_SOCKET:-}" ]; then
        EXTRA_FLAGS="${EXTRA_FLAGS} --remote-socket ${HELIX_REMOTE_SOCKET}"
        log "Remote control socket: ${HELIX_REMOTE_SOCKET}"
    fi
fi

# Run UI at reduced priority (nice +10) when co-hosted with Klipper/Moonraker
# so the printer control loop keeps CPU headroom for stepper timing and MCU
# comms. Skipped on standalone displays (remote SonicPad, dev workstation,
# kiosk pointed at a network printer). Done HERE — after platform hooks and
# the SysV init's platform_wait_for_services — so Klipper has had time to
# come up before we probe for it. Children inherit the nice value, so the
# watchdog, helix-screen, and splash all run at +10. Raising nice is
# unprivileged, so this works as the non-root service user.
# Override with HELIX_NICE=<n> in helixscreen.env (HELIX_NICE=0 disables).
if helix_klipper_co_hosted; then
    _helix_nice="${HELIX_NICE:-10}"
    if [ "${_helix_nice}" != "0" ]; then
        if renice "${_helix_nice}" $$ >/dev/null 2>&1; then
            log "Co-hosted with Klipper/Moonraker — running at nice +${_helix_nice}"
        fi
    fi
    unset _helix_nice

    # Volunteer helix-screen as the kernel's first OOM victim. Co-hosted means
    # Klipper is on this board, and Klipper cannot be restarted mid-print
    # without ruining the job, while helix-screen has helix-watchdog sitting
    # behind it. Measured on an AD5M (110MB total): every process sat at
    # oom_score_adj 0, leaving the kill order Moonraker (score 156), Klipper
    # (92), helix-screen (69) — exactly backwards.
    #
    # Exported rather than applied here, because oom_score_adj is inherited
    # across fork and preserved across exec: setting it on the launcher would
    # mark this shell and helix-watchdog too, and killing the watchdog is what
    # stops helix-screen from coming back. helix-screen applies it to
    # /proc/self instead, so only the process actually holding the memory
    # volunteers. Raising the value is unprivileged, so this works as the
    # non-root service user; only lowering below 0 needs CAP_SYS_RESOURCE.
    #
    # Not gated on RAM size: if Klipper is on this box then losing the UI is
    # the cheaper outcome no matter how much memory the board has.
    # Override with HELIX_OOM_SCORE_ADJ=<n> in helixscreen.env (0 disables).
    _helix_oom="${HELIX_OOM_SCORE_ADJ:-300}"
    if [ "${_helix_oom}" != "0" ]; then
        export HELIX_OOM_SCORE_ADJ="${_helix_oom}"
        log "Co-hosted with Klipper/Moonraker — helix-screen oom_score_adj +${_helix_oom}"
    fi
    unset _helix_oom
fi

# Runtime crash fallback predicate. Defined before the run loop so it is
# available on every iteration.
# Only retry on genuine crashes, NOT on signal-based exits (SIGTERM=143 from systemctl stop,
# SIGKILL=137, SIGINT=130, SIGHUP=129). Crash signals: SIGABRT=134, SIGFPE=136, SIGBUS=138, SIGSEGV=139.
#
# 42 is helix-watchdog's RESTART_LOOP_EXIT_CODE: the supervisor already retried
# and concluded the failure will not resolve itself. Re-running the whole
# watchdog against the fbdev binary just reproduces it, so it is excluded even
# though it falls inside the 1..127 "worth retrying" range.
WATCHDOG_GIVE_UP_EXIT_CODE=42

_is_crash_exit() {
    if [ "$1" = "$WATCHDOG_GIVE_UP_EXIT_CODE" ]; then
        return 1
    fi
    case "$1" in
        134|136|138|139) return 0 ;;  # ABRT, FPE, BUS, SEGV
    esac
    # Non-signal exits (1-127) are also worth retrying (e.g., GL init failure)
    [ "$1" -gt 0 ] && [ "$1" -lt 128 ]
}

# Boot-time SIGTERM self-heal (disabled by default; the Snapmaker U1 platform
# hook opts in via HELIX_BOOT_RESPAWN_MAX). Some firmwares send helix-screen a
# single SIGTERM during the busy boot sequence; helix-screen handles SIGTERM
# with a fast _exit(0) (see graceful_quit_signal_handler), expecting a
# supervisor to respawn it. On an unsupervised SysV boot (no helix-watchdog,
# busybox init does not respawn S99 children) that one signal is permanent — and
# because helix-screen owns the WiFi association on the U1, its death leaves the
# device dark AND off-network (unreachable). We cannot tell a boot-time SIGTERM
# (exit 0) from a deliberate user quit (also exit 0) by exit code, so we
# discriminate by UPTIME: a process that dies within RESPAWN_WINDOW seconds of
# launch lost the boot race (the UI never became interactive), whereas a real
# quit happens long after boot. Respawn up to RESPAWN_MAX times. A real
# `init stop` kills THIS launcher (the cleanup trap sets HELIX_SHUTTING_DOWN), so
# the loop never fires for an intentional stop.
RESPAWN_MAX="${HELIX_BOOT_RESPAWN_MAX:-0}"
RESPAWN_WINDOW="${HELIX_BOOT_RESPAWN_WINDOW:-25}"
RESPAWN_DELAY="${HELIX_BOOT_RESPAWN_DELAY:-3}"
RESPAWN_COUNT=0

# Seed the boot-splash heartbeat right before launching helix-screen. The splash
# treats the status file as a heartbeat and, once it has seen one, stays visible
# until helix-screen signals it (SIGUSR1) when the UI is ready — covering slow
# helix-screen startup with no blank gap and a live "Starting HelixScreen… Ns"
# counter (the splash owns the elapsed count). Without a heartbeat the splash
# self-caps at 30s and can blank on a slow device whose startup exceeds it.
#
# Platforms with a Moonraker wait gate (K2, AD5M-forgex) already wrote heartbeats
# during the gate; this is the universal fallback that brings the same gap-free
# handoff to gate-less platforms (Pi, K1, CC1, AD5X, …). Re-writing the same
# "ready" line where the gate ran is harmless. Skipped when the splash is
# disabled or unavailable. Path matches splash_status_path() in helix_splash.cpp.
if [ "${HELIX_NO_SPLASH:-0}" != "1" ] && [ -n "${SPLASH_ARGS}" ]; then
    echo "Starting HelixScreen…" > "${HELIX_SPLASH_STATUS_FILE:-/tmp/helix-splash-status}" \
        2>/dev/null || true
fi

# Run main application (via watchdog if available for crash recovery)
# Note: PASSTHROUGH_ARGS is unquoted to allow word splitting (POSIX compatible)
# Use "cmd || EXIT_CODE=$?" to capture non-zero exit codes under set -e,
# allowing the crash fallback logic below to run instead of aborting the script.
while :; do
    _run_start=$(date +%s 2>/dev/null || echo 0)
    EXIT_CODE=0
    if [ "${USE_WATCHDOG}" = "1" ]; then
        # Watchdog supervises helix-screen and manages splash lifecycle
        # Watchdog and splash auto-detect resolution from display hardware
        log "Starting via watchdog supervisor"
        # shellcheck disable=SC2086
        "${WATCHDOG_BIN}" ${SPLASH_ARGS} -- \
            "${MAIN_BIN}" ${EXTRA_FLAGS} ${PASSTHROUGH_ARGS} || EXIT_CODE=$?
    else
        # Direct launch (development, or watchdog not built)
        # shellcheck disable=SC2086
        "${MAIN_BIN}" ${EXTRA_FLAGS} ${PASSTHROUGH_ARGS} || EXIT_CODE=$?
    fi

    # Runtime crash fallback: step down exactly one rung and retry. A GPU
    # failure must not demote the board past DRM dumb buffers to fbdev — that
    # would hide a working middle rung behind a broken top one.
    RETRY_BIN=""
    RETRY_BACKEND=""
    case "$(basename "${MAIN_BIN}")" in
        helix-screen-egl)
            if [ -x "${BIN_DIR}/helix-screen" ]; then
                RETRY_BIN="${BIN_DIR}/helix-screen"
                RETRY_BACKEND=drm
            fi
            ;;
        helix-screen)
            if [ -x "${FALLBACK_BIN}" ]; then
                RETRY_BIN="${FALLBACK_BIN}"
                RETRY_BACKEND=fbdev
            fi
            ;;
    esac
    if _is_crash_exit ${EXIT_CODE} && [ -n "${RETRY_BIN}" ]; then
        log "$(basename "${MAIN_BIN}") exited with code ${EXIT_CODE}, retrying with $(basename "${RETRY_BIN}")..."
        export HELIX_DISPLAY_BACKEND="${RETRY_BACKEND}"
        if [ "${USE_WATCHDOG}" = "1" ]; then
            # shellcheck disable=SC2086
            "${WATCHDOG_BIN}" ${SPLASH_ARGS} -- \
                "${RETRY_BIN}" ${EXTRA_FLAGS} ${PASSTHROUGH_ARGS}
            EXIT_CODE=$?
        else
            # shellcheck disable=SC2086
            "${RETRY_BIN}" ${EXTRA_FLAGS} ${PASSTHROUGH_ARGS}
            EXIT_CODE=$?
        fi
        log "$(basename "${RETRY_BIN}") fallback exited with code ${EXIT_CODE}"
    fi

    _run_end=$(date +%s 2>/dev/null || echo 0)
    _run_uptime=$(( _run_end - _run_start ))
    if [ "${HELIX_SHUTTING_DOWN:-0}" != "1" ] \
       && [ "${RESPAWN_COUNT}" -lt "${RESPAWN_MAX}" ] \
       && [ "${_run_uptime}" -ge 0 ] \
       && [ "${_run_uptime}" -lt "${RESPAWN_WINDOW}" ]; then
        RESPAWN_COUNT=$((RESPAWN_COUNT + 1))
        log "helix-screen exited (code ${EXIT_CODE}) after ${_run_uptime}s — likely a boot-time kill; respawning (${RESPAWN_COUNT}/${RESPAWN_MAX})"
        if [ "${RESPAWN_DELAY}" -gt 0 ]; then
            sleep "${RESPAWN_DELAY}"
        fi
        continue
    fi
    break
done

log "Exiting with code ${EXIT_CODE}"
exit ${EXIT_CODE}
