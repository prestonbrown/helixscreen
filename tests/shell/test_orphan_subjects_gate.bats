#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_orphan_subjects.py — the gate catching LVGL
# subjects that C++ registers and keeps current but nothing ever reads.
#
# The gate runs at --max-allowed 0, so its whole value is that a dead subject
# cannot land. That makes a FALSE NEGATIVE the failure that matters: an orphan
# the gate clears is invisible, and the gate still reports green. Most of these
# tests therefore assert that a dead subject IS reported, in the layouts where
# clearing it would be easiest to do by accident.
#
# The opposing pressure is real. Registrations and observer calls live a few
# lines apart by construction, and a subject fetched by name is read through a
# pointer on the NEXT statement, so the name never appears at the read. A gate
# that ignored that shape would report live subjects and get switched off. Both
# directions are pinned here.

load helpers

GATE="scripts/check_orphan_subjects.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src" "$ROOT/include" "$ROOT/ui_xml"
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT" --list
}

# ------------------------------------------------------ false negatives (the point)

@test "a subject nothing reads is reported" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "totally_dead_subject", &dead_member_);
}
EOF
    run_gate
    contains "totally_dead_subject" "$output"
    contains "1 of 1 registered" "$output"
}

@test "a dead subject registered just above an unrelated observer is still reported" {
    # The fail-open shape: init_subjects() registers everything together and the
    # observer calls follow a few lines down, so a window that reaches backwards
    # from every read site clears every registration sitting above one.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "totally_dead_subject", &dead_member_);
    lv_xml_register_subject(nullptr, "live_subject", &live_member_);

    observe_int_sync<Panel>(&live_member_, cb);
}
EOF
    run_gate
    contains "totally_dead_subject" "$output"
    contains "1 of 2 registered" "$output"
}

@test "a dead subject registered just below an unrelated observer is still reported" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "live_subject", &live_member_);
    observe_int_sync<Panel>(&live_member_, cb);

    lv_xml_register_subject(nullptr, "totally_dead_subject", &dead_member_);
}
EOF
    run_gate
    contains "totally_dead_subject" "$output"
    contains "1 of 2 registered" "$output"
}

# ------------------------------------------------------ false positives (must stay fixed)

@test "a subject fetched by name and read through the pointer is not an orphan" {
    # The literal sits on the lv_xml_get_subject() line and the read is the next
    # statement, so the name never appears at the lv_subject_get_int() call.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "chamber_filter_fan_on", &filter_on_);
}

void ChamberPanel::apply() {
    lv_subject_t* on = lv_xml_get_subject(nullptr, "chamber_filter_fan_on");
    tc->set_chamber_filter_fan(!on || lv_subject_get_int(on) != 1);
}
EOF
    run_gate
    lacks "chamber_filter_fan_on" "$output"
    contains "0 of 1 registered" "$output"
}

@test "a subject observed by member pointer is not an orphan" {
    # Observers take the MEMBER, which routinely does not match the subject string.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "volume_value", &volume_value_subject_);
}

void SoundPanel::attach() {
    observe_int_sync<SoundPanel>(&volume_value_subject_, on_volume);
}
EOF
    run_gate
    contains "0 of 1 registered" "$output"
}

@test "a subject bound only from XML is not an orphan" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "nozzle_temp_text", &nozzle_text_);
}
EOF
    printf '<view><lv_label bind_text="nozzle_temp_text"/></view>\n' \
        > "$ROOT/ui_xml/panel.xml"
    run_gate
    contains "0 of 1 registered" "$output"
}

@test "a SUBJECT_OK opt-out on the call is honoured" {
    # The annotation rides the call or its continuation lines, not the line
    # above it: clang-format wraps these calls, and honouring the preceding
    # line would let one opt-out silently cover the next registration too.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "plugin_state",
                            &plugin_state_); // SUBJECT_OK: read by the plugin ABI
}
EOF
    run_gate
    contains "0 of 0 registered" "$output"
}

@test "one observer does not clear the whole block of registrations around it" {
    # A registration spells its own name and member, so scanning registrations
    # as read text makes each one vouch for itself and for its neighbours. The
    # window is 400 chars, which is most of an init_subjects() body: a single
    # observer anywhere in it would clear every registration in the block.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "first_dead_subject", &first_member_);
    lv_xml_register_subject(nullptr, "second_dead_subject", &second_member_);
    lv_xml_register_subject(nullptr, "live_subject", &live_member_);

    observe_int_sync<Panel>(&live_member_, cb);
}
EOF
    run_gate
    contains "first_dead_subject" "$output"
    contains "second_dead_subject" "$output"
    contains "2 of 3 registered" "$output"
}

# ------------------------------------------------------ the registering macros
#
# Most of the population never spells a quoted name at a raw registration call:
# it goes through INIT_SUBJECT_*, UI_MANAGED_SUBJECT_* or
# UI_SUBJECT_INIT_AND_REGISTER_*. A macro family the collector does not match is
# not merely unflagged, it is UNCOUNTED — and since the gate compares a count
# against a baseline, a population that shrinks can only report green. The
# "N of M registered" assertions below are what pins the population size, so
# each case checks both halves of that line.

@test "a subject registered only through INIT_SUBJECT_INT is counted and reported" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void PrinterThing::init_subjects(bool register_xml) {
    INIT_SUBJECT_INT(macro_dead_subject, 0, subjects_, register_xml);
}
EOF
    run_gate
    contains "macro_dead_subject" "$output"
    contains "1 of 1 registered" "$output"
}

@test "a subject registered only through UI_MANAGED_SUBJECT_INT is counted and reported" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void Panel::init_subjects() {
    UI_MANAGED_SUBJECT_INT(managed_dead_member_, 0, "managed_dead_subject", subjects_);
}
EOF
    run_gate
    contains "managed_dead_subject" "$output"
    contains "1 of 1 registered" "$output"
}

@test "a subject registered only through UI_SUBJECT_INIT_AND_REGISTER_INT is counted" {
    # This family ends with the name rather than a SubjectManager, so the name
    # sits one argument further right than in UI_MANAGED_SUBJECT_*.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_source_subjects() {
    UI_SUBJECT_INIT_AND_REGISTER_INT(s_dead_member_, 0, "registry_dead_subject");
}
EOF
    run_gate
    contains "registry_dead_subject" "$output"
    contains "1 of 1 registered" "$output"
}

@test "a live UI_MANAGED_SUBJECT_STRING is not an orphan and is still counted" {
    # The buffer and initial value sit between the member and the name, and the
    # initial value is itself a quoted literal — the name is the one nearest the
    # SubjectManager, not the first literal in the call.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void Panel::init_subjects() {
    UI_MANAGED_SUBJECT_STRING(status_member_, status_buf_, "Idle", "panel_status_text",
                              subjects_);
}
EOF
    printf '<view><lv_label bind_text="panel_status_text"/></view>\n' > "$ROOT/ui_xml/p.xml"
    run_gate
    lacks "panel_status_text" "$output"
    lacks "Idle" "$output"
    contains "0 of 1 registered" "$output"
}

@test "a dead UI_MANAGED registration beside an unrelated observer is still reported" {
    # The macro call spells both the name and the member, so left in the scanned
    # text it satisfies the read check for itself from inside the window around
    # any neighbouring read.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void Panel::init_subjects() {
    UI_MANAGED_SUBJECT_INT(dead_member_, 0, "managed_dead_subject", subjects_);
    UI_MANAGED_SUBJECT_INT(live_member_, 0, "managed_live_subject", subjects_);

    observe_int_sync<Panel>(&live_member_, cb);
}
EOF
    run_gate
    contains "managed_dead_subject" "$output"
    lacks "managed_live_subject" "$output"
    contains "1 of 2 registered" "$output"
}

@test "a macro whose name is composed at runtime is skipped, not guessed at" {
    # Only a plain literal can be matched against XML and read sites. Taking the
    # nearest literal instead would register the INITIAL VALUE as a subject name
    # and report it as an orphan that does not exist.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void Panel::init_subjects() {
    for (int i = 0; i < 4; ++i) {
        UI_MANAGED_SUBJECT_STRING(slot_members_[i], slot_bufs_[i], "empty",
                                  slot_key(i).c_str(), subjects_);
    }
}
EOF
    run_gate
    lacks "empty" "$output"
    contains "0 of 0 registered" "$output"
}

@test "a macro named only in a doc comment is not a registration" {
    # Every base class that mentions these macros documents them with a worked
    # example, and prose names them in passing.
    cat > "$ROOT/include/panel_base.h" <<'EOF'
/**
 * Subjects are published from init_subjects():
 *
 *     UI_MANAGED_SUBJECT_INT(my_subject_, 0, "my_subject", subjects_);
 *     INIT_SUBJECT_INT(temperature, 0, subjects_, true);
 */
class PanelBase {};
EOF
    run_gate
    lacks "my_subject" "$output"
    lacks "temperature" "$output"
    contains "0 of 0 registered" "$output"
}

@test "register_subject_in_current_scope is a registration" {
    # The scoped spelling is what the macros expand to, and callers that publish
    # a name a second time use it directly.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void AmsState::register_names() {
    helix::xml::register_subject_in_current_scope("scoped_dead_subject", &scoped_dead_);
}
EOF
    run_gate
    contains "scoped_dead_subject" "$output"
    contains "1 of 1 registered" "$output"
}

# ------------------------------------------- an accessor is not its own reader

@test "an out-of-line accessor definition does not vouch for its own subject" {
    # `Class::get_x_subject()` is the house spelling for a PrinterState or
    # AmsState accessor DEFINITION, and those definitions sit among the class's
    # own reads — so the signature lands inside the window around a neighbouring
    # read site. A predicate that accepts `::` there counts the definition as a
    # call, and every accessor then clears its own subject. The second subject is
    # the opposing pressure: an accessor that IS called must stay cleared.
    cat > "$ROOT/src/state.cpp" <<'EOF'
void AmsState::init_subjects(bool register_xml) {
    INIT_SUBJECT_STRING(lonely_accessor_text, "---", subjects_, register_xml);
    INIT_SUBJECT_STRING(busy_accessor_text, "---", subjects_, register_xml);
    INIT_SUBJECT_INT(sync_flag, 0, subjects_, register_xml);
}

void AmsState::sync_from_backend() {
    if (lv_subject_get_int(&sync_flag_) != 1) {
        lv_subject_set_int(&sync_flag_, 1);
    }
}

lv_subject_t* AmsState::get_lonely_accessor_text_subject() {
    return &lonely_accessor_text_;
}

lv_subject_t* AmsState::get_busy_accessor_text_subject() {
    return &busy_accessor_text_;
}
EOF
    cat > "$ROOT/src/card.cpp" <<'EOF'
void DryerCard::setup() {
    lv_label_bind_text(label_, AmsState::instance().get_busy_accessor_text_subject(), nullptr);
}
EOF
    run_gate
    contains "lonely_accessor_text" "$output"
    lacks "busy_accessor_text" "$output"
    contains "1 of 3 registered" "$output"
}

@test "a doc comment naming an accessor does not vouch for its subject" {
    # Header prose cites accessors by their qualified name, and it sits beside
    # the class's inline getters, so it lands in a read window too.
    cat > "$ROOT/src/state.cpp" <<'EOF'
void PrinterState::init_subjects(bool register_xml) {
    INIT_SUBJECT_INT(documented_only_flag, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(neighbour_flag, 0, subjects_, register_xml);
}
EOF
    cat > "$ROOT/include/state.h" <<'EOF'
class PrinterState {
  public:
    /// Derived from PrinterPrintState::get_documented_only_flag_subject().
    bool neighbour_set() const {
        return lv_subject_get_int(&neighbour_flag_) == 1;
    }

  private:
    lv_subject_t neighbour_flag_;
};
EOF
    run_gate
    contains "documented_only_flag" "$output"
    contains "1 of 2 registered" "$output"
}

@test "an accessor called through a pointer is still a read" {
    cat > "$ROOT/src/state.cpp" <<'EOF'
void PrinterState::init_subjects(bool register_xml) {
    INIT_SUBJECT_INT(pointer_read_flag, 0, subjects_, register_xml);
}
EOF
    cat > "$ROOT/src/widget.cpp" <<'EOF'
void Widget::attach(PrinterState* state) {
    lv_subject_add_observer(state->get_pointer_read_flag_subject(), cb, this);
}
EOF
    run_gate
    contains "0 of 1 registered" "$output"
}

@test "an accessor named after the member, not the subject, is still a read" {
    # The publisher chooses the XML name; the accessor is named after the member
    # it hands out. WidthSensorManager publishes "filament_width_diameter" from
    # diameter_ and exposes get_diameter_subject(), and the caller passes the
    # result straight in — so neither the subject name nor the member is spelled
    # at the read.
    cat > "$ROOT/src/manager.cpp" <<'EOF'
void WidthSensorManager::init_subjects() {
    UI_MANAGED_SUBJECT_INT(diameter_, -1, "sensor_width_diameter", subjects_);
}
EOF
    cat > "$ROOT/src/widget.cpp" <<'EOF'
void WidthSensorWidget::attach() {
    observe_int_sync<WidthSensorWidget>(wsm.get_diameter_subject(), cb);
}
EOF
    run_gate
    contains "0 of 1 registered" "$output"
}

# ------------------------------------------------------ the accepted-debt list
#
# The gate keys on NAMES, not on a count, so it also catches a swap: one orphan
# removed while another arrives leaves the count unchanged.

@test "an orphan on the baseline is accepted debt" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "known_debt_subject", &known_member_);
}
EOF
    printf '# accepted debt\nknown_debt_subject  # src/demo.cpp:2\n' > "$ROOT/base.txt"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/base.txt"
    [ "$status" -eq 0 ]
    contains "all accepted debt" "$output"
}

@test "an orphan that is not on the baseline fails the gate" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "known_debt_subject", &known_member_);
    lv_xml_register_subject(nullptr, "brand_new_orphan", &new_member_);
}
EOF
    printf 'known_debt_subject  # src/demo.cpp:2\n' > "$ROOT/base.txt"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/base.txt"
    [ "$status" -eq 1 ]
    contains "brand_new_orphan" "$output"
    lacks "known_debt_subject" "$output"
}

@test "a swap keeps the count and still fails the gate" {
    # The count is 1 either way, which is what a count-only ratchet reads as
    # unchanged.
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "replacement_orphan", &replacement_member_);
}
EOF
    printf 'known_debt_subject  # src/demo.cpp:2\n' > "$ROOT/base.txt"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/base.txt"
    [ "$status" -eq 1 ]
    contains "replacement_orphan" "$output"
}

@test "a baseline entry that stopped being an orphan asks to be dropped" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "was_debt_subject", &was_member_);
}

void Panel::attach() {
    observe_int_sync<Panel>(&was_member_, cb);
}
EOF
    printf 'was_debt_subject  # src/demo.cpp:2\n' > "$ROOT/base.txt"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/base.txt"
    [ "$status" -eq 0 ]
    contains "--write-baseline" "$output"
    contains "was_debt_subject" "$output"
}

@test "--write-baseline records every orphan with its site" {
    cat > "$ROOT/src/demo.cpp" <<'EOF'
void init_subjects() {
    lv_xml_register_subject(nullptr, "recorded_orphan", &recorded_member_);
}
EOF
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/base.txt" --write-baseline
    [ "$status" -eq 0 ]
    contains "recorded_orphan  # src/demo.cpp:2" "$(cat "$ROOT/base.txt")"
    # Round-trips: what it wrote is what it accepts.
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/base.txt"
    [ "$status" -eq 0 ]
}
