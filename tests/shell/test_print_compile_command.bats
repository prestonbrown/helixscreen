#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# `make print-compile-command F=<source>` prints the command the source's object
# rule runs, as its .ccj fragment records it. scripts/compile_command.py trusts it
# like a current database entry: the clang gate fails a push on what it says. So
# it has to carry each rule's own flags (the tests/ rules add include paths and
# the test binary's defines, three app objects add large-file support), print
# nothing for a file no rule compiles, and build nothing on the way.
#
# Every make here runs in scratch_tree: a make in the tree under test races a
# gate's own lanes there.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# stdout only: make's own "Error 1" lines go to stderr.
pcc() {
    scratch_make -s print-compile-command "$@" 2>/dev/null
}

@test "a src/ TU prints the app rule's command" {
    run pcc F=src/system/config.cpp
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    contains "-include include/lvgl_pch.h" "$output"
    contains "-DHELIX_VERSION=\"$(cat VERSION.txt)\"" "$output"
    contains "-DLV_CONF_INCLUDE_SIMPLE" "$output"
    lacks "-Itests" "$output"
    [[ "$output" == *" -c src/system/config.cpp" ]] || fail "not the file's command: $output"
}

@test "a tests/ TU carries the test rule's include path and the test binary's defines" {
    local app tst
    app=$(pcc F=src/system/config.cpp)
    tst=$(pcc F=tests/unit/test_json_utils.cpp)
    contains " -Itests " "$tst"
    [[ "$tst" == *" -c tests/unit/test_json_utils.cpp" ]] || fail "not the file's command: $tst"
    # $(TEST_BIN)'s target-specific CXXFLAGS reach every test object through
    # inheritance; the test TU's command carries them once more than the app's.
    local flag want
    want=$(scratch_make -s print-var-TEST_BIN_CXXFLAGS)
    [ -n "$want" ] || fail "TEST_BIN_CXXFLAGS is empty"
    for flag in $want; do
        [ "$(grep -o -- " $flag " <<<"$tst " | wc -l)" -gt "$(grep -o -- " $flag " <<<"$app " | wc -l)" ] \
            || fail "$flag is not added to the tests/ command"
    done
}

@test "application and mock tests carry their own rules' include paths" {
    run pcc F=tests/unit/application/application_test_fixture.cpp
    [ "$status" -eq 0 ]
    contains " -Itests/unit/application " "$output"
    run pcc F=tests/mocks/mock_websocket_server.cpp
    [ "$status" -eq 0 ]
    contains " -Itests/mocks " "$output"
    lacks " -Itests " "$output"
}

@test "a file no rule compiles prints nothing and fails" {
    run pcc F=README.md
    [ "$status" -ne 0 ]
    [ -z "$output" ] || fail "printed for a file no rule compiles: $output"
}

@test "with -k, a file no rule compiles does not hide the others" {
    run pcc -k F="README.md src/system/config.cpp"
    [ "$status" -ne 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [[ "$output" == *" -c src/system/config.cpp" ]] || fail "lost the compilable file: $output"
}

# A target-specific CXXFLAGS on an object never reaches a recipe outside that
# object, so print-compile-command repeats them; this fails the day an object
# gains one the target does not carry.
@test "every target-specific CXXFLAGS on an app object reaches its source's command" {
    local db obj flags src seen=0
    db=$(make_db)
    while IFS= read -r line; do
        obj=${line%%: *}
        flags=${line#*+= }
        src="src/${obj#build/obj/}"
        src="${src%.o}.cpp"
        [ -f "$src" ] || continue
        seen=$((seen + 1))
        run pcc F="$src"
        [ "$status" -eq 0 ] || fail "no command for $src"
        contains " $flags " "$output "
    done < <(grep -E '^build/obj/[^ :]+\.o: CXXFLAGS \+= ' <<<"$db")
    [ "$seen" -gt 0 ] || fail "the database showed no target-specific CXXFLAGS on any object"
}

@test "printing a command builds nothing" {
    local tree
    tree=$(scratch_tree)
    rm -rf "$tree/build"/*
    run pcc F="src/system/config.cpp tests/unit/test_json_utils.cpp"
    [ "$status" -eq 0 ]
    # Parsing the Makefile writes its dot-file stamps (flags, patch id, ABI), as
    # every invocation does; anything else is something built.
    run find "$tree/build" -type f ! -name '.*'
    [ -z "$output" ] || fail "files written: $output"
    [ -f "$tree/build/obj/.build-flags" ] || fail "the parse wrote no flags stamp: is this make reading the tree?"
}

# A syntax-only check of an unbuilt tree makes compile-prereqs and nothing else,
# so it must cover everything the object rules wait for.
@test "compile-prereqs covers every prerequisite of the app and test object rules" {
    local db want have p
    db=$(make_db)
    have=$(grep -E '^compile-prereqs:' <<<"$db")
    [ -n "$have" ] || fail "no compile-prereqs target"
    for want in '^build/obj/%\.o: src/%\.cpp ' '^build/obj/tests/%\.o: tests/unit/%\.cpp '; do
        line=$(grep -E "$want" <<<"$db")
        [ -n "$line" ] || fail "no rule matching $want"
        for p in $(sed -e 's/^[^:]*: [^ ]* //' -e 's/|//' <<<"$line"); do
            contains " $p" "$have " || fail "compile-prereqs lacks $p"
        done
    done
}
