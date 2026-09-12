#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/mutate_diff.py — the gate that reverts each changed
# hunk and looks for red.
#
# This is the only tool in the set that is an oracle rather than a screen, so
# what has to be pinned is the verdict logic and the safety of the tree. The
# fixture stubs `make test-build` and the test binary, which is enough: the
# script's real work is hunk surgery and verdict accounting, not building.
#
# The safety half matters as much as the verdicts. The script writes to the
# working tree, and it restores by writing back saved bytes rather than by
# `git checkout`, precisely so it can never discard unrelated uncommitted work.
# If "the tree is byte-identical afterwards" ever stops holding, the tool is
# dangerous rather than merely wrong.
#
# The stubs below model what a real suite and a real build tell the gate, not
# just whether they succeeded. A stub that means "a test detects this" prints
# what Catch2 prints for a failing assertion, and the stub `make test-build`
# leaves a different binary behind, because those are the two things a `killed`
# verdict rests on: the suite named a failing test, and it was running this
# mutant when it did.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"; mkdir -p "$WORK/scripts" "$WORK/src" "$WORK/build/bin"
    cp "$REPO_ROOT/scripts/mutate_diff.py" "$WORK/scripts/"
    # mutate_diff.py imports its base resolution from beside itself, so a fixture
    # that copies one without the other has no script to run at all.
    cp "$REPO_ROOT/scripts/diff_base.py" "$WORK/scripts/"

    printf 'int f(int n) {\n    return n + 1;\n}\n' > "$WORK/src/feature.cpp"
    git -C "$WORK" init -q
    git -C "$WORK" config user.email t@t
    git -C "$WORK" config user.name t
    git -C "$WORK" add src/feature.cpp scripts/mutate_diff.py
    git -C "$WORK" commit -qm base
    BASE=$(git -C "$WORK" rev-parse HEAD)

    # The change under test.
    printf 'int f(int n) {\n    return n + 2;   // NEW_BEHAVIOR\n}\n' > "$WORK/src/feature.cpp"

    stub_make_relinks

    # A tooling hunk is judged by bats AND pytest, and the script reaches pytest
    # through the repo venv's interpreter. Standing one up here makes pytest a
    # property of the fixture, so a verdict does not depend on what the host
    # happens to have installed globally.
    stub_pytest_installed
}

# What a Catch2 run prints when an assertion fails, as a line of shell for a
# stub suite to run. The gate reads this rather than the exit code: a runner
# that exits non-zero without naming a failing test has judged nothing, so a
# stub that only exits 1 models a crash and not a detection.
catch2_fail_cmd() {
    printf '%s\n' \
        'echo "tests/unit/test_feature.cpp:11: FAILED:"' \
        'echo "  REQUIRE( f(0) == 2 )"' \
        'echo "assertions: 1 | 1 failed"'
}

# `make test-build` as the gate reads it: a build that runs leaves a different
# binary behind. The gate fingerprints the test binary either side of a mutant's
# build, because a build that changes nothing leaves the suite running a binary
# the mutant is not in.
stub_make_relinks() {
    printf 'test-build:\n\t@[ -e build/bin/helix-tests ] && printf "\\n# relinked\\n" >> build/bin/helix-tests || true\n' \
        > "$WORK/Makefile"
}

# The venv interpreter the script runs pytest through, with pytest importable
# and its suite green: `-c` answers the availability probe, `-m` runs the suite.
# Any other invocation is one the fixture does not model, and fails loudly.
stub_pytest_installed() {
    mkdir -p "$WORK/.venv/bin"
    cat > "$WORK/.venv/bin/python3" <<'EOF'
#!/usr/bin/env bash
case "$1" in
    -c|-m) exit 0 ;;
esac
exit 1
EOF
    chmod +x "$WORK/.venv/bin/python3"
}

# The same interpreter with no pytest installed: it runs, and every invocation
# through it fails at the import.
stub_pytest_absent() {
    printf '#!/usr/bin/env bash\nexit 1\n' > "$WORK/.venv/bin/python3"
    chmod +x "$WORK/.venv/bin/python3"
}

# A stub suite that fails when the marker is gone — i.e. a test that DETECTS
# the change. Reverting the hunk must therefore kill the mutant. $1, when given,
# is echoed on every run, so a test can tell one run's log from another's.
stub_tests_that_detect() {
    cat > "$WORK/build/bin/helix-tests" <<EOF
#!/usr/bin/env bash
echo "${1:-catch2 stub}"
grep -q NEW_BEHAVIOR src/feature.cpp && exit 0
$(catch2_fail_cmd)
exit 1
EOF
    chmod +x "$WORK/build/bin/helix-tests"
}

# The venv interpreter with pytest importable, whose suite exits non-zero
# without naming a failing test once the marker is gone — a collection error,
# which judges nothing about the mutant.
stub_pytest_aborts_on_revert() {
    mkdir -p "$WORK/.venv/bin"
    cat > "$WORK/.venv/bin/python3" <<'EOF'
#!/usr/bin/env bash
case "$1" in
    -c) exit 0 ;;
    -m) grep -q new scripts/gate.sh && exit 0
        echo "ERROR tests/python/test_ok.py"
        echo "Interrupted: 1 error during collection"
        exit 2 ;;
esac
exit 1
EOF
    chmod +x "$WORK/.venv/bin/python3"
}

# A stub suite that passes either way — a test that does not detect the change.
stub_tests_that_ignore() {
    printf '#!/usr/bin/env bash\nexit 0\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
}

mutate() { ( cd "$WORK" && python3 scripts/mutate_diff.py --base "$BASE" --shards 1 "$@" ); }

# No --base: the script has to work out for itself what this branch was cut from.
mutate_auto() { ( cd "$WORK" && python3 scripts/mutate_diff.py --shards 1 "$@" ); }

@test "--list-only names the hunks and changes nothing" {
    stub_tests_that_detect
    run mutate --list-only
    [ "$status" -eq 0 ]
    contains "src/feature.cpp" "$output"
    run git -C "$WORK" diff --quiet -- src/feature.cpp
    [ "$status" -eq 1 ]   # still the modified version, untouched by the tool
}

@test "a hunk a test detects is reported killed" {
    stub_tests_that_detect
    run mutate
    [ "$status" -eq 0 ]
    contains "killed" "$output"
    [[ "$output" != *"SURVIVED"* ]]
}

@test "a hunk no test detects is reported SURVIVED and fails the gate" {
    stub_tests_that_ignore
    run mutate
    [ "$status" -eq 1 ]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "a survivor names the suite that judged it" {
    # Which suite stayed green is the whole content of the verdict, and a hunk's
    # suite follows its strategy. Quoting the Catch2 filter at a shell mutant
    # would report a suite that never ran.
    stub_tests_that_ignore
    run mutate --tests '[some_tag]'
    [ "$status" -eq 1 ]
    [[ "$output" == *"nothing in catch2 '[some_tag]' detects them"* ]]
}

@test "a surviving tooling hunk names bats and pytest, not the Catch2 filter" {
    mkdir -p "$WORK/tests/shell" "$WORK/tests/python"
    printf '#!/bin/sh\necho old\n' > "$WORK/scripts/gate.sh"
    printf '@test "t" { true; }\n' > "$WORK/tests/shell/test_gate.bats"
    printf 'def test_ok():\n    assert True\n' > "$WORK/tests/python/test_ok.py"
    git -C "$WORK" add src/feature.cpp scripts/gate.sh tests/shell/test_gate.bats tests/python/test_ok.py
    git -C "$WORK" commit -qm gate
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '#!/bin/sh\necho new\n' > "$WORK/scripts/gate.sh"
    stub_tests_that_detect
    run mutate --tests '[some_tag]'
    [ "$status" -eq 1 ]
    contains "nothing in bats + pytest detects them" "$output"
    [[ "$output" != *"some_tag"* ]]
}

@test "a red baseline is refused instead of reporting every hunk killed" {
    # Without this check a broken suite makes every mutant look detected.
    cat > "$WORK/build/bin/helix-tests" <<EOF
#!/usr/bin/env bash
$(catch2_fail_cmd)
exit 1
EOF
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 2 ]
    [[ "$output" == *"baseline catch2 suite is RED"* ]]
}

@test "a baseline suite that names no failing test stops the run just as hard" {
    # Not a red baseline: a runner that cannot report cannot establish one
    # either, and every mutant after it would be measured against nothing.
    printf '#!/usr/bin/env bash\nexit 134\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 2 ]
    contains "judged nothing" "$output"
    lacks "killed" "$output"
}

@test "a build that fails for the mutant is uncompilable, never a kill" {
    stub_tests_that_detect
    printf 'test-build:\n\t@grep -q NEW_BEHAVIOR src/feature.cpp\n' > "$WORK/Makefile"
    run mutate
    contains "uncompilable" "$output"
    [[ "$output" != *"killed"* ]]
}

@test "the working tree is byte-identical after a run" {
    stub_tests_that_detect
    before=$(sha256sum "$WORK/src/feature.cpp" | cut -d' ' -f1)
    run mutate
    after=$(sha256sum "$WORK/src/feature.cpp" | cut -d' ' -f1)
    [ "$before" = "$after" ]
}

@test "the tree is restored even when the mutant build fails" {
    stub_tests_that_detect
    printf 'test-build:\n\t@grep -q NEW_BEHAVIOR src/feature.cpp\n' > "$WORK/Makefile"
    before=$(sha256sum "$WORK/src/feature.cpp" | cut -d' ' -f1)
    run mutate
    after=$(sha256sum "$WORK/src/feature.cpp" | cut -d' ' -f1)
    [ "$before" = "$after" ]
}

@test "a killed run says CLEAN in as many words" {
    stub_tests_that_detect
    run mutate
    [ "$status" -eq 0 ]
    [[ "$output" == *"VERDICT: CLEAN"* ]]
}

# --- the run log ------------------------------------------------------------
#
# The log is opened with 'w' and carries every verdict and every suite's
# stdout. Two trees sharing one path truncate and interleave each other, which
# destroys the per-hunk attribution the gate exists to produce -- so the
# default is named for the worktree, and the path the run reports is the path
# it actually writes.

@test "the default log is named for the worktree" {
    stub_tests_that_detect
    run mutate
    [ "$status" -eq 0 ]
    contains "log: /tmp/mutate-diff-$(basename "$WORK").log" "$output"
}

@test "two worktrees do not share a default log" {
    stub_tests_that_detect
    run mutate
    [ "$status" -eq 0 ]
    first=$(printf '%s\n' "$output" | sed -n 's/^log: //p')

    # The same change in a tree that differs only in directory name, which is
    # what parallel worktrees of one branch look like.
    other="$(dirname "$WORK")/repo-elsewhere"
    rm -rf "$other"
    cp -a "$WORK" "$other"
    run bash -c "cd '$other' && python3 scripts/mutate_diff.py --base '$BASE' --shards 1"
    [ "$status" -eq 0 ]
    second=$(printf '%s\n' "$output" | sed -n 's/^log: //p')

    [ -n "$first" ]
    [ -n "$second" ]
    [ "$first" != "$second" ]
}

@test "an explicit --log outranks the per-worktree default" {
    stub_tests_that_detect
    run mutate --log "$BATS_TEST_TMPDIR/explicit.log"
    [ "$status" -eq 0 ]
    contains "log: $BATS_TEST_TMPDIR/explicit.log" "$output"
    [ -f "$BATS_TEST_TMPDIR/explicit.log" ]
}

@test "--help works where there is no worktree to name the log after" {
    outside="$BATS_TEST_TMPDIR/not-a-repo"
    mkdir -p "$outside/scripts"
    cp "$WORK/scripts/mutate_diff.py" "$WORK/scripts/diff_base.py" "$outside/scripts/"
    if git -C "$outside" rev-parse --show-toplevel >/dev/null 2>&1; then
        skip "the bats temp dir is itself inside a git repository"
    fi
    run bash -c "cd '$outside' && python3 scripts/mutate_diff.py --help"
    [ "$status" -eq 0 ]
    contains "--log" "$output"
}

# The log is also the only record of the output behind a verdict, and a verdict
# gets disputed after the run that produced it has ended. Opened 'w' with
# nothing kept, the next run is the one thing that has to happen for that record
# to be gone -- so the previous run's log is retained, and a listing, which
# judges nothing, does not get to spend that slot.

@test "the previous run's log is kept beside the new one" {
    log="$BATS_TEST_TMPDIR/run.log"
    stub_tests_that_detect RUN_ONE
    run mutate --log "$log"
    [ "$status" -eq 0 ]
    stub_tests_that_detect RUN_TWO
    run mutate --log "$log"
    [ "$status" -eq 0 ]
    grep -q RUN_ONE "$log.prev"
    grep -q RUN_TWO "$log"
}

@test "--list-only leaves the last real run's log alone" {
    log="$BATS_TEST_TMPDIR/run.log"
    stub_tests_that_detect RUN_ONE
    run mutate --log "$log"
    [ "$status" -eq 0 ]
    run mutate --log "$log" --list-only
    [ "$status" -eq 0 ]
    grep -q RUN_ONE "$log"
    [ ! -f "$log.prev" ]
}

# --- what a verdict rests on ------------------------------------------------
#
# `killed` is the verdict nobody re-checks: SURVIVED fails the gate and gets
# scrutiny, uncompilable and unreversible are documented as never a kill, but a
# kill is the answer the operator was hoping for and it gets quoted in a commit
# body as proof. So a kill has to be earned twice over -- the suite has to name
# a failing test, and it has to have been running this mutant when it did.
#
# Both halves fail in the same direction and borrow the PREVIOUS hunk's red: a
# process that exits non-zero for its own reasons reads as a detection, and a
# build that changes nothing leaves the previous mutant's binary under the next
# hunk's suite.

@test "a suite that exits non-zero without naming a failing test is not a kill" {
    # Exit 134 after a green summary is a shard aborting during static
    # destruction. By exit code alone that is indistinguishable from a
    # detection, so the verdict comes out of the suite's own output instead.
    cat > "$WORK/build/bin/helix-tests" <<'SUITE'
#!/usr/bin/env bash
grep -q NEW_BEHAVIOR src/feature.cpp && exit 0
echo "All tests passed (2 assertions in 1 test case)"
exit 134
SUITE
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    lacks "killed" "$output"
    contains "INCONCLUSIVE" "$output"
    contains "exited 134" "$output"
    [ "$status" -eq 3 ]
}

@test "a failure summary with no FAILED line is evidence enough" {
    # Catch2 reports a detection two ways, and a run that reaches the summary
    # without printing the assertion has still detected something.
    cat > "$WORK/build/bin/helix-tests" <<'SUITE'
#!/usr/bin/env bash
grep -q NEW_BEHAVIOR src/feature.cpp && exit 0
echo "assertions: 46 | 45 passed | 1 failed"
exit 1
SUITE
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 0 ]
    contains "killed" "$output"
}

@test "a case failing as expected is a pass, not a detection" {
    # A [!shouldfail] case that fails is the suite working. Counting its summary
    # line as a failure would hand back a kill for a suite that noticed nothing.
    cat > "$WORK/build/bin/helix-tests" <<'SUITE'
#!/usr/bin/env bash
grep -q NEW_BEHAVIOR src/feature.cpp && exit 0
echo "test cases: 2 | 1 passed | 1 failed as expected"
exit 4
SUITE
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    lacks "killed" "$output"
    contains "INCONCLUSIVE" "$output"
}

@test "a suite that judged nothing once is asked again before the hunk is given up on" {
    # The mutant is still in the tree, so the re-run costs a suite and no build.
    cat > "$WORK/build/bin/helix-tests" <<'SUITE'
#!/usr/bin/env bash
grep -q NEW_BEHAVIOR src/feature.cpp && exit 0
if [ -e .aborted-once ]; then
    echo "tests/unit/test_feature.cpp:11: FAILED:"
    exit 1
fi
: > .aborted-once
echo "All tests passed (2 assertions in 1 test case)"
exit 134
SUITE
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 0 ]
    contains "confirming" "$output"
    contains "killed" "$output"
}

@test "a tooling runner that exits without naming a failing test is not a kill" {
    # bats and pytest are read the same way as the C++ suite. A runner that died
    # collecting its tests exits non-zero having judged nothing, and inferring a
    # detection from that leaves the same hole open for every tooling hunk.
    mkdir -p "$WORK/tests/shell" "$WORK/tests/python"
    printf '#!/bin/sh\necho old\n' > "$WORK/scripts/gate.sh"
    printf '@test "t" { true; }\n' > "$WORK/tests/shell/test_gate.bats"
    printf 'def test_ok():\n    assert True\n' > "$WORK/tests/python/test_ok.py"
    git -C "$WORK" add src/feature.cpp scripts/gate.sh tests/shell/test_gate.bats tests/python/test_ok.py
    git -C "$WORK" commit -qm gate
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '#!/bin/sh\necho new\n' > "$WORK/scripts/gate.sh"
    stub_pytest_aborts_on_revert
    run mutate
    lacks "killed" "$output"
    contains "INCONCLUSIVE" "$output"
    [ "$status" -eq 3 ]
}

@test "a red suite after a build that changed nothing is not a kill" {
    # Nothing was relinked, so the suite ran whatever binary the last build left
    # and its red cannot be about this mutant.
    stub_tests_that_detect
    printf 'test-build:\n\t@true\n' > "$WORK/Makefile"
    run mutate
    lacks "killed" "$output"
    contains "INCONCLUSIVE" "$output"
    contains "left the test binary untouched" "$output"
    [ "$status" -eq 3 ]
}

@test "a build that changed nothing still reports SURVIVED when the suite is green" {
    # A hunk the test binary does not link produces the same binary either way.
    # Nothing detected it, which is both honest and the direction that fails the
    # gate; only a RED over a binary the mutant is not in is unattributable.
    stub_tests_that_ignore
    printf 'test-build:\n\t@true\n' > "$WORK/Makefile"
    run mutate
    [ "$status" -eq 1 ]
    contains "SURVIVED" "$output"
}

@test "a file left changed by one hunk stops the run instead of judging the next" {
    # The next hunk's suite would redden for this hunk's reversion, and the
    # verdict would be filed against the wrong hunk.
    printf 'int g(int n) {\n    return n + 9;   // SECOND\n}\n' > "$WORK/src/other.cpp"
    git -C "$WORK" add -N src/other.cpp
    # A suite run that writes into a file the run may mutate. However it comes
    # about, the tree is no longer the one the run started from.
    cat > "$WORK/build/bin/helix-tests" <<SUITE
#!/usr/bin/env bash
echo tampered >> src/other.cpp
grep -q NEW_BEHAVIOR src/feature.cpp && exit 0
$(catch2_fail_cmd)
exit 1
SUITE
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 2 ]
    contains "src/other.cpp" "$output"
    contains "no longer matches the tree this run started from" "$output"
}

@test "the drifted file is reported, not overwritten" {
    # What is on disk may be a reversion this run failed to undo or an edit
    # another session made to a shared tree, and from inside the run the two are
    # indistinguishable. Writing it back would destroy the second to repair the
    # first.
    printf 'int g(int n) {\n    return n + 9;   // SECOND\n}\n' > "$WORK/src/other.cpp"
    git -C "$WORK" add -N src/other.cpp
    cat > "$WORK/build/bin/helix-tests" <<SUITE
#!/usr/bin/env bash
echo tampered >> src/other.cpp
grep -q NEW_BEHAVIOR src/feature.cpp && exit 0
$(catch2_fail_cmd)
exit 1
SUITE
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 2 ]
    # More lines than the capture holds: the file is as the run left it, not as
    # the run would like it to be.
    [ "$(grep -c tampered "$WORK/src/other.cpp")" -gt 1 ]
}

# --- coverage honesty -------------------------------------------------------
#
# The gate's answer is cited in commit bodies as evidence that a change is
# pinned by tests, so the one thing it must never do is answer "clean" about a
# file it did not open. Anything the mutation operator cannot reach has to
# reach the report under its own name and take the run out of CLEAN, whether it
# was unreachable by path, by a missing runner, or by the operator's own
# --limit. These tests are that property.

@test "a changed file no strategy covers is named and takes the run out of CLEAN" {
    stub_tests_that_detect
    mkdir -p "$WORK/android/app"
    printf 'versionCode 7\n' > "$WORK/android/app/build.gradle"
    git -C "$WORK" add -N android/app/build.gradle
    run mutate
    [ "$status" -eq 3 ]
    contains "NOT COVERED" "$output"
    contains "android/app/build.gradle" "$output"
    contains "VERDICT: INCOMPLETE" "$output"
    [[ "$output" != *"VERDICT: CLEAN"* ]]
}

@test "--allow-incomplete accepts an incomplete run, and still says it was one" {
    stub_tests_that_detect
    mkdir -p "$WORK/android/app"
    printf 'versionCode 7\n' > "$WORK/android/app/build.gradle"
    git -C "$WORK" add -N android/app/build.gradle
    run mutate --allow-incomplete
    [ "$status" -eq 0 ]
    [[ "$output" == *"VERDICT: INCOMPLETE"* ]]
}

@test "documentation is named as not behavioural and keeps the run CLEAN" {
    stub_tests_that_detect
    printf 'x\n' > "$WORK/docs.md"
    git -C "$WORK" add -N docs.md
    run mutate
    [ "$status" -eq 0 ]
    contains "not behavioural" "$output"
    contains "docs.md" "$output"
    [[ "$output" == *"VERDICT: CLEAN"* ]]
}

@test "a changed test file is NOT COVERED, with the reason it cannot be mutated" {
    stub_tests_that_detect
    mkdir -p "$WORK/tests/unit"
    printf 'TEST_CASE("x") {}\n' > "$WORK/tests/unit/test_x.cpp"
    git -C "$WORK" add -N tests/unit/test_x.cpp
    run mutate
    [ "$status" -eq 3 ]
    contains "NOT COVERED" "$output"
    contains "tests/unit/test_x.cpp" "$output"
    [[ "$output" == *"proven by mutating the code it pins"* ]]
}

@test "a submodule pointer bump is NOT COVERED rather than silently dropped" {
    stub_tests_that_detect
    git init -q "$WORK/lib/engine"
    git -C "$WORK/lib/engine" config user.email t@t
    git -C "$WORK/lib/engine" config user.name t
    printf 'one\n' > "$WORK/lib/engine/parser.c"
    git -C "$WORK/lib/engine" add parser.c
    git -C "$WORK/lib/engine" commit -qm one
    git -C "$WORK" -c protocol.file.allow=always add lib/engine
    git -C "$WORK" commit -qm "add submodule"
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf 'int f(int n) {\n    return n + 2;   // NEW_BEHAVIOR\n}\n' > "$WORK/src/feature.cpp"
    printf 'two\n' > "$WORK/lib/engine/parser.c"
    git -C "$WORK/lib/engine" commit -qam two

    run mutate
    [ "$status" -eq 3 ]
    contains "NOT COVERED" "$output"
    contains "lib/engine" "$output"
    [[ "$output" == *"submodule"* ]]
}

@test "--limit reports what it set aside instead of narrowing in silence" {
    stub_tests_that_detect
    printf 'int g(int n) {\n    return n + 9;   // SECOND\n}\n' > "$WORK/src/other.cpp"
    git -C "$WORK" add -N src/other.cpp
    run mutate --limit 1
    [ "$status" -eq 3 ]
    contains "DEFERRED" "$output"
    [[ "$output" == *"VERDICT: INCOMPLETE"* ]]
}

@test "an uncompilable mutant leaves the run incomplete, never clean" {
    # A compiler error proves the code is load-bearing for the build, not that
    # any test would notice it changing, so the hunk is still unproven.
    stub_tests_that_detect
    printf 'test-build:\n\t@grep -q NEW_BEHAVIOR src/feature.cpp\n' > "$WORK/Makefile"
    run mutate
    [ "$status" -eq 3 ]
    contains "uncompilable" "$output"
    contains "VERDICT: INCOMPLETE" "$output"
    [[ "$output" != *"VERDICT: CLEAN"* ]]
}

@test "a strategy whose runner is not installed becomes NOT COVERED, not a pass" {
    stub_tests_that_detect
    printf '#!/bin/sh\necho hi\n' > "$WORK/scripts/gate.sh"
    git -C "$WORK" add -N scripts/gate.sh
    run mutate --shell-tests tests/nowhere --python-tests tests/nowhere
    [ "$status" -eq 3 ]
    contains "NOT COVERED" "$output"
    [[ "$output" == *"scripts/gate.sh"* ]]
}

@test "an interpreter that cannot import pytest is a gap, not an available suite" {
    # pytest runs as a module of an interpreter, so the interpreter existing says
    # nothing about the suite. Reading it as available runs a suite that dies at
    # the import, and the run blames the change for a red baseline.
    stub_tests_that_detect
    stub_pytest_absent
    mkdir -p "$WORK/tests/shell" "$WORK/tests/python"
    printf '@test "t" { true; }\n' > "$WORK/tests/shell/test_gate.bats"
    printf 'def test_ok():\n    assert True\n' > "$WORK/tests/python/test_ok.py"
    printf '#!/bin/sh\necho hi\n' > "$WORK/scripts/gate.sh"
    git -C "$WORK" add -N scripts/gate.sh
    run mutate
    grep -qF 'NOT COVERED' <<<"$output"
    grep -qF 'scripts/gate.sh' <<<"$output"
    grep -qF 'pytest is not importable' <<<"$output"
    grep -qF 'VERDICT: INCOMPLETE' <<<"$output"
    [ "$status" -eq 3 ]
}

# --- widened scope ----------------------------------------------------------

@test "a runtime XML change is mutated without a build" {
    mkdir -p "$WORK/ui_xml"
    printf '<view><lv_label text="old"/></view>\n' > "$WORK/ui_xml/home.xml"
    git -C "$WORK" add src/feature.cpp ui_xml/home.xml
    git -C "$WORK" commit -qm xml
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '<view><lv_label text="new"/></view>\n' > "$WORK/ui_xml/home.xml"
    # Reverting the XML must be visible to the suite with no compile, so the
    # stub Makefile fails loudly if the tool reaches for one.
    printf 'test-build:\n\t@true\n' > "$WORK/Makefile"
    cat > "$WORK/build/bin/helix-tests" <<EOF
#!/usr/bin/env bash
grep -q 'text="new"' ui_xml/home.xml && exit 0
$(catch2_fail_cmd)
exit 1
EOF
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 0 ]
    contains "ui_xml/home.xml" "$output"
    contains "[data]" "$output"
    [[ "$output" == *"killed"* ]]
}

@test "a runtime JSON change no test reads is reported SURVIVED" {
    mkdir -p "$WORK/assets/config"
    printf '{"mcu": "rp2040"}\n' > "$WORK/assets/config/printer_database.json"
    git -C "$WORK" add src/feature.cpp assets/config/printer_database.json
    git -C "$WORK" commit -qm db
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '{"mcu": "stm32"}\n' > "$WORK/assets/config/printer_database.json"
    stub_tests_that_ignore
    run mutate
    [ "$status" -eq 1 ]
    contains "assets/config/printer_database.json" "$output"
    [[ "$output" == *"SURVIVED"* ]]
}

# --- the suite binary behind a build-free strategy --------------------------
#
# A `data` hunk is visible with no build -- the binary reads ui_xml/ off the
# source tree -- but the suite that judges it is still a compiled program, so
# every catch2 run has to answer for the binary whether or not this hunk paid
# for a build. The binary can be absent without this run having removed it:
# `make test-build` drops it in prune-orphan-test-objs, a sibling prerequisite
# of the link rather than a step before it, so a peer make in the same tree
# removes what this one linked and still exits 0.
#
# The zero-build property is the constraint on the answer. Giving `data` a build
# would make the suite reachable and cost every XML hunk a compile and a
# whole-program link, which is what makes the widened scope affordable, so a
# build here has to be conditional on the binary actually being gone.

# Re-point the fixture at a data-only change: the C++ edit goes into the base, so
# the only hunk left is an XML attribute, whose strategy needs no build.
data_only_change() {
    mkdir -p "$WORK/ui_xml"
    printf '<view><lv_label text="old"/></view>\n' > "$WORK/ui_xml/home.xml"
    git -C "$WORK" add src/feature.cpp ui_xml/home.xml
    git -C "$WORK" commit -qm xml
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '<view><lv_label text="new"/></view>\n' > "$WORK/ui_xml/home.xml"
}

# A `make test-build` that links the stub suite from $WORK/.suite and records
# every invocation, so a test can assert how many builds a run cost.
stub_make_links_suite() {
    BUILD_LOG="$(dirname "$WORK")/builds"
    : > "$BUILD_LOG"
    printf 'test-build:\n\t@echo build >> %s\n\t@cp %s/.suite build/bin/helix-tests\n\t@chmod +x build/bin/helix-tests\n' \
        "$BUILD_LOG" "$WORK" > "$WORK/Makefile"
}

# A `make test-build` that exits 0 and leaves no binary — the shape the prune
# race produces, where the link succeeded and something else took the output.
stub_make_leaves_no_binary() {
    BUILD_LOG="$(dirname "$WORK")/builds"
    : > "$BUILD_LOG"
    printf 'test-build:\n\t@echo build >> %s\n' "$BUILD_LOG" > "$WORK/Makefile"
}

builds_run() { awk 'END { print NR }' "$BUILD_LOG"; }

# The stub suite detects the XML change, and a peer make prunes the binary once
# the first suite run is over: the next run finds nothing to exec.
stub_tests_pruned_after_first_run() {
    cat > "$WORK/.suite" <<EOF
#!/usr/bin/env bash
[ -e .pruned ] || { : > .pruned; rm -f build/bin/helix-tests; }
grep -q 'text="new"' ui_xml/home.xml && exit 0
$(catch2_fail_cmd)
exit 1
EOF
    cp "$WORK/.suite" "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/.suite" "$WORK/build/bin/helix-tests"
}

@test "a data hunk costs no build when the binary is already there" {
    data_only_change
    stub_make_links_suite
    cat > "$WORK/.suite" <<EOF
#!/usr/bin/env bash
grep -q 'text="new"' ui_xml/home.xml && exit 0
$(catch2_fail_cmd)
exit 1
EOF
    cp "$WORK/.suite" "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/.suite" "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 0 ]
    lacks "test binary rebuilt" "$output"
    # The one build is the baseline's, which establishes green before any hunk
    # is touched. The hunk itself must add none.
    [ "$(builds_run)" -eq 1 ]
}

@test "a data hunk rebuilds a test binary that went missing mid-run" {
    data_only_change
    stub_make_links_suite
    stub_tests_pruned_after_first_run
    run mutate
    [ "$status" -eq 0 ]
    contains "test binary rebuilt" "$output"
    contains "killed" "$output"
    lacks "Traceback" "$output"
}

@test "a build that leaves no test binary is diagnosed, never a traceback" {
    data_only_change
    stub_make_leaves_no_binary
    run mutate
    [ "$status" -eq 2 ]
    contains "build/bin/helix-tests" "$output"
    contains "prune-orphan-test-objs" "$output"
    lacks "Traceback" "$output"
}

@test "a suite that could not start is a harness stop, not a verdict" {
    # The binary goes missing after the baseline run, so the failure lands on the
    # hunk. Reporting that as killed would launder a mutant nothing judged.
    data_only_change
    stub_make_leaves_no_binary
    stub_tests_pruned_after_first_run
    run mutate
    [ "$status" -eq 2 ]
    lacks "killed" "$output"
    lacks "VERDICT" "$output"
}

@test "the tree is restored when a suite cannot start mid-run" {
    data_only_change
    stub_make_leaves_no_binary
    stub_tests_pruned_after_first_run
    before=$(sha256sum "$WORK/ui_xml/home.xml" | cut -d' ' -f1)
    run mutate
    after=$(sha256sum "$WORK/ui_xml/home.xml" | cut -d' ' -f1)
    [ "$before" = "$after" ]
}

@test "a shell script is mutated against the bats suite" {
    mkdir -p "$WORK/tests/shell" "$WORK/tests/python"
    printf '#!/bin/sh\necho old\n' > "$WORK/scripts/gate.sh"
    # Written with printf, not a heredoc: bats rewrites every line starting with
    # @test as it loads THIS file, heredoc bodies included, so a heredoc would
    # ship the fixture a bats_test_begin function and no test for bats to find.
    printf '%s\n' \
        '@test "gate says new" {' \
        '    grep -q new "$BATS_TEST_DIRNAME/../../scripts/gate.sh"' \
        '}' > "$WORK/tests/shell/test_gate.bats"
    printf 'def test_ok():\n    assert True\n' > "$WORK/tests/python/test_ok.py"
    git -C "$WORK" add src/feature.cpp scripts/gate.sh tests/shell/test_gate.bats tests/python/test_ok.py
    git -C "$WORK" commit -qm gate
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '#!/bin/sh\necho new\n' > "$WORK/scripts/gate.sh"
    stub_tests_that_detect
    run mutate
    contains "scripts/gate.sh" "$output"
    contains "[tooling]" "$output"
    contains "killed" "$output"
    [ "$status" -eq 0 ]
}

@test "a hash comment in a shell script is skipped, but a parameter expansion is not" {
    mkdir -p "$WORK/tests/shell" "$WORK/tests/python"
    printf '#!/bin/sh\n# old note\nX=${V#a}\n' > "$WORK/scripts/gate.sh"
    printf '@test "t" { true; }\n' > "$WORK/tests/shell/test_gate.bats"
    printf 'def test_ok():\n    assert True\n' > "$WORK/tests/python/test_ok.py"
    git -C "$WORK" add src/feature.cpp scripts/gate.sh tests/shell/test_gate.bats tests/python/test_ok.py
    git -C "$WORK" commit -qm gate
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '#!/bin/sh\n# new note\nX=${V#b}\n' > "$WORK/scripts/gate.sh"
    stub_tests_that_detect
    run mutate --list-only
    # ${V#a} -> ${V#b} is a behaviour change that a naive "# starts a comment"
    # scan would blank away, leaving two identical lines and a silent skip.
    contains "1 hunk(s) to mutate" "$output"
    [[ "$output" != *"comment/whitespace only"* ]]
}

@test "--limit caps the number of hunks" {
    stub_tests_that_detect
    printf 'int g(int n) {\n    return n + 9;   // SECOND\n}\n' >> "$WORK/src/feature.cpp"
    run mutate --list-only
    n_all=$(grep -c 'src/feature.cpp:' <<<"$output")
    run mutate --list-only --limit 1
    n_one=$(grep -c 'src/feature.cpp:' <<<"$output")
    [ "$n_one" -eq 1 ]
    [ "$n_all" -ge 1 ]
}

@test "--only restricts to matching files" {
    stub_tests_that_detect
    mkdir -p "$WORK/src/other"
    printf 'int h(int n) {\n    return n + 3;   // OTHER\n}\n' > "$WORK/src/other/thing.cpp"
    git -C "$WORK" add -N src/other/thing.cpp
    run mutate --list-only
    contains "other/thing.cpp" "$output"
    contains "src/feature.cpp" "$output"
    run mutate --list-only --only feature.cpp
    contains "src/feature.cpp" "$output"
    [[ "$output" != *"other/thing.cpp"* ]]
}

@test "a hunk in an untestable file is excluded with its reason, not mutated" {
    stub_tests_that_detect
    printf 'src/feature.cpp  # cannot run headless\n' > "$WORK/scripts/untestable_paths.txt"
    run mutate --list-only
    contains "EXCLUDED" "$output"
    contains "cannot run headless" "$output"
    [[ "$output" == *"0 hunk(s) to mutate"* ]]
}

@test "the exclusion is a path prefix, not a loose substring" {
    stub_tests_that_detect
    printf 'src/feat  # deliberately a partial path\n' > "$WORK/scripts/untestable_paths.txt"
    run mutate --list-only
    contains "EXCLUDED" "$output"
    printf 'feature.cpp  # not anchored at the start\n' > "$WORK/scripts/untestable_paths.txt"
    run mutate --list-only
    lacks "EXCLUDED" "$output"
    [[ "$output" == *"1 hunk(s) to mutate"* ]]
}

# --- comment/whitespace-only hunks ----------------------------------------
#
# Reverting a comment is a mutant no test can ever kill, so it lands in the
# tally as a survivor and reads as real debt while having cost a compile and a
# whole-program link to get there. The script drops those hunks up front.
#
# The risk is the opposite error: skipping a hunk that DOES change behaviour,
# which would hide exactly what this gate exists to find. That is why the
# classifier strips comments with a real scanner instead of matching on the
# shape of a line, and why the tests below are mostly the cases where a
# shape-matching heuristic would be wrong.

# Re-point the fixture at a base/changed pair of our own.
reset_to() {
    printf '%s' "$1" > "$WORK/src/feature.cpp"
    git -C "$WORK" add src/feature.cpp
    git -C "$WORK" commit -qm rebase --allow-empty
    BASE=$(git -C "$WORK" rev-parse HEAD)
    printf '%s' "$2" > "$WORK/src/feature.cpp"
}

@test "a comment-only hunk is skipped, not reported as a survivor" {
    stub_tests_that_ignore
    reset_to 'int f(int n) {
    // old note
    return n + 1;
}
' 'int f(int n) {
    // new note
    return n + 1;
}
'
    run mutate
    [ "$status" -eq 0 ]
    contains "comment/whitespace only" "$output"
    [[ "$output" != *"SURVIVED"* ]]
}

@test "a whitespace-only hunk is skipped" {
    stub_tests_that_ignore
    reset_to 'int f(int n) {
    return n + 1;
}
' 'int f(int n) {
        return n + 1;
}
'
    run mutate
    [ "$status" -eq 0 ]
    [[ "$output" == *"comment/whitespace only"* ]]
}

@test "--no-skip-comments mutates a comment-only hunk anyway" {
    stub_tests_that_ignore
    reset_to 'int f(int n) {
    // old note
    return n + 1;
}
' 'int f(int n) {
    // new note
    return n + 1;
}
'
    run mutate --no-skip-comments
    [ "$status" -eq 1 ]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "a comment edit riding along with a code change is still mutated" {
    stub_tests_that_ignore
    reset_to 'int f(int n) {
    return n + 1;  // note
}
' 'int f(int n) {
    return n + 2;  // NEW_BEHAVIOR
}
'
    run mutate
    [ "$status" -eq 1 ]
    contains "SURVIVED" "$output"
    [[ "$output" != *"comment/whitespace only"* ]]
}

@test "a changed string that merely looks like a comment is still mutated" {
    stub_tests_that_ignore
    reset_to 'const char* k() { return "// one"; }
' 'const char* k() { return "// two"; }
'
    run mutate
    [ "$status" -eq 1 ]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "a pointer store is not mistaken for a doc-comment continuation" {
    stub_tests_that_ignore
    reset_to 'void g(int* out) {
    *out = 1;
}
' 'void g(int* out) {
    *out = 2;
}
'
    run mutate
    [ "$status" -eq 1 ]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "a change inside a block comment with no leading star is skipped" {
    stub_tests_that_ignore
    reset_to '/*
   alpha
*/
int f(int n) { return n + 1; }
' '/*
   beta
*/
int f(int n) { return n + 1; }
'
    run mutate
    [ "$status" -eq 0 ]
    [[ "$output" == *"comment/whitespace only"* ]]
}

# --- diff base ------------------------------------------------------------
#
# The base decides what the run is ABOUT. Taking main when the branch was cut
# from a release branch hands the run everything that release branch has done
# since the two diverged, as though it were the change under test: dozens of
# foreign hunks, a build each, and verdicts about other people's code. Most come
# back `uncompilable`, which is correctly not a kill, so nothing about the output
# says "wrong base" -- it just reads as a stubborn change.
#
# The fixture below is that shape in miniature: a trunk and a release branch that
# both moved after they parted, and a feature branch cut from each in turn. A run
# measured against the wrong one of the two carries the other's work, which is
# what these tests look for.

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

@test "a branch cut from a release branch measures against it, not against main" {
    stub_tests_that_detect
    two_trunks
    run mutate_auto --list-only
    [ "$status" -eq 0 ]
    contains "release/1.0" "${lines[0]}"
    # The release branch's own work is not this branch's change.
    lacks "src/rel.cpp" "$output"
    contains "1 hunk(s) to mutate" "$output"
}

@test "a branch cut from main is not dragged back to an older release branch" {
    # Preference order alone would answer release/1.0 here, and be wrong: this
    # branch forked from main long after release/1.0 parted from it. Only the
    # nearest fork point tells the two apart.
    stub_tests_that_detect
    two_trunks
    git -C "$WORK" checkout -q -b feature/from-main main
    run mutate_auto --list-only
    [ "$status" -eq 0 ]
    contains "with main" "${lines[0]}"
    lacks "src/trunk.cpp" "$output"
    contains "1 hunk(s) to mutate" "$output"
}

@test "the base and the hunk count are the first two lines of output" {
    stub_tests_that_detect
    two_trunks
    run mutate_auto --list-only
    contains "base " "${lines[0]}"
    contains "release/1.0" "${lines[0]}"
    [ "${lines[1]}" = "diff 1 hunk(s) across 1 file(s)" ]
}

@test "an explicit --base is reported as given and not second-guessed" {
    stub_tests_that_detect
    two_trunks
    run mutate --list-only
    contains "--base $BASE" "${lines[0]}"
}

@test "a branch tracking its own remote copy still measures against its fork point" {
    # `git push -u` leaves the upstream pointing at this same branch. Believing
    # it would scope the run to whatever is not pushed yet, so the branch's own
    # committed work drops out of its own mutation run.
    stub_tests_that_detect
    two_trunks
    printf 'int mine(void) { return 4; }\n' > "$WORK/src/mine.cpp"
    git -C "$WORK" add src/mine.cpp
    git -C "$WORK" commit -qm "work on this branch, already pushed"
    git -C "$WORK" config remote.origin.url .
    git -C "$WORK" config remote.origin.fetch '+refs/heads/*:refs/remotes/origin/*'
    git -C "$WORK" update-ref refs/remotes/origin/fix/on-release HEAD
    git -C "$WORK" config branch.fix/on-release.remote origin
    git -C "$WORK" config branch.fix/on-release.merge refs/heads/fix/on-release
    run mutate_auto --list-only
    [ "$status" -eq 0 ]
    contains "release/1.0" "${lines[0]}"
    contains "src/mine.cpp" "$output"
}

@test "an upstream naming another branch outranks the release branches and main" {
    # A branch stacked on a branch: neither release/1.0 nor main is the fork
    # point, and only the recorded upstream knows that.
    stub_tests_that_detect
    two_trunks
    git -C "$WORK" checkout -q -b feature/a main
    printf 'int a(void) { return 2; }\n' > "$WORK/src/stack.cpp"
    git -C "$WORK" add src/stack.cpp
    git -C "$WORK" commit -qm "stacked work"
    git -C "$WORK" checkout -q -b feature/b
    git -C "$WORK" branch --set-upstream-to=feature/a feature/b >/dev/null
    run mutate_auto --list-only
    [ "$status" -eq 0 ]
    contains "feature/a" "${lines[0]}"
    lacks "src/stack.cpp" "$output"
}

@test "on the trunk itself, the base is the pushed tip and not HEAD" {
    # A branch is not its own fork point: main forks from main at HEAD, and a
    # base of HEAD narrows the run to uncommitted work. Committing straight onto
    # main is how this tree is often worked, and that commit has to stay in its
    # own mutation run.
    stub_tests_that_detect
    git -C "$WORK" branch -M main
    git -C "$WORK" config remote.origin.url .
    git -C "$WORK" config remote.origin.fetch '+refs/heads/*:refs/remotes/origin/*'
    git -C "$WORK" update-ref refs/remotes/origin/main HEAD     # the pushed tip
    printf 'int local(void) { return 3; }\n' > "$WORK/src/local.cpp"
    git -C "$WORK" add src/local.cpp
    git -C "$WORK" commit -qm "committed straight onto main"
    run mutate_auto --list-only
    [ "$status" -eq 0 ]
    contains "origin/main" "${lines[0]}"
    contains "src/local.cpp" "$output"
}

@test "an implausible hunk count off an auto-chosen base stops before any build" {
    # The refusal comes before the baseline build, so a wrong base is an instant
    # answer rather than an hour of verdicts.
    stub_tests_that_detect
    two_trunks
    printf 'int g(int n) {\n    return n + 9;   // SECOND\n}\n' > "$WORK/src/other.cpp"
    git -C "$WORK" add -N src/other.cpp
    run mutate_auto --max-hunks 1
    [ "$status" -eq 4 ]
    contains "more than --max-hunks 1" "$output"
    contains "--base" "$output"
    lacks "baseline" "$output"
}

@test "--max-hunks 0 disables the check" {
    stub_tests_that_detect
    two_trunks
    printf 'int g(int n) {\n    return n + 9;   // SECOND\n}\n' > "$WORK/src/other.cpp"
    git -C "$WORK" add -N src/other.cpp
    run mutate_auto --max-hunks 0 --list-only
    [ "$status" -eq 0 ]
    contains "2 hunk(s) to mutate" "$output"
}

@test "the hunk-count guard leaves an explicit --base alone" {
    stub_tests_that_detect
    two_trunks
    printf 'int g(int n) {\n    return n + 9;   // SECOND\n}\n' > "$WORK/src/other.cpp"
    git -C "$WORK" add -N src/other.cpp
    run mutate --max-hunks 1
    [ "$status" -ne 4 ]
    lacks "--max-hunks" "$output"
}
