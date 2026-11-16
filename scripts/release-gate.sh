#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The checks a tag push runs that per-push CI does not, run locally before tagging.
# Per-push CI builds the desktop app and links splash/watchdog for x86 only; the
# Release workflow cross-compiles every platform with HELIX_PACKAGING=1, so a
# packaged-only or target-only break (a MIPS32 link needing libatomic, a binary
# still carrying the ctl server) surfaces for the first time on the tag.
#
# usage: scripts/release-gate.sh [fast] [heavy]      (default: both)
#   fast   VERSION.txt has a CHANGELOG entry and is not tagged yet; the Play Store "What's
#          new" generates (a stable version needs android/whatsnew/<version>.txt);
#          installer bundles build;
#          release-health prints the newest beta against stable in telemetry (informational,
#          never fails; its table is in the step log; RELEASE_HEALTH=0 skips it)
#   heavy  per target: the packaged docker cross build, `make release-<target>` (which
#          asserts the binary's feature stamps), splash/watchdog present, no mock symbols
#
#   RELEASE_GATE_TARGETS   release platforms for the heavy tier (default: mips)
#
# Runs the cross builds through the local toolchain images under a heavy:release-gate
# claim. Each step's output goes to build/release-gate/<step>.log; the table at the
# end names every step, and the exit status is non-zero if any step failed.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2
MAKE="${MAKE:-make}"
TARGETS="${RELEASE_GATE_TARGETS:-mips}"
LOGDIR=build/release-gate
mkdir -p "$LOGDIR"

ROWS=()
FAILED=0

# step <name> <command...>: run it, log it, record a table row.
step() {
    local name=$1 log t0 status
    shift
    log="$LOGDIR/${name//:/-}.log"
    t0=$SECONDS
    echo "[$(date +%H:%M:%S)] $name ..."
    if "$@" >"$log" 2>&1; then
        status=PASS
    else
        status=FAIL
        FAILED=1
        # A parallel build keeps going after the failing link, so its tail is
        # usually some other unit's warnings: lead with the error lines.
        echo "  FAIL: $log" >&2
        grep -E 'error:|undefined reference|\*\*\* \[' "$log" | head -n 20 >&2 || tail -n 25 "$log" >&2
    fi
    LAST_SECS=$((SECONDS - t0))
    ROWS+=("$(printf '%-4s  %-24s %6ss  %s' "$status" "$name" "$LAST_SECS" "$log")")
    [ "$status" = PASS ]
}

skip() { ROWS+=("$(printf '%-4s  %-24s %7s  %s' SKIP "$1" - "$2")"); }

check_version() {
    local v
    v=$(tr -d '[:space:]' <VERSION.txt)
    echo "VERSION.txt: $v"
    [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || { echo "not MAJOR.MINOR.PATCH[-PRERELEASE]"; return 1; }
    grep -q "^## \[$v\] - " CHANGELOG.md || { echo "CHANGELOG.md has no '## [$v] - <date>' entry"; return 1; }
    grep -q "^\[$v\]: " CHANGELOG.md || { echo "CHANGELOG.md has no '[$v]:' compare link"; return 1; }
    if git rev-parse -q --verify "refs/tags/v$v" >/dev/null; then
        echo "tag v$v already exists: bump VERSION.txt first"
        return 1
    fi
}

# The docker target that builds what the Release workflow's build_target builds.
docker_target() {
    case "$1" in
        pi | pi32 | x86) echo "$1-all-docker" ;;
        *) echo "$1-docker" ;;
    esac
}

check_binaries() {
    local bin=build/$1/bin
    [ -x "$bin/helix-screen" ] || { echo "$bin/helix-screen missing"; return 1; }
    # Built under the same display-backend condition; one without the other is a
    # link that silently did not happen.
    if [ -e "$bin/helix-splash" ] && [ ! -x "$bin/helix-watchdog" ]; then
        echo "$bin/helix-splash built but $bin/helix-watchdog missing"
        return 1
    fi
    ls -l "$bin"
}

# A packaged build compiles the mock backends out (ENABLE_MOCKS=no).
check_no_mocks() {
    local sym=build/$1/bin/helix-screen.sym hits
    local pattern
    pattern="$(dirname "${BASH_SOURCE[0]}")/mock_symbol_pattern.txt"
    [ -s "$sym" ] || { echo "$sym missing or empty"; return 1; }
    hits=$(grep -cEf "$pattern" "$sym")
    [ "$hits" -eq 0 ] || { grep -Ef "$pattern" "$sym" | head -20; echo "$hits mock symbols"; return 1; }
}

fast() {
    step version check_version
    step whatsnew scripts/generate-whatsnew.sh "$LOGDIR/whatsnew.txt"
    step installer "$MAKE" --no-print-directory installer
    if [ "${RELEASE_HEALTH:-1}" = 0 ]; then
        skip release-health "RELEASE_HEALTH=0"
    else
        step release-health scripts/release-health.sh
        warnings=$(grep -c '^WARN ' "$LOGDIR/release-health.log" || true)
        if [ "${warnings:-0}" -gt 0 ]; then
            noun=warnings; [ "$warnings" -eq 1 ] && noun=warning
            ROWS[$((${#ROWS[@]} - 1))]="$(printf '%-4s  %-24s %6ss  %s' PASS "release-health ($warnings $noun)" "$LAST_SECS" "$LOGDIR/release-health.log")"
        fi
    fi
}

heavy() {
    local t
    for t in $TARGETS; do
        if ! step "$t:build" scripts/helix-claim run heavy:release-gate -- \
            "$MAKE" --no-print-directory "$(docker_target "$t")" HELIX_PACKAGING=1; then
            skip "$t:package" "build failed"
            skip "$t:binaries" "build failed"
            skip "$t:no-mocks" "build failed"
            continue
        fi
        step "$t:package" "$MAKE" --no-print-directory "release-$t"
        step "$t:binaries" check_binaries "$t"
        step "$t:no-mocks" check_no_mocks "$t"
    done
}

[ $# -gt 0 ] || set -- fast heavy
for tier in "$@"; do
    case "$tier" in
        fast | heavy) "$tier" ;;
        *) echo "unknown tier '$tier' (fast | heavy)" >&2; exit 2 ;;
    esac
done

echo
echo "Release gate"
printf '  %s\n' "${ROWS[@]}"
if [ "$FAILED" -ne 0 ]; then
    echo "RELEASE GATE: FAIL"
    exit 1
fi
echo "RELEASE GATE: PASS"
