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
XML_TOKENS=ui_xml/globals.xml
TABLE=src/generated/theme_token_table.cpp
SCHEMA=tools/xml-linter/schema/schema.json

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
    fixture a-ok.xml > "$XML_TOKENS"
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
    # The token table as the base XML renders it: a commit that stages no token
    # owes no regeneration.
    python3 scripts/gen_theme_tokens.py >/dev/null
    # The linter's snapshot, and a regeneration that counts the XML's tokens:
    # a staged token makes the snapshot stale, as in the real tree.
    mkdir -p tools/xml-linter/schema
    printf '{"tokens": "0"}\n' > "$SCHEMA"
    cat > Makefile <<'MAKEFILE'
regen-xml-schema:
	@printf '{"tokens": "%s"}\n' "$$(grep -c '<px name=' ui_xml/globals.xml)" > tools/xml-linter/schema/schema.json
MAKEFILE
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

@test "the gates after the C++ format read the formatted text, not the copy from before" {
  # clang-format drops the trailing spaces along with the rest of the repair, so
  # only the copy from before the format still has them to report.
  needs_venv
  stage "$CPP" a-unformatted-trailing-ws.cpp
  fixture a-unformatted-trailing-ws.cpp > "$CPP"

  hook

  [ "$status" -eq 0 ] || fail "$output"
  lacks "Found trailing whitespace" "$output"
  [ "$(staged "$CPP")" = "$(fixture a-formatted.cpp)" ] || fail "the index holds: $(staged "$CPP")"
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

@test "a staged token edit regenerates the token table into the index" {
    needs_venv
    stage "$XML_TOKENS" a-token.xml
    fixture a-token.xml > "$XML_TOKENS"

    hook

    [ "$status" -eq 0 ] || fail "$output"
    contains "Regenerated and staged $TABLE" "$output"
    [ "$(staged "$TABLE")" != "$(git show "HEAD:$TABLE")" ] || fail "the index kept the old table"
    contains "space_probe" "$(staged "$TABLE")"
    contains "space_probe" "$(cat "$TABLE")"

    # The commit holds the table its own XML renders, so the check alone passes.
    check
    [ "$status" -eq 0 ] || fail "$output"
    contains "Theme artefacts match 1 tokens" "$output"
}

# Gives the hook a git that holds the index lock for a second around every
# write (fixtures/staged-content/slow-git.sh), exported as a shell function so
# that it reaches the hook `git commit` runs too. Two writes in one commit
# then overlap for certain.
slow_git() {
    . "$FIXTURES/slow-git.sh"
    export -f git
}

@test "one commit that wakes both artifact gates stages both" {
    needs_venv
    # A token edit wakes qc_theme_artifacts by its path and qc_xml_schema by the
    # added definition. Both write the index, in the fan-out at the same time.
    slow_git
    stage "$XML_TOKENS" a-token.xml
    fixture a-token.xml > "$XML_TOKENS"

    hook

    [ "$status" -eq 0 ] || fail "$output"
    contains "Regenerated and staged $SCHEMA" "$output"
    contains "Regenerated and staged $TABLE" "$output"
    [ "$(staged "$SCHEMA")" = "$(printf '{"tokens": "1"}\n')" ] || fail "the index holds: $(staged "$SCHEMA")"
    contains "space_probe" "$(staged "$TABLE")"
}

# A pre-commit hook for the scratch repo with the line the real one ends on.
commit_hook() {
    mkdir -p "$R/.githooks"
    cat > "$R/.githooks/pre-commit" <<'HOOK'
#!/bin/bash
set -e
cd "$(git rev-parse --show-toplevel)"
exec ./scripts/quality-checks.sh --staged-only --auto-fix
HOOK
    chmod +x "$R/.githooks/pre-commit"
    git config core.hooksPath .githooks
}

@test "git commit -- <paths> hands the hook a temporary index, and both writes land in the commit" {
    needs_venv
    # That index is .git/next-index-<pid>.lock, so its lock ends in .lock.lock:
    # git's refusal names it, and the retry has to read that name too.
    slow_git
    commit_hook
    fixture a-token.xml > "$XML_TOKENS"

    run git commit -qm "a token" -- "$XML_TOKENS"

    [ "$status" -eq 0 ] || fail "$output"
    [ "$(git show "HEAD:$SCHEMA")" = "$(printf '{"tokens": "1"}\n')" ] || fail "the commit holds: $(git show "HEAD:$SCHEMA")"
    contains "space_probe" "$(git show "HEAD:$TABLE")"
    contains "$SCHEMA" "$(git show --stat --format= HEAD)"
    contains "$TABLE" "$(git show --stat --format= HEAD)"
}

# Holds git's index lock across a write made through qc_index_write, in the
# background: the write must wait, then land once the lock is moved away.
# The lock is renamed, never removed: the scratch repo is bats' to delete.
lock_waiter() (
    cd "$R" || exit 1
    . scripts/qc/_lib.sh
    lock="$(git rev-parse --git-dir)/index.lock"
    : > "$lock"
    qc_index_write git update-index --cacheinfo "100644,$(git hash-object -w --stdin < "$FIXTURES/a-more.cpp.txt"),$CPP" &
    writer=$!
    sleep 0.5
    if [ "$(staged "$CPP")" != "$(fixture a-ok.cpp)" ]; then kill "$writer" 2>/dev/null; exit 3; fi
    if ! kill -0 "$writer" 2>/dev/null; then exit 4; fi
    mv "$lock" "$lock.released"
    wait "$writer" || exit 5
    [ "$(staged "$CPP")" = "$(fixture a-more.cpp)" ] || exit 6
    [ ! -e "$lock" ] || exit 7
    echo LANDED
)

@test "an index write that meets a held index.lock waits for it and lands" {
    run lock_waiter
    [ "$status" -eq 0 ] || fail "exit $status: the write did not wait for the lock, or did not land after it: $output"
    contains "LANDED" "$output"
}

@test "the token table is regenerated in the index alone when the working copy holds unstaged edits" {
    needs_venv
    stage "$XML_TOKENS" a-token.xml
    fixture a-token.xml > "$XML_TOKENS"
    printf '// unstaged\n' >> "$TABLE"

    hook

    [ "$status" -eq 0 ] || fail "$output"
    contains "Regenerated and staged $TABLE" "$output"
    contains "space_probe" "$(staged "$TABLE")"
    # The line the author did not stage is neither in the commit nor lost.
    contains "// unstaged" "$(cat "$TABLE")"
    lacks "// unstaged" "$(staged "$TABLE")"
}

# clang-format and the XML formatter come from the venv. Without it the hook fails
# at the format check, so every case that needs the hook to pass says so and
# skips, rather than fail for a reason that is not what it pins.
needs_venv() {
    [ -x "$REPO/.venv/bin/clang-format" ] || skip "no pinned clang-format in .venv"
}
