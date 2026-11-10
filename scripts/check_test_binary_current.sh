#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exit 0 if the test binary is current enough to run against the staged code.
#
# `make -q` alone can never answer yes here: src/system/helix_version.cpp is
# regenerated on every invocation to embed the current git hash, so something is
# always out of date and a gate keyed on `make -q` skips every single time,
# reporting a tidy "binary is stale" while never running. This asks what is
# actually out of date and ignores the files that are dirty by design.
#
# Never builds, by contract: callers use it to decide whether running is cheap.
# Prints the genuinely stale sources when it says no.
#
# It fails closed: a plan make cannot produce (a Makefile error, a make older
# than the Makefile's 4.2 floor, which is Apple's /usr/bin/make) says stale, since
# an empty plan from a make that never read the rules would otherwise read as
# "nothing to rebuild" and run a binary weeks old against the staged code.
#
# Usage: check_test_binary_current.sh [target]
# Exit:  0 current enough    1 genuinely stale

set -uo pipefail
TARGET="${1:-build/bin/helix-tests}"

# Rebuilt every invocation to carry the git hash. Its staleness says nothing
# about whether the binary matches the code under test.
ALWAYS_DIRTY='^src/system/helix_version\.cpp$'

# shellcheck source=qc/_lib.sh
MAKE_BIN=$(. "$(dirname "$0")/qc/_lib.sh" && qc_make)

"$MAKE_BIN" -q _PARALLEL_GUARD=1 "$TARGET" >/dev/null 2>&1 && exit 0

if ! plan=$("$MAKE_BIN" -n _PARALLEL_GUARD=1 "$TARGET" 2>&1); then
    echo "$MAKE_BIN -n failed, so what is stale is unknown: $(printf '%s\n' "$plan" | tail -1)"
    exit 1
fi
stale=$(printf '%s\n' "$plan" \
    | grep -oE ' -c [^ ]+\.(cpp|cc|cxx|c|mm)' \
    | awk '{print $2}' | sort -u | grep -vE "$ALWAYS_DIRTY")

[ -z "$stale" ] && exit 0
printf '%s\n' "$stale"
exit 1
