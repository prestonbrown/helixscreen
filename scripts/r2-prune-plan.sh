#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# r2-prune-plan.sh — Decide which versions an R2 prune may delete.
#
# All channels share one releases/ (and symbols/) store, so "keep the N highest
# versions" lets a run of prereleases on the devel line outrank the stable
# release every stable printer downloads from. A version some channel manifest
# points at is never prunable: deleting it makes every update on that channel
# fail with a 404 while the manifest still advertises it.
#
#   r2-prune-plan.sh plan RETAIN [PINNED...] < versions
#       Prints the versions to delete, oldest first: everything except PINNED
#       and the RETAIN highest of the rest. Precedence is semver, from
#       version-compare.sh; a version it cannot rank fails the run.
#
#   r2-prune-plan.sh pinned
#       Prints the version each channel manifest in the bucket names. Reads the
#       bucket, not the CDN, so a manifest uploaded seconds ago is what counts.
#       Needs R2_BUCKET and R2_ENDPOINT plus AWS credentials. Any channel it
#       cannot read fails the run: a prune that cannot see a pin must not run.

set -euo pipefail

CHANNELS="stable beta dev"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    sed -n '4,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

cmd_plan() {
    [ $# -ge 1 ] || usage
    local retain="$1"
    shift
    [[ "$retain" =~ ^[0-9]+$ ]] || { echo "r2-prune-plan: RETAIN must be a number, got '$retain'" >&2; exit 2; }

    local sorted
    sorted=$({ grep . || true; } | sort -u | "$HERE/version-compare.sh" --sort) || exit 1

    local pinned=" $* "
    local candidates=()
    local v
    while IFS= read -r v; do
        [ -n "$v" ] || continue
        [[ "$pinned" == *" $v "* ]] && continue
        candidates+=("$v")
    done <<< "$sorted"

    local n=${#candidates[@]}
    local i
    for ((i = 0; i < n - retain; i++)); do
        echo "${candidates[$i]}"
    done
}

cmd_pinned() {
    : "${R2_BUCKET:?R2_BUCKET is required}" "${R2_ENDPOINT:?R2_ENDPOINT is required}"
    local channel version
    for channel in $CHANNELS; do
        if ! version=$(aws s3 cp "s3://${R2_BUCKET}/${channel}/manifest.json" - \
            --endpoint-url "$R2_ENDPOINT" | jq -r '.version // empty'); then
            echo "r2-prune-plan: cannot read ${channel}/manifest.json" >&2
            exit 1
        fi
        if [ -z "$version" ]; then
            echo "r2-prune-plan: ${channel}/manifest.json names no version" >&2
            exit 1
        fi
        echo "$version"
    done
}

case "${1:-}" in
    plan) shift; cmd_plan "$@" ;;
    pinned) shift; cmd_pinned ;;
    *) usage ;;
esac
