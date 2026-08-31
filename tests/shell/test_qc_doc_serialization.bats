#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for how scripts/quality-checks.sh SCHEDULES its two doc sections.
#
# qc_doc_refs and qc_doc_links both repair the same .md files under --auto-fix:
# qc_doc_refs runs doc_cite_anchors.py then gen_doc_links.py, qc_doc_links runs
# gen_doc_links.py. Both ran in the parallel fan-out, which broke two things at
# once.
#
#   Ordering. mk/tools.mk calls the anchors-then-links order "load-bearing":
#   anchors rewrite the line number INSIDE the link text, so the link generator
#   has to run second or it derives a URL from a number that is about to change.
#   Fanned out, the order was whatever the scheduler picked.
#
#   Atomicity. gen_doc_links.py reads a doc and then reopens it with
#   open(doc, 'w'), which truncates before a byte is written. A sibling reading
#   or writing the same file across that window loses the other's repair, and in
#   the worst case a committed doc is left truncated.
#
# The fix is scheduling, so that is what these pin: both sections serial, in
# that order, whenever --auto-fix can make them write.

QC="scripts/quality-checks.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# The line that extends QC_SERIAL when --auto-fix is on.
autofix_serial_line() {
    grep 'AUTO_FIX.*QC_SERIAL=' "$QC"
}

# Body of one qc_* section, first line to the terminating '# ===='.
section_body() {
    awk "/^$1\(\) \{/,/^# ====/" "$QC"
}

@test "premise: qc_doc_refs rewrites docs under --auto-fix" {
    body=$(section_body qc_doc_refs)
    echo "$body" | grep -q 'doc_cite_anchors.py'
    echo "$body" | grep -q 'gen_doc_links.py'
}

@test "premise: qc_doc_links rewrites docs under --auto-fix" {
    body=$(section_body qc_doc_links)
    # The unflagged invocation is the writing one; --diff/--check only report.
    echo "$body" | grep -qE 'gen_doc_links\.py( |$)'
}

@test "premise: gen_doc_links.py truncates in place, so a concurrent reader can lose" {
    grep -q "open(doc, *'w')" scripts/gen_doc_links.py
}

@test "both doc sections are serial under --auto-fix" {
    line=$(autofix_serial_line)
    echo "$line" | grep -q 'qc_doc_refs'
    echo "$line" | grep -q 'qc_doc_links'
}

@test "qc_doc_refs is scheduled before qc_doc_links" {
    # QC_SERIAL is consumed by a plain `for fn in $QC_SERIAL`, so the order in
    # the string IS the run order. Anchors first, links second.
    line=$(autofix_serial_line)
    refs=$(echo "$line" | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /qc_doc_refs/) print i }')
    links=$(echo "$line" | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /qc_doc_links/) print i }')
    [ -n "$refs" ]
    [ -n "$links" ]
    [ "$refs" -lt "$links" ]
}

@test "the serial list is consumed in order, not sorted or shuffled" {
    grep -q 'for fn in .QC_SERIAL; do' "$QC"
}

@test "serial sections run before the parallel fan-out" {
    # Serialising the two only helps if nothing in the fan-out overlaps them.
    serial_at=$(grep -n 'for fn in .QC_SERIAL; do' "$QC" | cut -d: -f1)
    parallel_at=$(grep -n 'for fn in .QC_PARALLEL; do' "$QC" | cut -d: -f1)
    [ -n "$serial_at" ]
    [ -n "$parallel_at" ]
    [ "$serial_at" -lt "$parallel_at" ]
}

@test "a serial section is excluded from the parallel list" {
    # QC_PARALLEL is built as QC_ALL minus QC_SERIAL; without that the sections
    # would simply run twice, concurrently with themselves.
    grep 'case " .QC_SERIAL " in' "$QC" | grep -q 'QC_PARALLEL='
}

@test "both doc sections are still declared in QC_ALL" {
    grep -q 'QC_ALL=.*qc_doc_refs' "$QC"
    grep -q 'QC_ALL=.*qc_doc_links' "$QC"
}
