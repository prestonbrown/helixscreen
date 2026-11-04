#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where the [helix_print] section lives in a Moonraker config, and how this
# host reaches Moonraker over HTTP: shared by the plugin's install.sh and the
# main installer (bundled into it from here), so both find the section in the
# same files and talk to Moonraker the same way.

[ -n "${_HELIX_PRINT_CONF_SOURCED:-}" ] && return 0
_HELIX_PRINT_CONF_SOURCED=1

# Print every Moonraker config file reachable from @1 through [include] lines,
# @1 first, one per line, each once. A relative include resolves against the
# directory of the file that names it, as the path was given (a symlinked
# moonraker.conf resolves against the link's directory). A glob expands the way
# Moonraker expands it. @2, when set, is prefixed to an absolute include: the
# root Moonraker runs under, seen from here. A file is walked once however
# many paths lead to it, so an include cycle ends. Paths holding blanks are
# not supported; no Moonraker layout uses them.
moonraker_conf_files() {
    _mcf_root="${2:-}"
    _mcf_todo="$1"
    _mcf_seen=" "
    while [ -n "$_mcf_todo" ]; do
        _mcf_file="${_mcf_todo%% *}"
        case "$_mcf_todo" in
            *" "*) _mcf_todo="${_mcf_todo#* }" ;;
            *) _mcf_todo="" ;;
        esac
        [ -n "$_mcf_file" ] || continue
        [ -f "$_mcf_file" ] || continue
        _mcf_key=$(readlink -f "$_mcf_file" 2>/dev/null) || _mcf_key=""
        [ -n "$_mcf_key" ] || _mcf_key="$_mcf_file"
        case "$_mcf_seen" in *" $_mcf_key "*) continue ;; esac
        _mcf_seen="$_mcf_seen$_mcf_key "
        printf '%s\n' "$_mcf_file"
        _mcf_dir=$(dirname "$_mcf_file")
        # Trailing blanks after "]" include the CR of a file saved on Windows.
        _mcf_incs=$(sed -n 's/^\[include[[:space:]]\{1,\}\([^]]*[^][:space:]]\)[[:space:]]*\][[:space:]]*$/\1/p' "$_mcf_file" 2>/dev/null)
        while IFS= read -r _mcf_pat; do
            [ -n "$_mcf_pat" ] || continue
            case "$_mcf_pat" in
                /*) _mcf_pat="$_mcf_root$_mcf_pat" ;;
                *) _mcf_pat="$_mcf_dir/$_mcf_pat" ;;
            esac
            # Unquoted on purpose: the pattern expands here, against its own directory.
            for _mcf_inc in $_mcf_pat; do
                if [ -f "$_mcf_inc" ]; then
                    _mcf_todo="${_mcf_todo:+$_mcf_todo }$_mcf_inc"
                fi
            done
        done <<MCF_INCLUDES
$_mcf_incs
MCF_INCLUDES
    done
    return 0
}

# Print each file in moonraker_conf_files @1 @2 holding a line that matches
# extended regex @3, one per line; nothing when none does.
helix_conf_files_matching() {
    moonraker_conf_files "$1" "${2:-}" | while IFS= read -r _hcm_file; do
        if grep -Eq "$3" "$_hcm_file" 2>/dev/null; then
            printf '%s\n' "$_hcm_file"
        fi
    done
    return 0
}

# Print the port Moonraker serves HTTP on: [server] port in the first of
# moonraker_conf_files @1 @2 that sets it, else 7125, Moonraker's default.
# Firmware moves it (some serve Moonraker on port 80).
moonraker_conf_port() {
    _mcp_port=$(moonraker_conf_files "$1" "${2:-}" | while IFS= read -r _mcp_file; do
        awk '/^\[/ { s = ($0 ~ /^\[server\][ \t\r]*$/) }
            s && /^port[ \t]*[:=]/ { sub(/^port[ \t]*[:=][ \t]*/, ""); if (match($0, /^[0-9]+/)) print substr($0, 1, RLENGTH); exit }' \
            "$_mcp_file" 2>/dev/null
    done | awk 'NR == 1')
    printf '%s' "${_mcp_port:-7125}"
}

# Print the URL Moonraker answers at from this host: MOONRAKER_URL when set,
# else loopback on moonraker_conf_port @1 @2.
moonraker_local_url() {
    if [ -n "${MOONRAKER_URL:-}" ]; then
        printf '%s' "$MOONRAKER_URL"
        return 0
    fi
    printf 'http://127.0.0.1:%s' "$(moonraker_conf_port "$1" "${2:-}")"
}

# The awk program that reads a process's argv, one element per line, and
# prints what makes it the python program named by the awk variable `name`:
# the <name>.py it runs, or "-m" and the module it runs (<name>, or
# <name>.<name> as in python3 -m moonraker.moonraker on the CC1), joined
# (-m<name>) or not. The program is a python interpreter (argv[0] python,
# python3, python3.11, ...); an init script, editor or log viewer whose argv
# names the file comes and goes beside it and is not the program.
# scripts/helix-launcher.sh looks for the same two shapes by substring, the
# module only in its spaced form. Prints nothing for any other process.
# shellcheck disable=SC2016  # awk's own $
HELIX_PYTHON_ARGV_AWK='
    function is_module(m) { return m == name || m == name "." name }
    NR == 1 { n = split($0, part, "/"); if (part[n] !~ /^python[0-9.]*$/) exit; next }
    module { if (is_module($0)) print "-m " $0; exit }
    $0 == "-m" { module = 1; next }
    /^-m/ { if (is_module(substr($0, 3))) print "-m " substr($0, 3); exit }
    $0 ~ ("(^|/)" name "\\.py$") { print; exit }'

# Print one line per running process of the python program $1 (klippy,
# moonraker): its /proc/<pid> directory under HELIX_PROC_ROOT (tests point it
# at a fixture tree), else /proc, a blank, and what HELIX_PYTHON_ARGV_AWK
# prints for it. Prints nothing when none runs.
running_python_module() {
    _rpm_root="${HELIX_PROC_ROOT:-/proc}"
    for _rpm_f in "$_rpm_root"/[0-9]*/cmdline; do
        [ -r "$_rpm_f" ] || continue
        _rpm_script=$(tr '\000' '\n' < "$_rpm_f" 2>/dev/null | awk -v name="$1" "$HELIX_PYTHON_ARGV_AWK")
        [ -n "$_rpm_script" ] || continue
        printf '%s %s\n' "$(dirname "$_rpm_f")" "$_rpm_script"
    done
    return 0
}

# Print one line per running Moonraker (running_python_module).
running_moonrakers() {
    running_python_module moonraker
}

# Print the first running Moonraker's /proc/<pid> directory and, on the next
# line, what it runs: its moonraker.py, or "-m" and its module
# (running_moonrakers). Returns 1, printing nothing, when no Moonraker runs.
running_moonraker_process() {
    _rmp_first=$(running_moonrakers | sed -n 1p)
    [ -n "$_rmp_first" ] || return 1
    printf '%s\n%s\n' "${_rmp_first%% *}" "${_rmp_first#* }"
}

# Print the running Moonraker's /proc directory (running_moonraker_process).
running_moonraker_proc() {
    _rmq_proc=$(running_moonraker_process) || return 1
    printf '%s' "${_rmq_proc%%
*}"
}

# Print the running Moonraker's argv, one argument per line.
running_moonraker_args() {
    _rmg_proc=$(running_moonraker_proc) || return 1
    tr '\000' '\n' < "$_rmg_proc/cmdline" 2>/dev/null
}

# Print where the running Moonraker's "/" is from here: empty when it shares
# ours or none runs, the chroot's directory when it runs in one (Forge-X runs
# Moonraker in /data/.mod/.forge-x). Every path in its argv and every absolute
# [include] it reads is relative to that.
running_moonraker_root() {
    _rmd_proc=$(running_moonraker_proc) || return 0
    _rmd_root=$(readlink "$_rmd_proc/root" 2>/dev/null) || return 0
    [ "$_rmd_root" = "/" ] && return 0
    printf '%s' "${_rmd_root%/}"
}

# Print the value the running Moonraker's argv gives option @1 (its short
# form) or @2 (its long form): the next argument, or what follows "=" or the
# short flag in the same one. A long form may be abbreviated, as Python's
# argparse accepts. Empty when no Moonraker runs or neither form appears.
running_moonraker_arg() {
    running_moonraker_args | awk -v s="$1" -v l="$2" '
        function is_long(a) { return length(a) > 2 && index(l, a) == 1 }
        take { print; exit }
        $0 == s || is_long($0) { take = 1; next }
        /^--[^=]+=/ { eq = index($0, "="); if (is_long(substr($0, 1, eq - 1))) { print substr($0, eq + 1); exit } }
        length($0) > 2 && substr($0, 1, 2) == s { print substr($0, 3); exit }'
}

# Print the moonraker.conf the running Moonraker loads, as a path from here:
# the file its -c names, else config/moonraker.conf under the data path its -d
# names, whichever is first a file. Paths are absolute in its root; a relative
# one names nothing from here and is skipped. Empty when no Moonraker runs,
# when it names neither (it then loads its user's ~/printer_data, which a
# caller finds by home), or when more than one runs: several instances
# (KIAUH printer_1_data, printer_2_data) list in no stable order, so the
# caller's own lookup decides. On Creality K2 this is the only way to the
# answer: procd starts Moonraker with -c /usr/share/moonraker/moonraker.conf.
running_moonraker_conf() {
    running_moonrakers | awk 'END { exit NR != 1 }' || return 0
    _rmc_root=$(running_moonraker_root)
    _rmc_data=$(running_moonraker_arg -d --datapath)
    for _rmc_conf in "$(running_moonraker_arg -c --configfile)" \
                     "${_rmc_data:+${_rmc_data%/}/config/moonraker.conf}"; do
        case "$_rmc_conf" in
            /*) ;;
            *) continue ;;
        esac
        if [ -f "$_rmc_root$_rmc_conf" ]; then
            printf '%s' "$_rmc_root$_rmc_conf"
            return 0
        fi
    done
    return 0
}

# Print the running Moonraker's effective uid, then its groups (primary
# first), space-separated. Empty when no Moonraker runs here.
running_moonraker_ids() {
    _rmi_proc=$(running_moonraker_proc) || return 0
    awk '/^Uid:/ { u = $3 } /^Gid:/ { g = $3 }
        /^Groups:/ { for (i = 2; i <= NF; i++) gs = gs " " $i }
        END { if (u != "") print u " " g gs }' "$_rmi_proc/status" 2>/dev/null || true
}

# The same probe as _has_real_curl in scripts/lib/installer/release.sh and
# moonraker-plugin/remote-install.sh, which run where this file is not sourced.
# Detect whether `curl` is curl proper, with the flags we pass it. Creality K1
# firmware ships libhv's example client as /usr/bin/curl: it answers to the name,
# prints "curl version 1.0.0" and rejects -s, -S, -L, -o and -f. Real curl's first
# --version output starts "curl <digit>...". Shell builtins only, so it holds on
# a PATH with no head or grep. Cache the result so we only probe once.
_REAL_CURL=""
_has_real_curl() {
    if [ -z "$_REAL_CURL" ]; then
        _REAL_CURL=no
        if command -v curl >/dev/null 2>&1; then
            case "$(curl --version 2>/dev/null)" in
                "curl "[0-9]*) _REAL_CURL=yes ;;
            esac
        fi
    fi
    [ "$_REAL_CURL" = "yes" ]
}

# Run wget with arguments @..., once and with its messages in English, which
# wget_error_status reads. GNU wget retries a failed request up to 20 times
# by default, a POST included, so it is told --tries=1; a BusyBox wget never
# retries, and one built without long options refuses the flag, so it is
# not. Whether wget is GNU's is probed once, like _REAL_CURL.
_GNU_WGET=""
helix_wget() {
    if [ -z "$_GNU_WGET" ]; then
        _GNU_WGET=no
        case "$(wget --version 2>/dev/null)" in
            "GNU Wget"*) _GNU_WGET=yes ;;
        esac
    fi
    if [ "$_GNU_WGET" = yes ]; then
        LC_ALL=C LANG=C wget --tries=1 "$@"
    else
        LC_ALL=C LANG=C wget "$@"
    fi
}

# The python an HTTP request falls back on when this host has neither real
# curl nor wget (Creality K2 firmware ships neither): python3, else python,
# whichever imports urllib.request in isolated mode (-I, Python 3.4 on), which
# leaves the current directory and PYTHON* variables out of its imports.
# Cached like _REAL_CURL.
_HTTP_PY=""
_HTTP_PY_PROBED=""
_has_http_python() {
    if [ -z "$_HTTP_PY_PROBED" ]; then
        _HTTP_PY_PROBED=1
        for _hhp_cand in python3 python; do
            if command -v "$_hhp_cand" >/dev/null 2>&1 &&
                "$_hhp_cand" -I -c 'import urllib.request' >/dev/null 2>&1; then
                _HTTP_PY="$_hhp_cand"
                break
            fi
        done
    fi
    [ -n "$_HTTP_PY" ]
}

# Request URL @2 with method @1 (POST sends an empty body) through
# _has_http_python's python, waiting at most @3 seconds. Prints the HTTP
# status on the first line and the body after it, an error status included.
# Fails when nothing answered.
python_http() {
    _has_http_python || return 1
    HELIX_HTTP_METHOD="$1" HELIX_HTTP_URL="$2" HELIX_HTTP_TIMEOUT="$3" "$_HTTP_PY" -I - <<'PYEOF'
import os, sys, urllib.error, urllib.request
method = os.environ["HELIX_HTTP_METHOD"]
try:
    req = urllib.request.Request(os.environ["HELIX_HTTP_URL"],
                                 data=b"" if method == "POST" else None, method=method)
    with urllib.request.urlopen(req, timeout=float(os.environ["HELIX_HTTP_TIMEOUT"])) as resp:
        code, body = resp.status, resp.read()
except urllib.error.HTTPError as err:
    code, body = err.code, b""
except Exception:
    sys.exit(1)
sys.stdout.write("%d\n" % code)
sys.stdout.flush()
sys.stdout.buffer.write(body)
PYEOF
}

# Request URL @2 with method @1 (GET, or POST with an empty body), waiting at
# most @3 seconds: real curl, then wget, and python only when neither is
# here. BusyBox systems (Forge-X outside its chroot) carry wget but no curl, a
# K1's /usr/bin/curl is an impostor that rejects -s, and a K2 has python3
# alone. Sets HTTP_STATUS to the status the server answered with, empty when
# nothing answered, and HTTP_BODY to the body of a 2xx answer. Returns 0 for
# a 2xx answer. wget reports no status of its own: a success is taken as
# 200, and a failure's status is read from its error message. One request
# lasts @3 seconds at most through curl, and twice that through wget or
# python, whose limit holds for the connect and again for each read.
moonraker_request() {
    HTTP_STATUS=""
    HTTP_BODY=""
    if _has_real_curl; then
        _mrq_out=$(curl -s --max-time "$3" -X "$1" -w '\n%{http_code}' "$2" 2>/dev/null) || true
        HTTP_STATUS="${_mrq_out##*
}"
        HTTP_BODY="${_mrq_out%
*}"
    elif command -v wget >/dev/null 2>&1; then
        if [ "$1" = POST ]; then
            if _mrq_out=$(helix_wget -T "$3" -O /dev/null --post-data='' "$2" 2>&1); then
                HTTP_STATUS=200
            else
                HTTP_STATUS=$(printf '%s\n' "$_mrq_out" | wget_error_status)
            fi
        else
            # A failed GET is asked again for its status, unless the first
            # try already used its time: nothing answered then.
            _mrq_t0=$(helix_now)
            if HTTP_BODY=$(helix_wget -q -T "$3" -O - "$2" 2>/dev/null); then
                HTTP_STATUS=200
            else
                HTTP_BODY=""
                [ "$(($(helix_now) - _mrq_t0))" -ge "$3" ] ||
                    HTTP_STATUS=$(helix_wget -T "$3" -O /dev/null "$2" 2>&1 | wget_error_status)
            fi
        fi
    elif _mrq_out=$(python_http "$1" "$2" "$3"); then
        HTTP_STATUS="${_mrq_out%%
*}"
        case "$_mrq_out" in
            *"
"*) HTTP_BODY="${_mrq_out#*
}" ;;
        esac
    fi
    case "$HTTP_STATUS" in
        [1-5][0-9][0-9]) ;;
        *) HTTP_STATUS="" ;;
    esac
    case "$HTTP_STATUS" in
        2[0-9][0-9]) return 0 ;;
    esac
    HTTP_BODY=""
    return 1
}

# Print the HTTP status in the wget error output on stdin: BusyBox's "server
# returned error: HTTP/1.1 401 Unauthorized", GNU's "awaiting response... 401
# Unauthorized" or "ERROR 404: Not Found". Nothing when no server answered.
wget_error_status() {
    awk 'match($0, /(HTTP\/[0-9.]+|response\.\.\.|ERROR) [1-5][0-9][0-9]/) {
        s = substr($0, RSTART, RLENGTH); print substr(s, length(s) - 2); exit }'
}

# Print the body of a GET of URL @1, waiting at most @2 seconds (default 2).
# Fails on anything but a 2xx answer.
moonraker_get() {
    moonraker_request GET "$1" "${2:-2}" || return 1
    printf '%s' "$HTTP_BODY"
}

# Whether a GET of URL @1 succeeds within @2 seconds (default 2).
moonraker_answers() {
    moonraker_get "$1" "${2:-}" > /dev/null
}

# Print the HelixPrint plugin version in the /server/helix/status body on
# stdin; nothing when it names none.
plugin_version_in() {
    awk 'match($0, /"version"[[:space:]]*:[[:space:]]*"[^"]*"/) {
        s = substr($0, RSTART, RLENGTH); sub(/^"version"[[:space:]]*:[[:space:]]*"/, "", s)
        sub(/"$/, "", s); print s; exit }'
}

# Print the HelixPrint plugin version the Moonraker at base URL @1 runs, from
# /server/helix/status, waiting at most @2 seconds (default 5); nothing when
# it runs none or does not say.
moonraker_plugin_version() {
    moonraker_get "$1/server/helix/status" "${2:-5}" | plugin_version_in
}

# Print the moonraker_restart mode that has the Moonraker at base URL @1 run
# plugin version @2: "load" when it runs no plugin, as an in-place restart
# imports one it has not; "plugin" when it runs another version, which only
# a new process reads; "none" when it runs @2 already, or any version with
# @2 empty.
# UNCALLED_OK: moonraker-plugin/install.sh calls it; the main installer never
# restarts Moonraker for the plugin.
plugin_restart_mode() {
    _prm_running=$(moonraker_plugin_version "$1")
    if [ -z "$_prm_running" ]; then
        printf 'load'
    elif [ -n "${2:-}" ] && [ "$_prm_running" != "$2" ]; then
        printf 'plugin'
    else
        printf 'none'
    fi
}

# Print the name of the service the Moonraker at base URL @1 runs under, the
# one /machine/services/restart restarts it by, from /machine/system_info:
# its instance_ids.moonraker, when available_services lists it too, so its
# service manager can act on it. Moonraker reports "moonraker" when it cannot
# tell, so that name beside another moonraker unit names nothing for sure.
# Fails, printing nothing, when the name is not certain.
moonraker_service_name() {
    _msn_body=$(moonraker_get "$1/machine/system_info" 2 | tr -d '\n') || return 1
    _msn_name=$(printf '%s' "$_msn_body" | awk 'match($0, /"instance_ids"[[:space:]]*:[[:space:]]*\{[^}]*\}/) {
        s = substr($0, RSTART, RLENGTH)
        if (match(s, /"moonraker"[[:space:]]*:[[:space:]]*"[^"]*"/)) {
            s = substr(s, RSTART, RLENGTH); sub(/^"moonraker"[[:space:]]*:[[:space:]]*"/, "", s)
            sub(/"$/, "", s); print s } }')
    [ -n "$_msn_name" ] || return 1
    _msn_listed=$(printf '%s' "$_msn_body" | awk -v name="$_msn_name" '
        match($0, /"available_services"[[:space:]]*:[[:space:]]*\[[^]]*\]/) {
            s = substr($0, RSTART, RLENGTH); sub(/^[^[]*\[/, "", s); sub(/\]$/, "", s)
            n = split(s, a, ",")
            for (i = 1; i <= n; i++) {
                gsub(/^[[:space:]]*"|"[[:space:]]*$/, "", a[i])
                if (a[i] == name) found = 1
                else if (a[i] ~ /^moonraker/) other = 1
            } }
        END { if (found && !(name == "moonraker" && other)) print "yes" }')
    [ "$_msn_listed" = yes ] || return 1
    printf '%s' "$_msn_name"
}

# Print the seconds since boot, from /proc/uptime (HELIX_UPTIME_FILE, where
# tests keep a clock), else the wall clock's seconds. Every wait here runs on
# this clock, so a probe that takes seconds of its own counts against the wait
# instead of stretching it, and a clock set back mid-wait does not stretch it.
helix_now() {
    _hn_up=$(awk '{ print int($1) }' "${HELIX_UPTIME_FILE:-/proc/uptime}" 2>/dev/null) || _hn_up=""
    case "$_hn_up" in
        '' | *[!0-9]*) date +%s ;;
        *) printf '%s\n' "$_hn_up" ;;
    esac
}

# Print what the Moonraker at base URL @1 reports of a print: "active" for one
# printing or paused, "idle" for none, "unknown" when it cannot be asked.
# print_stats is asked every time, right before a restart. HELIX_JOB_STATE
# (idle or active, from the app) stands in for it only when that query gets
# a 401 or no answer; an "active" one holds the restart either way. @2, the
# body of /server/info, naming a Klipper that is not ready means no print
# runs.
moonraker_job_state() {
    if [ "${HELIX_JOB_STATE:-}" = active ]; then
        printf 'active'
        return 0
    fi
    if printf '%s' "${2:-}" | grep -q '"klippy_state"' &&
        ! printf '%s' "${2:-}" | grep -Eq '"klippy_state"[[:space:]]*:[[:space:]]*"ready"'; then
        printf 'idle'
        return 0
    fi
    if moonraker_request GET "$1/printer/objects/query?print_stats=state" 5; then
        if printf '%s' "$HTTP_BODY" | grep -Eq '"state"[[:space:]]*:[[:space:]]*"(printing|paused)"'; then
            printf 'active'
        else
            printf 'idle'
        fi
    elif [ "${HELIX_JOB_STATE:-}" = idle ] && { [ -z "$HTTP_STATUS" ] || [ "$HTTP_STATUS" = 401 ]; }; then
        printf 'idle'
    else
        printf 'unknown'
    fi
}

# Exit codes of moonraker_restart, which install.sh --restart-moonraker exits
# with and HelixPluginInstaller reads.
HELIX_RESTART_DONE=0
HELIX_RESTART_MANUAL=3
HELIX_RESTART_HELD=4
HELIX_RESTART_UNCHECKED=6
HELIX_RESTART_DOWN=7

# Restart the Moonraker at base URL @2 over its HTTP API, the only way any
# caller here restarts it: no service manager, init script or process is
# touched, so it runs the same under sudo, NoNewPrivileges, a chroot or a
# vendor firmware. @1 is what changed:
#   config  the config (an install, an uninstall, an updater section):
#           POST /server/restart, Moonraker's in-place restart, which reads
#           the config again
#   load    helix_print.py where Moonraker runs no plugin: POST
#           /server/restart, which imports it, and the restart counts once
#           /server/helix/status reports version @5 (with @5 empty, any)
#   plugin  helix_print.py over a plugin Moonraker already loaded: an
#           in-place restart keeps the module it imported, so POST
#           /machine/services/restart has Moonraker restart its own service
#           (moonraker_service_name; none when that is not certain) with its
#           own rights, and the restart counts once /server/helix/status
#           reports version @5. With @5 empty, once Moonraker has gone
#           quiet and reports one again.
# plugin_restart_mode says which of load and plugin a plugin change needs.
# @3 and @4 are called with each line to report, as information and as a
# warning. A Moonraker that does not answer (a 401 answers) is left alone. A
# print printing or paused holds the restart off (moonraker_job_state); one
# that cannot be ruled out holds it off too. After the request, one wait of
# HELIX_MOONRAKER_UP_SECS (60: a slow board answers 10 to 40 seconds after a
# restart) for Moonraker to come back. Returns a HELIX_RESTART_* code: DONE;
# MANUAL when Moonraker refused the request, did not restart, did not load
# the plugin or still runs the old one; HELD for a print; UNCHECKED when the
# print state cannot be read; DOWN when Moonraker does not answer, before the
# request or after it.
#
# Its worst case, with every request running to its limit and each limit
# doubled for wget or python (moonraker_request): /server/info 4s, the print
# state 10s, the service name 4s, the request 10s, the 60s wait by the
# clock, one more probe and sleep past its end 5s, and the closing
# /server/info 4s: 97 seconds. install.sh --restart-moonraker asks for the
# running plugin version first (plugin_restart_mode, 10s): 107 seconds,
# against the app's kRestartTimeoutSeconds of 180
# (src/ui/plugin/plugin_self_heal.cpp).
moonraker_restart() {
    _mrs_mode="$1"
    _mrs_url="$2"
    _mrs_info="$3"
    _mrs_warn="$4"
    _mrs_version="${5:-}"
    _mrs_secs="${HELIX_MOONRAKER_UP_SECS:-60}"
    if [ "$_mrs_mode" = plugin ]; then
        _mrs_redo="Restart Moonraker or reboot the printer to finish the plugin update; the plugin it runs keeps working until then"
    elif [ "$_mrs_mode" = load ]; then
        _mrs_redo="Restart Moonraker or reboot the printer to load the HelixPrint plugin"
    else
        _mrs_redo="Restart Moonraker from Mainsail or Fluidd, or reboot the printer, for the change to take effect"
    fi

    moonraker_request GET "$_mrs_url/server/info" 2 || :
    if [ -z "$HTTP_STATUS" ]; then
        "$_mrs_warn" "Moonraker is not running; start it and the change takes effect"
        return "$HELIX_RESTART_DOWN"
    fi
    case "$(moonraker_job_state "$_mrs_url" "$HTTP_BODY")" in
        idle) ;;
        active)
            "$_mrs_warn" "A print is running, so Moonraker was not restarted; it needs a restart after the print for the change to take effect"
            return "$HELIX_RESTART_HELD"
            ;;
        *)
            if [ "$HTTP_STATUS" = 401 ]; then
                "$_mrs_warn" "Moonraker requires a login, so this cannot check for a running print and did not restart it"
            else
                "$_mrs_warn" "Moonraker did not report its print state, so this cannot check for a running print and did not restart it"
            fi
            "$_mrs_warn" "$_mrs_redo"
            return "$HELIX_RESTART_UNCHECKED"
            ;;
    esac

    if [ "$_mrs_mode" = plugin ]; then
        if ! _mrs_service=$(moonraker_service_name "$_mrs_url"); then
            "$_mrs_warn" "Moonraker does not say which service it runs as, so it was not restarted"
            "$_mrs_warn" "$_mrs_redo"
            return "$HELIX_RESTART_MANUAL"
        fi
        _mrs_path="/machine/services/restart?service=$_mrs_service"
    else
        _mrs_path="/server/restart"
    fi
    if ! moonraker_request POST "$_mrs_url$_mrs_path" 5; then
        "$_mrs_warn" "Moonraker refused to restart (${HTTP_STATUS:-no answer})"
        "$_mrs_warn" "$_mrs_redo"
        return "$HELIX_RESTART_MANUAL"
    fi
    case "$_mrs_mode" in
        plugin) "$_mrs_info" "Asked Moonraker to restart its service to load the new plugin" ;;
        load) "$_mrs_info" "Asked Moonraker to restart itself to load the plugin" ;;
        *) "$_mrs_info" "Asked Moonraker to restart itself" ;;
    esac

    # Each wait ends on its time by the clock, and after as many one-second
    # sleeps as it has seconds whatever the clock says.
    _mrs_end=$(($(helix_now) + _mrs_secs))
    # The instance that accepted an in-place restart answers for a moment
    # after it, and may miss a probe while busy stopping; it is gone once two
    # probes in a row go unanswered, or after 10 seconds.
    if [ "$_mrs_mode" = config ]; then
        _mrs_gone=$(($(helix_now) + 10))
        _mrs_n=0
        _mrs_down=0
        while [ "$_mrs_down" -lt 2 ] && [ "$_mrs_n" -lt 10 ] && [ "$(helix_now)" -lt "$_mrs_gone" ]; do
            moonraker_request GET "$_mrs_url/server/info" 2 || :
            if [ -z "$HTTP_STATUS" ]; then
                _mrs_down=$((_mrs_down + 1))
            else
                _mrs_down=0
            fi
            sleep 1
            _mrs_n=$((_mrs_n + 1))
        done
    fi
    # A plugin restart counts once the plugin reports the version wanted. The
    # instance that took the request reports the old one, or none, so it is
    # never taken for the new; with no version wanted, Moonraker has to go
    # quiet first. Going quiet also tells a service restart that happened
    # from one Moonraker accepted and never carried out.
    _mrs_n=0
    _mrs_quiet=""
    while :; do
        if [ "$_mrs_mode" = config ]; then
            moonraker_request GET "$_mrs_url/server/info" 2 || :
            [ -z "$HTTP_STATUS" ] || break
        else
            moonraker_request GET "$_mrs_url/server/helix/status" 2 || :
            _mrs_now=$(printf '%s' "$HTTP_BODY" | plugin_version_in)
            if [ -n "$_mrs_version" ]; then
                [ "$_mrs_now" != "$_mrs_version" ] || break
            elif [ -n "$_mrs_now" ] && { [ "$_mrs_mode" = load ] || [ -n "$_mrs_quiet" ]; }; then
                break
            fi
        fi
        [ -n "$HTTP_STATUS" ] || _mrs_quiet=1
        if [ "$_mrs_n" -ge "$_mrs_secs" ] || [ "$(helix_now)" -ge "$_mrs_end" ]; then
            moonraker_request GET "$_mrs_url/server/info" 2 || :
            if [ -z "$HTTP_STATUS" ]; then
                "$_mrs_warn" "Moonraker has not answered within ${_mrs_secs} seconds of its restart; check that it is running"
                return "$HELIX_RESTART_DOWN"
            fi
            if [ "$_mrs_mode" = load ]; then
                "$_mrs_warn" "Moonraker restarted but did not load the HelixPrint plugin${_mrs_version:+ $_mrs_version}; moonraker.log says why"
            elif [ -z "$_mrs_quiet" ]; then
                "$_mrs_warn" "Moonraker did not restart: its service restart is not available on this host"
            else
                "$_mrs_warn" "Moonraker still runs the old HelixPrint plugin after its service restart"
            fi
            "$_mrs_warn" "$_mrs_redo"
            return "$HELIX_RESTART_MANUAL"
        fi
        sleep 1
        _mrs_n=$((_mrs_n + 1))
    done
    "$_mrs_info" "Moonraker is ready"
    return "$HELIX_RESTART_DONE"
}

# Section headers this plugin owns in a Moonraker config: its own section, and
# the [update_manager helix_print] a git-clone install of the plugin wrote.
# shellcheck disable=SC2034  # read by the scripts that source this file
HELIX_PRINT_SECTION_RE='^\[helix_print\]'
HELIX_PRINT_UPDATER_RE='^\[update_manager helix_print\]'
HELIX_PRINT_OWNED_RE='^\[(helix_print|update_manager helix_print)\]'

# Print each file in moonraker_conf_files @1 @2 that holds a [helix_print]
# section, one per line; nothing when none does.
# UNCALLED_OK: moonraker-plugin/install.sh calls it; the main installer only
# removes sections, through helix_print_owned_files.
helix_print_section_files() {
    helix_conf_files_matching "$1" "${2:-}" "$HELIX_PRINT_SECTION_RE"
}

# Print each file in moonraker_conf_files @1 @2 that holds any section this
# plugin owns, one per line: what an uninstall has to clean.
helix_print_owned_files() {
    helix_conf_files_matching "$1" "${2:-}" "$HELIX_PRINT_OWNED_RE"
}

# Replace config file @1 with the output of awk program @2 run over it, each
# command run under prefix @3 when set (the main installer's sudo). The new
# text is written beside the real file (a symlinked config is followed and
# stays a symlink) into a copy that keeps its mode and owner, then renamed over
# it, so a write that fails part-way leaves the original whole. BusyBox awk
# exits 0 when a write fails, so the staged copy must also hold as many bytes
# as a second run of the program writes into a pipe, which no full disk cuts
# short. Returns 1 when the file could not be rewritten.
helix_rewrite_conf() {
    _rw_pre="${3:-}"
    _rw_real=$(readlink -f "$1" 2>/dev/null) || _rw_real=""
    [ -n "$_rw_real" ] || _rw_real="$1"
    _rw_tmp="$_rw_real.helixtmp.$$"
    # The program is an argument rather than part of the -c string, so its $0
    # and $1 stay awk's and are never expanded by a shell.
    # shellcheck disable=SC2016,SC2086  # the -c string's own $; a word-list prefix
    if $_rw_pre cp -p "$_rw_real" "$_rw_tmp" 2>/dev/null &&
        $_rw_pre sh -c 'awk "$1" "$2" > "$3" &&
            [ "$(awk "$1" "$2" | wc -c)" -eq "$(wc -c < "$3")" ]' sh "$2" "$_rw_real" "$_rw_tmp" &&
        $_rw_pre mv -f "$_rw_tmp" "$_rw_real"; then
        return 0
    fi
    # shellcheck disable=SC2086
    $_rw_pre rm -f "$_rw_tmp"
    return 1
}

# Print an awk program that prints a config file without the sections whose
# header matches extended regex @1 (anchored, bracketed: ^\[(a|b)\]). A run of
# such sections goes as one, along with the "#~# --- HelixPrint Plugin ---"
# line a git-clone install wrote above it. A run that reaches the end of the
# file also takes the one blank line before it, which is what an install writes
# ahead of the section it appends, so an install and uninstall leave the file as
# it was. A run between two other sections keeps that line: it separates them.
helix_strip_awk() {
    # shellcheck disable=SC2016  # awk's own $ fields
    printf '%s' '
    { line[NR] = $0 }
    END {
        skip = 0
        for (i = 1; i <= NR; i++) {
            if (line[i] ~ /'"$1"'/) {
                if (!skip) {
                    start = i
                    if (i > 1 && line[i - 1] == "#~# --- HelixPrint Plugin ---") { drop[i - 1] = 1; start = i - 1 }
                }
                skip = 1
            }
            else if (line[i] ~ /^\[/) { skip = 0 }
            if (skip) { drop[i] = 1 }
        }
        if (skip && start > 1 && line[start - 1] ~ /^[ \t\r]*$/) { drop[start - 1] = 1 }
        for (i = 1; i <= NR; i++) { if (!drop[i]) print line[i] }
    }
'
}

# Programs for helix_strip_awk: every section the plugin owns (uninstall), and
# only the git-clone install's updater entry (an install, whose link makes that
# entry follow a checkout Moonraker no longer loads).
# shellcheck disable=SC2034  # read by the scripts that source this file
HELIX_PRINT_STRIP_AWK=$(helix_strip_awk "$HELIX_PRINT_OWNED_RE")
# shellcheck disable=SC2034
HELIX_PRINT_UPDATER_STRIP_AWK=$(helix_strip_awk "$HELIX_PRINT_UPDATER_RE")
