#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# land-gate runs its generated-artefact checks (make check-theme-tokens and the
# rest of its fresh lane) while other lanes compile in the same tree, and every
# compile rewrites its .d file in place. A make that parses a .d mid-write stops
# with "missing separator", so the goals that compile nothing do not read them.

load helpers

setup() {
    OBJ="$(scratch_tree)/build/obj"
    mkdir -p "$OBJ"
    # A -MP depfile cut off mid-way through its phony header lines.
    printf 'build/obj/half.o: include/a.h \\\n include/b.h\ninclude/a.h:\ninclude/b\n' > "$OBJ/half.d"
}

teardown() {
    rm -f "$OBJ/half.d"
}

@test "a goal that compiles nothing parses past a half-written .d" {
    run scratch_make check-theme-tokens
    [ "$status" -eq 0 ] || fail "$output"
    lacks "missing separator" "$output"
}

@test "every fresh-lane goal of land-gate is one that skips the .d files" {
    run scratch_make -n check-xml-schema lint-xml check-lvgl-event-codes test-python
    lacks "missing separator" "$output"
}

@test "a goal that compiles still reads the .d files" {
    run scratch_make -n help
    contains "missing separator" "$output"
}
