#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

# A system spdlog is looked for on the build host only for builds that run
# there (native) or whose compiler resolves the same paths in its sysroot
# (yocto). Every other platform target builds the bundled lib/spdlog, so a
# host's own spdlog never leaks into a cross build (#1979).

load helpers

setup() {
    fake="$BATS_TEST_TMPDIR/include/spdlog/spdlog.h"
    mkdir -p "${fake%/*}"
    : >"$fake"
}

# Prints "SPDLOG_SYSTEM_HEADER=<SPDLOG_SYSTEM_HEADER>" then "inc=<SPDLOG_INC>". Each value is
# the last line of its own make run, below the banner a cross target echoes. Toolchain
# warnings go to stderr and are not part of the value.
spdlog_header_for() {
    local v
    for v in SPDLOG_SYSTEM_HEADER SPDLOG_INC; do
        printf '%s=%s\n' "$v" "$(scratch_make -s "$@" CC=gcc CXX=g++ \
            SPDLOG_HOST_HEADER_PATHS="$fake" print-var-$v 2>/dev/null | tail -n 1)"
    done
}

@test "native builds use a system spdlog the host has" {
    run spdlog_header_for PLATFORM_TARGET=native
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "SPDLOG_SYSTEM_HEADER=$fake" ]
    [[ "$output" != *lib/spdlog/include* ]]
}

@test "yocto builds keep the system spdlog probe" {
    run spdlog_header_for PLATFORM_TARGET=yocto
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = "SPDLOG_SYSTEM_HEADER=$fake" ]
}

@test "cross builds ignore the host's spdlog and use the bundled copy" {
    for target in pi pi32 ad5m k1 cc1 x86; do
        run spdlog_header_for PLATFORM_TARGET=$target CROSS_COMPILE=
        [ "$status" -eq 0 ]
        [ "${lines[0]}" = "SPDLOG_SYSTEM_HEADER=" ] || {
            echo "$target picked up the host spdlog: $output"
            return 1
        }
        [[ "$output" == *"-isystem lib/spdlog/include"* ]] || {
            echo "$target does not include the bundled spdlog: $output"
            return 1
        }
    done
}
