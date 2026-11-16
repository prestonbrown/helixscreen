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
# K2: /mnt/UDISK/helixscreen, and /opt/helixscreen until it migrates
# Pi: /opt/helixscreen
# CC1 (COSMOS): /user-resource/helixscreen (/ is RO squashfs)
# Snapmaker U1: /userdata/helixscreen
# shellcheck disable=SC2034  # consumed by uninstall.sh (sweep of all known install locations)
HELIX_INSTALL_DIRS="/root/printer_software/helixscreen /opt/helixscreen /mnt/UDISK/helixscreen /usr/data/helixscreen /srv/helixscreen /user-resource/helixscreen /userdata/helixscreen"

# Where cache/ and logs/ live. Deliberately NOT inside an install root: the
# payload is what an update replaces, and Moonraker's type:web entry rmtree()s
# it first. Swept on uninstall, since nothing else ever removes them.
# Mirrors kStateRoots in include/helix_install_roots.h.
# shellcheck disable=SC2034  # consumed by uninstall.sh
HELIX_STATE_DIRS="/mnt/UDISK/helixscreen-state /mnt/UDISK/helixscreen /data/.helixscreen /data/helixscreen /usr/data/helixscreen-state /user-resource/helixscreen-state /userdata/helixscreen-state /srv/helixscreen-state /opt/config/mod_data/helixscreen-state"

# Mounts release.sh's detect_rollback_dir() tries, in order, for an
# off-partition update-backup when the install filesystem is too tight to
# hold the old and new tree at once (HELIX_ROLLBACK_CANDIDATES overrides this
# for tests). Shared here, rather than left local to release.sh, so
# uninstall.sh's disabled-services ledger lookup can also recognise a backup
# under one of these mounts without depending on release.sh, which the
# standalone uninstaller does not bundle.
# shellcheck disable=SC2034  # consumed by release.sh and uninstall.sh
HELIX_ROLLBACK_CANDIDATES_DEFAULT="/mnt/UDISK /usr/data /mnt/data /data /user-resource /oem /userdata /var/tmp"

# Remove a state root that is now empty.
#
# The sweep above takes cache/ and logs/ but leaves the directory that held
# them. Only a name this installer coins is removed: the "-state" suffix, and
# the dot-prefixed AD5M root (no operator names a directory with a leading
# dot by hand). A bare ".../helixscreen" state root is left alone even when
# empty - /data/helixscreen and the pre-migration /mnt/UDISK/helixscreen are
# plain enough names that the operator may have meant that directory
# themselves.
#
# rmdir carries the rest of the safety: it refuses a directory with anything
# still in it, so a root someone has put their own files in survives.
helix_state_prune_empty_roots() {
    for _hsper in $HELIX_STATE_DIRS; do
        case "$_hsper" in
            */helixscreen-state|*/.helixscreen) ;;
            *) continue ;;
        esac
        [ -d "$_hsper" ] || continue
        rmdir "$_hsper" 2>/dev/null || $SUDO rmdir "$_hsper" 2>/dev/null || true
    done
}

# Cache and log directories an uninstall removes: every declared state dir, plus
# the in-payload locations older installs still carry. Emitting the legacy ones
# is what makes an upgrade-then-uninstall clean, since a box installed before
# the state moved still has them.
# shellcheck disable=SC2034  # consumed by uninstall.sh
helix_state_sweep_paths() {
    for _hssp in $HELIX_STATE_DIRS; do
        printf '%s/cache\n%s/logs\n' "$_hssp" "$_hssp"
    done
    printf '%s\n' /root/.cache/helix /tmp/helix_thumbs /.cache/helix \
        /data/helixscreen/cache /usr/data/helixscreen/cache \
        /user-resource/helixscreen/cache /userdata/helixscreen/cache \
        /srv/helixscreen/cache
}

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

# The directory the running install.sh lives in, or return 1 when it has
# none: under `curl ... | sh` the script is stdin and $0 names the shell.
installer_self_dir() {
    [ -f "$0" ] || return 1
    (cd "$(dirname "$0")" 2>/dev/null && pwd -P)
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

# Remove an install-dir state file. When it is a symlink into printer_data the
# target goes too; unlinking only the link would leave a stale ledger behind
# that a later install reads as its own.
# Args: $1 = path
remove_state_file() {
    local f="$1" t=""
    [ -L "$f" ] && t="$(readlink "$f" 2>/dev/null)"
    $(path_sudo "$f") rm -f "$f" 2>/dev/null || true
    [ -n "$t" ] && { $(path_sudo "$t") rm -f "$t" 2>/dev/null || true; }
    return 0
}

# Pin the trust properties of helixscreen.env: the launcher's env-file parse
# evaluates the file's lines, so its owner and mode decide who can run code as
# the launcher's user (root on every SysV firmware device). State 0644 and
# service-user ownership instead of inheriting whatever the staging umask left
# behind; with no KLIPPER_USER (root-run firmware) the file stays root's.
# Resolves through the printer_data symlink: pinning the link's own mode does
# nothing to the file the launcher reads. The launcher re-checks on every load,
# so a file this helper never reached is refused rather than evaluated.
pin_env_file() {
    local file="${INSTALL_DIR}/config/helixscreen.env"
    [ -f "$file" ] || return 0

    local real="$file"
    if [ -L "$file" ]; then
        real=$(readlink -f "$file" 2>/dev/null || echo "$file")
    fi
    [ -n "$real" ] && [ -f "$real" ] || real="$file"

    # printer_data is writable by the Klipper user, who can swap any name in it
    # for a symlink after readlink resolved it. chown -h changes the link, never
    # its target, and chmod has no such form, so it skips a name that is a link.
    # Failures warn rather than fail the install, but never silently: an
    # unpinned file is one the launcher refuses on every boot, and an
    # unreported chmod is indistinguishable from a pinned one at install time.
    if [ -L "$real" ] || ! $(file_sudo "$real") chmod 0644 "$real" 2>/dev/null; then
        log_warn "pin_env_file: could not chmod 0644 '$real' (the launcher will refuse this file until fixed)"
    fi

    # Behind the printer_data symlink, the file belongs to whoever owns
    # printer_data/config: Moonraker saves it as that user (lava on the U1, where
    # the launcher and KLIPPER_USER are root), and the launcher trusts that owner
    # only while the directory holding the file has no group or world write bit.
    # The directory also holds settings.json, which the app writes as its own
    # user, so it changes hands only when the app runs as root or as that owner.
    local user="${KLIPPER_USER:-}"
    if [ "$real" != "$file" ]; then
        local data_dir="${real%/*}" pd_owner
        pd_owner=$(_file_owner_name "${data_dir%/*}")
        if [ -n "$pd_owner" ] && [ "$pd_owner" != "root" ] &&
            { [ "$user" = "root" ] || [ "$user" = "$pd_owner" ]; }; then
            user="$pd_owner"
            local dgroup="$user"
            if type _resolve_primary_group >/dev/null 2>&1; then
                dgroup=$(_resolve_primary_group "$user")
            fi
            if ! $(file_sudo "$data_dir") chown -h "${user}:${dgroup}" "$data_dir" 2>/dev/null; then
                log_warn "pin_env_file: could not chown ${user}:${dgroup} '$data_dir'"
            fi
            if [ -L "$data_dir" ] || ! $(file_sudo "$data_dir") chmod 0755 "$data_dir" 2>/dev/null; then
                log_warn "pin_env_file: could not chmod 0755 '$data_dir' (the launcher will refuse the env file until fixed)"
            fi
        fi
    fi
    if [ -n "$user" ]; then
        local group="$user"
        if type _resolve_primary_group >/dev/null 2>&1; then
            group=$(_resolve_primary_group "$user")
        fi
        if ! $(file_sudo "$real") chown -h "${user}:${group}" "$real" 2>/dev/null; then
            log_warn "pin_env_file: could not chown ${user}:${group} '$real'"
        fi
    fi
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

# printer_data/config/helixscreen/ for this install, or empty when no Klipper
# config dir is known. The single definition of where user config lives; the
# installed init script and systemd unit record it for restore-user-config.sh.
helix_pd_helix_dir() {
    local cfg
    cfg="$(klipper_config_dir)"
    [ -n "$cfg" ] && echo "${cfg}/helixscreen"
    return 0
}

# Where a ledger named $1 (a file under config/, such as .klipper_includes) can
# be, in the order they are trusted: the install dir, then printer_data. Both
# are the same file on a completed install (a symlink); after a web update has
# removed the install dir, only the printer_data copy is left. With a second
# argument the stranded-copy locations described above follow, which only the
# disabled-services ledger needs.
#
# A location this run cannot resolve (no Klipper config dir known) is omitted
# rather than probed with an empty prefix.
# Args: $1 = ledger file name, $2 = "wide" to include the backup forms
_ledger_candidates() {
    local _name="$1" _pd_config="" _newest_old _rollback_mount
    if type klipper_config_dir >/dev/null 2>&1; then
        _pd_config="$(klipper_config_dir)"
    fi
    echo "${INSTALL_DIR}/config/${_name}"
    if [ -n "$_pd_config" ]; then
        echo "${_pd_config}/helixscreen/${_name}"
        [ "$_name" = ".disabled_services" ] && echo "${_pd_config}/${_name}.clean-keep"
    fi
    [ "${2:-}" = "wide" ] || return 0
    echo "${INSTALL_DIR}.old/config/${_name}"
    _newest_old="$(_newest_timestamped_install_backup)"
    [ -n "$_newest_old" ] && echo "${_newest_old}/config/${_name}"
    for _rollback_mount in ${HELIX_ROLLBACK_CANDIDATES:-$HELIX_ROLLBACK_CANDIDATES_DEFAULT}; do
        echo "${_rollback_mount}/helixscreen-rollback/helixscreen/config/${_name}"
    done
    return 0
}

# The existing copy of ledger $1, else where a new one belongs (the install
# dir). Writers and readers both go through this, so a ledger that survived in
# printer_data is the one that is appended to and later undone.
# Args: $1 = ledger file name
_ledger_path() {
    local _candidate _list
    _list="$(_ledger_candidates "$1")"
    while IFS= read -r _candidate; do
        # A .clean-keep carry is a stranded copy for the uninstaller to find,
        # never a place to write.
        case "$_candidate" in *.clean-keep) continue ;; esac
        if [ -n "$_candidate" ] && [ -f "$_candidate" ]; then
            echo "$_candidate"
            return 0
        fi
    done <<EOF
$_list
EOF
    echo "${INSTALL_DIR}/config/$1"
}

# Escape a value for the replacement half of a sed s||| command (backslash, the
# delimiter and & are special there). One helper for every installer sed that
# writes a path.
# Args: $1 = value
sed_escape_replacement() {
    printf '%s' "$1" | sed 's/[\\&|]/\\&/g'
}

# Track what we've done for cleanup
CLEANUP_TMP=false
BACKUP_CONFIG=""
BACKUP_ENV=""

# Output is decided by stderr, where every log line goes: under `curl | sh`
# stdin is the script and stdout may be a pipe while stderr is the terminal.
# HELIX_INSTALL_TTY=0|1 overrides the probe (tests, and callers that know).
# shellcheck disable=SC2034  # UI_UTF8, BOLD and DIM are consumed by the step layer and main.sh
ui_detect() {
    case "${HELIX_INSTALL_TTY:-}" in
        0) UI_TTY=0 ;;
        1) UI_TTY=1 ;;
        *) if [ -t 2 ]; then UI_TTY=1; else UI_TTY=0; fi ;;
    esac

    _ui_locale="${LC_ALL:-${LC_CTYPE:-${LANG:-}}}"
    case "$_ui_locale" in
        *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) UI_UTF8=1 ;;
        *) UI_UTF8=0 ;;
    esac

    UI_COLOR=0
    if [ "$UI_TTY" = 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
        case "${TERM:-}:${COLORTERM:-}" in
            *256color*|*:truecolor|*:24bit) UI_COLOR=256 ;;
            *) UI_COLOR=16 ;;
        esac
    fi

    if [ "$UI_COLOR" != 0 ]; then
        RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
        CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'
    else
        RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; DIM=''; NC=''
    fi
}
ui_detect

# The log file. Lines logged before log_open (detection runs before the
# install has anywhere to write) are held in _LOG_BUFFER and flushed by it.
INSTALL_LOG=""
_LOG_BUFFER=""

_log_write() {
    _lw_line="[$(date +%H:%M:%S)] $1"
    if [ -n "$INSTALL_LOG" ]; then
        printf '%s\n' "$_lw_line" >> "$INSTALL_LOG" 2>/dev/null || true
    else
        _LOG_BUFFER="${_LOG_BUFFER}${_lw_line}
"
    fi
}

log_open() {
    INSTALL_LOG="$1"
    : > "$INSTALL_LOG" 2>/dev/null || { INSTALL_LOG=""; return 1; }
    printf '%s' "$_LOG_BUFFER" >> "$INSTALL_LOG"
    _LOG_BUFFER=""
}

# True when a question can reach a person: the controlling terminal opens, or
# stdin is one. The probes redirect into `true`, never `:`: a redirection that
# fails on a special builtin ends a dash or BusyBox ash script, and /dev/tty
# fails to open in any run without a controlling terminal.
tty_can_ask() {
    # shellcheck disable=SC2217  # the redirect is the probe; true only absorbs it
    { true < "${HELIX_TTY_DEVICE:-/dev/tty}"; } 2>/dev/null || [ -t 0 ]
}

# Ask a yes/no question. Under `curl | sh` stdin is the script, so the answer
# comes from the controlling terminal; with neither, the default stands. An
# empty line takes the default; end of input (Ctrl-D) is a no, or with
# on_eof=abort returns 2: for a question where neither answer is the safe one.
tty_confirm() { # question default(y|n) [on_eof(abort)]
    [ "${ASSUME_YES:-false}" = true ] && return 0
    _tc_dev="${HELIX_TTY_DEVICE:-/dev/tty}"
    _tc_hint="[y/N]"; [ "$2" = y ] && _tc_hint="[Y/n]"
    _tc_ans=""
    _tc_eof=""
    # shellcheck disable=SC2217  # open probe, see tty_can_ask
    if { true < "$_tc_dev"; } 2>/dev/null; then
        printf '%s %s ' "$1" "$_tc_hint" >&2
        IFS= read -r _tc_ans < "$_tc_dev" || [ -n "$_tc_ans" ] || _tc_eof=1
    elif [ -t 0 ]; then
        printf '%s %s ' "$1" "$_tc_hint" >&2
        IFS= read -r _tc_ans || [ -n "$_tc_ans" ] || _tc_eof=1
    else
        _tc_ans="$2"
    fi
    if [ -n "$_tc_eof" ]; then
        if [ "${3:-}" = abort ]; then
            _log_write "ASK $1 -> (end of input)"
            return 2
        fi
        _tc_ans=n
    fi
    _log_write "ASK $1 -> ${_tc_ans:-$2}"
    case "${_tc_ans:-$2}" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# Strip \033[...m sequences from a message before it reaches the log. The ESC
# byte comes from printf: BusyBox sed does not understand \x1b.
_ESC=$(printf '\033')
_log_plain() { printf '%b' "$1" | sed "s/${_ESC}\\[[0-9;]*m//g"; }

# Steps: a titled unit of work that resolves to done, failed or skipped. On a
# terminal the open step is a spinner line that later lines redraw beneath;
# without one nothing prints until the step resolves, as one numbered line.
# STEP_TOTAL is set by the plan; 0 means "do not number".
STEP_TOTAL=0
STEP_NUM=0
STEP_OPEN=0
STEP_TITLE=""
STEP_DONE_TITLE=""
STEP_PROGRESS_N=0
_SPIN_I=0

# Machine-readable progress for a driving program (--progress): one
# tab-separated line per event on stdout, where the human output goes to
# stderr. Tabs and newlines inside a field are flattened to single spaces so a
# title can never split a record.
PROGRESS=0
# shellcheck disable=SC2034  # read and set by main.sh (_progress_result)
PROGRESS_RESULT_SENT=0
_progress_emit() { # kind field...
    [ "$PROGRESS" = 1 ] || return 0
    _pe_line="HELIX-PROGRESS"
    for _pe_f in "$@"; do
        _pe_f=$(printf '%s' "$_pe_f" | tr '\t\n\r' '   ')
        _pe_line="$_pe_line$(printf '\t')$_pe_f"
    done
    printf '%s\n' "$_pe_line"
}

_ui_marks() {
    if [ "$UI_UTF8" = 1 ]; then
        MARK_OK="✓"; MARK_FAIL="✗"; ELLIPSIS="…"
    else
        MARK_OK="ok"; MARK_FAIL="FAIL"; ELLIPSIS="..."
    fi
}

_spin_frame() {
    if [ "$UI_UTF8" = 1 ]; then
        set -- ⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
    else
        set -- '|' '/' '-' '\'
    fi
    _SPIN_I=$(( (_SPIN_I % $#) + 1 ))
    eval "_SPIN_CH=\${$_SPIN_I}"
}

# Redraw the open step's line (terminal only). Called by step and by every
# line printed while the step is open, so the spinner advances as work logs.
_step_redraw() {
    [ "$UI_TTY" = 1 ] && [ "$STEP_OPEN" = 1 ] || return 0
    _spin_frame
    printf '\r\033[K  %b%s%b %s%s' "$CYAN" "$_SPIN_CH" "$NC" "$STEP_TITLE" "$ELLIPSIS" >&2
}

# Screen output for the four levels: indented under an open step, as given
# otherwise.
_ui_emit() {
    if [ "$STEP_OPEN" = 1 ]; then
        [ "$UI_TTY" = 1 ] && printf '\r\033[K' >&2
        printf '%b\n' "      $1" >&2
        _step_redraw
    else
        printf '%b\n' "$1" >&2
    fi
}

log_info() {
    _log_write "INFO $(_log_plain "$1")"
    [ "${HELIX_INSTALL_VERBOSE:-0}" = 1 ] && _ui_emit "${CYAN}[INFO]${NC} $1"
    return 0
}
log_success() {
    _log_write "OK $(_log_plain "$1")"
    [ "${HELIX_INSTALL_VERBOSE:-0}" = 1 ] && _ui_emit "${GREEN}[OK]${NC} $1"
    return 0
}
log_warn() {
    _log_write "WARN $(_log_plain "$1")"
    _ui_emit "${YELLOW}[WARN]${NC} $1"
}
log_error() {
    _log_write "ERROR $(_log_plain "$1")"
    _ui_emit "${RED}[ERROR]${NC} $1"
}
# A detail line a regular user should see. Used sparingly.
log_note() {
    _log_write "NOTE $(_log_plain "$1")"
    _ui_emit "    $1"
}

# A step has a running title, shown by the spinner and by a failure (what was
# attempted), and a done title, shown once it succeeds; one title serves both.
step() { # running-title [done-title]
    [ "$STEP_OPEN" = 1 ] && step_done
    _ui_marks
    STEP_TITLE="$1"
    STEP_DONE_TITLE="${2:-$1}"
    STEP_OPEN=1
    STEP_PROGRESS_N=$((STEP_NUM + 1))
    _progress_emit step "$STEP_PROGRESS_N" "$STEP_TOTAL" start "$STEP_TITLE" ""
    _log_write "STEP $1"
    _step_redraw
}

_step_close() { # mark color word detail title status
    STEP_NUM=$((STEP_NUM + 1))
    if [ "$UI_TTY" = 1 ]; then
        if [ -n "$4" ]; then
            printf '\r\033[K  %b%s%b %-22s %b%s%b\n' "$2" "$1" "$NC" "$5" "$DIM" "$4" "$NC" >&2
        else
            printf '\r\033[K  %b%s%b %s\n' "$2" "$1" "$NC" "$5" >&2
        fi
    else
        _sc_prefix=""
        [ "$STEP_TOTAL" -gt 0 ] && _sc_prefix="[$STEP_NUM/$STEP_TOTAL] "
        if [ -n "$4" ]; then
            printf '%s%s ... %s (%s)\n' "$_sc_prefix" "$5" "$3" "$4" >&2
        else
            printf '%s%s ... %s\n' "$_sc_prefix" "$5" "$3" >&2
        fi
    fi
    _progress_emit step "$STEP_NUM" "$STEP_TOTAL" "$6" "$5" "$4"
    STEP_OPEN=0
}

# shellcheck disable=SC2120  # step() closes a stale step with no detail
step_done() {
    [ "$STEP_OPEN" = 1 ] || return 0
    if [ -n "${1:-}" ]; then _log_write "DONE $STEP_DONE_TITLE ($1)"; else _log_write "DONE $STEP_DONE_TITLE"; fi
    _step_close "$MARK_OK" "$GREEN" ok "${1:-}" "$STEP_DONE_TITLE" ok
}

step_fail() {
    [ "$STEP_OPEN" = 1 ] || return 0
    _log_write "FAIL $STEP_TITLE${1:+ ($1)}"
    _step_close "$MARK_FAIL" "$RED" FAILED "${1:-}" "$STEP_TITLE" fail
}

step_skip() {
    [ "$STEP_OPEN" = 1 ] || return 0
    _log_write "SKIP $STEP_TITLE"
    _progress_emit step "$STEP_PROGRESS_N" "$STEP_TOTAL" skip "$STEP_TITLE" ""
    [ "$UI_TTY" = 1 ] && printf '\r\033[K' >&2
    STEP_OPEN=0
}

# A path for display, with $HOME shown as ~.
display_path() {
    if [ -n "${HOME:-}" ] && [ "$HOME" != / ]; then
        case "$1" in "$HOME"/*) printf '~%s' "${1#"$HOME"}"; return 0 ;; esac
    fi
    printf '%s' "$1"
}

# A file's size for display, in whole MB, or KB below one MB.
file_size_text() {
    [ -f "$1" ] || return 0
    _fst=$(wc -c < "$1")
    if [ "$_fst" -ge 1048576 ]; then
        printf '%s MB' "$((_fst / 1048576))"
    else
        printf '%s KB' "$((_fst / 1024))"
    fi
}

# Where the install log is kept: the platform's own log dir when it names one
# (set_install_paths), else Moonraker's logs root, which Mainsail and Fluidd
# show, else the logs/ of a state root outside the payload, which an update
# replaces whole.
install_log_dest() {
    if [ -n "${INSTALL_LOG_DIR:-}" ]; then
        printf '%s/helixscreen-install.log' "$INSTALL_LOG_DIR"
    elif [ -n "${KLIPPER_HOME:-}" ] && [ -d "$KLIPPER_HOME/printer_data" ]; then
        printf '%s' "$KLIPPER_HOME/printer_data/logs/helixscreen-install.log"
    else
        printf '%s/logs/helixscreen-install.log' "$(install_state_root)"
    fi
}

# The platform's declared state root (set_install_paths), else the
# <install>-state sibling the app also reads (HELIX_STATE_DIRS), else the
# .helixscreen sibling the update service keeps its sentinel in. Uninstall
# sweeps all three.
install_state_root() {
    if [ -n "${STATE_DIR:-}" ]; then
        printf '%s' "$STATE_DIR"
    elif [ -n "${STATE_ROOT:-}" ]; then
        printf '%s' "$STATE_ROOT"
    else
        case " $HELIX_STATE_DIRS " in
            *" ${INSTALL_DIR%/}-state "*) printf '%s' "${INSTALL_DIR%/}-state" ;;
            *) printf '%s/.helixscreen' "$(dirname "$INSTALL_DIR")" ;;
        esac
    fi
}

# Move this run's log from the scratch dir to install_log_dest, keeping the
# previous run's as .1. The in-app updater sends stdout and stderr to a log
# file of its own, through a descriptor that does not append: should that be
# the same file, this run's log is written through that descriptor, and the
# file is never rotated, replaced or appended to by name. Sets
# INSTALL_LOG_KEPT to where the log now is.
finalize_install_log() {
    [ -n "$INSTALL_LOG" ] && [ -f "$INSTALL_LOG" ] || return 0
    _fil_dest=$(install_log_dest)
    [ "$INSTALL_LOG" = "$_fil_dest" ] && return 0
    # -ef is in dash, BusyBox ash and bash, every shell this runs under.
    # shellcheck disable=SC3013
    if [ "$_fil_dest" -ef /proc/self/fd/2 ]; then
        cat "$INSTALL_LOG" >&2; INSTALL_LOG=""; INSTALL_LOG_KEPT="$_fil_dest"; return 0
    fi
    # shellcheck disable=SC3013
    if [ "$_fil_dest" -ef /proc/self/fd/1 ]; then
        cat "$INSTALL_LOG"; INSTALL_LOG=""; INSTALL_LOG_KEPT="$_fil_dest"; return 0
    fi
    _fil_dir=$(dirname "$_fil_dest")
    mkdir -p "$_fil_dir" 2>/dev/null || $SUDO mkdir -p "$_fil_dir" 2>/dev/null || return 0
    _fil_sudo=$(file_sudo "$_fil_dest")
    if [ -f "$_fil_dest" ]; then
        $_fil_sudo mv -f "$_fil_dest" "$_fil_dest.1" 2>/dev/null || true
    fi
    $_fil_sudo mv -f "$INSTALL_LOG" "$_fil_dest" 2>/dev/null || return 0
    INSTALL_LOG="$_fil_dest"
    # shellcheck disable=SC2034  # read by main.sh and plan.sh
    INSTALL_LOG_KEPT="$_fil_dest"
}

RUN_LOGGED_TAIL=${RUN_LOGGED_TAIL:-15}

# The failure block: what ran, how it ended, and the last lines it said.
print_failure() { # description rc output-file [hint]
    _ui_emit "${RED}$1 failed (exit $2):${NC}"
    if [ -s "$3" ]; then
        tail -n "$RUN_LOGGED_TAIL" "$3" | while IFS= read -r _pf_line || [ -n "$_pf_line" ]; do
            _ui_emit "  $_pf_line"
        done
    fi
    if [ -n "${4:-}" ]; then _ui_emit "$4"; fi
    return 0
}

# The command runs in the foreground so a sudo inside it can still prompt;
# its output goes to a temp file, never a pipe, so $? is the command's own.
# The exit code is captured with && ||, so a failing command does not abort a
# caller running under set -e before the failure block prints.
run_logged() {
    _log_write "RUN $*"
    _rl_out=$(mktemp "${TMPDIR:-/tmp}/helix-run.XXXXXX") || { "$@" && return 0 || return $?; }
    _step_redraw
    "$@" > "$_rl_out" 2>&1 && _rl_rc=0 || _rl_rc=$?
    if [ -n "$INSTALL_LOG" ]; then
        cat "$_rl_out" >> "$INSTALL_LOG" 2>/dev/null || true
        # Command substitution drops a trailing newline, so a non-empty result
        # means the output ended mid-line; terminate it before the next entry.
        if [ -s "$_rl_out" ] && [ -n "$(tail -c 1 "$_rl_out")" ]; then
            printf '\n' >> "$INSTALL_LOG" 2>/dev/null || true
        fi
    else
        _LOG_BUFFER="${_LOG_BUFFER}$(cat "$_rl_out")
"
    fi
    if [ "${HELIX_INSTALL_VERBOSE:-0}" = 1 ]; then
        while IFS= read -r _rl_line || [ -n "$_rl_line" ]; do _ui_emit "  $_rl_line"; done < "$_rl_out"
    fi
    if [ "$_rl_rc" -ne 0 ]; then print_failure "$*" "$_rl_rc" "$_rl_out"; fi
    rm -f "$_rl_out"
    return "$_rl_rc"
}

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
        pin_env_file
    fi

    # A ledger stop_competing_uis already wrote records a disable (chmod -x on
    # a stock UI's init script, a systemd unit taken down) that is still in
    # effect on the live system; losing the only copy leaves nothing for a
    # later uninstall to reverse it with (prestonbrown/helixscreen#1618). A
    # copy surviving anywhere but here is proof one was written, so carry it
    # forward the same way settings.json and helixscreen.env are above.
    local _ledger_restored=true
    if [ ! -f "${INSTALL_DIR}/config/.disabled_services" ] \
       && type _disabled_services_ledger_candidates >/dev/null 2>&1; then
        local _ledger_src=""
        local _ledger_candidate
        for _ledger_candidate in $(_disabled_services_ledger_candidates); do
            [ "$_ledger_candidate" = "${INSTALL_DIR}/config/.disabled_services" ] && continue
            if [ -f "$_ledger_candidate" ]; then
                _ledger_src="$_ledger_candidate"
                break
            fi
        done
        if [ -n "$_ledger_src" ]; then
            _ledger_restored=false
            if $(file_sudo "${INSTALL_DIR}/config") cp "$_ledger_src" "${INSTALL_DIR}/config/.disabled_services" 2>/dev/null; then
                log_success "Disabled-services ledger recovered from $_ledger_src"
                _ledger_restored=true
            else
                log_warn "Could not recover the disabled-services ledger from $_ledger_src"
            fi
        fi
    fi

    # The log lives in TMP_DIR until it is kept.
    finalize_install_log

    # Cleanup temporary files after restores are done
    if [ "$CLEANUP_TMP" = true ] && [ -d "$TMP_DIR" ]; then
        _safe_remove_tmp_dir "$TMP_DIR"
    fi

    echo ""
    log_error "Installation was NOT completed."
    if [ "$_ledger_restored" = true ]; then
        log_error "Your system should be in its original state."
    else
        log_error "A previously disabled system service could not be recorded for recovery."
        log_error "Re-run this script, or run it with --uninstall, to finish reversing it."
    fi
    echo ""
    log_info "For help, please:"
    log_info "  1. Check the error message above"
    log_info "  2. Verify network connectivity"
    log_info "  3. Report issues at: https://github.com/${GITHUB_REPO}/issues"
    echo ""

    exit $exit_code
}

# ---------------------------------------------------------------------------
# Filesystem measurement helpers
#
# Every free-space and same-filesystem question the installer asks goes
# through these two, so the df parse lives in one place.
#
# `-P` is load-bearing, not decoration: without it BusyBox df wraps a long
# device name onto its own line, so the last line's $1 is a BLOCK COUNT and its
# $4 is Use% ("44%"), which then fails every integer test. POSIX output is one
# line per filesystem.
#
# `-P` alone reports 512-byte blocks, so pair it with `-k` to get the 1K units
# the arithmetic below assumes. Verified on BusyBox 1.29.3 and 1.33.2.

# Echo the filesystem identity for a path (df's device column). Two paths with
# the same value are on one filesystem, so a mv between them is a rename.
_fs_id() {
    df -kP "$1" 2>/dev/null | tail -1 | awk '{print $1}'
}

# Echo free space in MB on the filesystem holding a path.
_fs_free_mb() {
    df -kP "$1" 2>/dev/null | tail -1 | awk '{print int($4/1024)}'
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
# in-app updater hands over via TMP_DIR (update_checker.cpp kStagingName).
# Mod-owned refusal is NOT here: common.sh is the bundle's first module and
# must not call into later ones, so that guard rides detect_tmp_dir's user
# override branch in platform.sh.
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
# Mod-owned refusal is NOT here (same reason as validate_tmp_dir above):
# it rides set_install_paths' final gate in platform.sh.
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
    # Mountpoint detection: the mount table and device ids always, mountpoint(1)
    # on top where it exists. A "mount" from either refuses; a dir nothing can
    # judge stays in place.
    local _mount_state _mp_rc=1
    _mount_state=$(_dir_mount_state "$d")
    if command -v mountpoint >/dev/null 2>&1; then
        mountpoint -q "$d" && _mp_rc=0 || _mp_rc=$?
        case "$_mp_rc" in
            0) _mount_state=mount ;;
            # util-linux answers "not a mountpoint" with 32. Any other status
            # is an error there, and BusyBox uses 1 for both, so it settles
            # nothing the table did not.
            32) [ "$_mount_state" = unknown ] && _mount_state=plain ;;
        esac
    fi
    case "$_mount_state" in
        plain) ;;
        mount)
            log_warn "Refusing to rm -rf mountpoint TMP_DIR='$d' (would wipe a live partition); leaving it in place."
            return 0
            ;;
        nested)
            log_warn "Refusing to rm -rf TMP_DIR='$d' (a filesystem is mounted inside it); leaving it in place."
            return 0
            ;;
        *)
            log_warn "Refusing to rm -rf TMP_DIR='$d' (cannot tell whether it is a mountpoint: no usable mountpoint, stat or mount table); leaving it in place."
            return 0
            ;;
    esac
    rm -rf "$d"
}

# Whether DIR is a mount point: prints "mount", "nested" when a filesystem is
# mounted somewhere below it, "plain", or "unknown" when nothing could judge
# it. Forge-X's BusyBox may lack stat, so the mount table is read first: DIR's
# physical path matched against field 2, which escapes space, tab and
# backslash as \040, \011 and \134. The device id of DIR against its parent
# backs it up where stat exists, and also catches a mount the table spells
# through another path. An empty or unreadable table judges nothing.
_dir_mount_state() {
    local d="$1" mounts="${HELIX_PROC_ROOT:-/proc}/mounts" known=0
    local phys="" esc="" lines=0 _dev _mnt _rest _ddev _pdev
    phys=$(cd "$d" 2>/dev/null && pwd -P) || phys=""
    if [ -n "$phys" ] && [ -r "$mounts" ]; then
        esc=$(printf '%s\n' "$phys" | sed -e 's/\\/\\134/g' -e 's/ /\\040/g' -e 's/	/\\011/g') || esc=""
    fi
    if [ -n "$esc" ]; then
        while read -r _dev _mnt _rest; do
            lines=$((lines + 1))
            case "$_mnt" in
                "$esc")
                    echo mount
                    return 0
                    ;;
                "$esc"/*)
                    echo nested
                    return 0
                    ;;
            esac
        done < "$mounts"
        [ "$lines" -gt 0 ] && known=1
    fi
    _ddev=$(stat -c '%d' "$d" 2>/dev/null) || _ddev=""
    _pdev=$(stat -c '%d' "$d/.." 2>/dev/null) || _pdev=""
    if [ -n "$_ddev" ] && [ -n "$_pdev" ]; then
        if [ "$_ddev" != "$_pdev" ]; then
            echo mount
            return 0
        fi
        known=1
    fi
    if [ "$known" = 1 ]; then echo plain; else echo unknown; fi
}

# Cleanup function for normal exit
cleanup_on_success() {
    if [ -d "$TMP_DIR" ]; then
        _safe_remove_tmp_dir "$TMP_DIR"
    fi
}

# Kill the process(es) running one exact executable path.
# A stock UI whose binary has a generic basename cannot go through
# kill_process_by_name: the QIDI Q2's stock screen is literally `client`, and
# `pidof client` on a general-purpose SBC matches whatever else answers to that
# name. Resolving /proc/<pid>/exe identifies the binary instead of trusting its
# name. SIGTERM first, then SIGKILL any survivor, matching the sibling above.
# Args: /absolute/path/to/binary
# Returns: 0 if any process was killed, 1 if none found
kill_process_by_path() {
    local target="$1"
    local killed_any=false
    local procdir pid exe

    [ -n "$target" ] || return 1

    for procdir in /proc/[0-9]*; do
        exe=$(readlink "$procdir/exe" 2>/dev/null) || continue
        [ "$exe" = "$target" ] || continue
        pid="${procdir#/proc/}"
        $SUDO kill "$pid" 2>/dev/null || true
        killed_any=true
    done

    [ "$killed_any" = true ] || return 1

    sleep 1
    for procdir in /proc/[0-9]*; do
        exe=$(readlink "$procdir/exe" 2>/dev/null) || continue
        [ "$exe" = "$target" ] || continue
        $SUDO kill -9 "${procdir#/proc/}" 2>/dev/null || true
    done

    return 0
}

# Kill process(es) by name — SIGTERM first, then SIGKILL any survivors.
# helix-watchdog and helix-screen catch SIGTERM but don't always exit (e.g.
# during splash handoff or when blocked on I/O), so the installer must
# escalate or uninstall leaves zombie processes behind.
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
# Retire the pre-settings.json rolling backup.
#
# Each backup tier (/var/lib/helixscreen via systemd StateDirectory, and
# $HOME/.helixscreen where there is none) holds three files. Two are current --
# settings.json.backup and helixscreen.env.backup -- and helixconfig.json.backup
# is the superseded one, kept only as the lowest-priority entry in Config::init's
# restore chain (legacy_config_backup_primary/fallback, include/app_constants.h).
#
# ONLY removed when settings.json.backup exists beside it. That is what makes the
# migration provably complete: the current backup is present, so the legacy file
# can no longer be the only thing standing between a user and their settings. On
# a machine old enough to have just the legacy file, it is left alone and the
# restore chain still finds it.
#
# Uninstall already takes these with the whole state dir (clean_helix_state_dirs
# below); this is the install/update path, where nothing swept them and the file
# sat on the smallest partition on the box indefinitely.
#
# Reads: KLIPPER_HOME, SUDO, HELIX_STATE_VAR_LIB, HELIX_STATE_ROOT_HOME
retire_legacy_config_backups() {
    local tier
    config_backup_tiers | while IFS= read -r tier; do
        [ -f "${tier}/helixconfig.json.backup" ] || continue
        # The gate: no current backup means the legacy file is still load-bearing.
        [ -f "${tier}/settings.json.backup" ] || continue

        if $SUDO rm -f "${tier}/helixconfig.json.backup" 2>/dev/null; then
            log_info "Removed superseded config backup: ${tier}/helixconfig.json.backup"
        fi
    done
}

# The directories Config::init searches for a rolling settings backup, one per
# line: the StateDirectory, then each HOME/.helixscreen the service may run with
# (config_backup_primary/fallback, include/app_constants.h). A desktop install
# has only its user's.
# Reads: platform, HOME, KLIPPER_HOME, HELIX_STATE_VAR_LIB, HELIX_STATE_ROOT_HOME
config_backup_tiers() {
    # The desktop app runs as the user, so its backup is that user's.
    if [ "${platform:-}" = "x86-desktop" ]; then
        [ -n "${HOME:-}" ] && echo "${HOME%/}/.helixscreen"
        return 0
    fi
    echo "${HELIX_STATE_VAR_LIB:-/var/lib/helixscreen}"
    echo "${HELIX_STATE_ROOT_HOME:-/root/.helixscreen}"
    [ -n "${KLIPPER_HOME:-}" ] && echo "${KLIPPER_HOME}/.helixscreen"
    return 0
}

# The rolling settings backup file names Config::init restores from, in its
# priority order (config_backup_search_paths, src/system/config.cpp).
config_backup_names() {
    echo "settings.json.backup"
    echo "helixconfig.json.backup"
}

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
