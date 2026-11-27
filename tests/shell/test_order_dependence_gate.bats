#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_test_order_dependence.py — the gate that finds
# tests which pass only because of what ran before them.
#
# Built against a confirmed specimen: test_grid_edit_mode.cpp "build_default_grid
# only sets positions for anchor widgets" passes in the full suite and fails 5/5
# alone. None of the other four gates can see it — it asserts real computed
# values, its lines are covered, and reverting the production hunk would report
# it killed.
#
# The escaping test below is not hypothetical. Catch2 test specs give , [ ] * ~
# their own meaning, and this suite has three case names containing a comma. An
# unescaped name makes Catch2 reject the WHOLE file with `Invalid Filter` and
# emit a report containing nothing but an XML header — which reads as "no
# findings" unless the gate checks. That silent-zero is the failure this gate
# most needs to not have.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"; mkdir -p "$WORK/scripts" "$WORK/build/bin"
    cp "$REPO_ROOT/scripts/check_test_order_dependence.py" "$WORK/scripts/"
    git -C "$WORK" init -q
    git -C "$WORK" config user.email t@t
    git -C "$WORK" config user.name t
    git -C "$WORK" commit -q --allow-empty -m base
}

# Full-suite report: $1/$2 are the "success" values for case A / case B.
full_report() {
    cat > "$WORK/full.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun name="t">
  <TestCase name="case A" filename="tests/unit/test_x.cpp" line="1">
    <OverallResult success="$1" skips="0"/>
  </TestCase>
  <TestCase name="case B" filename="tests/unit/test_x.cpp" line="9">
    <OverallResult success="$2" skips="0"/>
  </TestCase>
</Catch2TestRun>
EOF
}

# Stub binary: emits an isolated report where case A takes $1 and case B $2.
stub_binary() {
    cat > "$WORK/build/bin/helix-tests" <<EOF
#!/usr/bin/env bash
out=""
while [ \$# -gt 0 ]; do
  case "\$1" in --out) out="\$2"; shift 2;; -f) spec="\$2"; shift 2;; *) shift;; esac
done
cp "\$spec" /tmp/last-spec.txt 2>/dev/null || true
cat > "\$out" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun name="t">
  <TestCase name="case A" filename="tests/unit/test_x.cpp" line="1">
    <OverallResult success="$1" skips="0"/>
  </TestCase>
  <TestCase name="case B" filename="tests/unit/test_x.cpp" line="9">
    <OverallResult success="$2" skips="0"/>
  </TestCase>
</Catch2TestRun>
XML
EOF
    chmod +x "$WORK/build/bin/helix-tests"
}

gate() { ( cd "$WORK" && python3 scripts/check_test_order_dependence.py full.xml --jobs 1 "$@" ); }

@test "passes in suite, fails alone is reported order-dependent" {
    full_report true true
    stub_binary false true
    run gate --list
    contains "order-dependent" "$output"
    [[ "$output" == *"case A"* ]]
}

@test "fails in suite, passes alone is reported as pollution" {
    full_report false true
    stub_binary true true
    run gate --list
    [[ "$output" == *"pollution"* ]]
}

@test "consistent results are not flagged" {
    full_report true true
    stub_binary true true
    run gate --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"findings: 0"* ]]
}

@test "a consistently failing test is not flagged (it is just broken)" {
    full_report false false
    stub_binary false false
    run gate --list
    [[ "$output" == *"findings: 0"* ]]
}

@test "test names are escaped for the Catch2 spec parser" {
    # An unescaped comma makes Catch2 reject the whole file.
    cat > "$WORK/full.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun name="t">
  <TestCase name="Drag end uses snap position, not release point" filename="tests/unit/test_x.cpp" line="1">
    <OverallResult success="true" skips="0"/>
  </TestCase>
</Catch2TestRun>
EOF
    stub_binary true true
    rm -f /tmp/last-spec.txt
    run gate --list
    [ -f /tmp/last-spec.txt ]
    grep -q 'snap position\\, not release' /tmp/last-spec.txt
}

@test "an isolated run that produces nothing is surfaced, not read as clean" {
    full_report true true
    printf '#!/usr/bin/env bash\nexit 0\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
    run gate --list
    contains "not-run" "$output"
    [[ "$output" != *"order-dependent"* ]]
}

@test "--max-allowed ratchets" {
    full_report true true
    stub_binary false true
    run gate --max-allowed 0
    [ "$status" -eq 1 ]
    run gate --max-allowed 1
    [ "$status" -eq 0 ]
}

@test "a missing binary is an error, not an empty pass" {
    full_report true true
    rm -f "$WORK/build/bin/helix-tests"
    run gate
    [ "$status" -ne 0 ]
    [[ "$output" == *"make test"* ]]
}

@test "sharding partitions files and covers every one exactly once" {
    # Two files, two shards: each shard must take exactly one.
    cat > "$WORK/full.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun name="t">
  <TestCase name="case A" filename="tests/unit/test_a.cpp" line="1">
    <OverallResult success="true" skips="0"/>
  </TestCase>
  <TestCase name="case B" filename="tests/unit/test_b.cpp" line="1">
    <OverallResult success="true" skips="0"/>
  </TestCase>
</Catch2TestRun>
EOF
    stub_binary true true
    run gate --shard-count 2 --shard-index 0
    contains "running 1 file" "$output"
    run gate --shard-count 2 --shard-index 1
    [[ "$output" == *"running 1 file"* ]]
}

@test "an out-of-range shard index is rejected" {
    full_report true true
    stub_binary true true
    run gate --shard-count 2 --shard-index 2
    [ "$status" -ne 0 ]
    [[ "$output" == *"shard-index"* ]]
}

@test "a hung isolated run is abandoned at the timeout, not waited on forever" {
    # One stuck file starved the whole pool for 30 minutes before this existed.
    full_report true true
    printf '#!/usr/bin/env bash\nsleep 60\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
    start=$(date +%s)
    run gate --timeout 2 --list
    elapsed=$(( $(date +%s) - start ))
    [ "$elapsed" -lt 30 ]
    contains "not-run" "$output"
    [[ "$output" == *"hung past"* ]]
}

# Evidence for these tests lives beside the report (full.xml) the gate was
# pointed at, one flattened directory per source file. The retention this
# pins was not cosmetic: the 2026-08-31 nightly fired a finding whose isolated
# run nobody could autopsy, because DEVNULL ate the child's output and the
# tempdir ate its report.
# The gate prints this path resolved absolute. $WORK can sit under a symlinked
# parent (/var -> /private/var on macOS), so resolve here too or the expected
# string is not the one the gate prints.
ev() { echo "$(cd "$WORK" && pwd -P)/order-dependence-evidence/tests_unit_test_x.cpp"; }

@test "a finding retains the isolated run's report, captured output, and spec" {
    full_report true true
    # Bespoke stub: writes last words on stderr so the capture is provably the
    # child's own output, not an empty file the gate dreamed up.
    cat > "$WORK/build/bin/helix-tests" <<'STUB'
#!/usr/bin/env bash
out=""
while [ $# -gt 0 ]; do
  case "$1" in --out) out="$2"; shift 2;; -f) spec="$2"; shift 2;; *) shift;; esac
done
echo "last words on stderr" >&2
cat > "$out" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun name="t">
  <TestCase name="case A" filename="tests/unit/test_x.cpp" line="1">
    <OverallResult success="false" skips="0"/>
  </TestCase>
  <TestCase name="case B" filename="tests/unit/test_x.cpp" line="9">
    <OverallResult success="true" skips="0"/>
  </TestCase>
</Catch2TestRun>
XML
STUB
    chmod +x "$WORK/build/bin/helix-tests"
    run gate --list
    contains "order-dependent" "$output"
    [ -f "$(ev)/r.xml" ]
    grep -q 'success="false"' "$(ev)/r.xml"
    grep -q "last words on stderr" "$(ev)/output.txt"
    # The finding says where the evidence landed.
    [[ "$output" == *"$(ev)"* ]]
}

@test "a pollution finding retains its green run's report too" {
    # Pollution is the mirror shape: the isolated run is GREEN, so "retain
    # when the run fails" alone would keep nothing - and the green report is
    # precisely the proof the case passes alone.
    full_report false true
    stub_binary true true
    run gate --list
    contains "pollution" "$output"
    [ -f "$(ev)/r.xml" ]
    grep -q 'success="true"' "$(ev)/r.xml"
}

@test "a green isolated run leaves no evidence behind" {
    full_report true true
    stub_binary true true
    run gate --list
    contains "findings: 0" "$output"
    [ ! -e "$WORK/order-dependence-evidence" ]
}

@test "a consistently failing file keeps no evidence (its story is the suite's)" {
    # Fails alone exactly as it fails in the suite: no ordering signal, and a
    # red suite must not accumulate a directory per red file.
    full_report false false
    stub_binary false false
    run gate --list
    contains "findings: 0" "$output"
    [ ! -e "$WORK/order-dependence-evidence" ]
}

@test "an un-judgeable run retains whatever it managed to write" {
    full_report true true
    printf '#!/usr/bin/env bash\nexit 0\n' > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
    run gate --list
    contains "not-run" "$output"
    contains "evidence retained at $(ev)" "$output"
    # No report was written; the escaped spec is the remaining record of what
    # the child was asked to run.
    [ -f "$(ev)/names.txt" ]
    grep -q "case A" "$(ev)/names.txt"
}

@test "a name Catch2's --input-file reader would drop or mangle is escaped" {
    # Catch2 strips any spec line starting with '#' as a comment, and re-quotes
    # a line that does not already start with '"'. An issue-numbered case name
    # is therefore removed from the spec entirely, and a name carrying a quote
    # is re-quoted into a spec that matches nothing.
    cat > "$WORK/full.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun name="t">
  <TestCase name="#1127 seeding state costs no extra bytes" filename="tests/unit/test_x.cpp" line="1">
    <OverallResult success="true" skips="0"/>
  </TestCase>
  <TestCase name="a root with permissions &quot;r&quot; is not writable" filename="tests/unit/test_x.cpp" line="9">
    <OverallResult success="true" skips="0"/>
  </TestCase>
</Catch2TestRun>
EOF
    stub_binary true true
    rm -f /tmp/last-spec.txt
    run gate --list
    [ -f /tmp/last-spec.txt ]
    # A line the reader treats as a comment never reaches the spec parser, and a
    # spec file with every line commented out is no filter at all.
    if grep -q '^#' /tmp/last-spec.txt; then
        echo "spec line still begins with # - Catch2 drops it" >&2
        return 1
    fi
    grep -q '\\#1127 seeding state' /tmp/last-spec.txt
    grep -q 'permissions \\"r\\" is not writable' /tmp/last-spec.txt
}

# --------------------------------------------------------- the report and the budget

# What the host offers the gate for a run's report: a memfd.
can_stage() { python3 -c 'import os; os.memfd_create("r")' 2>/dev/null; }

# Stub binary: sleeps $1 seconds first when given one, reports both cases
# green, and records into $RECORD the path it was told to write its report to.
green_stub() {
    {
        echo '#!/usr/bin/env bash'
        [ -z "${1:-}" ] || echo "sleep $1"
        cat <<'STUB'
out=""
while [ $# -gt 0 ]; do
  case "$1" in --out) out="$2"; shift 2;; -f) spec="$2"; shift 2;; *) shift;; esac
done
echo "$out" > "$RECORD"
cat > "$out" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun name="t">
  <TestCase name="case A" filename="tests/unit/test_x.cpp" line="1">
    <OverallResult success="true" skips="0"/>
  </TestCase>
  <TestCase name="case B" filename="tests/unit/test_x.cpp" line="9">
    <OverallResult success="true" skips="0"/>
  </TestCase>
</Catch2TestRun>
XML
STUB
    } > "$WORK/build/bin/helix-tests"
    chmod +x "$WORK/build/bin/helix-tests"
}

@test "an isolated run writes its report to memory where the host has a memfd" {
    can_stage || skip "no memfd on this host"
    full_report true true
    export RECORD="$WORK/out-path.txt"
    green_stub
    run gate --list
    [ "$status" -eq 0 ]
    contains "findings: 0" "$output"
    [[ "$(cat "$RECORD")" == /proc/self/fd/* ]]
}

@test "HELIX_REPORT_DIRECT=1 has an isolated run write its report to a file" {
    full_report true true
    export RECORD="$WORK/out-path.txt"
    export HELIX_REPORT_DIRECT=1
    green_stub
    run gate --list
    [ "$status" -eq 0 ]
    contains "findings: 0" "$output"
    [[ "$(cat "$RECORD")" == */r.xml ]]
}

# A full report in which case A made $1 assertions in the suite.
full_report_asserting() {
    local line='    <Expression success="true" type="REQUIRE" filename="tests/unit/test_x.cpp" line="2"><Original>1 == 1</Original><Expanded>1 == 1</Expanded></Expression>'
    {
        printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<Catch2TestRun name="t">' \
            '  <TestCase name="case A" filename="tests/unit/test_x.cpp" line="1">'
        printf "${line}\n%.0s" $(seq "$1")
        printf '%s\n' '    <OverallResult success="true" skips="0"/>' '  </TestCase>' \
            '  <TestCase name="case B" filename="tests/unit/test_x.cpp" line="9">' \
            '    <OverallResult success="true" skips="0"/>' '  </TestCase>' '</Catch2TestRun>'
    } > "$WORK/full.xml"
}

@test "a file's time budget grows with the assertions it makes" {
    # A file that asserts per pixel spends its run writing its report. Twenty
    # thousand assertions add ten seconds to a one-second --timeout, so a two
    # second run is judged instead of abandoned.
    full_report_asserting 20000
    export RECORD="$WORK/out-path.txt"
    green_stub 2
    run gate --timeout 1 --list
    [ "$status" -eq 0 ]
    contains "findings: 0" "$output"
    lacks "not-run" "$output"
}

@test "a file that made no assertions gets only the base timeout" {
    full_report true true
    export RECORD="$WORK/out-path.txt"
    green_stub 2
    run gate --timeout 1 --list
    contains "not-run" "$output"
    [[ "$output" == *"hung past its 1s timeout"* ]]
}
