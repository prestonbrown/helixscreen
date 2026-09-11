#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pins the size-flag treatment the 128MB-class boards depend on: ENABLE_MOCKS=no,
# -DNDEBUG, and -fno-rtti on our C++ but never on the submodules.
#
# On these boards helix-screen's file-backed text competes for page cache with
# Klipper, and a Klipper stalled on flash IO is a "Timer too close". The flags
# are worth ~1.2MB of binary and ~1MB of resident file-backed pages, so losing
# one to a cross.mk reshuffle is silent everywhere a build or a test suite can
# see it. Both halves of the contract are asserted: the class targets carry the
# flags, and the membership list is exactly this set, so adding a target to the
# filter or forgetting one is a red test rather than a measurement months later.

load helpers

# The 110-128MB boards. cc1 is 112MB, ad5m/ad5m-br are the same 110MB hardware.
LEAN_TARGETS="cc1 ad5m ad5m-br"

# Roomy board, same Makefile paths: the control that proves these assertions
# discriminate rather than pass on everything.
CONTROL_TARGET="pi"

mkvar() {
    local target="$1" var="$2"
    make -n PLATFORM_TARGET="$target" CROSS_COMPILE= CC=gcc CXX=g++ "print-var-$var" 2>/dev/null
}

@test "size flags: class targets build mock-free" {
    for t in $LEAN_TARGETS; do
        mkvar "$t" ENABLE_MOCKS | grep -qE '(^|")no("|$)' || {
            echo "$t does not set ENABLE_MOCKS := no:"
            mkvar "$t" ENABLE_MOCKS
            return 1
        }
        # The variable is only half of it — what ships is the define.
        mkvar "$t" CXXFLAGS | grep -q 'DHELIX_ENABLE_MOCKS' && {
            echo "$t still compiles with -DHELIX_ENABLE_MOCKS despite ENABLE_MOCKS=no:"
            mkvar "$t" CXXFLAGS
            return 1
        }
    done
    return 0
}

@test "size flags: class targets carry -DNDEBUG in every flag set" {
    for t in $LEAN_TARGETS; do
        for v in CFLAGS CXXFLAGS SUBMODULE_CFLAGS SUBMODULE_CXXFLAGS; do
            mkvar "$t" "$v" | grep -q 'DNDEBUG' || {
                echo "$t is missing -DNDEBUG in $v:"
                mkvar "$t" "$v"
                return 1
            }
        done
    done
    return 0
}

@test "size flags: -fno-rtti reaches our C++ but never the submodules" {
    for t in $LEAN_TARGETS; do
        mkvar "$t" CXXFLAGS | grep -q 'fno-rtti' || {
            echo "$t is missing -fno-rtti in CXXFLAGS:"
            mkvar "$t" CXXFLAGS
            return 1
        }
        # libhv throws, and its catch clauses want typeinfo for the thrown
        # types. -fno-rtti there is a link failure deep in a cross build.
        mkvar "$t" SUBMODULE_CXXFLAGS | grep -q 'fno-rtti' && {
            echo "$t applies -fno-rtti to SUBMODULE_CXXFLAGS, which breaks libhv's catch clauses:"
            mkvar "$t" SUBMODULE_CXXFLAGS
            return 1
        }
    done
    return 0
}

@test "size flags: a roomy target gets none of them" {
    mkvar "$CONTROL_TARGET" CXXFLAGS | grep -q 'DHELIX_ENABLE_MOCKS' || {
        echo "$CONTROL_TARGET should keep mocks; the assertions above are not discriminating:"
        mkvar "$CONTROL_TARGET" CXXFLAGS
        return 1
    }
    mkvar "$CONTROL_TARGET" CXXFLAGS | grep -qE 'DNDEBUG|fno-rtti' && {
        echo "$CONTROL_TARGET unexpectedly carries size flags:"
        mkvar "$CONTROL_TARGET" CXXFLAGS
        return 1
    }
    return 0
}

@test "size flags: the class membership list is exactly cc1 ad5m ad5m-br" {
    local all found=""
    all=$(grep -oE 'ifeq \(\$\(PLATFORM_TARGET\),[a-z0-9_-]+\)' "$BATS_TEST_DIRNAME/../../mk/cross.mk" \
        | sed 's/.*,\(.*\))/\1/' | sort -u)

    [ -n "$all" ] || {
        echo "could not enumerate PLATFORM_TARGETs from mk/cross.mk"
        return 1
    }

    for t in $all; do
        if mkvar "$t" CXXFLAGS | grep -q 'fno-rtti'; then
            found="$found $t"
        fi
    done

    local expected
    expected=$(echo "$LEAN_TARGETS" | tr ' ' '\n' | sort | tr '\n' ' ')
    found=$(echo "$found" | tr ' ' '\n' | grep -v '^$' | sort | tr '\n' ' ')

    [ "$found" = "$expected" ] || {
        echo "size-flag membership drifted."
        echo "  expected: $expected"
        echo "  actual:   $found"
        echo "A new 128MB-class board needs its own cross build and a boot check"
        echo "on the real device before it joins this list."
        return 1
    }
}
