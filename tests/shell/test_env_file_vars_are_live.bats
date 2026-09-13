#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# config/helixscreen.env ships to every device and is the file users edit to
# change runtime behaviour. An uncommented VAR=value line in it reads as
# authoritative, so a variable nothing consumes is worse than an absent one:
# the user edits it, nothing happens, and there is no error to search for.
#
# Every uncommented variable must be consumed by something - getenv() in the
# app, or the launcher, which turns some of them into helix-screen flags.
# Documentation-only entries belong commented out, with a pointer to whatever
# actually controls the setting.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
ENV_FILE="$WORKTREE_ROOT/config/helixscreen.env"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers
}

# Variable names on uncommented assignment lines.
active_vars() {
    grep -E '^[A-Za-z_][A-Za-z0-9_]*=' "$ENV_FILE" | sed 's/=.*//' | sort -u
}

# Is $1 consumed by the app or the launcher?
var_is_consumed() {
    grep -rqF "getenv(\"$1\")" "$WORKTREE_ROOT/src" "$WORKTREE_ROOT/include" 2>/dev/null && return 0
    grep -qE "\\\$\{?$1\b" "$LAUNCHER" 2>/dev/null && return 0
    return 1
}

@test "every uncommented variable in the shipped env file is actually read" {
    local dead=""
    for v in $(active_vars); do
        var_is_consumed "$v" || dead="$dead $v"
    done
    [ -z "$dead" ] || {
        echo "Uncommented but consumed by nothing:$dead"
        echo "Comment the line out (with a pointer to what really controls it), or wire it up."
        false
    }
}

@test "active_vars extracts an uncommented assignment and ignores a commented one" {
    # The check above passes trivially when active_vars finds nothing, and this
    # file legitimately ships every example commented out. Pin the parser
    # against a fixture so a silent parse break cannot read as "all clean".
    local fixture="$BATS_TEST_TMPDIR/env_fixture"
    printf '# comment\n#COMMENTED=1\nLIVE_ONE=abc\n  SPACED=2\n' > "$fixture"

    run env ENV_FILE="$fixture" bash -c '
        grep -E "^[A-Za-z_][A-Za-z0-9_]*=" "$ENV_FILE" | sed "s/=.*//" | sort -u'
    [ "$status" -eq 0 ]
    [[ "$output" == *"LIVE_ONE"* ]]
    [[ "$output" != *"COMMENTED"* ]]
}

@test "the Moonraker target is not presented as an env-file setting" {
    # settings.json (moonraker_host / moonraker_port) and --moonraker own this.
    run grep -E '^MOONRAKER_(HOST|PORT)=' "$ENV_FILE"
    [ "$status" -ne 0 ]
}
