#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for validate_binary_architecture() in scripts/lib/installer/release.sh

RELEASE_SH="scripts/lib/installer/release.sh"

setup() {
    source tests/shell/helpers.bash
    export GITHUB_REPO="prestonbrown/helixscreen"
    source "$RELEASE_SH"
}

# --- ARM 32-bit binary tests ---

@test "validate_binary_architecture: ARM32 binary passes for ad5m" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    run validate_binary_architecture "$binary" "ad5m"
    [ "$status" -eq 0 ]
}

@test "validate_binary_architecture: MIPS binary passes for k1" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_mips_elf "$binary"
    run validate_binary_architecture "$binary" "k1"
    [ "$status" -eq 0 ]
}

@test "validate_binary_architecture: MIPS binary passes for ad5x" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_mips_elf "$binary"
    run validate_binary_architecture "$binary" "ad5x"
    [ "$status" -eq 0 ]
}

@test "validate_binary_architecture: ARM32 binary passes for pi32" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    run validate_binary_architecture "$binary" "pi32"
    [ "$status" -eq 0 ]
}

# --- AARCH64 binary tests ---

@test "validate_binary_architecture: AARCH64 binary passes for pi" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_aarch64_elf "$binary"
    run validate_binary_architecture "$binary" "pi"
    [ "$status" -eq 0 ]
}

# --- Cross-architecture mismatch tests ---

@test "validate_binary_architecture: AARCH64 binary FAILS for ad5m" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_aarch64_elf "$binary"
    run validate_binary_architecture "$binary" "ad5m"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: AARCH64 binary FAILS for k1" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_aarch64_elf "$binary"
    run validate_binary_architecture "$binary" "k1"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: ARM32 binary FAILS for k1" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    run validate_binary_architecture "$binary" "k1"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: ARM32 binary FAILS for ad5x" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    run validate_binary_architecture "$binary" "ad5x"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: AARCH64 binary FAILS for ad5x" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_aarch64_elf "$binary"
    run validate_binary_architecture "$binary" "ad5x"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: AARCH64 binary FAILS for pi32" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_aarch64_elf "$binary"
    run validate_binary_architecture "$binary" "pi32"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: ARM32 binary FAILS for pi" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    run validate_binary_architecture "$binary" "pi"
    [ "$status" -eq 1 ]
}

# --- Error message content ---

@test "validate_binary_architecture: mismatch reports expected vs actual" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_aarch64_elf "$binary"
    # Override log_error stub to capture output
    log_error() { echo "$@"; }
    export -f log_error
    run validate_binary_architecture "$binary" "ad5m"
    [ "$status" -eq 1 ]
    contains "Architecture mismatch" "${output}"
    contains "ARM 32-bit" "${output}"
    [[ "${output}" == *"AARCH64"* ]]
}

# --- Edge cases ---

@test "validate_binary_architecture: missing file returns error" {
    run validate_binary_architecture "/nonexistent/binary" "ad5m"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: non-ELF file returns error" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    echo "not an ELF file at all" > "$binary"
    run validate_binary_architecture "$binary" "ad5m"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: truncated file (less than 20 bytes) is handled" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    printf '\x7fELF\x01' > "$binary"  # Only 5 bytes
    run validate_binary_architecture "$binary" "ad5m"
    # Should fail: hexdump output will have fewer fields, awk fields 19/20 empty
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: empty file returns error" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    touch "$binary"
    # Empty file produces no hexdump output, so header is empty.
    # Empty/unreadable binaries should fail validation (not silently pass).
    run validate_binary_architecture "$binary" "ad5m"
    [ "$status" -eq 1 ]
}

@test "validate_binary_architecture: unknown platform skips validation" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    run validate_binary_architecture "$binary" "windows"
    [ "$status" -eq 0 ]
}

# --- Hex-reader fallback chain (see validate_binary_architecture) ---
#
# Each test shadows tools on PATH to simulate a real device profile from the
# physical fleet, then asserts a valid binary still validates.

# Shadow the named commands with a stub behaving like $2 ("missing" or "segv").
_shadow_tools() {
    local mode=$1; shift
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    local t
    for t in "$@"; do
        if [ "$mode" = "segv" ]; then
            printf '#!/bin/sh\nkill -SEGV $$\n' > "$BATS_TEST_TMPDIR/bin/$t"
        else
            printf '#!/bin/sh\necho "%s: not found" >&2\nexit 127\n' "$t" > "$BATS_TEST_TMPDIR/bin/$t"
        fi
        chmod +x "$BATS_TEST_TMPDIR/bin/$t"
    done
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

@test "validate_binary_architecture: segfaulting dd does not break validation (Pi CM4 libarmmem)" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    _shadow_tools segv dd
    run validate_binary_architecture "$binary" "pi32"
    [ "$status" -eq 0 ]
}

@test "validate_binary_architecture: works with no hexdump (BTT CB1 profile)" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    _shadow_tools missing hexdump
    run validate_binary_architecture "$binary" "pi32"
    [ "$status" -eq 0 ]
}

@test "validate_binary_architecture: works with only hexdump (Elegoo CC1 profile)" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    _shadow_tools missing od xxd
    run validate_binary_architecture "$binary" "pi32"
    [ "$status" -eq 0 ]
}

@test "validate_binary_architecture: an od emitting address offsets falls through, not misread" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    # hexdump absent, and od ignores -A n so it prefixes an offset column.
    _shadow_tools missing hexdump
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    printf '#!/bin/sh\necho "0000000 7f 45 4c 46 01 01 01 00"\n' > "$BATS_TEST_TMPDIR/bin/od"
    chmod +x "$BATS_TEST_TMPDIR/bin/od"
    # xxd must still carry it to a correct verdict.
    run validate_binary_architecture "$binary" "pi32"
    [ "$status" -eq 0 ]
}

@test "validate_binary_architecture: no working hex reader at all reports read failure" {
    local binary="$BATS_TEST_TMPDIR/helix-screen"
    create_fake_arm32_elf "$binary"
    _shadow_tools missing hexdump od xxd dd
    # helpers.bash stubs log_error to a no-op; override it to capture the text.
    log_error() { echo "$@"; }
    export -f log_error
    run validate_binary_architecture "$binary" "pi32"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no working hex reader"* ]]
}
