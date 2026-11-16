#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_old_toolchain.py: every src/ translation unit in
# compile_commands.json is re-run with its own flags through an older compiler
# with -fsyntax-only (nightly.yml). The compiler here is a fake that records its
# argv and rejects a file spelling `from_chars<float>`, the construct GCC 10's
# libstdc++ lacks, so the tests pin the rewrite of the recorded command and the
# verdicts without needing g++-10. Every way of checking nothing is a failure.

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    load helpers
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    BIN="${BATS_TEST_TMPDIR}/bin"
    rm -rf "$WORK" "$BIN"
    mkdir -p "$WORK/src" "$WORK/tests" "$BIN"
    git -C "$WORK" init -q
    printf 'int ok() { return 0; }\n' > "$WORK/src/good.cpp"
    printf 'int t() { return 0; }\n' > "$WORK/tests/t.cpp"
    cat > "$BIN/oldcc" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$OLDCC_LOG"
for a in "$@"; do
    case "$a" in *.cpp) grep -q 'from_chars<float>' "$a" && { echo "$a: error: no from_chars<float>" >&2; exit 1; } ;; esac
done
exit 0
EOF
    chmod +x "$BIN/oldcc"
    export OLDCC_LOG="$BATS_TEST_TMPDIR/oldcc.log"
    : > "$OLDCC_LOG"
}

# compile_commands.json with one entry per argument (a path under $WORK).
ccj() {
    python3 - "$WORK" "$@" <<'EOF'
import json, sys
root, files = sys.argv[1], sys.argv[2:]
print(json.dumps([{"directory": root, "file": f"{root}/{f}",
                   "arguments": ["ccache", "/usr/bin/clang++", "-std=c++20", "-c", "-o",
                                 f"build/{f}.o", f]} for f in files]))
EOF
}

gate() {
    run bash -c 'cd "$1" && shift && PATH="$0:$PATH" python3 "$@"' \
        "$BIN" "$WORK" "$REPO_ROOT/scripts/check_old_toolchain.py" --compiler oldcc --jobs 2 "$@"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "a unit the old headers reject fails the run and is named" {
    printf '#include <charconv>\nauto f = &std::from_chars<float>;\n' > "$WORK/src/bad.cpp"
    ccj src/good.cpp src/bad.cpp > "$WORK/compile_commands.json"
    gate
    [ "$status" -eq 1 ]
    contains "src/bad.cpp: FAILED" "$output"
    lacks "src/good.cpp: FAILED" "$output"
    contains "summary: 2 checked, 1 failed" "$output"
}

@test "a named file with no database entry is a failure, not a skip" {
    ccj src/good.cpp > "$WORK/compile_commands.json"
    gate src/never_built.cpp
    [ "$status" -eq 1 ]
    contains "src/never_built.cpp: FAILED (no compile_commands.json entry)" "$output"
}

@test "an entry with no recognisable compiler fails rather than guessing" {
    python3 -c 'import json,sys; print(json.dumps([{"directory": sys.argv[1], "file": sys.argv[1] + "/src/good.cpp", "arguments": ["mystery-wrapper", "-c", "src/good.cpp"]}]))' \
        "$WORK" > "$WORK/compile_commands.json"
    gate
    [ "$status" -eq 1 ]
    contains "src/good.cpp: FAILED" "$output"
}

@test "no compiler, no database and an empty corpus each refuse with exit 2" {
    ccj src/good.cpp > "$WORK/compile_commands.json"
    run bash -c 'cd "$0" && python3 "$1" --compiler no-such-cc-xyz' "$WORK" "$REPO_ROOT/scripts/check_old_toolchain.py"
    [ "$status" -eq 2 ]
    rm "$WORK/compile_commands.json"
    gate
    [ "$status" -eq 2 ]
    contains "not found" "$output"
    ccj tests/t.cpp > "$WORK/compile_commands.json"
    gate
    [ "$status" -eq 2 ]
    contains "corpus is empty" "$output"
}

# ------------------------------------------------ idioms that must stay QUIET

@test "clean units pass, each run with its own flags as a syntax-only check" {
    ccj src/good.cpp > "$WORK/compile_commands.json"
    gate
    [ "$status" -eq 0 ]
    contains "summary: 1 checked, 0 failed" "$output"
    run cat "$OLDCC_LOG"
    [ "$output" = "-std=c++20 -c src/good.cpp -fsyntax-only" ]
}

@test "units outside src/ are not checked by default" {
    ccj src/good.cpp tests/t.cpp > "$WORK/compile_commands.json"
    gate
    [ "$status" -eq 0 ]
    contains "Checking 1 translation unit(s)" "$output"
    refute_grep "tests/t.cpp" "$OLDCC_LOG"
}

@test "the nightly workflow runs the gate" {
    grep -q 'scripts/check_old_toolchain.py' .github/workflows/nightly.yml
}
