#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The init script calls platform_pre_start() before it launches helix-screen,
# and helix-launcher.sh only exports variables from helixscreen.env that are
# not already set. A hook that assigns unconditionally therefore outranks every
# other source, including the user's own environment, and does it silently -
# the documented override appears to be accepted and is discarded.
#
# Hooks supply the platform DEFAULT. Anything already set wins.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOKS_DIR="$WORKTREE_ROOT/assets/config/platform"

setup() {
    load helpers
}

# export lines inside platform_pre_start(), excluding comments.
pre_start_exports() {
    awk '/^platform_pre_start\(\)/{i=1; next} i && /^}/{exit} i' "$1" \
        | grep -E '^\s*export [A-Z_][A-Z0-9_]*=' || true
}

@test "every platform hook defaults its exports instead of overriding them" {
    local offenders=""
    for f in "$HOOKS_DIR"/hooks-*.sh; do
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            var="$(echo "$line" | sed -E 's/^\s*export ([A-Z_][A-Z0-9_]*)=.*/\1/')"
            echo "$line" | grep -qF "\${$var:-" || \
                offenders="$offenders
  $(basename "$f"): $(echo "$line" | sed 's/^\s*//')"
        done <<< "$(pre_start_exports "$f")"
    done
    [ -z "$offenders" ] || {
        echo "These assignments discard a value the user already set:$offenders"
        echo 'Use: export VAR="${VAR:-default}"'
        false
    }
}

@test "the hooks actually export something (the check above is not vacuous)" {
    local n=0
    for f in "$HOOKS_DIR"/hooks-*.sh; do
        n=$((n + $(pre_start_exports "$f" | grep -c . || true)))
    done
    [ "$n" -ge 20 ]
}

@test "the defaulting form keeps a preset value and supplies one otherwise" {
    # Pin the semantics the rule depends on, so a future rewrite that looks
    # equivalent but is not gets caught here rather than on a device.
    run sh -c 'HELIX_CACHE_DIR=/user/choice; export HELIX_CACHE_DIR
               export HELIX_CACHE_DIR="${HELIX_CACHE_DIR:-/platform/default}"
               echo "$HELIX_CACHE_DIR"'
    [ "$status" -eq 0 ]
    [ "$output" = "/user/choice" ]

    run sh -c 'unset HELIX_CACHE_DIR
               export HELIX_CACHE_DIR="${HELIX_CACHE_DIR:-/platform/default}"
               echo "$HELIX_CACHE_DIR"'
    [ "$status" -eq 0 ]
    [ "$output" = "/platform/default" ]
}
