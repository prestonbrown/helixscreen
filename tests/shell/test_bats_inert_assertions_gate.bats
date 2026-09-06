#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_bats_inert_assertions.py - the gate that finds
# assertions bash 3.2 swallows.
#
# Both halves matter and the quiet half matters more. The suite is ~600
# legitimate `[[ ]]` in final position; a gate that flagged those would be
# switched off within a day, and the four honoured-but-not-final forms below
# (|| fail, || { }, && continue, || problems=) are all in real use.
#
# The catch half is written as fixtures rather than assertions about the tree,
# so the gate's contract does not move when the tree does.

load helpers

GATE="scripts/check_bats_inert_assertions.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/probe"
    rm -rf "$WORK"; mkdir -p "$WORK"
}

# $1 = filename, stdin = file body.
#
# bats rewrites any line starting with `@test` into its own function form
# BEFORE the file runs, and it does that inside heredocs too - so a fixture
# spelling `@test` literally would reach disk as `bats_test_function ...` and
# the gate would correctly find no test bodies in it. Fixtures write %TEST%
# and it is restored here.
fixture() {
    sed 's/^%TEST%/@test/' > "$WORK/$1"
}

gate() { run python3 "$GATE" --list "$WORK"; }

# ------------------------------------------------------------------ catches

@test "flags a bare mid-body [[ ]]" {
    fixture a.bats <<'EOF'
%TEST% "t" {
    [[ "$output" == *"needle"* ]]
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "a.bats:2" "$output"
}

@test "flags a mid-body (( )) - arithmetic is exempt from errexit too" {
    fixture b.bats <<'EOF'
%TEST% "t" {
    (( count == 2 ))
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "b.bats:2" "$output"
}

@test "flags a mid-body ! [[ ]] - the bang is exempt as well" {
    fixture c.bats <<'EOF'
%TEST% "t" {
    ! [[ "$output" == *"gone"* ]]
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "c.bats:2" "$output"
}

@test "flags a mid-body [[ ]] that carries a trailing comment" {
    fixture d.bats <<'EOF'
%TEST% "t" {
    [[ "$output" == *"needle"* ]]   # why this matters
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "d.bats:2" "$output"
}

@test "a brace inside a heredoc does not end the body early and hide a site" {
    fixture e.bats <<'EOF'
%TEST% "t" {
    cat > "$f" <<'CPP'
namespace helix {
void f() {
    int local = 1;
}
}
CPP
    [[ "$output" == *"needle"* ]]
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "e.bats:9" "$output"
}

@test "a brace inside a multi-line quoted string does not hide a site either" {
    fixture f.bats <<'EOF'
%TEST% "t" {
    f=$(fixture probe.cpp 'namespace helix {
void g() {
}
}')
    [[ "$output" == *"needle"* ]]
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "f.bats:6" "$output"
}

@test "counts every site in a body, not just the first" {
    fixture g.bats <<'EOF'
%TEST% "t" {
    [[ "$output" == *"one"* ]]
    [[ "$output" == *"two"* ]]
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 1 ]
    contains "g.bats:2" "$output"
    contains "g.bats:3" "$output"
}

# -------------------------------------------------------------------- quiet

@test "silent on a [[ ]] that is the last statement of its body" {
    fixture h.bats <<'EOF'
%TEST% "t" {
    [ "$status" -eq 0 ]
    [[ "$output" == *"needle"* ]]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "h.bats" "$output"
}

@test "silent on [[ ]] || fail" {
    fixture i.bats <<'EOF'
%TEST% "t" {
    [[ "$output" == *"needle"* ]] || fail "no needle"
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "i.bats" "$output"
}

@test "silent on a || fail carried onto a continuation line" {
    fixture j.bats <<'EOF'
%TEST% "t" {
    [[ "$output" == *"a"*"b"* ]] \
        || fail "not both"
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "j.bats" "$output"
}

@test "silent on [[ ]] || { ...; return 1; }" {
    fixture k.bats <<'EOF'
%TEST% "t" {
    [[ "$output" == *"needle"* ]] || {
        echo "no needle"
        return 1
    }
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "k.bats" "$output"
}

@test "silent on control flow - && continue and && skip" {
    fixture l.bats <<'EOF'
%TEST% "t" {
    while read -r line; do
        [[ -z "$line" ]] && continue
        [[ "$line" == \#* ]] && continue
    done <<< "$output"
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "l.bats" "$output"
}

@test "silent on a [[ ]] used as an if condition" {
    fixture m.bats <<'EOF'
%TEST% "t" {
    if [[ "$output" == *"needle"* ]]; then
        echo found
    fi
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "m.bats" "$output"
}

@test "silent on the accumulate-into-a-variable form" {
    fixture n.bats <<'EOF'
%TEST% "t" {
    local problems=""
    [[ "$output" == *LINKED* ]] || problems="${problems}link did not run; "
    [[ "$output" != *FAIL* ]] || problems="${problems}it failed; "
    [ -z "$problems" ] || { echo "$problems"; false; }
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "n.bats" "$output"
}

@test "silent on a [[ ]] that only appears inside a heredoc body" {
    fixture o.bats <<'EOF'
%TEST% "t" {
    cat > "$stub" <<'SH'
#!/usr/bin/env bash
[[ "$1" == "--flag" ]]
echo done
SH
    [ "$status" -eq 0 ]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "o.bats" "$output"
}

@test "silent on POSIX [ ] and on contains/lacks" {
    fixture p.bats <<'EOF'
%TEST% "t" {
    [ "$status" -eq 0 ]
    contains "needle" "$output"
    lacks "gone" "$output"
    [ -f "$file" ]
}
EOF
    gate
    [ "$status" -eq 0 ]
    lacks "p.bats" "$output"
}

# ----------------------------------------------------------------- coverage

@test "states how much it examined, so a zero means something" {
    fixture q.bats <<'EOF'
%TEST% "t" {
    [ "$status" -eq 0 ]
    [[ "$output" == *"needle"* ]]
}
EOF
    run python3 "$GATE" "$WORK"
    [ "$status" -eq 0 ]
    contains "assertion(s) examined across" "$output"
    contains "1 file(s)" "$output"
}

@test "examining no files is a failure, not a clean pass" {
    mkdir -p "$WORK/empty"
    run python3 "$GATE" "$WORK/empty"
    [ "$status" -eq 1 ]
    contains "no .bats files examined" "$output"
}

@test "--max-allowed ratchets" {
    fixture r.bats <<'EOF'
%TEST% "t" {
    [[ "$output" == *"one"* ]]
    [ "$status" -eq 0 ]
}
EOF
    run python3 "$GATE" --max-allowed 1 "$WORK"
    [ "$status" -eq 0 ]
    run python3 "$GATE" --max-allowed 0 "$WORK"
    [ "$status" -eq 1 ]
    contains "exceeds baseline" "$output"
}

@test "the tree itself is clean" {
    run python3 "$GATE"
    contains "assertion(s) examined across" "$output"
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------------- wiring
#
# A gate nobody runs is a file. The pre-commit hook, the pre-push hook and the
# Code Quality workflow all reach it through quality-checks.sh, so the wiring
# is pinned the same way the other gates pin theirs.

@test "gate is wired into quality-checks.sh" {
    run grep -c 'qc_bats_inert' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    # definition, QC_ALL registration, and the path-gating trigger row
    [ "$output" -ge 3 ]
}

@test "gate section is registered in the quality-checks section list" {
    run grep -q 'QC_ALL=.*qc_bats_inert' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate wakes on .bats files, on helpers.bash, and on itself" {
    run bash -c "sed -n '/qc_bats_inert)/,/;;/p' scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
    contains '.bats$' "$output"
    contains 'helpers\.bash' "$output"
    contains 'check_bats_inert_assertions\.py' "$output"
}

@test "quality-checks.sh actually runs the gate in its section" {
    run bash -c "sed -n '/^qc_bats_inert() {/,/^}/p' scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
    contains 'python3 scripts/check_bats_inert_assertions.py' "$output"
}
