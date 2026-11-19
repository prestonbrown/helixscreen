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
