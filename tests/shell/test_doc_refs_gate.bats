#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for check_doc_refs.py --devel: the docs/devel dead-path and
# dead-link gate added for the architecture overhaul.

setup() {
    CHECK="$BATS_TEST_DIRNAME/../../scripts/check_doc_refs.py"
    FIX="$BATS_TEST_TMPDIR/devel"
    mkdir -p "$FIX"
}

@test "devel: clean doc with file ref, :line ref, and live link passes" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    # The `:42` suffix names a line; the gate strips it and checks the file.
    for i in $(seq 50); do echo "line $i"; done > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/good.md" <<'EOF'
See `src/zz_fixture_real.cpp` and `src/zz_fixture_real.cpp:42` and
[the other doc](other.md). Glob `src/*.cpp` and `<placeholder>` are skipped.
EOF
    echo x > "$FIX/other.md"
    run python3 "$CHECK" --devel "$FIX/good.md"
    [ "$status" -eq 0 ]
}

@test "devel: dead file path fails with file:line report" {
    cat > "$FIX/bad.md" <<'EOF'
Cites `src/zz_fixture_missing_thing.cpp` which does not exist.
EOF
    run python3 "$CHECK" --devel "$FIX/bad.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_fixture_missing_thing.cpp"* ]]
}

@test "devel: dead relative markdown link fails" {
    echo x > "$FIX/target.md"
    cat > "$FIX/badlink.md" <<'EOF'
See [gone](nope.md) and [anchor ok](target.md#section).
EOF
    run python3 "$CHECK" --devel "$FIX/badlink.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"nope.md"* ]]
}

@test "devel: plans and printer-research subdirs are exempt" {
    mkdir -p "$FIX/plans"
    cat > "$FIX/plans/old.md" <<'EOF'
`src/zz_fixture_missing_thing.cpp`
EOF
    run python3 "$CHECK" --devel "$FIX"
    [ "$status" -eq 0 ]
}

@test "devel: C++ lambda with unnamed pointer param is not a link" {
    cat > "$FIX/lambda.md" <<'EOF'
```cpp
.on_destroy = [](lv_obj_t*) {
    g_api->log_debug("cleanup");
}
```
EOF
    run python3 "$CHECK" --devel "$FIX/lambda.md"
    [ "$status" -eq 0 ]
    [[ "$output" != *"lv_obj_t*"* ]]
}

@test "devel: an anchor citation names the file before the '#'" {
    # A citation names a place inside the file - `path#symbol`. The fragment is
    # not part of the path, so the gate must strip it and check the file.
    mkdir -p "$BATS_TEST_TMPDIR/src"
    printf 'void widget_attach(void) {}\n' > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/anchor_ok.md" <<'MD'
See `src/zz_fixture_real.cpp#widget_attach` for the entry point.
MD
    run python3 "$CHECK" --devel "$FIX/anchor_ok.md"
    [ "$status" -eq 0 ]
}

@test "devel: an anchor citation to a missing file fails the path check" {
    # The fragment must not smuggle a dead path past the check: strip it, and
    # what is left is a file that has to exist.
    cat > "$FIX/anchor_bad.md" <<'MD'
Cites `src/zz_fixture_missing_thing.cpp#some_symbol` which does not exist.
MD
    run python3 "$CHECK" --devel "$FIX/anchor_bad.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_fixture_missing_thing.cpp#some_symbol"* ]]
}

@test "stale: report mode runs clean on a doc with no file cites" {
    cat > "$FIX/nocites.md" <<'EOF'
Plain prose, no backticked paths at all.
EOF
    run python3 "$CHECK" --stale --devel "$FIX/nocites.md"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No citation is older than its doc"* ]]
}

@test "devel: a RANGE citation to a missing file fails the path check" {
    # `file.cpp:63-65` used not to match PATH_RE at all, so a citation naming a
    # block was exempt from the one check that proves its file exists — while
    # the identical `file.cpp:70` form failed. That asymmetry is what let
    # RPC_ERROR_OWNERSHIP.md carry a range pointing ~355 lines from the gate it
    # described, invisible to every gate at once.
    cat > "$FIX/range.md" <<'MD'
Cites `src/zz_fixture_missing_thing.cpp:63-65` which does not exist.
MD
    run python3 "$CHECK" --devel "$FIX/range.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"zz_fixture_missing_thing.cpp:63-65"* ]]
}

@test "devel: a RANGE citation to a real file still passes" {
    mkdir -p "$BATS_TEST_TMPDIR/src"
    for i in $(seq 80); do echo "line $i"; done > "$BATS_TEST_TMPDIR/src/zz_fixture_real.cpp"
    cat > "$FIX/rangeok.md" <<'MD'
Cites `src/zz_fixture_real.cpp:63-65`, a real block.
MD
    run python3 "$CHECK" --devel "$FIX/rangeok.md"
    [ "$status" -eq 0 ]
}

@test "scope: .claude/worktrees is skipped, .claude/skills is not" {
    # The harness keeps its live agent checkouts under .claude/worktrees/, each
    # a COMPLETE copy of this repo — CLAUDE.md, docs/ and all. A by-name prune
    # cannot express "skip that but keep .claude/skills", so the walk descended
    # into them and every doc and citation appeared once per running agent. Six
    # copies of CLAUDE.md's `temperature_service.cpp:667` turned main red, and
    # only after the resolver stopped having a nearest-line fallback to swallow
    # them with. Nothing asserted the walk's scope, which is why it survived.
    REPO="$BATS_TEST_TMPDIR/scoperepo"
    mkdir -p "$REPO/.claude/worktrees/agent-deadbeef" "$REPO/.claude/skills/thing" \
             "$REPO/docs/devel" "$REPO/src"
    echo 'Root doc.' > "$REPO/CLAUDE.md"
    echo 'A live agent copy of the root doc.' \
        > "$REPO/.claude/worktrees/agent-deadbeef/CLAUDE.md"
    printf -- '---\nname: thing\n---\nA real skill doc.\n' \
        > "$REPO/.claude/skills/thing/SKILL.md"
    cd "$REPO"

    run python3 "$CHECK" --list
    [ "$status" -eq 0 ]
    # The skills doc is deliberately in scope...
    [[ "$output" == *".claude/skills/thing/SKILL.md"* ]]
    # ...and the agent worktree is deliberately not.
    [[ "$output" != *"agent-deadbeef"* ]]
    # The repo's own CLAUDE.md is still scanned, exactly once.
    [ "$(printf '%s\n' "$output" | grep -c 'scanned: CLAUDE.md$')" -eq 1 ]
}

@test "scope: a doc inside .claude/worktrees cannot fail the gate" {
    # The end-to-end shape of the bug: a citation that resolves against the
    # AGENT's tree, not the one being checked, reported against a path nobody
    # can act on. Deleting those worktrees is not available as a fix — they hold
    # other agents' uncommitted work.
    REPO="$BATS_TEST_TMPDIR/scoperepo2"
    mkdir -p "$REPO/.claude/worktrees/agent-deadbeef" "$REPO/docs/devel"
    echo 'Clean.' > "$REPO/CLAUDE.md"
    echo 'Cites `src/zz_definitely_missing.cpp` which does not exist.' \
        > "$REPO/.claude/worktrees/agent-deadbeef/CLAUDE.md"
    cd "$REPO"

    run python3 "$CHECK" --refs
    [ "$status" -eq 0 ]
    [[ "$output" != *"zz_definitely_missing"* ]]
}
