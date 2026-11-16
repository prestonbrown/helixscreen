#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mk/enable-flags.mk, included by a scratch Makefile that defaults one flag and
# tests it the way the real makefiles do.

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    export JOBPOOL=0
    unset ENABLE_FEATURE
    T="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$T"
    cat >"$T/Makefile" <<MK
include $REPO_ROOT/mk/enable-flags.mk
ENABLE_FEATURE ?= yes
ifeq (\$(ENABLE_FEATURE),yes)
STATE := on
else
STATE := off
endif
show:
	@echo "\$(STATE)"
MK
    cd "$T" || return 1
}

feature() {
    run make -s show "$@"
}

@test "on-words in any case turn a command-line flag on" {
    for v in 1 yes true on YES True ON; do
        feature ENABLE_FEATURE="$v"
        [ "$status" -eq 0 ] || fail "$v: $output"
        [ "$output" = on ] || fail "$v read as $output"
    done
}

@test "off-words and an empty value turn a command-line flag off" {
    for v in 0 no false off NO Off ""; do
        feature ENABLE_FEATURE="$v"
        [ "$status" -eq 0 ] || fail "'$v': $output"
        [ "$output" = off ] || fail "'$v' read as $output"
    done
}

@test "an unknown value stops the build and names the flag" {
    feature ENABLE_FEATURE=maybe
    [ "$status" -ne 0 ] || fail "$output"
    contains "ENABLE_FEATURE=maybe is not a yes/no value" "$output"
}

@test "an unset flag keeps the makefile's default" {
    feature
    [ "$output" = on ] || fail "$output"
}
