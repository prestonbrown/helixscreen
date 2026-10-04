#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

# Every pi32 target links libstdc++ statically: Raspberry Pi OS 32-bit's shared
# libstdc++ uses a different shared_ptr lock policy than the headers we build
# against, which corrupts the heap (#1732).

load helpers

@test "pi32 targets link libstdc++ and libgcc statically" {
    for target in pi32 pi32-fbdev pi32-both; do
        run make -s PLATFORM_TARGET=$target CROSS_COMPILE= CC=gcc CXX=g++ print-target-ldflags
        [ "$status" -eq 0 ]
        echo "$output" | grep -qE '(^|\s)-static-libstdc\+\+(\s|$)' || {
            echo "$target TARGET_LDFLAGS lacks -static-libstdc++: $output"
            return 1
        }
        echo "$output" | grep -qE '(^|\s)-static-libgcc(\s|$)' || {
            echo "$target TARGET_LDFLAGS lacks -static-libgcc: $output"
            return 1
        }
    done
}
