#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/qc_file_cache.py, the per-file cache the registration and
# ESP32 gates share. A cache that answers for a file it has not read is a gate that
# passes on yesterday's tree, so the edits below keep the size and the modification
# time and change only the bytes: content alone must decide.

load helpers

CALLBACKS="scripts/check_orphan_callbacks.py"
SUBJECTS="scripts/check_orphan_subjects.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src" "$ROOT/include" "$ROOT/ui_xml"
    export HELIX_QC_CACHE_DIR="$BATS_TEST_TMPDIR/cache"
    echo '<component><view><x callback="on_live"/></view></component>' > "$ROOT/ui_xml/demo.xml"
}

# Rewrite FILE with TEXT, keeping its modification time; TEXT keeps the size.
same_stamp_edit() {
    local file="$1" text="$2"
    touch -r "$file" "$BATS_TEST_TMPDIR/stamp"
    printf '%s' "$text" > "$file"
    touch -r "$BATS_TEST_TMPDIR/stamp" "$file"
}

@test "a registration removed after a cached run is reported, at the same size and time" {
    printf 'void r() { lv_xml_register_event_cb(nullptr, "on_live", f); }\n' > "$ROOT/src/a.cpp"
    run python3 "$CALLBACKS" --repo-root "$ROOT" --list
    ! contains "unregistered:on_live" "$output" || fail "a registered callback read as missing: $output"
    [ -s "$HELIX_QC_CACHE_DIR/callback_registrations.json" ] || fail "the first run cached nothing"

    same_stamp_edit "$ROOT/src/a.cpp" 'void r() { lv_xml_register_event_cb(nullptr, "on_gone", f); }
'
    run python3 "$CALLBACKS" --repo-root "$ROOT" --list
    contains "unregistered:on_live" "$output" || fail "the cache answered for an edited file: $output"
}

@test "the cache's second run gives the first run's answer" {
    printf 'void r() { register_xml_callbacks({{"on_live", f}, {"on_dead", g}}); }\n' > "$ROOT/src/a.cpp"
    run python3 "$CALLBACKS" --repo-root "$ROOT" --list
    local cold="$output"
    run python3 "$CALLBACKS" --repo-root "$ROOT" --list
    [ "$output" = "$cold" ] || fail "warm run differs: cold=[$cold] warm=[$output]"
    contains "unreferenced:on_dead" "$output"
}

@test "a subject registration's cached facts follow the file's bytes" {
    printf 'void i() { UI_MANAGED_SUBJECT_INT(a_, 0, "live_count", s); }\n' > "$ROOT/src/s.cpp"
    echo '<component><view><lv_label bind_text="live_count"/></view></component>' > "$ROOT/ui_xml/demo.xml"
    run python3 "$SUBJECTS" --repo-root "$ROOT" --list
    local cold="$output"
    same_stamp_edit "$ROOT/src/s.cpp" 'void i() { UI_MANAGED_SUBJECT_INT(a_, 0, "dead_count", s); }
'
    run python3 "$SUBJECTS" --repo-root "$ROOT" --list
    contains "dead_count" "$output" || fail "the cache kept the old registration: cold=[$cold] warm=[$output]"
}

@test "a corrupt or foreign cache file is ignored, never trusted" {
    printf 'void r() { lv_xml_register_event_cb(nullptr, "on_live", f); }\n' > "$ROOT/src/a.cpp"
    mkdir -p "$HELIX_QC_CACHE_DIR"
    # An entry under this file's real key, saying it registers nothing, written by
    # checker code that is not this one.
    python3 - "$ROOT/src/a.cpp" "$HELIX_QC_CACHE_DIR/callback_registrations.json" <<'PY'
import json, sys
sys.path.insert(0, "scripts")
from qc_file_cache import digest
raw = open(sys.argv[1], "rb").read()
json.dump({"salt": "not-this-one", "entries": {digest("raw", "replace", raw): [0, []]}},
          open(sys.argv[2], "w"))
PY
    run python3 "$CALLBACKS" --repo-root "$ROOT" --list
    ! contains "unregistered:on_live" "$output" || fail "a foreign cache changed the answer: $output"
    printf 'garbage{' > "$HELIX_QC_CACHE_DIR/callback_registrations.json"
    run python3 "$CALLBACKS" --repo-root "$ROOT" --list
    [ "$status" -ne 2 ] || fail "a corrupt cache broke the gate: $output"
    ! contains "unregistered:on_live" "$output" || fail "a corrupt cache changed the answer: $output"
}

@test "HELIX_QC_FILE_CACHE=0 writes no cache" {
    printf 'void r() { lv_xml_register_event_cb(nullptr, "on_live", f); }\n' > "$ROOT/src/a.cpp"
    HELIX_QC_FILE_CACHE=0 run python3 "$CALLBACKS" --repo-root "$ROOT" --list
    [ ! -e "$HELIX_QC_CACHE_DIR/callback_registrations.json" ] || fail "a disabled cache was written"
}
