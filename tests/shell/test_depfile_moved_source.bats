#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# -MP lists every header of a .d as a phony target but never the main source.
# An object that keeps its name when its source moves (the display library
# names objects by basename) would then carry a prerequisite nothing can make.
# A tiny project with the same shape includes the real mk/depfiles.mk.

load helpers

setup() {
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd -P)"
    P="$BATS_TEST_TMPDIR/proj"
    mkdir -p "$P/old" "$P/new" "$P/obj" "$P/include"
    cat >"$P/Makefile" <<EOF
all: obj/widget.o
include $REPO/mk/depfiles.mk
obj/%.o: new/%.c
	cc \$(DEPFLAGS) -Iinclude -c \$< -o \$@
obj/%.o: old/%.c
	cc \$(DEPFLAGS) -Iinclude -c \$< -o \$@
-include \$(call depfiles,obj/*.d)
EOF
    echo '#define WIDGET 1' >"$P/include/widget.h"
    printf '#include "widget.h"\nint widget(void) { return WIDGET; }\n' >"$P/old/widget.c"
}

pmake() {
    make -C "$P" --no-print-directory "$@"
}

@test "a source moved under an object that keeps its name rebuilds instead of wedging" {
    run pmake
    [ "$status" -eq 0 ] || fail "$output"
    contains "old/widget.c" "$(cat "$P/obj/widget.d")"

    # mv keeps the mtime, so the object is newer than the moved source.
    mv "$P/old/widget.c" "$P/new/widget.c"
    run pmake
    [ "$status" -eq 0 ] || fail "$output"
    lacks "No rule to make target" "$output"
    contains "-c new/widget.c" "$output"
    contains "new/widget.c" "$(cat "$P/obj/widget.d")"

    run pmake
    [ "$status" -eq 0 ] || fail "$output"
    lacks "cc " "$output"
}

@test "a rebuilt object's header tracking survives the move" {
    pmake >/dev/null
    mv "$P/old/widget.c" "$P/new/widget.c"
    pmake >/dev/null
    sleep 1
    touch "$P/include/widget.h"
    run pmake
    [ "$status" -eq 0 ] || fail "$output"
    contains "-c new/widget.c" "$output"
}

@test "a source that is gone for good still fails the build loudly" {
    pmake >/dev/null
    rm "$P/old/widget.c"
    run pmake
    [ "$status" -ne 0 ] || fail "$output"
    contains "No rule to make target" "$output"
}

@test "a .d cut off at its source name is left alone, as one still being written" {
    touch "$P/obj/half.o"
    printf 'obj/half.o: old/gon' >"$P/obj/half.d"
    run pmake -n
    [ -e "$P/obj/half.o" ] || fail "the object of a half-written .d was deleted"
    [ -e "$P/obj/half.d" ] || fail "a half-written .d was deleted"
}

@test "a stale .d is found among more files than one shell argument can name" {
    pmake >/dev/null
    local i pad=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
    for i in $(seq 3000); do
        printf 'obj/%s_%s.o: include/widget.h\n' "$pad" "$i" >"$P/obj/${pad}_$i.d"
    done
    mv "$P/old/widget.c" "$P/new/widget.c"
    run pmake
    [ "$status" -eq 0 ] || fail "$output"
    contains "-c new/widget.c" "$output"
}
