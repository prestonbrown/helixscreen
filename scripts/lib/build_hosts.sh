#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where this machine's build and test hosts are configured: the one parser for
# that file. Scripts source it; make runs it (mk/remote.mk), and so do the bats
# suite's setup (for the key list).
#
#   ${XDG_CONFIG_HOME:-$HOME/.config}/helixscreen/build-hosts.env
#
# One file outside the tree, so every worktree and clone shares it.
# HELIX_BUILD_HOSTS_FILE names another. The rules:
#
#   KEY=VALUE      at the start of the line, no spaces around =. KEY must be in
#                  BUILD_HOSTS_KEYS; nothing else is ever set.
#   # comment      only as a whole line. A value holds no whitespace and no
#                  quotes, so "KEY=v # note" and KEY="v" are refused, not half-read.
#   literal        no $ expansion, nothing runs; a trailing \r is dropped.
#   last wins      when a key appears twice.
#   env first      a key already set in the environment keeps its value.
#   candidates     a host key (BUILD_HOSTS_HOST_KEYS) may name several hosts,
#                  comma-separated, best first: zeus.local,zeus.lan,192.168.1.58.
#                  The first that answers ssh wins (resolve_build_host); a
#                  single host is used as written and never probed.
#
# A line that breaks a rule is named on stderr and skipped. docs/devel/
# BUILD_SYSTEM.md "Using your own build/test host" says what each key does.
#
#   . scripts/lib/build_hosts.sh        sets the keys, environment first
#   scripts/lib/build_hosts.sh --keys   the allowlist, one per line
#   scripts/lib/build_hosts.sh --make K K's value for make, environment first, $ doubled;
#                                       a host key's candidates are resolved first

BUILD_HOSTS_KEYS="HELIX_TEST_HOST HELIX_TEST_CONTAINER HELIX_TEST_WORKDIR HELIX_TEST_TREES_HOST
HELIX_TEST_TREES HELIX_TEST_CCACHE HELIX_TEST_LOCK_DIR HELIX_TEST_HOST_AUTO
REMOTE_HOST REMOTE_USER REMOTE_DIR REMOTE_BUILD_DIR HELIX_LAND_GATE_HOSTS"

# The keys that name a host to ssh to, and so may hold a candidate list.
BUILD_HOSTS_HOST_KEYS="REMOTE_HOST HELIX_TEST_HOST"

# Seconds one candidate gets to answer, name lookup included: ssh's
# ConnectTimeout starts after the lookup, and an mDNS name off its own subnet
# can hang in the lookup for good.
BUILD_HOST_PROBE_SECS="${HELIX_HOST_PROBE_SECS:-5}"

BUILD_HOSTS_FILE="${HELIX_BUILD_HOSTS_FILE:-${XDG_CONFIG_HOME:-$HOME/.config}/helixscreen/build-hosts.env}"

# Prints "KEY VALUE" for each line the rules accept, in file order.
build_hosts_parse() {
    local line key val n=0 warn
    if [ ! -e "$BUILD_HOSTS_FILE" ]; then return 0; fi
    if [ ! -r "$BUILD_HOSTS_FILE" ]; then
        echo "⚠ cannot read $BUILD_HOSTS_FILE (check its permissions); no build hosts loaded" >&2
        return 0
    fi
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        line=${line%$'\r'}
        case "$line" in ''|'#'*) continue ;; esac
        warn="$BUILD_HOSTS_FILE:$n:"
        if [[ ! "$line" =~ ^([A-Za-z_][A-Za-z0-9_]*)=(.*)$ ]]; then
            echo "⚠ $warn not KEY=VALUE, skipped" >&2; continue
        fi
        key=${BASH_REMATCH[1]} val=${BASH_REMATCH[2]}
        case " ${BUILD_HOSTS_KEYS//$'\n'/ } " in
            *" $key "*) ;;
            *) echo "⚠ $warn $key is not a build-hosts key, skipped" >&2; continue ;;
        esac
        case "$val" in
            *[[:space:]]*) echo "⚠ $warn $key: whitespace in the value (a trailing comment?), skipped" >&2; continue ;;
            *[\"\']*) echo "⚠ $warn $key: quotes in the value, skipped" >&2; continue ;;
        esac
        printf '%s %s\n' "$key" "$val"
    done < "$BUILD_HOSTS_FILE"
}

# Sets every key the file names that the environment does not, last line winning.
load_build_hosts() {
    local key val
    local -A from_file=()
    while read -r key val; do from_file[$key]=$val; done < <(build_hosts_parse)
    for key in "${!from_file[@]}"; do
        [ -n "${!key+x}" ] || export "$key=${from_file[$key]}"
    done
}

is_build_host_key() { case " $BUILD_HOSTS_HOST_KEYS " in *" $1 "*) return 0 ;; esac; return 1; }

# Runs `ssh <target> <command>` (default `true`) for at most
# BUILD_HOST_PROBE_SECS, killing it past that, since macOS has no timeout(1).
# Returns ssh's exit status (255 when ssh itself failed, otherwise the remote
# command's), or 124 past the limit. On success prints the command's stdout;
# on failure, why: timeout, refused, no such name, auth refused, or ssh's own
# last line.
probe_build_host() { # <ssh target> [remote command]
    local err out pid tenths=0 limit=$((BUILD_HOST_PROBE_SECS * 10)) rc why
    err=$(mktemp "${TMPDIR:-/tmp}/build-host-probe.XXXXXX") || return 1
    out=$(mktemp "${TMPDIR:-/tmp}/build-host-probe.XXXXXX") || { rm -f "$err"; return 1; }
    ssh -o BatchMode=yes -o ConnectTimeout=3 "$1" "${2:-true}" </dev/null >"$out" 2>"$err" &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$tenths" -ge "$limit" ]; then
            kill "$pid" 2>/dev/null; sleep 0.1; kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            rm -f "$err" "$out"; echo "timeout after ${BUILD_HOST_PROBE_SECS}s"; return 124
        fi
        sleep 0.1; tenths=$((tenths + 1))
    done
    rc=0; wait "$pid" || rc=$?
    if [ "$rc" -ne 0 ]; then
        case "$(cat "$err")" in
            *"Could not resolve"*|*"not known"*) why="no such name" ;;
            *"Connection refused"*) why="refused" ;;
            *"timed out"*) why="timeout" ;;
            *"Permission denied"*) why="auth refused" ;;
            *) why=$(grep -v '^[[:space:]]*$' "$err" | tail -1); why=${why:-"ssh exit $rc"} ;;
        esac
        rm -f "$err" "$out"; echo "$why"; return "$rc"
    fi
    cat "$out"
    rm -f "$err" "$out"
}

# Narrows a host key holding a candidate list to the first candidate that
# answers, exports it, and names the pick on stderr. A single host is left as
# it is without a probe, which is also why a resolved value, inherited by a
# child process, is never probed again. The pick is never saved anywhere: which
# candidate answers depends on the network this machine is on right now.
resolve_build_host() { # <variable>
    local val=${!1:-} cand target why tried="" cands
    case "$val" in *,*) ;; *) return 0 ;; esac
    IFS=, read -r -a cands <<< "$val"
    for cand in "${cands[@]}"; do
        [ -n "$cand" ] || continue
        target=$cand
        [ "$1" = REMOTE_HOST ] && [ -n "${REMOTE_USER:-}" ] && target="$REMOTE_USER@$cand"
        if why=$(probe_build_host "$target"); then
            echo "→ $1=$cand${tried:+ (not answering: ${tried%; })}" >&2
            export "$1=$cand"
            return 0
        fi
        tried+="$cand: $why; "
    done
    echo "✗ $1: no candidate answered ssh (tried ${tried%; })" >&2
    return 1
}

# Fails with one line naming the variable and the file when it is unset, so a
# command that needs a host never guesses one. A candidate list is resolved.
require_build_host() { # <variable>
    if [ -z "${!1:-}" ]; then
        echo "✗ $1 is not set: add $1=<host> to $BUILD_HOSTS_FILE (or export it)" >&2
        return 2
    fi
    resolve_build_host "$1" || return 2
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    case "${1:-}" in
        --keys) printf '%s\n' $BUILD_HOSTS_KEYS ;;
        --make)
            # make reads the result as a value, so $ is doubled to stay literal.
            load_build_hosts
            [ -n "${2:-}" ] || exit 2
            if is_build_host_key "$2"; then resolve_build_host "$2" || exit 1; fi
            v=${!2:-}; printf '%s\n' "${v//\$/\$\$}" ;;
        *) echo "usage: $0 --keys | --make KEY" >&2; exit 2 ;;
    esac
    exit 0
fi

load_build_hosts
