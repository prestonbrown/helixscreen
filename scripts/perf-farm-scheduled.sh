#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs perf-farm-check.sh unattended and keeps what every run found, so the CPU cost of
# each board can be read as a trend across builds (prestonbrown/helixscreen#2042).
#
#   perf-farm-scheduled.sh                       one run: take the lock, log it, keep its records
#   perf-farm-scheduled.sh --trend [DEVICE] [N]  the last N runs of each device (default 10)
#
# State lives in PERF_FARM_DIR (default ${XDG_STATE_HOME:-~/.local/state}/helix-perf-farm):
#   results.jsonl     one JSON record per device per run, only ever appended
#   runs/<stamp>.log  the full output of each run; the newest PERF_FARM_KEEP_LOGS (60) stay
#   lock              held for the whole run, so a second run never meets the first on a device
#
# Exit: perf-farm-check.sh's own (0 in budget, 1 over, 2 no usable sample), or 3 when
# another run holds the lock. Every variable perf-farm-check.sh reads (BUDGETS,
# PERF_SSH_OPTS, ONLY, IDLE_ONLY, the password variables) passes through.
#
# scripts/perf-farm/ holds a systemd user timer that runs this. Installing it decides which
# printers are touched unattended and at what hour (prestonbrown/helixscreen#1910).

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${PERF_FARM_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/helix-perf-farm}"
KEEP="${PERF_FARM_KEEP_LOGS:-60}"
RESULTS="$DIR/results.jsonl"

command -v jq >/dev/null || { echo "jq is required" >&2; exit 2; }

trend() { # [device] [n]
    local device="${1:-}" n="${2:-10}"
    [[ -s "$RESULTS" ]] || { echo "no results yet in $RESULTS" >&2; return 2; }
    [[ "$n" =~ ^[1-9][0-9]*$ ]] || { echo "N must be a positive whole number, not '$n'" >&2; return 2; }
    # The median takes the lower middle sample, as perf-farm-check.sh scores it.
    {
        printf 'time\tdevice\tstatus\tidle\tprint\tprint-off\tversion\n'
        jq -rs --arg device "$device" --argjson n "$n" '
            def med: if length == 0 then "-" else (sort | .[((length + 1) / 2 | floor) - 1] | tostring) end;
            map(select(.device != null and ($device == "" or .device == $device)))
            | group_by(.device)[] | .[-$n:][]
            | [.ts, .device, .status, (.idle | med), (.print | med), (.print_anim_off | med),
               (if (.version // "") == "" then "-" else .version end)]
            | @tsv' "$RESULTS"
    } | column -t -s $'\t'
}

case "${1:-}" in
    --trend) shift; trend "$@"; exit ;;
    "") ;;
    *) sed -n '7,8p' "${BASH_SOURCE[0]}" | sed 's/^# //' >&2; exit 2 ;;
esac

mkdir -p "$DIR/runs" || exit 2
exec 9>"$DIR/lock"
if ! flock -n 9; then
    echo "another perf-farm run holds $DIR/lock" >&2
    exit 3
fi

stamp=$(date -u +%Y%m%dT%H%M%SZ)
log="$DIR/runs/$stamp.log"
PERF_RESULTS="$RESULTS" PERF_RUN_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    "$HERE/perf-farm-check.sh" >"$log" 2>&1 9>&-
rc=$?
echo "exit $rc" >>"$log"

# Logs are named by UTC stamp, so name order is age order.
mapfile -t logs < <(find "$DIR/runs" -maxdepth 1 -name '*.log' | sort)
excess=$(( ${#logs[@]} - KEEP ))
for (( k = 0; k < excess; k++ )); do
    rm -f -- "${logs[$k]}"
done

echo "perf farm exit $rc, log $log"
exit "$rc"
