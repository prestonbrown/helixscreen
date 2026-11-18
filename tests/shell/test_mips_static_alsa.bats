#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

# The unified MIPS build carries the ALSA backend, linked against the static
# alsa-lib in its toolchain sysroot: the K1 series' card is opened in-process
# (#1895). Every spelling of the target must agree, since `k1` and `ad5x` are
# the same binary.

load helpers

@test "unified MIPS targets build the ALSA backend against a static libasound" {
    for target in mips k1 ad5x; do
        run scratch_make -s PLATFORM_TARGET=$target CROSS_COMPILE= CC=gcc CXX=g++ print-var-ALSA_CXXFLAGS
        [ "$status" -eq 0 ]
        echo "$output" | grep -qE '(^|\s)-DHELIX_HAS_ALSA(\s|$)' || {
            echo "$target ALSA_CXXFLAGS lacks -DHELIX_HAS_ALSA: $output"
            return 1
        }
        run scratch_make -s PLATFORM_TARGET=$target CROSS_COMPILE= CC=gcc CXX=g++ print-var-ALSA_LIBS
        [ "$status" -eq 0 ]
        echo "$output" | grep -qE '(^|\s)-lasound(\s|$)' || {
            echo "$target ALSA_LIBS lacks -lasound: $output"
            return 1
        }
        run scratch_make -s PLATFORM_TARGET=$target CROSS_COMPILE= CC=gcc CXX=g++ print-target-ldflags
        [ "$status" -eq 0 ]
        echo "$output" | grep -qE '(^|\s)-static(\s|$)' || {
            echo "$target links dynamically, so -lasound would need the board's libasound.so: $output"
            return 1
        }
    done
}
