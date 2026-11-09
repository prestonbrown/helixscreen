#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

# The unified MIPS build (and its k1/ad5x aliases) carries the sysfs PWM backend
# for a buzzer named in sound.pwm_channel, without the unprompted pwm6 export
# that only the AD5M family gets (prestonbrown/helixscreen#1974).

load helpers

setup() {
    cd "$(scratch_tree)" || return 1
}

@test "mips targets compile the sysfs PWM backend without auto-export" {
    for target in mips k1 ad5x; do
        run make -n PLATFORM_TARGET=$target CROSS_COMPILE= CC=gcc CXX=g++ print-cxxflags
        [ "$status" -eq 0 ] || { echo "$target: $output"; return 1; }
        echo "$output" | grep -q 'DHELIX_HAS_PWM_SOUND' || {
            echo "$target: expected -DHELIX_HAS_PWM_SOUND in CXXFLAGS:"
            echo "$output"
            return 1
        }
        ! echo "$output" | grep -q 'DHELIX_PWM_AUTO_EXPORT' || {
            echo "$target: unexpected -DHELIX_PWM_AUTO_EXPORT (no unprompted channel export off the AD5M family):"
            echo "$output"
            return 1
        }
        run make -n PLATFORM_TARGET=$target CROSS_COMPILE= CC=gcc CXX=g++ print-var-APP_SRCS
        [ "$status" -eq 0 ] || { echo "$target: $output"; return 1; }
        echo "$output" | grep -q 'system/pwm_sound_backend.cpp' || {
            echo "$target: expected src/system/pwm_sound_backend.cpp in APP_SRCS:"
            echo "$output"
            return 1
        }
    done
}
