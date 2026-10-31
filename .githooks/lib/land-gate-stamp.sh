# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by scripts/land-gate.sh, which records a commit it passed, and by
# pre-push, which skips the quality sweep that pass already ran on that commit.
#
# A stamp is one file named by the full commit SHA. Only an exact match counts:
# a new commit, an amend or a merge made after the gate is a different SHA and
# gets the full sweep.
#
# Every stamp is written twice: into the clone (land_gate_stamp_dir) and into a
# per-user directory every clone on the host shares (land_gate_shared_stamp_dir).
# The shared copy is what another clone on the same host, and pre-push on
# another machine asking over ssh (land_gate_ask_hosts), find.

# land_gate_stamp_dir: where this clone's stamps live. The git common dir is
# one directory for every worktree of a clone, so a gate run in a worktree
# covers the push made from the main tree. HELIX_LAND_GATE_STAMP_DIR relocates it.
land_gate_stamp_dir() {
    if [ -n "${HELIX_LAND_GATE_STAMP_DIR:-}" ]; then
        printf '%s\n' "$HELIX_LAND_GATE_STAMP_DIR"
        return 0
    fi
    local common
    common=$(git rev-parse --git-common-dir 2>/dev/null) || return 1
    common=$(cd "$common" && pwd) || return 1
    printf '%s/helix-land-gate-pass\n' "$common"
}

# land_gate_shared_stamp_dir: the per-user record every clone on this host
# shares. land_gate_ask_hosts spells the same default out for the remote shell,
# which resolves it there. HELIX_LAND_GATE_SHARED_STAMP_DIR relocates it.
land_gate_shared_stamp_dir() {
    printf '%s\n' "${HELIX_LAND_GATE_SHARED_STAMP_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/helixscreen/land-gate-pass}"
}

land_gate_sha_ok() { case "$1" in "" | *[!0-9a-f]*) return 1 ;; esac; }

# land_gate_stamp_write SHA [HOST]: record that make land-gate passed SHA, in
# this clone and in the shared record. HOST names where the gate ran when that
# was another machine; without it the record names this host and this repo.
# Stamps older than 30 days are pruned; nobody pushes a commit gated that long
# ago. The shared copy is best effort: a clone whose own stamp is written is
# covered either way.
land_gate_stamp_write() {
    local sha=$1 host=${2:-} dir shared record
    land_gate_sha_ok "$sha" || return 1
    dir=$(land_gate_stamp_dir) || return 1
    record="make land-gate PASS at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    if [ -n "$host" ]; then
        record+=$'\n'"host $host (found by pre-push on $(hostname))"
    else
        record+=$'\n'"host $(hostname)"$'\n'"repo $(git rev-parse --show-toplevel 2>/dev/null || pwd)"
    fi
    mkdir -p "$dir" || return 1
    find "$dir" -type f -mtime +30 -delete 2>/dev/null
    printf '%s\n' "$record" >"$dir/$sha" || return 1
    shared=$(land_gate_shared_stamp_dir)
    if mkdir -p "$shared" 2>/dev/null; then
        find "$shared" -type f -mtime +30 -delete 2>/dev/null
        printf '%s\n' "$record" >"$shared/$sha" 2>/dev/null
    fi
    return 0
}

# land_gate_stamp_remove SHA: drop SHA's stamp from this clone and from the
# shared record, for a result set of SHA that is not all PASS.
land_gate_stamp_remove() {
    local dir
    land_gate_sha_ok "$1" || return 1
    dir=$(land_gate_stamp_dir) && rm -f "$dir/$1"
    rm -f "$(land_gate_shared_stamp_dir)/$1"
    return 0
}

# land_gate_passed SHA: true when make land-gate passed exactly SHA, in this
# clone or in another clone on this host.
land_gate_passed() {
    local dir
    land_gate_sha_ok "$1" || return 1
    dir=$(land_gate_stamp_dir) || return 1
    [ -f "$dir/$1" ] || [ -f "$(land_gate_shared_stamp_dir)/$1" ]
}

# land_gate_ask_hosts SHA: ask each host in HELIX_LAND_GATE_HOSTS (a
# build-hosts key, comma-separated) whether its shared record holds SHA. On the
# first yes it caches the stamp here, so the next push of SHA asks nobody,
# prints the host on stdout and succeeds. Otherwise it prints one line on
# stderr naming what each host answered and fails. With no hosts configured it
# fails silently and runs no ssh. Each ask is bounded by HELIX_HOST_PROBE_SECS
# (default 8), name lookup included.
land_gate_ask_hosts() {
    local sha=$1
    land_gate_sha_ok "$sha" || return 1
    (
        lib="$(dirname "${BASH_SOURCE[0]}")/../../scripts/lib/build_hosts.sh"
        [ -f "$lib" ] || exit 1
        # shellcheck source=../../scripts/lib/build_hosts.sh
        . "$lib"
        [ -n "${HELIX_LAND_GATE_HOSTS:-}" ] || exit 1
        # shellcheck disable=SC2034  # read by probe_build_host
        BUILD_HOST_PROBE_SECS=${HELIX_HOST_PROBE_SECS:-8}
        # The remote login shell parses this once; sh then resolves the remote
        # user's own state directory. sha is hex, checked above. Only the token
        # line counts as yes: a forced command or a wrapper shell that exits 0
        # without running this would otherwise vouch for every SHA.
        token="helix-land-gate-pass:$sha"
        remote="sh -c 'test -f \"\${XDG_STATE_HOME:-\$HOME/.local/state}/helixscreen/land-gate-pass/$sha\" && echo $token'"
        tried=""
        IFS=, read -r -a hosts <<<"$HELIX_LAND_GATE_HOSTS"
        for host in "${hosts[@]}"; do
            [ -n "$host" ] || continue
            case "$host" in
                -* | *[!A-Za-z0-9._@-]*) tried+="$host: not a host name; "; continue ;;
            esac
            rc=0
            why=$(probe_build_host "$host" "$remote") || rc=$?
            if [ "$rc" -eq 0 ] && [ "${why%$'\r'}" = "$token" ]; then
                land_gate_stamp_write "$sha" "$host"
                printf '%s\n' "$host"
                exit 0
            fi
            # Exit 1 is the remote test saying no; ssh itself fails with 255.
            if [ "$rc" -eq 0 ]; then
                why="answered without the record token"
            elif [ "$rc" -eq 1 ]; then
                why="no record"
            fi
            tried+="$host: $why; "
        done
        echo "→ pre-push: no make land-gate record for ${sha:0:9} on any gate host (${tried%; }); running the sweep" >&2
        exit 1
    )
}
