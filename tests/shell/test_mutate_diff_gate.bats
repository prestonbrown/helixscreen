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

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"; mkdir -p "$WORK/scripts" "$WORK/src" "$WORK/build/bin"
    cp "$REPO_ROOT/scripts/mutate_diff.py" "$WORK/scripts/"

    printf 'int f(int n) {\n    return n + 1;\n}\n' > "$WORK/src/feature.cpp"
    git -C "$WORK" init -q
    git -C "$WORK" config user.email t@t
    git -C "$WORK" config user.name t
    git -C "$WORK" add src/feature.cpp scripts/mutate_diff.py
    git -C "$WORK" commit -qm base
    BASE=$(git -C "$WORK" rev-parse HEAD)

    # The change under test.
    printf 'int f(int n) {\n    return n + 2;   // NEW_BEHAVIOR\n}\n' > "$WORK/src/feature.cpp"

    printf 'test-build:\n\t@true\n' > "$WORK/Makefile"

    # A tooling hunk is judged by bats AND pytest, and the script reaches pytest
    # through the repo venv's interpreter. Standing one up here makes pytest a
    # property of the fixture, so a verdict does not depend on what the host
    # happens to have installed globally.
    stub_pytest_installed
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
# the change. Reverting the hunk must therefore kill the mutant.
stub_tests_that_detect() {
    cat > "$WORK/build/bin/helix-tests" <<'EOF'
#!/usr/bin/env bash
grep -q NEW_BEHAVIOR src/feature.cpp || exit 1
exit 0
EOF
    chmod +x "$WORK/build/bin/helix-tests"
}

# A stub suite that passes either way — a test that does not detect the change.
stub_tests_that_ignore() {
    printf '#!/usr/bin/env bash\nexit 0\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
}

mutate() { ( cd "$WORK" && python3 scripts/mutate_diff.py --base "$BASE" --shards 1 "$@" ); }

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
    printf '#!/usr/bin/env bash\nexit 1\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 2 ]
    [[ "$output" == *"baseline catch2 suite is RED"* ]]
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
    cat > "$WORK/build/bin/helix-tests" <<'EOF'
#!/usr/bin/env bash
grep -q 'text="new"' ui_xml/home.xml || exit 1
exit 0
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

@test "a shell script is mutated against the bats suite" {
    mkdir -p "$WORK/tests/shell" "$WORK/tests/python"
    printf '#!/bin/sh\necho old\n' > "$WORK/scripts/gate.sh"
    cat > "$WORK/tests/shell/test_gate.bats" <<'EOF'
@test "gate says new" {
    grep -q new "$BATS_TEST_DIRNAME/../../scripts/gate.sh"
}
EOF
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
