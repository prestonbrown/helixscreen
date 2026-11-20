#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/ratchet_report.py, the weekly trend of the bookkeeping ratchets: each
# count at HEAD against the last commit before the cutoff and against its
# baseline, taken from commits and never from the working tree.

load helpers

REPORT="$BATS_TEST_DIRNAME/../../scripts/ratchet_report.py"

commit_at() { # <days ago> <message>
    local when
    when=$(date -d "-$1 days" '+%Y-%m-%dT%H:%M:%S')
    git -C "$REPO" add -A
    GIT_AUTHOR_DATE=$when GIT_COMMITTER_DATE=$when git -C "$REPO" commit -q --allow-empty -m "$2"
}

setup() {
    REPO="$BATS_TEST_TMPDIR/repo"
    git init -q -b main "$REPO"
    git -C "$REPO" config user.email t@t
    git -C "$REPO" config user.name t
    mkdir -p "$REPO/tests/unit" "$REPO/include" "$REPO/src" "$REPO/ui_xml" "$REPO/scripts/qc"
    printf 'TEST_CASE("a") {}\n' >"$REPO/tests/unit/test_a.cpp"
    printf 'class W {\n    friend class WTestAccess;\n};\n' >"$REPO/include/w.h"
    echo '<component><view><lv_obj style_pad_all="12"/></view></component>' >"$REPO/ui_xml/a.xml"
    echo 'python3 scripts/check_hardcoded_pixels.py --max-allowed 3 --summary' >"$REPO/scripts/qc/design_pixels.sh"
    printf '# accepted\nunreferenced:on_old\n' >"$REPO/scripts/orphan_callback_baseline.txt"
    commit_at 10 old
}

row() { # <ratchet name>: that row of the last run's output
    grep -E "^  $1  " <<<"$output"
}

@test "each count is set against the commit before the cutoff and against its baseline" {
    printf 'TEST_CASE("b") {\n}\n' >"$REPO/tests/unit/test_b.cpp"
    printf 'class V {\n    friend class VTestAccess;\n    friend struct Peek;\n};\n' >"$REPO/include/v.h"
    echo '<component><view><lv_obj style_pad_all="8"/></view></component>' >"$REPO/ui_xml/b.xml"
    commit_at 1 new
    run python3 "$REPORT" --root "$REPO" --days 7
    [ "$status" -eq 0 ] || fail "$output"
    contains "against 7 days ago" "$output"
    [[ "$(row 'C\+\+ test lines')" =~ \ 3\ +1\ +\+2\ \(worse\) ]] || fail "$(row 'C\+\+ test lines')"
    [[ "$(row 'TestAccess references')" =~ \ 2\ +1\ +\+1\ \(worse\) ]] || fail "$(row 'TestAccess references')"
    [[ "$(row 'friend declarations')" =~ \ 3\ +1\ +\+2\ \(worse\) ]] || fail "$(row 'friend declarations')"
    [[ "$(row 'hardcoded pixels')" =~ \ 2\ +1\ +\+1\ \(worse\)\ +3\ \(lower\ it\ to\ 2\) ]] || fail "$(row 'hardcoded pixels')"
    [[ "$(row 'orphan callbacks')" =~ \ 0\ +0\ +flat\ +1\ \(lower\ it\ to\ 0\) ]] || fail "$(row 'orphan callbacks')"
}

@test "uncommitted work is not counted" {
    commit_at 1 new
    printf 'TEST_CASE("wip") {\n}\n' >"$REPO/tests/unit/test_wip.cpp"
    run python3 "$REPORT" --root "$REPO" --days 7
    [ "$status" -eq 0 ]
    [[ "$(row 'C\+\+ test lines')" =~ \ 1\ +1\ +flat ]] || fail "$(row 'C\+\+ test lines')"
}

@test "--since names the commit to compare against, and --markdown prints a table" {
    commit_at 1 new
    run python3 "$REPORT" --root "$REPO" --since HEAD~1 --markdown
    [ "$status" -eq 0 ]
    contains "| ratchet | now | HEAD~1 | change | baseline |" "$output"
    contains "| hardcoded pixels | 1 | 1 | flat | 3 (lower it to 1) |" "$output"
}

@test "a history younger than the cutoff fails and says how to compare" {
    run python3 "$REPORT" --root "$REPO" --days 30
    [ "$status" -eq 2 ]
    contains "pass --since REV" "$output"
}

@test "each new design-ratchet site is listed with the commit that added it and its stated reason" {
    printf 'namespace helix {\nint ok = 1;\n}\n' >"$REPO/src/x.cpp"
    commit_at 9 "before the window"
    printf 'class NewGlobal { int y; };\n' >>"$REPO/src/x.cpp"
    git -C "$REPO" add -A
    GIT_AUTHOR_DATE=$(date -d '-2 days' '+%Y-%m-%dT%H:%M:%S') GIT_COMMITTER_DATE=$(date -d '-2 days' '+%Y-%m-%dT%H:%M:%S') \
        git -C "$REPO" commit -q -m "add a global" -m "Ratchet: namespace: the C ABI needs a global symbol"
    printf 'namespace helix {\nbool cfs(AmsType t) { return t == AmsType::CFS; }\n}\n' >"$REPO/src/v.cpp"
    commit_at 1 "add a vendor branch"
    run python3 "$REPORT" --root "$REPO" --days 7
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$(row 'design: namespace')" =~ \ 2\ +1\ +\+1\ \(worse\) ]] || fail "$(row 'design: namespace')"
    contains "New design-ratchet sites in the window: 2" "$output"
    contains "  namespace  src/x.cpp:4" "$output"
    contains "add a global" "$output"
    contains "reason: the C ABI needs a global symbol" "$output"
    contains "  vendor-branches  src/v.cpp:2" "$output"
    contains "add a vendor branch" "$output"
    contains "reason: (no reason stated)" "$output"
}

@test "a site older than the window is not listed as growth" {
    printf 'class OldGlobal { int y; };\n' >"$REPO/src/x.cpp"
    commit_at 9 "old global"
    commit_at 1 "nothing new"
    run python3 "$REPORT" --root "$REPO" --days 7
    [ "$status" -eq 0 ] || fail "$output"
    contains "New design-ratchet sites in the window: none" "$output"
}

@test "a reason stated by the merge that brought a site in, or a later commit of its branch, is the site's reason" {
    printf 'namespace helix {\nint ok = 1;\n}\n' >"$REPO/src/x.cpp"
    commit_at 9 "before the window"
    git -C "$REPO" checkout -q -b one
    printf 'class FromMerge { int y; };\n' >>"$REPO/src/x.cpp"
    commit_at 3 "add one global"
    git -C "$REPO" checkout -q main
    GIT_COMMITTER_DATE=$(date -d '-3 days' '+%Y-%m-%dT%H:%M:%S') git -C "$REPO" merge -q --no-ff one \
        -m "Merge one" -m "Ratchet: namespace: the merge says why"
    git -C "$REPO" checkout -q -b two
    printf 'class FromBranch { int z; };\n' >"$REPO/src/y.cpp"
    commit_at 2 "add another global"
    printf '// tidy\n' >>"$REPO/src/y.cpp"
    git -C "$REPO" add -A
    GIT_AUTHOR_DATE=$(date -d '-2 days' '+%Y-%m-%dT%H:%M:%S') GIT_COMMITTER_DATE=$(date -d '-2 days' '+%Y-%m-%dT%H:%M:%S') \
        git -C "$REPO" commit -q -m "follow-up" -m "Ratchet: namespace: a later branch commit says why"
    git -C "$REPO" checkout -q main
    GIT_COMMITTER_DATE=$(date -d '-1 days' '+%Y-%m-%dT%H:%M:%S') git -C "$REPO" merge -q --no-ff two -m "Merge two"
    run python3 "$REPORT" --root "$REPO" --days 7
    [ "$status" -eq 0 ] || fail "$output"
    contains "  namespace  src/x.cpp:4" "$output"
    contains "reason: the merge says why" "$output"
    contains "  namespace  src/y.cpp:1" "$output"
    contains "reason: a later branch commit says why" "$output"
}
