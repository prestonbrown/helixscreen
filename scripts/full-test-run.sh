#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make full-test-run: the completion gate, the C++ unit sweep and the bats
# shell suite. Nothing else runs bats locally - not the commit hook, not
# test-xml - so without it the shell suite reaches CI unrun.
#
#   TEST_HOST=1  the sweep runs on the test host (scripts/test-host-run.sh sweep,
#                against this tree's mirror) while bats runs here; the gate
#                costs the longer of the two, and fails if either fails. bats
#                stays here because the test container is root with no shellcheck.
#   TEST_HOST=0  both run here, sweep first.
#   unset        automatic: with a test host configured, test-host-run.sh --probe
#                decides from the link and prints the numbers
#                (HELIX_TEST_HOST_AUTO=0 keeps it local). With no test host
#                configured, both run here, silently.
#
# [.] and [slow] stay outside deliberately: quality-checks.sh runs [.] on any
# staged code change, and nightly CI runs [slow].
set -uo pipefail

MAKE=${MAKE:-make}
TEST_HOST_RUN=${TEST_HOST_RUN:-$(dirname "$0")/test-host-run.sh}
# A gate step has its own time limit and is the work the host is there for, so it
# does not wait behind whatever else is loading the host.
export HELIX_TEST_MAX_LOAD=${HELIX_TEST_MAX_LOAD:-0}
# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/build_hosts.sh"

# shellcheck source-path=SCRIPTDIR source=lib/sweep_location.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/sweep_location.sh"
resolve_sweep_location || exit $?
where=$SWEEP_WHERE

done_msg() {
    echo "✓ Completion gate passed: unit sweep + shell suite"
    echo "  Outside this gate: [.] (commit hook) and [slow] (nightly). Both: make test-all"
}

if [ "$where" = local ]; then
    "$MAKE" --no-print-directory unit-sweep || exit $?
    "$MAKE" --no-print-directory test-shell || exit $?
    done_msg
    exit 0
fi

zlog="${TMPDIR:-/tmp}/full-test-run-sweep.$$.log"
echo "→ unit sweep on $HELIX_TEST_HOST (output: $zlog), shell suite here"
# Its own process group, so one signal reaches its job ssh too: a background
# job ignores the terminal's Ctrl-C, setsid detaches it from the terminal's
# hangup, and a sweep left behind keeps its tree's lock and a build nobody is
# waiting for.
if command -v setsid >/dev/null 2>&1; then
    setsid "$TEST_HOST_RUN" sweep >"$zlog" 2>&1 &
else
    "$TEST_HOST_RUN" sweep >"$zlog" 2>&1 &
fi
zpid=$!
trap 'kill -TERM -- "-$zpid" 2>/dev/null || kill -TERM "$zpid" 2>/dev/null; exit 130' INT TERM HUP
"$MAKE" --no-print-directory test-shell
bats_rc=$?
if kill -0 "$zpid" 2>/dev/null; then echo "→ shell suite done; waiting for the test-host sweep"; fi
wait "$zpid"
remote_rc=$?
trap - INT TERM HUP

verdict() { [ "$1" -eq 0 ] && echo passed || echo "FAILED (exit $1)"; }
if [ "$remote_rc" -ne 0 ]; then
    echo "--- $HELIX_TEST_HOST sweep, last 40 lines of $zlog ---"
    tail -n 40 "$zlog"
fi
echo "  test-host unit sweep: $(verdict "$remote_rc")"
echo "  local shell suite: $(verdict "$bats_rc")"
[ "$remote_rc" -eq 0 ] && [ "$bats_rc" -eq 0 ] || exit 1
done_msg
