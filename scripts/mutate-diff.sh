#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make mutate-diff: run the mutation gate where it can run in parallel.
#
# mutate_diff.py mutates several hunks at once only in a standalone clone with
# reflink copies beside it (GNU cp --reflink, on btrfs, XFS or ZFS) and enough
# cores; it says so itself (--probe-workers). Anywhere else it runs one hunk at
# a time, and a batch takes hours, so the run goes to the test host
# (scripts/test-host-run.sh mutate), which has the copies and the cores. That run
# mutates the PUSHED HEAD, so this refuses when HEAD is not pushed or the tree
# holds uncommitted work, rather than reporting on something other than the tree
# here.
#
#   MUTATE_LOCAL=1   run here anyway, serially
#
# Arguments pass to mutate_diff.py unchanged.
set -u

cd "$(git rev-parse --show-toplevel)" || exit 2

if [ "${MUTATE_LOCAL:-0}" = 1 ]; then
    exec python3 scripts/mutate_diff.py "$@"
fi
if why=$(python3 scripts/mutate_diff.py --probe-workers 2>&1); then
    exec python3 scripts/mutate_diff.py --workers auto --pool "$@"
fi

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/build_hosts.sh"
load_build_hosts
if [ -z "${HELIX_TEST_HOST:-}" ]; then
    echo "mutate-diff: the gate would run one hunk at a time here ($why)," >&2
    echo "  and no test host is configured to hand it to" >&2
    echo "  (docs/devel/BUILD_SYSTEM.md, \"Using your own build/test host\")." >&2
    echo "  MUTATE_LOCAL=1 runs it here serially." >&2
    exit 2
fi

# Any uncommitted file: a test written to kill a survivor counts as much as code.
dirty=$(git status --porcelain --untracked-files=no
        git status --porcelain --untracked-files=all -- src include tests ui_xml scripts mk \
            | grep '^??')
if [ -n "$dirty" ]; then
    echo "mutate-diff: the test host mutates the pushed HEAD, and these are not in it:" >&2
    printf '%s\n' "$dirty" | sed 's/^/    /' >&2
    echo "  Commit and push them, or MUTATE_LOCAL=1 to run here serially." >&2
    exit 2
fi
if [ -z "$(git branch -r --contains HEAD 2>/dev/null)" ]; then
    echo "mutate-diff: the test host mutates the pushed HEAD, and $(git rev-parse --short HEAD) is not pushed." >&2
    echo "  Push it, or MUTATE_LOCAL=1 to run here serially." >&2
    exit 2
fi

echo "mutate-diff: one hunk at a time here ($why); running on the test host ($HELIX_TEST_HOST)" >&2
exec scripts/test-host-run.sh mutate --pool "$@"
