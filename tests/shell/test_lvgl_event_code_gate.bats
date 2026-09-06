#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/gen_lvgl_event_codes.py - the generator that mirrors
# LVGL's lv_event_code_t into the crash worker's code -> name table.
#
# The failure mode it exists for: the worker stamps "code=N (NAME)" on every
# auto-filed crash issue, and that name is frequently the entire diagnosis. The
# table was typed by hand, LVGL 9.5 inserted SINGLE/DOUBLE/TRIPLE_CLICKED and
# STATE_CHANGED mid-enum, and 58 of 63 entries silently shifted. #1356 - a
# use-after-free in a widget's LV_EVENT_DELETE handler - was filed as
# SCREEN_UNLOAD_START, which is the one label that would have named the bug.
# Nothing was red: the worker's own unit tests spot-checked codes against the
# same stale table they were meant to police.
#
# Both halves are pinned here. The catch half proves drift fails. The quiet half
# matters just as much - a generator that rewrote the table on every run, or
# that guessed at a conditional enumerator, would either churn the diff or
# renumber every code below it without saying so.

GEN="scripts/gen_lvgl_event_codes.py"
WORKER="server/crash-worker/src/index.ts"

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1

    # A throwaway copy of just what the generator reads and writes. Its REPO is
    # derived from its own path, so this is enough of a tree - and it keeps
    # every mutation below out of the real working copy.
    TREE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$TREE/scripts" "$TREE/lib/lvgl/src/misc" "$TREE/server/crash-worker/src"
    cp "$GEN" "$TREE/scripts/"
    cp lv_conf.h "$TREE/lv_conf.h"
    cp lib/lvgl/src/misc/lv_event.h "$TREE/lib/lvgl/src/misc/"
    cp "$WORKER" "$TREE/$WORKER"
    GEN_COPY="$TREE/scripts/gen_lvgl_event_codes.py"
    WORKER_COPY="$TREE/$WORKER"
}

# --- the committed artifact matches its source ---

@test "committed worker table is up to date with lv_event_code_t" {
    run python3 "$GEN" --check
    [ "$status" -eq 0 ]
}

@test "regenerating is idempotent - a second run rewrites nothing" {
    run python3 "$GEN_COPY"
    [ "$status" -eq 0 ]
    run diff -q "$WORKER_COPY" "$WORKER"
    [ "$status" -eq 0 ]
}

@test "the table decodes 42 as DELETE" {
    # The specific regression. 42 was labelled SCREEN_UNLOAD_START on #1356.
    run grep -qE '^ *42: "DELETE",' "$WORKER"
    [ "$status" -eq 0 ]
}

@test "the codes LVGL 9.5 inserted are present" {
    run grep -qE '^ *5: "SINGLE_CLICKED",' "$WORKER"
    [ "$status" -eq 0 ]
    run grep -qE '^ *40: "STATE_CHANGED",' "$WORKER"
    [ "$status" -eq 0 ]
}

# --- the catch half ---

@test "a hand-edited table name fails --check" {
    require_gnu_sed
    sed -i 's/42: "DELETE",/42: "SCREEN_UNLOAD_START",/' "$WORKER_COPY"
    run python3 "$GEN_COPY" --check
    [ "$status" -eq 1 ]
    [[ "$output" == *"drifted"* ]]
}

@test "a code added to the enum fails --check" {
    require_gnu_sed
    # What LVGL 9.5 did: an enumerator inserted mid-list shifts everything after
    # it. This is the shape the old hand-maintained table slept through.
    sed -i 's/    LV_EVENT_DELETE,/    LV_EVENT_BRAND_NEW,\n    LV_EVENT_DELETE,/' \
        "$TREE/lib/lvgl/src/misc/lv_event.h"
    run python3 "$GEN_COPY" --check
    [ "$status" -eq 1 ]
    run python3 "$GEN_COPY"
    [ "$status" -eq 0 ]
    run grep -qE '^ *43: "DELETE",' "$WORKER_COPY"
    [ "$status" -eq 0 ]
}

@test "--diff names the code that moved" {
    require_gnu_sed
    sed -i 's/42: "DELETE",/42: "NOPE",/' "$WORKER_COPY"
    run python3 "$GEN_COPY" --diff
    [ "$status" -eq 1 ]
    [[ "$output" == *'42: "DELETE"'* ]]
}

# --- the quiet half ---

@test "sentinels and bit flags are not emitted as codes" {
    # LV_EVENT_LAST is a count, never dispatched. PREPROCESS (0x8000) and
    # MARKED_DELETING (0x10000) are flags OR'd onto a code - emitting either as
    # a table entry would collide with the mask in lvglEventCodeName().
    run grep -qE '"LAST"|"PREPROCESS"|"MARKED_DELETING"' "$WORKER"
    [ "$status" -ne 0 ]
}

@test "a conditional enumerator is resolved from lv_conf.h, not guessed" {
    require_gnu_sed
    # LV_USE_TRANSLATION gates the last enumerator. Turning it off must drop
    # that name rather than leave a code pointing at nothing.
    run grep -q 'TRANSLATION_LANGUAGE_CHANGED' "$WORKER_COPY"
    [ "$status" -eq 0 ]
    sed -i 's/#define LV_USE_TRANSLATION    1/#define LV_USE_TRANSLATION    0/' "$TREE/lv_conf.h"
    run python3 "$GEN_COPY"
    [ "$status" -eq 0 ]
    run grep -q 'TRANSLATION_LANGUAGE_CHANGED' "$WORKER_COPY"
    [ "$status" -ne 0 ]
}

@test "an unevaluable conditional aborts instead of renumbering silently" {
    require_gnu_sed
    sed -i 's/^#if LV_USE_TRANSLATION$/#if defined(SOMETHING) \&\& OTHER/' \
        "$TREE/lib/lvgl/src/misc/lv_event.h"
    run python3 "$GEN_COPY"
    [ "$status" -ne 0 ]
    [[ "$output" == *"cannot evaluate"* ]]
}

@test "a checkout without submodules skips instead of failing" {
    # CI and fresh clones run gates before `git submodule update`. The artifact
    # is committed, so there is nothing to verify and nothing to repair.
    rm -rf "$TREE/lib/lvgl"
    run python3 "$GEN_COPY" --check
    [ "$status" -eq 0 ]
    [[ "$output" == *"skipping"* ]]
}

# --- wiring ---

@test "gate is wired into quality-checks.sh" {
    run grep -q "gen_lvgl_event_codes.py" scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate section is registered in the quality-checks section list" {
    run grep -q 'QC_ALL=.*qc_lvgl_event_codes' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate wakes on the worker, the enum config, and the generator" {
    run bash -c "sed -n '/qc_lvgl_event_codes)/,/;;/p' scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
    contains "^server/crash-worker/" "$output"
    contains "gen_lvgl_event_codes" "$output"
    [[ "$output" == *"lv_conf"* ]]
}

@test "make exposes regen and check targets" {
    run grep -qE '^regen-lvgl-event-codes:' mk/tools.mk
    [ "$status" -eq 0 ]
    run grep -qE '^check-lvgl-event-codes:' mk/tools.mk
    [ "$status" -eq 0 ]
    run grep -q 'regen-lvgl-event-codes' Makefile
    [ "$status" -eq 0 ]
}
