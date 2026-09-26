#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_ams_xml_mirror.py: AmsState's re-entry mirror
# register_xml_subject_names() must publish exactly what init_subjects() does
# (prestonbrown/helixscreen#1439).
#
# Each case mutates one thing in a copy of the real src/printer/ams_state.cpp,
# so a red result is attributable to that mutation alone.

load helpers

SCRIPT="scripts/check_ams_xml_mirror.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    SRC="$BATS_TEST_TMPDIR/ams_state.cpp"
    cp src/printer/ams_state.cpp "$SRC"
}

# Insert $2 after the first line of $SRC matching the fixed string $1.
insert_after() {
    python3 - "$SRC" "$1" "$2" <<'EOF'
import sys
path, anchor, line = sys.argv[1:]
lines = open(path).read().split("\n")
idx = next(i for i, l in enumerate(lines) if anchor in l)
lines.insert(idx + 1, line)
open(path, "w").write("\n".join(lines))
EOF
}

@test "the real ams_state.cpp passes" {
    run python3 "$SCRIPT"
    [ "$status" -eq 0 ]
    contains "registrations match" "$output"
}

@test "a name added to init_subjects() only is caught" {
    insert_after 'INIT_SUBJECT_INT(backend_count, 0, subjects_, register_xml);' \
        '    INIT_SUBJECT_INT(ams_gate_probe, 0, subjects_, register_xml);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "ams_gate_probe (&ams_gate_probe_) is published by AmsState::init_subjects() but not" "$output"
}

@test "a name added to register_xml_subject_names() only is caught" {
    insert_after 'register_subject_in_current_scope("backend_count", &backend_count_);' \
        '    lv_xml_register_subject(nullptr, "ams_gate_probe", &gate_probe_);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "ams_gate_probe (&gate_probe_) is published by AmsState::register_xml_subject_names() but not" "$output"
}

@test "a loop entry dropped from the mirror is caught" {
    # 8-space indent is the mirror's loop; init_subjects() nests it one deeper.
    sed -i '/^        lv_xml_register_subject(nullptr, name_buf, &slot_error_severity_\[i\]);$/d' "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "ams_slot_%d_error_severity[MAX_SLOTS]" "$output"
}

@test "a mirror entry commented out is caught" {
    sed -i 's#^    lv_xml_register_subject(nullptr, "ams_bypass_active", &bypass_active_);$#    // &#' "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "ams_bypass_active (&bypass_active_)" "$output"
}

@test "a macro passing a literal false publishes nothing and needs no mirror" {
    insert_after 'INIT_SUBJECT_INT(backend_count, 0, subjects_, register_xml);' \
        '    INIT_SUBJECT_INT(ams_gate_probe, 0, subjects_, false);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 0 ]
}

@test "a registration whose name the gate cannot reduce fails closed" {
    insert_after 'register_subject_in_current_scope("backend_count", &backend_count_);' \
        '    lv_xml_register_subject(nullptr, probe_name.c_str(), &gate_probe_);'
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "cannot reduce registration" "$output"
}

@test "bodies with no registrations fail instead of passing vacuously" {
    cat > "$SRC" <<'EOF'
void AmsState::init_subjects(bool register_xml) {
    spdlog::trace("nothing here");
}
void AmsState::register_xml_subject_names() {
}
EOF
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "AmsState::init_subjects(): no XML subject registrations found" "$output"
    contains "AmsState::register_xml_subject_names(): no XML subject registrations found" "$output"
}

@test "a renamed mirror function fails instead of passing vacuously" {
    sed -i 's/AmsState::register_xml_subject_names()/AmsState::publish_xml_names()/' "$SRC"
    run python3 "$SCRIPT" --file "$SRC"
    [ "$status" -eq 1 ]
    contains "AmsState::register_xml_subject_names() not found" "$output"
}

@test "a missing file is reported without a verdict" {
    run python3 "$SCRIPT" --file "$BATS_TEST_TMPDIR/no-such-file.cpp"
    [ "$status" -eq 2 ]
}

@test "the pre-commit gate runs it and wakes on ams_state.cpp" {
    run grep -c "check_ams_xml_mirror.py" scripts/quality-checks.sh
    [ "$output" -ge 1 ]
    run bash -c "sed -n '/^qc_trigger_re()/,/^}/p' scripts/quality-checks.sh | grep qc_ams_xml_mirror"
    [ "$status" -eq 0 ]
    contains 'src/printer/ams_state' "$output"
}
