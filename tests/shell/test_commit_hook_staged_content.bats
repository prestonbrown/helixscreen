#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The commit hook judges what is staged, not the working tree. A per-file gate
# takes its files from the index, and it has to read their content from the
# index too: stage an offense and then clean the working copy, and a gate that
# reads the disk finds nothing while the bad blob commits. Each case runs the
# real driver, --staged-only, in a scratch repository with the real scripts
# linked in: once with the offense in the index alone, which must fail (or be
# repaired in the index), and once on disk alone, which must pass untouched.

load helpers

REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
FIXTURES="$REPO/tests/shell/fixtures/staged-content"
CPP=src/ui/a.cpp
XML=ui_xml/a.xml
SH=config/platform/x.sh

setup() {
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_INDEX_VERSION GIT_OBJECT_DIRECTORY
    R="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$R/$(dirname "$CPP")" "$R/$(dirname "$XML")" "$R/$(dirname "$SH")" "$R/include" "$R/mk"
    cd "$R" || return 1
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test"
    fixture a-ok.cpp > "$CPP"
    fixture a-ok.xml > "$XML"
    # The icon rule refuses a tree without one <icon> tag: its pattern
    # scanning nothing is not a clean tree.
    fixture a-icons.xml > ui_xml/icons.xml
    fixture a-ok.sh > "$SH"
    # What the XML rules read beside the staged files.
    printf 'static const IconEntry kIconMap[] = {\n' > include/ui_icon_codepoints.h
    printf '    {"alert", "\\xF3\\xB0\\x80\\xA6"},\n};\n' >> include/ui_icon_codepoints.h
    printf 'DEV_PANEL_XML := dev_panel.xml\n' > mk/cross.mk
    ln -s "$REPO/scripts" scripts
    cp "$REPO/.clang-format" "$REPO/requirements.txt" .
    if [ -e "$REPO/.venv" ]; then ln -s "$REPO/.venv" .venv; fi
    git add -A && git commit -qm base
}

# Each fixture is "<name>.txt": a gate judges a staged file by its extension, so
# an offending fixture with the extension it imitates would fail its own test's
# commit.
fixture() { # $1 = a file under fixtures/staged-content, printed as it is
    cat "$FIXTURES/$1.txt"
}

# Puts fixture $2 in the index for $1, byte for byte, and leaves the working
# copy as it is: `fixture $2 > "$1"` then writes the same bytes to disk.
stage() {
    git update-index --cacheinfo "100644,$(git hash-object -w --stdin < "$FIXTURES/$2.txt"),"$1""
}

# The index content of $1.
staged() {
    git show ":$1"
}

# The index back to HEAD for every path: the next half of a case starts clean.
unstage_all() {
    git read-tree HEAD
}

hook() { # as git runs it before a commit
    run bash -c 'cd "$1" && QC_NO_CACHE=1 ./scripts/quality-checks.sh --staged-only --auto-fix' _ "$R"
}

check() { # the same, without the repair
    run bash -c 'cd "$1" && QC_NO_CACHE=1 ./scripts/quality-checks.sh --staged-only' _ "$R"
}

# A gate that reads a file. First the offense in the index alone, with the
# working copy clean: the hook fails and names it. Then the same offense on
# disk alone, under a clean staged change so the file is staged at all: the
# hook passes and says nothing about it.
# $1 = the path, $2 = the offending fixture, $3 = the clean staged fixture,
# $4 = what the transcript must say.
assert_judged_on_staged_file() {
    stage "$1" "$2"
    check
    [ "$status" -ne 0 ] || fail "an offense in the index alone passed the hook: $output"
    contains "$4" "$output"

    unstage_all
    fixture "$2" > "$1"
    stage "$1" "$3"
    check
    [ "$status" -eq 0 ] || fail "an offense on disk alone failed the hook: $output"
    lacks "$4" "$output"
}

@test "a missing copyright header is judged on the staged file" {
    assert_judged_on_staged_file "$CPP" a-no-header.cpp a-more.cpp \
        "Missing GPL v3 header: $CPP"
}

@test "a merge conflict marker is judged on the staged file" {
    assert_judged_on_staged_file "$CPP" a-conflict.cpp a-more.cpp \
        "Merge conflict markers found in:"
}

@test "trailing whitespace is reported on the staged file" {
    stage "$CPP" a-trailing-ws.cpp
    check
    [ "$status" -eq 0 ] || fail "$output"
    contains "Found trailing whitespace:" "$output"
    contains "$CPP:3:" "$output"

    unstage_all
    fixture a-trailing-ws.cpp > "$CPP"
    stage "$CPP" a-more.cpp
    check
    [ "$status" -eq 0 ] || fail "$output"
    lacks "Found trailing whitespace:" "$output"
}

@test "malformed XML is judged on the staged file" {
    assert_judged_on_staged_file "$XML" a-broken.xml a-more.xml \
        "Invalid XML: $XML"
}

@test "a hand-built header button is judged on the staged XML, not the working copy" {
    assert_judged_on_staged_file "$XML" a-bad-header.xml a-more.xml \
        "Run: python3 scripts/check_xml_rules.py --rule header-actions --list"
}

@test "a stray printf is judged on the staged file" {
    assert_judged_on_staged_file "$CPP" a-printf.cpp a-more.cpp \
        "Use spdlog::info/debug/warn/error instead"
}

@test "an UPPER_SNAKE constant is judged on the staged file" {
    assert_judged_on_staged_file "$CPP" a-upper.cpp a-more.cpp \
        "Rename to kCamelCase"
}

@test "unformatted C++ is judged on the staged file" {
    needs_venv
    assert_judged_on_staged_file "$CPP" a-unformatted.cpp a-more.cpp \
        "Unformatted C++"
}

@test "the hook formats a staged C++ file in the index and on disk when nothing is held back" {
    needs_venv
    stage "$CPP" a-unformatted.cpp
    fixture a-unformatted.cpp > "$CPP"

    hook

    [ "$status" -eq 0 ] || fail "$output"
    [ "$(staged "$CPP")" = "$(fixture a-formatted.cpp)" ] || fail "the index holds: $(staged "$CPP")"
    [ "$(cat "$CPP")" = "$(fixture a-formatted.cpp)" ] || fail "the working copy was not formatted"
}

@test "the hook formats a partially staged C++ file in the index alone and leaves the held-back line on disk" {
    needs_venv
    stage "$CPP" a-unformatted.cpp
    fixture a-unformatted.cpp > "$CPP"
    printf 'int unstaged;\n' >> "$CPP"

    hook

    [ "$status" -eq 0 ] || fail "$output"
    [ "$(staged "$CPP")" = "$(fixture a-formatted.cpp)" ] || fail "the index holds: $(staged "$CPP")"
    # The line the author did not stage is neither in the commit nor lost.
    contains "int unstaged;" "$(cat "$CPP")"
    contains "int    a=1;" "$(cat "$CPP")"
}

@test "the hook leaves a clean staged C++ file alone when only the working copy is unformatted" {
    needs_venv
    stage "$CPP" a-more.cpp
    fixture a-unformatted.cpp > "$CPP"

    hook

    [ "$status" -eq 0 ] || fail "$output"
    [ "$(staged "$CPP")" = "$(fixture a-more.cpp)" ] || fail "the index changed: $(staged "$CPP")"
    [ "$(cat "$CPP")" = "$(fixture a-unformatted.cpp)" ] || fail "the working copy changed"
}

@test "the hook judges and formats XML on the staged file, not the working copy" {
    needs_venv
    stage "$XML" a-unformatted.xml
    fixture a-unformatted.xml > "$XML"

    hook

    [ "$status" -eq 0 ] || fail "$output"
    [ "$(staged "$XML")" = "$(fixture a-ok.xml)" ] || fail "the index holds: $(staged "$XML")"
    [ "$(cat "$XML")" = "$(fixture a-ok.xml)" ] || fail "the working copy was not formatted"

    unstage_all
    fixture a-unformatted.xml > "$XML"
    stage "$XML" a-more.xml

    hook

    [ "$status" -eq 0 ] || fail "$output"
    [ "$(staged "$XML")" = "$(fixture a-more.xml)" ] || fail "the index changed: $(staged "$XML")"
    [ "$(cat "$XML")" = "$(fixture a-unformatted.xml)" ] || fail "the working copy changed"
}

@test "a shellcheck finding is judged on the staged script, not the working copy" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck is not installed"
    assert_judged_on_staged_file "$SH" a-bad.sh a-ok.sh "shellcheck found issues in 1 file(s)"
}

# clang-format and the XML formatter come from the venv; without it the driver
# cannot format, and the cases that need one say so rather than pass for free.
needs_venv() {
    [ -x "$REPO/.venv/bin/clang-format" ] || skip "no pinned clang-format in .venv"
}
