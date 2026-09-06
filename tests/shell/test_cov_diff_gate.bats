#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/cov_diff.py — the screen that asks which changed lines
# the suite actually executed.
#
# It is a SCREEN, not a verdict, and the tests below pin both sides of that.
# The catch half: a changed line no test runs cannot be tested, and a changed
# file not linked into the test binary is worse still. The quiet half: an
# executed line is reported clean even when nothing asserts on it, because this
# tool genuinely cannot see that — `make mutate-diff` is what can.
#
# These build a real instrumented binary with gcc --coverage rather than a
# fixture .gcda, because the thing most likely to break is the gcov JSON
# handling, and a hand-written fixture would only test the parser against
# itself.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"; mkdir -p "$WORK/scripts" "$WORK/src" "$WORK/build/obj-cov"
    cp "$REPO_ROOT/scripts/cov_diff.py" "$WORK/scripts/"
    # cov_diff.py imports its base resolution from beside itself, so a fixture
    # that copies one without the other has no script to run at all.
    cp "$REPO_ROOT/scripts/diff_base.py" "$WORK/scripts/"

    cat > "$WORK/src/calc.cpp" <<'EOF'
int classify(int n) {
    if (n > 0) {
        return 1;
    }
    return 0;
}
int main() { return classify(5) == 1 ? 0 : 1; }
EOF
    git -C "$WORK" init -q
    git -C "$WORK" config user.email t@t
    git -C "$WORK" config user.name t
    git -C "$WORK" add src/calc.cpp scripts/cov_diff.py
    git -C "$WORK" commit -qm base
    BASE=$(git -C "$WORK" rev-parse HEAD)
}

# Compile with coverage into the layout cov_diff.py expects, run it to emit
# .gcda, and leave the tree at whatever src/calc.cpp currently says.
build_and_run() {
    ( cd "$WORK" && g++ -std=c++17 --coverage -fprofile-abs-path \
        -c src/calc.cpp -o build/obj-cov/calc.o 2>/dev/null &&
      g++ --coverage build/obj-cov/calc.o -o build/prog 2>/dev/null &&
      ./build/prog )
}

@test "a changed line the run never executes is reported" {
    cat > "$WORK/src/calc.cpp" <<'EOF'
int classify(int n) {
    if (n > 0) {
        return 1;
    }
    if (n < -999) {
        return 42;
    }
    return 0;
}
int main() { return classify(5) == 1 ? 0 : 1; }
EOF
    build_and_run
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE" --list
    [ "$status" -eq 1 ]
    contains "never executed" "$output"
    [[ "$output" == *"calc.cpp"* ]]
}

@test "a changed line the run does execute is not reported" {
    cat > "$WORK/src/calc.cpp" <<'EOF'
int classify(int n) {
    if (n > 0) {
        return 2;
    }
    return 0;
}
int main() { return classify(5) == 2 ? 0 : 1; }
EOF
    build_and_run
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 never executed"* ]]
}

@test "an executed but unasserted line is still reported clean (this is a screen)" {
    # The whole point of the caveat in the docs: coverage cannot see that
    # nothing checks the value. mutate-diff is the tool for that.
    cat > "$WORK/src/calc.cpp" <<'EOF'
int classify(int n) {
    if (n > 0) {
        return 2;
    }
    return 0;
}
int main() { classify(5); return 0; }
EOF
    build_and_run
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE" --list
    [ "$status" -eq 0 ]
}

@test "a changed file with no .gcda at all is reported as not linked" {
    cat > "$WORK/src/orphan.cpp" <<'EOF'
int unused_helper(int n) { return n + 1; }
EOF
    git -C "$WORK" add -N src/orphan.cpp   # untracked files are absent from git diff
    build_and_run
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE" --list
    [ "$status" -eq 1 ]
    contains "NOT LINKED" "$output"
    [[ "$output" == *"orphan.cpp"* ]]
}

@test "no changed src/ lines is a clean pass, not an error" {
    build_and_run
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No changed src/ lines"* ]]
}

@test "a missing coverage object tree is an explicit error, not a clean report" {
    rm -rf "$WORK/build/obj-cov"
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE"
    [ "$status" -ne 0 ]
    [[ "$output" == *"cov-build"* ]]
}

@test "a file listed as untestable is excluded with its reason, not counted as uncovered" {
    # The exclusion exists so a file the suite physically cannot run does not
    # generate permanent false debt. It must state why, and must not fail the run.
    mkdir -p "$WORK/scripts"
    printf 'src/calc.cpp  # no headless path for this\n' > "$WORK/scripts/untestable_paths.txt"
    cat > "$WORK/src/calc.cpp" <<'EOF'
int classify(int n) {
    if (n < -999) {
        return 42;
    }
    return 0;
}
int main() { return classify(5) == 0 ? 0 : 1; }
EOF
    build_and_run
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE" --list
    [ "$status" -eq 0 ]
    contains "EXCLUDED" "$output"
    contains "no headless path for this" "$output"
    # The summary line always carries the words "never executed" ("0 never
    # executed"), so assert the count, not the substring.
    [[ "$output" == *"0 never executed"* ]]
}

@test "an unlisted file is still judged normally when an exclusion file exists" {
    printf 'src/somethingelse.cpp  # unrelated\n' > "$WORK/scripts/untestable_paths.txt"
    cat > "$WORK/src/calc.cpp" <<'EOF'
int classify(int n) {
    if (n < -999) {
        return 42;
    }
    return 0;
}
int main() { return classify(5) == 0 ? 0 : 1; }
EOF
    build_and_run
    cd "$WORK" && run python3 scripts/cov_diff.py --base "$BASE" --list
    [ "$status" -eq 1 ]
    [[ "$output" == *"never executed"* ]]
}

# --- diff base ------------------------------------------------------------
#
# The base decides what the report is ABOUT. Taking main when the branch was cut
# from a release branch hands the report everything that release branch has done
# since the two diverged, as though this change had touched it: other people's
# files, named as never executed or not linked, with nothing in the output
# saying the scope is wrong.
#
# The fixture below is that shape in miniature: a trunk and a release branch that
# both moved after they parted, and a feature branch cut from each in turn. A
# report measured against the wrong one of the two carries the other's work,
# which is what these tests look for. The resolution itself lives in
# scripts/diff_base.py and is shared with mutate_diff.py, whose gate pins the
# same behaviours.

cov()      { ( cd "$WORK" && python3 scripts/cov_diff.py --base "$BASE" "$@" ); }
# No --base: the script has to work out for itself what this branch was cut from.
cov_auto() { ( cd "$WORK" && python3 scripts/cov_diff.py "$@" ); }

two_trunks() {
    git -C "$WORK" branch -M main
    git -C "$WORK" checkout -q -b release/1.0
    printf 'int r(void) { return 10; }\n' > "$WORK/src/rel.cpp"
    git -C "$WORK" add src/rel.cpp
    git -C "$WORK" commit -qm "release-only work"
    git -C "$WORK" checkout -q main
    printf 'int m(void) { return 1; }\n' > "$WORK/src/trunk.cpp"
    git -C "$WORK" add src/trunk.cpp
    git -C "$WORK" commit -qm "trunk work"
    git -C "$WORK" checkout -q -b fix/on-release release/1.0
}

# The change under test, written so the run leaves one branch of it unexecuted.
# A fully covered change is reported only as a count, and these tests need the
# FILE named so its presence or absence in the report is observable.
uncovered_change() {
    cat > "$WORK/src/calc.cpp" <<'EOF'
int classify(int n) {
    if (n > 0) {
        return 1;
    }
    if (n < -999) {
        return 42;
    }
    return 0;
}
int main() { return classify(5) == 1 ? 0 : 1; }
EOF
}

@test "a branch cut from a release branch measures against it, not against main" {
    two_trunks
    uncovered_change
    build_and_run
    run cov_auto --list
    [ "$status" -eq 1 ]
    contains "release/1.0" "${lines[0]}"
    contains "calc.cpp" "$output"
    # The release branch's own work is not this branch's change.
    lacks "rel.cpp" "$output"
}

@test "a branch cut from main is not dragged back to an older release branch" {
    # Preference order alone would answer release/1.0 here, and be wrong: this
    # branch forked from main long after release/1.0 parted from it. Only the
    # nearest fork point tells the two apart.
    two_trunks
    git -C "$WORK" checkout -q -b feature/from-main main
    uncovered_change
    build_and_run
    run cov_auto --list
    [ "$status" -eq 1 ]
    contains "with main" "${lines[0]}"
    contains "calc.cpp" "$output"
    lacks "trunk.cpp" "$output"
}

@test "the base is the first line of the report, above the coverage heading" {
    two_trunks
    uncovered_change
    build_and_run
    run cov_auto --list
    contains "base " "${lines[0]}"
    contains "release/1.0" "${lines[0]}"
    contains "Diff coverage vs" "${lines[1]}"
}

@test "an explicit --base is reported as given and not second-guessed" {
    two_trunks
    uncovered_change
    build_and_run
    run cov --list
    contains "--base $BASE" "${lines[0]}"
}

@test "a branch tracking its own remote copy still measures against its fork point" {
    # `git push -u` leaves the upstream pointing at this same branch. Believing
    # it would scope the report to whatever is not pushed yet, so the branch's
    # own committed work drops out of its own coverage report.
    two_trunks
    uncovered_change
    git -C "$WORK" add src/calc.cpp
    git -C "$WORK" commit -qm "work on this branch, already pushed"
    git -C "$WORK" config remote.origin.url .
    git -C "$WORK" config remote.origin.fetch '+refs/heads/*:refs/remotes/origin/*'
    git -C "$WORK" update-ref refs/remotes/origin/fix/on-release HEAD
    git -C "$WORK" config branch.fix/on-release.remote origin
    git -C "$WORK" config branch.fix/on-release.merge refs/heads/fix/on-release
    build_and_run
    run cov_auto --list
    [ "$status" -eq 1 ]
    contains "release/1.0" "${lines[0]}"
    contains "calc.cpp" "$output"
}

@test "an upstream naming another branch outranks the release branches and main" {
    # A branch stacked on a branch: neither release/1.0 nor main is the fork
    # point, and only the recorded upstream knows that.
    two_trunks
    git -C "$WORK" checkout -q -b feature/a main
    printf 'int a(void) { return 2; }\n' > "$WORK/src/stack.cpp"
    git -C "$WORK" add src/stack.cpp
    git -C "$WORK" commit -qm "stacked work"
    git -C "$WORK" checkout -q -b feature/b
    git -C "$WORK" branch --set-upstream-to=feature/a feature/b >/dev/null
    uncovered_change
    build_and_run
    run cov_auto --list
    [ "$status" -eq 1 ]
    contains "feature/a" "${lines[0]}"
    lacks "stack.cpp" "$output"
}

@test "on the trunk itself, the base is the pushed tip and not HEAD" {
    # A branch is not its own fork point: main forks from main at HEAD, and a
    # base of HEAD narrows the report to uncommitted work. Committing straight
    # onto main is how this tree is often worked, and that commit has to stay in
    # its own coverage report.
    git -C "$WORK" branch -M main
    git -C "$WORK" config remote.origin.url .
    git -C "$WORK" config remote.origin.fetch '+refs/heads/*:refs/remotes/origin/*'
    git -C "$WORK" update-ref refs/remotes/origin/main HEAD     # the pushed tip
    uncovered_change
    git -C "$WORK" add src/calc.cpp
    git -C "$WORK" commit -qm "committed straight onto main"
    build_and_run
    run cov_auto --list
    [ "$status" -eq 1 ]
    contains "origin/main" "${lines[0]}"
    contains "calc.cpp" "$output"
}
