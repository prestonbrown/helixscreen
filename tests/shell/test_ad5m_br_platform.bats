#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

# Verify PLATFORM_TARGET=ad5m-br is recognized by the Makefile and produces
# the expected CFLAGS/LDFLAGS (no -static, no strip, toolchain-neutral).

load helpers

setup() {
    cd "$(scratch_tree)" || return 1
}

@test "ad5m-br: make accepts PLATFORM_TARGET=ad5m-br without unknown-target error" {
    run make -n PLATFORM_TARGET=ad5m-br CROSS_COMPILE= CC=gcc CXX=g++ cross-info
    [ "$status" -eq 0 ] || {
        echo "stdout: $output"
        return 1
    }
}

@test "ad5m-br: does NOT set -static in TARGET_LDFLAGS" {
    run make -n PLATFORM_TARGET=ad5m-br CROSS_COMPILE= CC=gcc CXX=g++ print-target-ldflags
    [ "$status" -eq 0 ]
    ! echo "$output" | grep -qE '(^|\s)-static(\s|$)' || {
        echo "Unexpected -static in ad5m-br TARGET_LDFLAGS:"
        echo "$output"
        return 1
    }
}

@test "ad5m-br: STRIP_BINARY is not yes" {
    run make -n PLATFORM_TARGET=ad5m-br CROSS_COMPILE= CC=gcc CXX=g++ print-strip
    [ "$status" -eq 0 ]
    ! echo "$output" | grep -qE 'STRIP_BINARY=yes' || {
        echo "Unexpected STRIP_BINARY=yes in ad5m-br:"
        echo "$output"
        return 1
    }
}

@test "ad5m-br: inherits ad5m TARGET_CFLAGS cpu/fpu flags" {
    run make -n PLATFORM_TARGET=ad5m-br CROSS_COMPILE= CC=gcc CXX=g++ print-target-cflags
    [ "$status" -eq 0 ]
    echo "$output" | grep -qE 'mtune=cortex-a7' || {
        echo "Missing cortex-a7 tuning in ad5m-br TARGET_CFLAGS:"
        echo "$output"
        return 1
    }
    echo "$output" | grep -qE 'mfpu=neon-vfpv4' || {
        echo "Missing neon-vfpv4 in ad5m-br TARGET_CFLAGS:"
        echo "$output"
        return 1
    }
}

@test "ad5m-br: sound enabled (HELIX_HAS_SOUND), tracker disabled" {
    run make -n PLATFORM_TARGET=ad5m-br CROSS_COMPILE= CC=gcc CXX=g++ print-cxxflags
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'DHELIX_HAS_SOUND' || {
        echo "Expected -DHELIX_HAS_SOUND in ad5m-br CXXFLAGS:"
        echo "$output"
        return 1
    }
    ! echo "$output" | grep -q 'DHELIX_HAS_TRACKER' || {
        echo "Unexpected -DHELIX_HAS_TRACKER (should be off on ad5m-br):"
        echo "$output"
        return 1
    }
}

@test "ad5m: sound enabled, tracker disabled (note fallback is not print-safe)" {
    run make -n PLATFORM_TARGET=ad5m CROSS_COMPILE= CC=gcc CXX=g++ print-cxxflags
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'DHELIX_HAS_SOUND' || {
        echo "Expected -DHELIX_HAS_SOUND in ad5m CXXFLAGS:"
        echo "$output"
        return 1
    }
    ! echo "$output" | grep -q 'DHELIX_HAS_TRACKER' || {
        echo "Unexpected -DHELIX_HAS_TRACKER: supports_render_source() is false on the PWM backend, so tracker would run the set_voice note fallback on the un-demoted sequencer thread:"
        echo "$output"
        return 1
    }
    echo "$output" | grep -q 'DHELIX_PWM_AUTO_EXPORT' || {
        echo "Expected -DHELIX_PWM_AUTO_EXPORT in ad5m CXXFLAGS (stock kernel ships pwm6 unexported):"
        echo "$output"
        return 1
    }
}

@test "ad5x: tracker enabled for the jz PC-speaker path" {
    run make -n PLATFORM_TARGET=ad5x CROSS_COMPILE= CC=gcc CXX=g++ print-cxxflags
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'DHELIX_HAS_TRACKER' || {
        echo "Expected -DHELIX_HAS_TRACKER in ad5x CXXFLAGS (tracker rides the jz PC-speaker path - per-note buffers through the fx-pwm daemon, no PCM render loop):"
        echo "$output"
        return 1
    }
    ! echo "$output" | grep -q 'DHELIX_PWM_AUTO_EXPORT' || {
        echo "Unexpected -DHELIX_PWM_AUTO_EXPORT (pwm6 function unverified on ad5x):"
        echo "$output"
        return 1
    }
}

@test "ad5m-br: no libusb" {
    # Pass CROSS_COMPILE so the cross-target LDFLAGS branch fires and libusb
    # filter is evaluated meaningfully.
    run make -n PLATFORM_TARGET=ad5m-br CROSS_COMPILE=arm-buildroot-linux-gnueabihf- CC=gcc CXX=g++ print-ldflags
    [ "$status" -eq 0 ]
    ! echo "$output" | grep -qE '(^|[[:space:]"])-lusb-1\.0([[:space:]"]|$)' || {
        echo "Unexpected -lusb-1.0 in ad5m-br LDFLAGS:"
        echo "$output"
        return 1
    }
}

@test "ad5m-br: OpenSSL linked dynamically (no -Wl,-Bstatic -lssl)" {
    run make -n PLATFORM_TARGET=ad5m-br CROSS_COMPILE=arm-buildroot-linux-gnueabihf- CC=gcc CXX=g++ print-ldflags
    [ "$status" -eq 0 ]
    ! echo "$output" | grep -qE -- '-Wl,-Bstatic[[:space:]]+-lssl' || {
        echo "OpenSSL is statically linked in ad5m-br (should be dynamic):"
        echo "$output"
        return 1
    }
    echo "$output" | grep -qE -- '-lssl' || {
        echo "Expected -lssl in ad5m-br LDFLAGS:"
        echo "$output"
        return 1
    }
}

# Buildroot's TARGET_CONFIGURE_OPTS passes the toolchain as CC/CXX and never sets
# CROSS_COMPILE. The build must still take the cross path: target tuning, and no
# host include path, which Buildroot's compiler wrapper refuses outright.
@test "ad5m-br: a Buildroot-style CC without CROSS_COMPILE builds for the target" {
    local cc=/opt/br/host/bin/arm-buildroot-linux-gnueabihf-gcc
    run env -u CROSS_COMPILE make -n PLATFORM_TARGET=ad5m-br CC="$cc" CXX="${cc%gcc}g++" print-cxxflags
    [ "$status" -eq 0 ] || { echo "$output"; return 1; }
    ! echo "$output" | grep -q '/usr/include/libusb-1.0' || {
        echo "Host libusb include path in a Buildroot ad5m-br build:"; echo "$output"; return 1
    }
    echo "$output" | grep -qE 'mtune=cortex-a7' || {
        echo "ad5m-br target tuning missing (took the host branch):"; echo "$output"; return 1
    }
}
