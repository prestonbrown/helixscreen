#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pre-release CPU budget check across the physical test farm.
#
# Steady-state CPU cost is invisible to ordinary CI: every gate we have runs on an
# x86 runner where a wasted 30 Hz repaint costs nothing measurable. On a printer
# host sharing two cores with Klipper's motion queue, the same repaint is the
# difference between a print finishing and an MCU timer shutdown. This measures
# that number on the hardware where it decides outcomes.
#
# Run it from a host that reaches the fleet and stays up (zeus).
#
#   0  every device inside budget
#   1  a device over budget
#   2  a device produced no usable sample - NOT a pass
#
# ---------------------------------------------------------------------------
# Why there is a print-active phase, and why it is the important one
# ---------------------------------------------------------------------------
# An idle-only gate would have passed a machine exhibiting #1440. Measured on a
# K1C: idle 6.5%, active print with a short filename 12.5%, active print with a
# 58-character filename 26.4%. The cost lives in the print path, and specifically
# in animations whose per-frame invalidation scales with how much text overflows.
#
# So the print phase deliberately uses a LONG filename. A short one does not
# overflow its label, no scroll animation starts, and the regression this gate
# exists to catch is invisible.
#
# It also samples with animations OFF. That is the sharpest probe available: with
# the preference off, any remaining per-frame animation is one that ignores it,
# which is exactly the defect class #1440 turned out to be. On the unfixed build
# roughly 8 points of one core survived animations_enabled=false.
#
# Nothing physical happens. The mock printer drives a simulated print with no
# motion, heat or extrusion, so this is safe to run against a live printer that
# is not currently printing - which the script verifies before it touches
# anything.
#
# ---------------------------------------------------------------------------
# Constraints worth knowing before editing
# ---------------------------------------------------------------------------
# * Mocks are compiled out of the CC1 build (mk/cross.mk, ENABLE_MOCKS := no), so
#   CC1 can only be measured idle. Any device may join it; the probe checks.
# * assets/test_gcodes/ is stripped from release payloads, so HELIX_MOCK_AUTO_PRINT
#   alone finds no file and never starts a print. Pass --gcode-file explicitly.
#   Any gcode on the device will do; the gate copies one under the name it wants.
# * A fresh instance starts with the screen awake. The running service may have
#   dimmed hours ago, which is why every phase hand-launches rather than sampling
#   whatever is on screen. The whole run must fit inside the device's dim timeout.
# * Percent of one core is (delta_jiffies / HZ) / window * 100. A jiffie delta is
#   NOT a percentage unless the window is exactly one second.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUDGETS="${BUDGETS:-$HERE/../config/perf_budgets.json}"
WINDOW="${WINDOW:-10}"
SETTLE="${SETTLE:-30}"
SAMPLES="${SAMPLES:-2}"
ONLY="${ONLY:-}"
IDLE_ONLY="${IDLE_ONLY:-false}"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o StrictHostKeyChecking=no)

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'
CYAN=$'\033[0;36m'; BOLD=$'\033[1m'; RESET=$'\033[0m'

command -v jq >/dev/null || { echo "${RED}jq is required${RESET}" >&2; exit 2; }
[[ -f "$BUDGETS" ]] || { echo "${RED}No budget file at $BUDGETS${RESET}" >&2; exit 2; }

# Everything below runs ON the device under whatever /bin/sh it has. BusyBox-safe:
# no `pgrep -x`, no `head -c`, no bare `head -N`. The comm field is stripped before
# indexing so a process name containing a space cannot shift utime/stime.
read -r -d '' REMOTE <<'REMOTE_EOF'
set -u
BIN="$1"; FLAGS="$2"; STOP="$3"; START="$4"; SETTLE="$5"; WINDOW="$6"; SAMPLES="$7"; IDLE_ONLY="$8"

say() { echo "RESULT $*"; }
find_pid() { ps w 2>/dev/null | grep '[h]elix-screen' | grep -v watchdog | grep -v ' ctl ' \
             | awk '{print $1}' | head -n 1; }
cut_stat() { sed 's/.*) //' "$1" 2>/dev/null | awk '{print $12+$13}'; }

UP=$(awk '{print $1}' /proc/uptime)
TJ=$(awk '/^cpu0 /{s=0; for(i=2;i<=8;i++) s+=$i; print s}' /proc/stat)
HZ=$(awk -v t="$TJ" -v u="$UP" 'BEGIN{r=t/u; if(r<160) print 100; else if(r<400) print 250; else print 1000}')

# Never disturb a machine that is mid-print.
STATE=$(curl -s --max-time 5 "http://127.0.0.1:7125/printer/objects/query?print_stats" 2>/dev/null \
        | sed 's/.*"state"[[:space:]]*:[[:space:]]*"//' | sed 's/".*//')
case "$STATE" in
    printing|paused) say "SKIPPED the printer is busy (print_stats.state=$STATE)"; exit 0 ;;
esac

[ -n "$STOP" ] && [ -n "$START" ] && [ -n "$BIN" ] || { say "SKIPPED no service control configured"; exit 0; }

# Does this build carry the mock? CC1 ships without it and cannot do the print phase.
HAS_MOCK=0
strings -a "$BIN" 2>/dev/null | grep -q HELIX_MOCK_AUTO_PRINT && HAS_MOCK=1

WORK=/tmp/.hsperf
rm -rf "$WORK"; mkdir -p "$WORK/gc" "$WORK/cfg"
MYPID=""

restore() {
    [ -n "$MYPID" ] && { kill "$MYPID" 2>/dev/null; sleep 3; kill -9 "$MYPID" 2>/dev/null; }
    rm -rf "$WORK"
    sh -c "$START" >/dev/null 2>&1
    sleep 4
    if [ -n "$(find_pid)" ]; then echo "RESTORED ok"; else echo "RESTORED FAILED"; fi
}
trap restore EXIT HUP INT TERM

sh -c "$STOP" >/dev/null 2>&1
sleep 5

# One sample = SAMPLES windows against a freshly launched instance.
measure() {
    _extra="$1"; _anim="$2"; _auto="${3:-0}"
    rm -rf "$WORK/cfg"; mkdir -p "$WORK/cfg"
    cp "$(dirname "$(dirname "$BIN")")/config/settings.json" "$WORK/cfg/settings-test.json" 2>/dev/null
    [ -f "$WORK/cfg/settings-test.json" ] || return 1
    sed -i "s/\"animations_enabled\"[[:space:]]*:[[:space:]]*[a-z]*/\"animations_enabled\": $_anim/" "$WORK/cfg/settings-test.json"
    # shellcheck disable=SC2086
    HELIX_CONFIG_DIR="$WORK/cfg" HELIX_MOCK_AUTO_PRINT="$_auto" \
        $BIN --test $FLAGS $_extra \
        --log-level=warn --log-dest=file --log-file="$WORK/run.log" >/dev/null 2>&1 &
    MYPID=$!
    sleep "$SETTLE"
    [ -d "/proc/$MYPID" ] || { MYPID=""; return 1; }
    _out=""; _i=0
    while [ "$_i" -lt "$SAMPLES" ]; do
        _a=$(cut_stat "/proc/$MYPID/stat"); sleep "$WINDOW"; _b=$(cut_stat "/proc/$MYPID/stat")
        [ -n "$_a" ] && [ -n "$_b" ] || { MYPID=""; return 1; }
        _out="$_out $(awk -v d="$((_b - _a))" -v h="$HZ" -v w="$WINDOW" 'BEGIN{printf "%.1f", d/h/w*100}')"
        _i=$((_i + 1))
    done
    kill "$MYPID" 2>/dev/null; sleep 3; kill -9 "$MYPID" 2>/dev/null; MYPID=""
    echo "$_out"
    return 0
}

IDLE=$(measure "" true 0) || { say "INCONCLUSIVE idle instance died within ${SETTLE}s"; exit 0; }

if [ "$IDLE_ONLY" = "true" ] || [ "$HAS_MOCK" = "0" ]; then
    _why=$([ "$HAS_MOCK" = "0" ] && echo " (no mock in this build)" || echo "")
    say "MEASURED hz=$HZ idle=$IDLE print= printoff=$_why"
    exit 0
fi

# A long name is the point: overflow is what starts the scroll animation.
GC=$(ls /usr/data/printer_data/gcodes/*.gcode /mnt/UDISK/printer_data/gcodes/*.gcode \
        /usr/data/printer_data/gcodes/**/*.gcode 2>/dev/null | head -n 1)
if [ -z "$GC" ]; then
    say "MEASURED hz=$HZ idle=$IDLE print= printoff= (no gcode on device for the print phase)"
    exit 0
fi
LONG="$WORK/gc/Delta_filament_barrel_base_Hyper_PLA_49m_overflowing_name.gcode"
cp "$GC" "$LONG" 2>/dev/null || { say "MEASURED hz=$HZ idle=$IDLE print= printoff= (could not stage a gcode)"; exit 0; }

PRINT=$(measure "--sim-speed 3 --gcode-file $LONG" true 1)  || PRINT=""
PRINTOFF=$(measure "--sim-speed 3 --gcode-file $LONG" false 1) || PRINTOFF=""
say "MEASURED hz=$HZ idle=$IDLE print=$PRINT printoff=$PRINTOFF"
REMOTE_EOF

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

overall=0
declare -a SUMMARY=()

score() { # name phase values budget
    local name="$1" phase="$2" vals="$3" budget="$4" med
    [[ -z "${vals// }" ]] && { printf '    %-9s %sno sample%s\n' "$phase" "$YELLOW" "$RESET"; return 2; }
    # shellcheck disable=SC2086
    med=$(median $vals)
    if awk -v m="$med" -v b="$budget" 'BEGIN{exit !(m > b)}'; then
        printf '    %-9s %sOVER%s %s%% > %s%%   [%s ]\n' "$phase" "$RED" "$RESET" "$med" "$budget" "$vals"
        return 1
    fi
    printf '    %-9s %sok%s   %s%% (budget %s%%)   [%s ]\n' "$phase" "$GREEN" "$RESET" "$med" "$budget" "$vals"
    return 0
}

for i in $(seq 0 $(($(jq -r '.devices | length' "$BUDGETS") - 1))); do
    name=$(jq -r ".devices[$i].name" "$BUDGETS")
    [[ -n "$ONLY" && "$ONLY" != "$name" ]] && continue
    host=$(jq -r  ".devices[$i].ssh" "$BUDGETS")
    bin=$(jq -r   ".devices[$i].binary // \"\"" "$BUDGETS")
    stop=$(jq -r  ".devices[$i].service_stop // \"\"" "$BUDGETS")
    start=$(jq -r ".devices[$i].service_start // \"\"" "$BUDGETS")
    flags=$(jq -r ".devices[$i].extra_flags // \"\"" "$BUDGETS")
    b_idle=$(jq -r ".devices[$i].idle_budget_pct" "$BUDGETS")
    b_print=$(jq -r ".devices[$i].print_budget_pct // empty" "$BUDGETS")
    b_poff=$(jq -r  ".devices[$i].print_anim_off_budget_pct // empty" "$BUDGETS")

    printf '%s%-6s%s\n' "$BOLD" "$name" "$RESET"
    if ! ssh "${SSH_OPTS[@]}" "$host" true 2>/dev/null; then
        printf '    %sUNREACHABLE%s (%s)\n' "$RED" "$RESET" "$host"
        SUMMARY+=("$name UNREACHABLE"); overall=2; continue
    fi

    out=$(ssh "${SSH_OPTS[@]}" "$host" \
          "sh -s -- '$bin' '$flags' '$stop' '$start' $SETTLE $WINDOW $SAMPLES $IDLE_ONLY" \
          <<<"$REMOTE" 2>/dev/null)
    res=$(grep '^RESULT ' <<<"$out" | head -n 1 | sed 's/^RESULT //')
    restored=$(grep '^RESTORED ' <<<"$out" | head -n 1 | sed 's/^RESTORED //')

    if [[ -n "$restored" && "$restored" != "ok" ]]; then
        printf '    %sSERVICE DID NOT RESTART%s - fix this device by hand before anything else\n' "$RED" "$RESET"
        SUMMARY+=("$name SERVICE-DOWN"); overall=2; continue
    fi

    case "$res" in
        MEASURED*)
            idle=$(sed 's/.*idle=//;   s/ print=.*//'    <<<"$res")
            pr=$(sed   's/.*[^f]print=//; s/ printoff=.*//' <<<"$res")
            poff=$(sed 's/.*printoff=//' <<<"$res")
            worst=0
            score "$name" idle     "$idle" "$b_idle";                     r=$?; [[ $r -gt $worst ]] && worst=$r
            if [[ -n "$b_print" ]]; then
                score "$name" print    "$pr"   "$b_print";                r=$?; [[ $r -gt $worst ]] && worst=$r
                score "$name" print-off "$poff" "$b_poff";                r=$?; [[ $r -gt $worst ]] && worst=$r
            fi
            case $worst in
                0) SUMMARY+=("$name ok") ;;
                1) SUMMARY+=("$name OVER BUDGET"); [[ $overall -eq 0 ]] && overall=1 ;;
                *) SUMMARY+=("$name INCOMPLETE"); overall=2 ;;
            esac ;;
        SKIPPED*|INCONCLUSIVE*|"")
            printf '    %s%s%s\n' "$YELLOW" "${res:-NO RESPONSE}" "$RESET"
            SUMMARY+=("$name ${res:-NO-RESPONSE}"); overall=2 ;;
        *)  printf '    %sUNPARSED:%s %s\n' "$YELLOW" "$RESET" "$res"
            SUMMARY+=("$name UNPARSED"); overall=2 ;;
    esac
done

echo
printf '%s=== summary ===%s\n' "$CYAN" "$RESET"
printf '  %s\n' "${SUMMARY[@]}"
case "$overall" in
    0) printf '%sAll devices within budget.%s\n' "$GREEN" "$RESET" ;;
    1) printf '%sAt least one device is over budget.%s\n' "$RED" "$RESET" ;;
    2) printf '%sAt least one device produced no usable sample. Not a pass.%s\n' "$YELLOW" "$RESET" ;;
esac
exit "$overall"
