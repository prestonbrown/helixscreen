#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for symbol extraction Makefile targets and variables
# Split from test_symbol_extraction.bats for parallel execution.

# Cache make -n -p output once per file (expensive: parses entire Makefile tree)

load helpers
setup_file() {
    export MAKE_DB_CACHE="$BATS_FILE_TMPDIR/make_db.txt"
    export MAKE_DB_STRIP_CACHE="$BATS_FILE_TMPDIR/make_db_strip.txt"
    make -n -p 2>/dev/null > "$MAKE_DB_CACHE" || true
    make -n -p STRIP_BINARY=yes CROSS_COMPILE=fake- 2>/dev/null > "$MAKE_DB_STRIP_CACHE" || true
}

@test "symbols target is defined in Makefile" {
    # Use make database (cached) instead of make -n which fails without deps
    grep -q '^symbols:' "$MAKE_DB_CACHE"
}

@test "strip target is defined in Makefile" {
    grep -q '^strip:' "$MAKE_DB_CACHE"
}

@test "symbols and strip are in .PHONY" {
    local phony_lines
    phony_lines=$(grep '^\.PHONY:' "$MAKE_DB_CACHE")
    contains "symbols" "$phony_lines"
    [[ "$phony_lines" == *"strip"* ]]
}

@test "LDFLAGS does not contain -s for cross-compile builds" {
    # Verify the old LDFLAGS += -s pattern is gone
    # Check the actual Makefile source, not make output (which varies by host)
    # Match -s as a complete flag (followed by end-of-line or whitespace) so
    # legitimate flags like -static-libasan don't trip the assertion.
    ! grep -qE 'LDFLAGS \+= -s($|[[:space:]])' Makefile
}

@test "STRIP_CMD is defined when STRIP_BINARY=yes" {
    grep -q 'STRIP_CMD' "$MAKE_DB_STRIP_CACHE"
}

@test "NM_CMD is defined when STRIP_BINARY=yes" {
    grep -q 'NM_CMD' "$MAKE_DB_STRIP_CACHE"
}

@test "symbols target uses nm -nC" {
    # The Makefile uses $(NM_CMD) -nC — check for the -nC flags
    grep -A5 '^symbols:' Makefile | grep -q '\-nC'
}

@test "strip target uses strip command" {
    grep -A2 '^strip:' Makefile | grep -q 'STRIP_CMD'
}

@test "symbol map output goes to TARGET.sym" {
    grep -A2 '^symbols:' Makefile | grep -q '$(TARGET).sym'
}
