#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/resolve-backtrace.sh
# Verifies backtrace address resolution against symbol map files.

SCRIPT="scripts/resolve-backtrace.sh"

setup() {
    load helpers
    TEST_DIR="$(mktemp -d)"

    # Create a mock symbol file (nm -nC output format)
    cat > "$TEST_DIR/test.sym" << 'EOF'
0000000000400000 T _start
0000000000400100 T main
0000000000400200 T PrinterState::update()
0000000000400400 T WebSocketClient::connect(std::string const&)
0000000000400800 T lv_obj_create
0000000000401000 T __libc_start_main
EOF
}

teardown() {
    rm -rf "$TEST_DIR"
}

@test "resolve-backtrace.sh has valid bash syntax" {
    bash -n "$SCRIPT"
}

@test "resolve-backtrace.sh passes shellcheck" {
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck -e SC2034 -e SC2016 "$SCRIPT"
}

@test "shows usage with no arguments" {
    run bash "$SCRIPT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Usage:"* ]]
}

@test "shows usage with too few arguments" {
    run bash "$SCRIPT" 0.9.9 pi
    [ "$status" -ne 0 ]
    [[ "$output" == *"Usage:"* ]]
}

@test "resolves address to nearest symbol" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    run bash "$SCRIPT" 0.9.9 pi 0x400150
    [ "$status" -eq 0 ]
    [[ "$output" == *"main+0x50"* ]]
}

@test "resolves address at exact symbol start" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    run bash "$SCRIPT" 0.9.9 pi 0x400100
    [ "$status" -eq 0 ]
    [[ "$output" == *"main+0x0"* ]]
}

@test "resolves address in demangled C++ symbol" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    run bash "$SCRIPT" 0.9.9 pi 0x400250
    [ "$status" -eq 0 ]
    [[ "$output" == *"PrinterState::update()+0x50"* ]]
}

@test "resolves multiple addresses" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    run bash "$SCRIPT" 0.9.9 pi 0x400100 0x400250 0x400900
    [ "$status" -eq 0 ]
    [[ "$output" == *"main"* ]]
    [[ "$output" == *"PrinterState::update()"* ]]
    [[ "$output" == *"lv_obj_create"* ]]
}

@test "handles addresses without 0x prefix" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    run bash "$SCRIPT" 0.9.9 pi 400150
    [ "$status" -eq 0 ]
    [[ "$output" == *"main+0x50"* ]]
}

@test "handles uppercase hex addresses" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    run bash "$SCRIPT" 0.9.9 pi 0x400ABC
    [ "$status" -eq 0 ]
    # 0x400ABC (4196028) is between lv_obj_create (0x400800) and __libc_start_main (0x401000)
    [[ "$output" == *"lv_obj_create"* ]]
}

@test "fails with missing sym file" {
    export HELIX_SYM_FILE="$TEST_DIR/nonexistent.sym"
    run bash "$SCRIPT" 0.9.9 pi 0x400100
    [ "$status" -ne 0 ]
    [[ "$output" == *"not found"* ]]
}

@test "fails with empty sym file" {
    touch "$TEST_DIR/empty.sym"
    export HELIX_SYM_FILE="$TEST_DIR/empty.sym"
    run bash "$SCRIPT" 0.9.9 pi 0x400100
    [ "$status" -ne 0 ]
    [[ "$output" == *"empty"* ]]
}

# =============================================================================
# --issue mode
#
# The worker emits the backtrace in two shapes: a table when symbols resolved
# server-side, and a bare code block when they didn't (#1240). Register values
# live in their own tables and are NOT code addresses — resolving SP or r0-r12
# as if they were frames invents symbols that were never on the stack.
# =============================================================================

# Stub `gh issue view` so --issue mode reads a fixture instead of the network.
stub_gh_issue() {
    printf '%s' "$1" > "$TEST_DIR/issue-body.md"
    mock_command_script "gh" "cat '$TEST_DIR/issue-body.md'"
}

# An unresolved report: bare addresses in a fenced block, plus a register table.
UNRESOLVED_ISSUE='## Crash Summary

| Field | Value |
|-------|-------|
| **Signal** | 11 (SIGSEGV) |
| **Version** | 0.9.9 |

## Registers

| Register | Value |
|----------|-------|
| **LR** | `0x400410` |
| **PC** | `0x400210` |
| **SP** | `0x7fd4f57890` |

## System Info

| Field | Value |
|-------|-------|
| **Platform** | pi |

## Backtrace

```
0x400210
0x400410
0x400810
```

<sub>No symbol file found for v0.9.9/pi</sub>
'

@test "--issue parses every frame from an unresolved code block" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    stub_gh_issue "$UNRESOLVED_ISSUE"
    run bash "$SCRIPT" --issue 1240 --repo owner/repo
    [ "$status" -eq 0 ]
    [[ "$output" == *"3 addresses"* ]]
    [[ "$output" == *"PrinterState::update()"* ]]
    [[ "$output" == *"WebSocketClient::connect"* ]]
    [[ "$output" == *"lv_obj_create"* ]]
}

@test "--issue does not treat the stack pointer as a frame" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    stub_gh_issue "$UNRESOLVED_ISSUE"
    run bash "$SCRIPT" --issue 1240 --repo owner/repo
    [ "$status" -eq 0 ]
    [[ "$output" != *"0x7fd4f57890"* ]]
}

@test "--issue parses a resolved backtrace table without swallowing registers" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    stub_gh_issue '## Registers

| Register | Value |
|----------|-------|
| **LR** | `0x400410` |
| **PC** | `0x400210` |
| **SP** | `0x7e8017a8` |

### All Registers

| Register | Value |
|----------|-------|
| **r0** | `0x400810` |
| **r1** | `0x4d1` |

| Field | Value |
|-------|-------|
| **Version** | 0.9.9 |
| **Platform** | pi |

## Backtrace

| # | Address | Symbol |
|---|---------|--------|
| 0 | `0x400210` | `PrinterState::update()` |
| 1 | `0x400410` | `<shared library>` |
'
    run bash "$SCRIPT" --issue 1239 --repo owner/repo
    [ "$status" -eq 0 ]
    # Two table rows — not the four register cells above them
    [[ "$output" == *"2 addresses"* ]]
    [[ "$output" != *"0x4d1"* ]]
}

@test "--issue keeps stack-scan candidates separate from reliable frames" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    stub_gh_issue '| **Version** | 0.9.9 |
| **Platform** | pi |

## Backtrace

| # | Address | Symbol |
|---|---------|--------|
| 0 | `0x400210` | `PrinterState::update()` |

## Stack Scan (likely call chain)

| SP+offset | Address | Symbol |
|-----------|---------|--------|
| SP+0x10 | `0x400810` | `lv_obj_create` |
'
    run bash "$SCRIPT" --issue 1239 --repo owner/repo
    [ "$status" -eq 0 ]
    [[ "$output" == *"2 addresses"* ]]
    [[ "$output" == *"reliable frames"* ]]
    [[ "$output" == *"stack-scan candidates"* ]]
}

@test "--issue falls back to PC/LR when there is no backtrace section" {
    export HELIX_SYM_FILE="$TEST_DIR/test.sym"
    stub_gh_issue '## Registers

| Register | Value |
|----------|-------|
| **LR** | `0x400410` |
| **PC** | `0x400210` |
| **SP** | `0x7fd4f57890` |

| **Version** | 0.9.9 |
| **Platform** | pi |
'
    run bash "$SCRIPT" --issue 1239 --repo owner/repo
    [ "$status" -eq 0 ]
    [[ "$output" == *"2 addresses"* ]]
    [[ "$output" != *"0x7fd4f57890"* ]]
}
