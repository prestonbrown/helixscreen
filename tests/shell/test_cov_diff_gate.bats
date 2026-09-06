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
