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
    [[ "$output" == *"src/feature.cpp"* ]]
    run git -C "$WORK" diff --quiet -- src/feature.cpp
    [ "$status" -eq 1 ]   # still the modified version, untouched by the tool
}

@test "a hunk a test detects is reported killed" {
    stub_tests_that_detect
    run mutate
    [ "$status" -eq 0 ]
    [[ "$output" == *"killed"* ]]
    [[ "$output" != *"SURVIVED"* ]]
}

@test "a hunk no test detects is reported SURVIVED and fails the gate" {
    stub_tests_that_ignore
    run mutate
    [ "$status" -eq 1 ]
    [[ "$output" == *"SURVIVED"* ]]
}

@test "a red baseline is refused instead of reporting every hunk killed" {
    # Without this check a broken suite makes every mutant look detected.
    printf '#!/usr/bin/env bash\nexit 1\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
    run mutate
    [ "$status" -eq 2 ]
    [[ "$output" == *"baseline suite is RED"* ]]
}

@test "a build that fails for the mutant is uncompilable, never a kill" {
    stub_tests_that_detect
    printf 'test-build:\n\t@grep -q NEW_BEHAVIOR src/feature.cpp\n' > "$WORK/Makefile"
    run mutate
    [[ "$output" == *"uncompilable"* ]]
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

@test "only src/ and include/ are considered" {
    stub_tests_that_detect
    printf 'x\n' > "$WORK/docs.md"
    git -C "$WORK" add -N docs.md
    run mutate --list-only
    [[ "$output" != *"docs.md"* ]]
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
    [[ "$output" == *"other/thing.cpp"* ]]
    [[ "$output" == *"src/feature.cpp"* ]]
    run mutate --list-only --only feature.cpp
    [[ "$output" == *"src/feature.cpp"* ]]
    [[ "$output" != *"other/thing.cpp"* ]]
}

@test "a hunk in an untestable file is excluded with its reason, not mutated" {
    stub_tests_that_detect
    printf 'src/feature.cpp  # cannot run headless\n' > "$WORK/scripts/untestable_paths.txt"
    run mutate --list-only
    [[ "$output" == *"EXCLUDED"* ]]
    [[ "$output" == *"cannot run headless"* ]]
    [[ "$output" == *"0 hunk(s) to mutate"* ]]
}

@test "the exclusion is a path prefix, not a loose substring" {
    stub_tests_that_detect
    printf 'src/feat  # deliberately a partial path\n' > "$WORK/scripts/untestable_paths.txt"
    run mutate --list-only
    [[ "$output" == *"EXCLUDED"* ]]
    printf 'feature.cpp  # not anchored at the start\n' > "$WORK/scripts/untestable_paths.txt"
    run mutate --list-only
    [[ "$output" != *"EXCLUDED"* ]]
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
    [[ "$output" == *"comment/whitespace only"* ]]
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
    [[ "$output" == *"SURVIVED"* ]]
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
