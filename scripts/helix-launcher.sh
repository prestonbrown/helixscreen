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
#
# Environment variables:
#   HELIX_DATA_DIR=<d>   Override asset directory (ui_xml/, assets/, config/)
#   HELIX_LOG_LEVEL=<l>  Log level (preferred over HELIX_DEBUG)
#   HELIX_DEBUG=1        Same as --debug (legacy, use HELIX_LOG_LEVEL instead)
#   HELIX_LOG_DEST=<d>   Same as --log-dest (auto|journal|syslog|file|console)
#   HELIX_LOG_FILE=<f>   Same as --log-file
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
# whether to nice the UI down. Defined here, called late, so platform hooks
# and the init script's platform_wait_for_services have had time to bring
# Klipper / Moonraker up before we look for them.
helix_klipper_co_hosted() {
    if command -v pgrep >/dev/null 2>&1; then
        pgrep -f '[k]lippy\.py'    >/dev/null 2>&1 && return 0
        pgrep -f '[m]oonraker\.py' >/dev/null 2>&1 && return 0
    fi
    # Fallback for systems without pgrep -f: check default unix sockets.
    [ -S /tmp/klippy_uds ]      && return 0
    [ -S /tmp/moonraker.sock ]  && return 0
    return 1
}

# Stop firmware display-management services that conflict with HelixScreen.
# Creality SonicPad/Nebula Pad ships display-sleep.sh which polls X11 DPMS via
# xset. When X isn't running (fbdev mode), xset fails and the script interprets
# the empty response as "monitor Off", killing the backlight every 2 seconds.
if command -v systemctl >/dev/null 2>&1; then
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
# Development: launcher is in scripts/, binaries in build/bin/
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

# Select the appropriate binary: DRM (primary) or fbdev (fallback)
# Checks: env override → ldd shared lib resolution → default to primary
select_binary() {
    _sb_bin_dir=$1
    _sb_primary="${_sb_bin_dir}/helix-screen"
    _sb_fallback="${_sb_bin_dir}/helix-screen-fbdev"

    # No fallback available (non-Pi, dev builds)
    if [ ! -x "$_sb_fallback" ]; then
        echo "$_sb_primary"
        return
    fi

    # User forced fbdev via env — skip DRM entirely
    if [ "${HELIX_DISPLAY_BACKEND:-}" = "fbdev" ]; then
        echo "$_sb_fallback"
        return
    fi

    # Check if primary binary's shared libs are all resolvable
    if command -v ldd >/dev/null 2>&1; then
        if ldd "$_sb_primary" 2>/dev/null | grep -q "not found"; then
            echo "$_sb_fallback"
            return
        fi
    fi

    echo "$_sb_primary"
}

SPLASH_BIN="${BIN_DIR}/helix-splash"
WATCHDOG_BIN="${BIN_DIR}/helix-watchdog"
FALLBACK_BIN="${BIN_DIR}/helix-screen-fbdev"

# Derive the install root (parent of bin/)
INSTALL_DIR="$(cd "${BIN_DIR}/.." && pwd)"

# Ensure SSL certificate verification works for HTTPS requests (e.g., update checker).
# Static glibc builds embed OpenSSL with compiled-in cert paths from the Docker build
# container, which don't exist on the target device. Set SSL_CERT_FILE to a valid path.
if [ -z "${SSL_CERT_FILE:-}" ]; then
    for _cert_path in \
        /etc/ssl/certs/ca-certificates.crt \
        /etc/pki/tls/certs/ca-bundle.crt \
        /etc/ssl/cert.pem \
        "${INSTALL_DIR}/certs/ca-certificates.crt"; do
        if [ -f "$_cert_path" ]; then
            export SSL_CERT_FILE="$_cert_path"
            break
        fi
    done
    unset _cert_path
fi

# Source environment configuration file if present.
# Supports both installed (/etc/helixscreen/) and deployed (config/) locations.
# Variables already set in the environment take precedence — the env file only
# provides defaults for unset variables.
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

if [ -n "$_helix_env_file" ]; then
    # Read each VAR=value line; only export if not already set.
    # Tolerant of common typos so users don't get a silent no-op:
    #   - CRLF line endings (env file edited on Windows)
    #   - Leading/trailing whitespace
    #   - `export VAR=value` (bash habit)
    #   - `VAR = value` (spaces around the equals sign)
    # Malformed lines emit a stderr warning instead of being dropped silently.
    _lineno=0
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
                continue
                ;;
        esac
        _var="${_line%%=*}"
        case "$_var" in
            *[!A-Za-z0-9_]*)
                log "warning: ${_helix_env_file}:${_lineno}: invalid variable name '$_var'"
                continue
                ;;
        esac
        # Only set if not already in environment (systemd Environment= /
        # exported parent shell vars win over the file).
        eval "_existing=\"\${${_var}:-}\""
        if [ -z "$_existing" ]; then
            if ! eval "export $_line" 2>/dev/null; then
                log "warning: ${_helix_env_file}:${_lineno}: failed to export: $_line"
            fi
        fi
    done < "$_helix_env_file"
    unset _line _var _existing _lineno
fi
unset _helix_env_file

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

# AD5X (MIPS, ZMOD on Flashforge AD5X — Ingenic X2600).
if [ "$_arch" = "mips" ] && [ -d /usr/data ] && { [ -d /usr/prog ] || [ -f /ZMOD ]; }; then
    _enable_heap_diag=1
fi

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

# Default display backend based on which binary was selected.
# Only set explicitly when dual binaries exist (Pi with DRM+fbdev).
# Non-Pi platforms (AD5M, K1, etc.) have only one binary and the C++ code
# auto-detects the backend, so we leave the env var unset.
if [ -z "${HELIX_DISPLAY_BACKEND:-}" ]; then
    if [ "$(basename "${MAIN_BIN}")" = "helix-screen-fbdev" ]; then
        export HELIX_DISPLAY_BACKEND=fbdev
    elif [ -x "${FALLBACK_BIN}" ]; then
        # Dual-binary Pi: primary selected, use DRM
        export HELIX_DISPLAY_BACKEND=drm
    fi
fi

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
# script (S90helixscreen) also runs this on production boot, but dev deploys
# (`make deploy-*` → restart launcher directly) bypass init.d, so the
# launcher needs to fire it too — otherwise platform-specific setup like
# stopping the stock UI or loading the WiFi driver never happens during
# iterative testing. Calls are idempotent (active flag is just a touch,
# load functions check before acting), so production boot stays correct.
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
# MUST come after the platform-hooks block above. Six of the seven platform
# hooks export HELIX_LOG_DEST / HELIX_LOG_FILE from platform_pre_start
# (ad5m-zmod, ad5m-forgex, ad5m-kmod, k1, k2, cc1) to steer the app log onto a
# partition that is persistent AND captured by that firmware's log archiver.
# Resolving before platform_pre_start ran read an unset variable, so
# --log-dest/--log-file never reached the binary and the app log fell back to
# auto-detection (syslog on Linux). helixscreen.init happens to call
# platform_pre_start itself before exec'ing us, which masked this on a normal
# boot — but NOT for `make deploy-*` restarts, hand-started launchers, or any
# third-party init script that leaves the hook to us. See issue #1249.
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
    killall helix-watchdog helix-screen helix-screen-fbdev helix-splash 2>/dev/null || true
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
    log "Remote control enabled (HELIX_REMOTE_CONTROL=1)"
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

    # Runtime crash fallback: if DRM binary crashed and fbdev fallback exists, retry.
    if _is_crash_exit ${EXIT_CODE} && [ "$(basename "${MAIN_BIN}")" = "helix-screen" ] \
       && [ -x "${FALLBACK_BIN}" ]; then
        log "DRM binary exited with code ${EXIT_CODE}, retrying with fbdev fallback..."
        export HELIX_DISPLAY_BACKEND=fbdev
        if [ "${USE_WATCHDOG}" = "1" ]; then
            # shellcheck disable=SC2086
            "${WATCHDOG_BIN}" ${SPLASH_ARGS} -- \
                "${FALLBACK_BIN}" ${EXTRA_FLAGS} ${PASSTHROUGH_ARGS}
            EXIT_CODE=$?
        else
            # shellcheck disable=SC2086
            "${FALLBACK_BIN}" ${EXTRA_FLAGS} ${PASSTHROUGH_ARGS}
            EXIT_CODE=$?
        fi
        log "fbdev fallback exited with code ${EXIT_CODE}"
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
