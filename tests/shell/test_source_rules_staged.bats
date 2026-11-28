#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for --staged-only of scripts/check_source_rules.py: a file-set rule judges
# the paths the commit stages under its scope, each read as its staged blob, and never
# the working tree. section-returns stands in for every rule, because the mode belongs
# to the script and not to the rule; one case runs them all, and the rest hold what the
# mode refuses.

load helpers

RULES="python3 scripts/check_source_rules.py"
# Every rule --staged-only applies to.
FILE_SET_RULES="print-state-cast json-dump-utf8 raw-print-state xml-create-then-hide section-returns init-list-ternary getenv-setenv lvgl-subject-api"
# getenv-setenv's scan roots. A root the index holds no file under fails the rule, so a
# repo that runs it needs a file in each.
GETENV_ROOTS="tests src include tools firmware android server moonraker-plugin ui_xml"
# A return that hides a later sibling section: section-returns' site.
SECTIONS='TEST_CASE("x") {
    SECTION("a") { return; }
    SECTION("b") {}
}'
# An lv_subject_t held by value: lvgl-subject-api's site.
STORAGE='lv_subject_t by_value_;'

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    mkdir -p "$REPO"
    git -C "$REPO" init -q
    git -C "$REPO" config user.email t@t
    git -C "$REPO" config user.name t
}

# stage <path under the repo> <text>: writes the file and stages it.
stage() {
    mkdir -p "$REPO/$(dirname "$1")"
    printf '%s\n' "$2" > "$REPO/$1"
    git -C "$REPO" add "$1"
}

# The rules over the repo's staged blobs: `staged --rule NAME`, or every rule with no argument.
staged() {
    run $RULES --root "$REPO" "$@" --staged-only
}

# A committed file in each of getenv-setenv's roots, so none is missing.
rooted_tree() {
    local d
    for d in $GETENV_ROOTS; do
        stage "$d/base.cpp" 'int base;'
    done
    git -C "$REPO" commit -qm base
}

# One committed file per file-set rule, each holding a site of that rule.
site_tree() {
    rooted_tree
    stage src/cast.cpp 'int s = static_cast<PrintState>(lv_subject_get_int(state.get_print_state_enum_subject()));'
    stage src/dump.cpp 'void save() { std::ofstream ofs("a.json"); ofs << doc.dump(2); }'
    stage src/raw.cpp 'bool busy = s.get_print_job_state() == PrintJobState::PRINTING;'
    stage src/hide.cpp 'void build(lv_obj_t* parent) {
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(parent, "demo", nullptr));
    lv_obj_set_hidden(root, true);
}'
    stage tests/unit/sections.cpp "$SECTIONS"
    stage src/ternary.cpp 'void pick(bool c) {
    const auto keys = c ? std::initializer_list<int>{1} : std::initializer_list<int>{2};
}'
    stage tests/unit/env.cpp 'TEST_CASE("env") {
    const char* original = std::getenv("PATH");
    setenv("PATH", "", 1);
    if (original) {}
}'
    stage src/subject.cpp "$STORAGE"
    git -C "$REPO" commit -qm sites
}

# ------------------------------------------------------------ what a commit stages

@test "a site in a staged blob fails, read from the index while the working copy is clean" {
    stage tests/unit/test_x.cpp "$SECTIONS"
    printf 'int clean;\n' > "$REPO/tests/unit/test_x.cpp"
    run $RULES --root "$REPO" --rule section-returns
    [ "$status" -eq 0 ] || fail "the working copy holds no site, yet the tree run failed: $output"
    staged --rule section-returns
    [ "$status" -eq 1 ] || fail "a staged site passed: $output"
    contains "tests/unit/test_x.cpp:2:" "$output"
}

@test "a site that exists only in an unstaged edit is not judged" {
    stage tests/unit/test_x.cpp 'int clean;'
    printf '%s\n' "$SECTIONS" > "$REPO/tests/unit/test_x.cpp"
    run $RULES --root "$REPO" --rule section-returns
    [ "$status" -eq 1 ] || fail "the site is on disk, yet the tree run passed: $output"
    staged --rule section-returns
    [ "$status" -eq 0 ] || fail "an unstaged edit failed the commit: $output"
    [ -z "$output" ] || fail "a passing rule printed something: $output"
}

@test "every file-set rule is quiet with nothing staged, over a tree that holds a site for each" {
    local rule
    site_tree
    for rule in $FILE_SET_RULES; do
        run $RULES --root "$REPO" --rule "$rule"
        [ "$status" -eq 1 ] || fail "$rule does not see its site in the tree: $output"
        staged --rule "$rule"
        [ "$status" -eq 0 ] || fail "$rule judged the tree, not the index: $output"
        [ -z "$output" ] || fail "$rule printed on a pass: $output"
    done
}

# --------------------------------------------------------------- the scope held

@test "a staged file outside a rule's directories is not judged" {
    stage src/sections.cpp "$SECTIONS"
    staged --rule section-returns
    [ "$status" -eq 0 ] || fail "section-returns judged a file under src/: $output"
    stage tests/unit/sections.cpp "$SECTIONS"
    staged --rule section-returns
    [ "$status" -eq 1 ] || fail "the same site under tests/ was not judged: $output"
}

@test "a staged file under tools/ is not the subject rule's, and one under build/ is skipped" {
    stage tools/subject.cpp "$STORAGE"
    staged --rule lvgl-subject-api
    [ "$status" -eq 0 ] || fail "lvgl-subject-api judged a file under tools/: $output"
    stage src/build/generated.cpp "$STORAGE"
    staged --rule lvgl-subject-api
    [ "$status" -eq 0 ] || fail "lvgl-subject-api judged a file under build/: $output"
    stage src/subject.cpp "$STORAGE"
    staged --rule lvgl-subject-api
    [ "$status" -eq 1 ] || fail "the same site under src/ was not judged: $output"
    contains "src/subject.cpp:1: storage" "$output"
    lacks "src/build/generated.cpp" "$output"
}

# ---------------------------------------------------------------- the refusals

@test "a pinned rule refuses --staged-only with exit 2 and one line saying why" {
    local rule
    for rule in touch-rotation-source grid-metrics-single-source filament-ops-neutral; do
        staged --rule "$rule"
        [ "$status" -eq 2 ] || fail "$rule did not refuse --staged-only: $output"
        [ "${#lines[@]}" -eq 1 ] || fail "$rule printed ${#lines[@]} lines, not one: $output"
        contains "--staged-only does not apply to $rule: it checks named files whole" "$output"
        contains "its wrapper runs it whole at commit" "$output"
    done
}

@test "xml-create-registered refuses --staged-only: the names it accepts come from the whole tree" {
    staged --rule xml-create-registered
    [ "$status" -eq 2 ] || fail "xml-create-registered did not refuse --staged-only: $output"
    [ "${#lines[@]}" -eq 1 ] || fail "expected one line: $output"
    contains "--staged-only does not apply to xml-create-registered: " "$output"
    contains "from every source under src/, which the staged blobs alone cannot answer for" "$output"
}

@test "FILE arguments with --staged-only are a usage error, and nothing is judged" {
    stage tests/unit/test_x.cpp "$SECTIONS"
    run $RULES --root "$REPO" --rule section-returns --staged-only "$REPO/tests/unit/test_x.cpp"
    [ "$status" -eq 2 ] || fail "FILE arguments with --staged-only did not exit 2: $output"
    [ "${#lines[@]}" -eq 1 ] || fail "expected one line: $output"
    contains "--staged-only and FILE arguments do not combine: " "$output"
    contains "FILE arguments are read from the working tree" "$output"
}

@test "--staged-only with a root outside a git repository is a usage error, not a pass" {
    local plain="${BATS_TEST_TMPDIR}/plain"
    mkdir -p "$plain/src"
    printf '%s\n' "$STORAGE" > "$plain/src/x.cpp"
    run $RULES --root "$plain" --rule lvgl-subject-api --staged-only
    [ "$status" -eq 2 ] || fail "a root outside a repository did not exit 2: $output"
    [ "${#lines[@]}" -eq 1 ] || fail "expected one line: $output"
    contains "--staged-only reads the index, and --root is not inside a git repository: $plain" "$output"
}

@test "every rule selected with --staged-only runs the file-set rules and fails naming each that refuses" {
    local rule
    rooted_tree
    stage src/subject.cpp "$STORAGE"
    staged
    [ "$status" -eq 2 ] || fail "expected exit 2 for the refusing rules, got $status: $output"
    contains "src/subject.cpp:1: storage" "$output"
    for rule in xml-create-registered touch-rotation-source grid-metrics-single-source filament-ops-neutral; do
        contains "--staged-only does not apply to $rule:" "$output"
    done
    [ "${#lines[@]}" -eq 6 ] || fail "expected the site's two lines and four refusals: $output"
}

# ------------------------------------------------------- a scope that is not there

@test "getenv-setenv fails naming a scan root the index holds no file under" {
    local d
    for d in $GETENV_ROOTS; do
        [ "$d" != tools ] || continue
        stage "$d/base.cpp" 'int base;'
    done
    staged --rule getenv-setenv
    [ "$status" -eq 1 ] || fail "a scan root with nothing in the index passed: $output"
    [ "${#lines[@]}" -eq 1 ] || fail "expected the one missing root: $output"
    contains "scan root does not exist: tools" "$output"
}
