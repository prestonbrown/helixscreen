#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for print_platform_banner() in main.sh.
#
# Covers the user-facing wording that distinguishes "Detected platform: pi"
# (the install package) from the actual hardware. Reported by a QIDI Q2/Plus
# owner who read "pi" and thought we'd mis-identified their printer.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
MAIN_SH="$WORKTREE_ROOT/scripts/lib/installer/main.sh"

setup() {
    load helpers

    # Override the stub log_info from helpers.bash so we can capture output.
    # bats runs each test in a subshell, so this redefine is test-scoped.
    log_info() { printf '[INFO] %s\n' "$*"; }
    export -f log_info

    # BOLD/NC are unset in tests so substring matches are easy.
    BOLD=""
    NC=""

    # Source main.sh (which just defines functions; doesn't invoke main).
    unset _HELIX_MAIN_SOURCED
    . "$MAIN_SH"
}

# --- Pi-class package, hardware actually a Raspberry Pi ---

@test "print_platform_banner: real Pi keeps the original ordering" {
    describe_hardware() { echo "Raspberry Pi 4 Model B Rev 1.4"; }

    run print_platform_banner "pi"
    [ "$status" -eq 0 ]
    # First line is the platform; second line is the hardware label.
    [ "${lines[0]}" = "[INFO] Detected platform: pi" ]
    [ "${lines[1]}" = "[INFO] Hardware: Raspberry Pi 4 Model B Rev 1.4" ]
}

# --- Pi-class package, non-Pi hardware (the reported confusion) ---

@test "print_platform_banner: QIDI Q2 leads with hardware, demotes pi to package" {
    describe_hardware() {
        echo "QIDI-class SBC (likely Q2/Plus, hostname: linaro-alip, user: mks)"
    }

    run print_platform_banner "pi"
    [ "$status" -eq 0 ]
    # Hardware FIRST so the user sees their own device reflected back.
    [ "${lines[0]}" = "[INFO] Detected hardware: QIDI-class SBC (likely Q2/Plus, hostname: linaro-alip, user: mks)" ]
    # "pi" reframed as the install package, not a hardware claim.
    [[ "${lines[1]}" == "[INFO] Install package: pi"* ]] || fail "line 2 does not lead with the package framing: ${lines[1]}"
    contains "compatible with your SBC" "${lines[1]}"
    # The bare "Detected platform: pi" string MUST NOT appear — that's the
    # phrase that confused the QIDI user in the original bug report.
    [[ "${output}" != *"Detected platform: pi"* ]]
}

@test "print_platform_banner: BTT CB1 leads with hardware, demotes pi to package" {
    describe_hardware() { echo "BIGTREETECH SBC (BTT CB1)"; }

    run print_platform_banner "pi"
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "[INFO] Detected hardware: BIGTREETECH SBC (BTT CB1)" ]
    [[ "${lines[1]}" == "[INFO] Install package: pi"* ]]
}

@test "print_platform_banner: generic MKS-branded SBC leads with hardware" {
    describe_hardware() { echo "MKS-branded SBC (user: mks)"; }

    run print_platform_banner "pi"
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "[INFO] Detected hardware: MKS-branded SBC (user: mks)" ]
    [[ "${lines[1]}" == "[INFO] Install package: pi"* ]]
}

# --- pi32 follows the same logic as pi ---

@test "print_platform_banner: pi32 on QIDI hardware also reframes" {
    describe_hardware() { echo "QIDI-class SBC (hostname: linaro-alip)"; }

    run print_platform_banner "pi32"
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "[INFO] Detected hardware: QIDI-class SBC (hostname: linaro-alip)" ]
    [[ "${lines[1]}" == "[INFO] Install package: pi32"* ]]
}

@test "print_platform_banner: pi32 on real Pi keeps original ordering" {
    describe_hardware() { echo "Raspberry Pi Zero 2 W Rev 1.0"; }

    run print_platform_banner "pi32"
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "[INFO] Detected platform: pi32" ]
    [ "${lines[1]}" = "[INFO] Hardware: Raspberry Pi Zero 2 W Rev 1.0" ]
}

# --- Non-Pi platforms get the single-line treatment, no hardware lookup ---

@test "print_platform_banner: ad5m emits single line, no hardware lookup" {
    # If describe_hardware is called for non-pi platforms, fail loudly.
    describe_hardware() { echo "SHOULD NOT BE CALLED"; return 1; }

    run print_platform_banner "ad5m"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [ "${lines[0]}" = "[INFO] Detected platform: ad5m" ]
}

@test "print_platform_banner: k1 emits single line" {
    describe_hardware() { echo "SHOULD NOT BE CALLED"; return 1; }

    run print_platform_banner "k1"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [ "${lines[0]}" = "[INFO] Detected platform: k1" ]
}

@test "print_platform_banner: k2 emits single line" {
    describe_hardware() { echo "SHOULD NOT BE CALLED"; return 1; }

    run print_platform_banner "k2"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [ "${lines[0]}" = "[INFO] Detected platform: k2" ]
}

@test "print_platform_banner: snapmaker-u1 emits single line" {
    describe_hardware() { echo "SHOULD NOT BE CALLED"; return 1; }

    run print_platform_banner "snapmaker-u1"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [ "${lines[0]}" = "[INFO] Detected platform: snapmaker-u1" ]
}

@test "print_platform_banner: x86 emits single line" {
    describe_hardware() { echo "SHOULD NOT BE CALLED"; return 1; }

    run print_platform_banner "x86"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [ "${lines[0]}" = "[INFO] Detected platform: x86" ]
}
