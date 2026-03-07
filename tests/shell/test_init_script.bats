#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for helixscreen.init (SysV init script).
# Verifies HOME export and backup directory creation that prevent
# config loss on Moonraker updates (K1/AD5M).

INIT_SCRIPT="config/helixscreen.init"

setup() {
    load helpers
}

# =============================================================================
# HOME environment variable
# =============================================================================

@test "init script sets HOME when unset" {
    # Extract the HOME-setting block and verify it defaults to /root
    grep -q 'HOME.*=/root' "$INIT_SCRIPT"
}

@test "init script exports HOME" {
    grep -q 'export HOME' "$INIT_SCRIPT"
}

@test "init script does not override existing HOME" {
    # The := syntax means "set only if unset"
    grep -q '${HOME:=' "$INIT_SCRIPT"
}

# =============================================================================
# Backup directory creation
# =============================================================================

@test "init script creates /var/lib/helixscreen backup directory" {
    grep -q 'mkdir.*-p.*/var/lib/helixscreen' "$INIT_SCRIPT"
}

@test "init script creates HOME/.helixscreen fallback backup directory" {
    grep -q 'mkdir.*\.helixscreen' "$INIT_SCRIPT"
}

@test "backup dir creation does not fail if mkdir fails" {
    # The mkdir commands should have || true or similar to prevent script exit
    # on systems where /var/lib is read-only
    local line
    line=$(grep 'mkdir.*-p.*/var/lib/helixscreen' "$INIT_SCRIPT")
    echo "$line" | grep -qE '\|\|.*true|2>/dev/null'
}
