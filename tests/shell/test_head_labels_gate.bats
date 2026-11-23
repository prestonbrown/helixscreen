#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_head_labels.py: on a rig whose G-code tool
# numbers name filament lanes, a head must be named by the head identity and
# mounted by the backend's head_select_gcode(), never by a tool number.
#
# The verdict over the real tree is qc_head_labels'; these cases pin the shapes
# the gate catches, the idioms it stays quiet on, and its wiring.

GATE="scripts/check_head_labels.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/head_labels"
    mkdir -p "$FIXTURE_DIR"
}

# Write $2 into a fixture .cpp and run the gate over that file alone.
run_gate() {
    printf '%s\n' "$2" > "$FIXTURE_DIR/$1.cpp"
    run python3 "$GATE" "$FIXTURE_DIR/$1.cpp"
}

# --- shapes that must fail ---------------------------------------------------

@test "flags a ToolInfo display_label read as head text" {
    run_gate label 'std::string f(const ToolInfo& t) {
    return t.display_label;
}'
    [ "$status" -eq 1 ]
    contains "[display-label]" "$output"
    contains ":2:" "$output"
}

@test "flags a raw ExtruderInfo display_name read, by variable and by map access" {
    run_gate extname 'void f(const PrinterTemperatureState& s) {
    for (const auto& [name, info] : s.extruders()) {
        title = info.display_name;
    }
    const ExtruderInfo& e = s.extruders().at("extruder");
    other = e.display_name;
    third = s.extruders()["extruder1"].display_name;
}'
    [ "$status" -eq 1 ]
    contains ":3: [extruder-name]" "$output"
    contains ":6: [extruder-name]" "$output"
    contains ":7: [extruder-name]" "$output"
}

@test "flags a label function fed representative_tool, active_tool_index or a head" {
    run_gate fed 'void f() {
    auto a = tool_label(head.representative_tool(active));
    auto b = active_tool_label(tool_label(i), tools.active_tool_index(), form);
    auto c = active_tool_badge_label(name, head.tool);
    auto d = active_tool_name_label(tool_label(heads[i].tool), heads[i].tool);
}'
    [ "$status" -eq 1 ]
    contains ":2: [tool-label]" "$output"
    contains ":3: [tool-label]" "$output"
    contains ":4: [tool-label]" "$output"
    contains ":5: [tool-label]" "$output"
}

@test "flags a T<n>, SELECT_TOOL or ACTIVATE_EXTRUDER command built from a head choice" {
    run_gate cmd 'void f(int tool) {
    api->execute_gcode("T" + std::to_string(tool));
    const std::string cmd = fmt::format("T{}", tool);
    gcode_line = "SELECT_TOOL T=" + std::to_string(tool);
    run("ACTIVATE_EXTRUDER EXTRUDER=" + name);
    std::string gcode = helix::ui::tool_label(tool);
}'
    [ "$status" -eq 1 ]
    contains ":2: [head-command]" "$output"
    contains ":3: [head-command]" "$output"
    contains ":4: [head-command]" "$output"
    contains ":5: [head-command]" "$output"
    contains ":6: [head-command]" "$output"
}

@test "a statement split over lines reports the flagged line and honors a token on any line" {
    run_gate multi 'void f(int t) {
    api->execute_gcode(
        "T" + std::to_string(t));
}'
    [ "$status" -eq 1 ]
    contains ":3: [head-command]" "$output"
    run_gate multi_ok 'void f(int t) {
    api->execute_gcode(
        "T" + std::to_string(t)); // HEAD_LABEL_OK: a lane macro
}'
    [ "$status" -eq 0 ] || fail "$output"
}

# --- idioms that must stay quiet ----------------------------------------------

@test "an annotated line, on the line or on the comment line above, passes" {
    run_gate annotated 'void f(const ToolInfo& t, int tool) {
    auto a = t.display_label; // HEAD_LABEL_OK: a lane badge
    // HEAD_LABEL_OK: the toolchanger mounts head n
    api->execute_gcode("SELECT_TOOL T=" + std::to_string(tool));
}'
    [ "$status" -eq 0 ] || fail "$output"
}

@test "an annotation does not leak onto the next statement" {
    run_gate leak 'void f(int tool) {
    auto a = fmt::format("T{}", tool); // HEAD_LABEL_OK: lane macro
    api->execute_gcode(fmt::format("T{}", tool));
}'
    [ "$status" -eq 1 ]
    contains ":3: [head-command]" "$output"
    [[ "$output" != *":2: [head-command]"* ]]
}

@test "lane badges, slot labels and plain tool_label of a lane index pass" {
    run_gate lanes 'void f(const LaneInfo& lane, int slot) {
    auto a = helix::ui::tool_label(lane.tool);
    auto b = active_tool_lane_label(noun, slot);
    auto c = active_tool_label(tool_label(slot), slot, form);
    auto d = active_head_name_label(tools.head_identity_for_tool(tools.active_tool_index()));
    title = nozzle_display_name(name, info.display_name);
}'
    [ "$status" -eq 0 ] || fail "$output"
}

@test "a head_select_gcode implementation may build the command, in a header or a source" {
    run_gate impl 'std::string AmsBackendAfc::head_select_gcode(int tool) const {
    if (toolchanger_) {
        return "AFC_SELECT_TOOL TOOL=" + extruder_for(tool);
    }
    return "T" + std::to_string(tool);
}
class X {
    std::string head_select_gcode(int tool) const override { return "SELECT_TOOL T=" + std::to_string(tool); }
};'
    [ "$status" -eq 0 ] || fail "$output"
}

@test "the code after a head_select_gcode body is scanned again" {
    run_gate after 'std::string B::head_select_gcode(int tool) const {
    return "T" + std::to_string(tool);
}
void g(int tool) { api->execute_gcode("T" + std::to_string(tool)); }'
    [ "$status" -eq 1 ]
    contains ":4: [head-command]" "$output"
}

@test "spdlog diagnostics, messages and firmware status keys pass" {
    run_gate quiet 'void f(int tool) {
    spdlog::info("[X] sending T{} via gcode, SELECT_TOOL T={}", tool,
                 tool_label(head.tool));
    auto key = "T" + std::to_string(tool);
    auto msg = fmt::format(lv_tr("Sliced for T{}"), tool);
    auto name = tool_label(tool);
}'
    [ "$status" -eq 0 ] || fail "$output"
}

@test "comments and string text that only mention the shapes pass" {
    run_gate comments '// returns t.display_label, never "T" + std::to_string(n) for SELECT_TOOL
/* api->execute_gcode("T{}", head.tool); tool_label(head) */
const char* kHelp = "use display_label, not SELECT_TOOL";
void f() {}'
    [ "$status" -eq 0 ] || fail "$output"
}

@test "an assignment to display_label is the producer, not a read" {
    run_gate producer 'void f(ToolInfo& t) {
    t.display_label = noun_tool_label(t.name, t.index);
    if (t.display_label == other) {}
}'
    [ "$status" -eq 1 ]
    contains ":3: [display-label]" "$output"
    [[ "$output" != *":2:"* ]]
}

# --- scope and wiring ---------------------------------------------------------

@test "only .cpp and .h files named on the command line are scanned" {
    printf 'auto a = t.display_label;\n' > "$FIXTURE_DIR/notes.txt"
    run python3 "$GATE" "$FIXTURE_DIR/notes.txt"
    [ "$status" -eq 0 ]
}

@test "the pre-commit gate runs it, on the staged blobs at commit and the tree at push" {
    run grep -q 'QC_ALL=.*qc_head_labels' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
    run grep -c 'check_head_labels.py' scripts/qc/head_labels.sh
    [ "$output" -ge 1 ]
    run grep -c -- '--staged-only' scripts/qc/head_labels.sh
    [ "$output" -ge 1 ]
    run qc_trigger head_labels
    [ "$status" -eq 0 ]
    contains 'src/(ui|printer)' "$output"
    contains 'check_head_labels' "$output"
}

@test "--staged-only scans the staged blob, not the working tree" {
    local repo="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$repo/src/ui" "$repo/scripts"
    cp scripts/check_head_labels.py scripts/source_strip.py scripts/staged_content.py "$repo/scripts/"
    git -C "$repo" init -q
    printf 'auto a = t.display_label;\n' > "$repo/src/ui/x.cpp"
    git -C "$repo" add src/ui/x.cpp
    printf 'auto a = 1;\n' > "$repo/src/ui/x.cpp"
    run bash -c "cd '$repo' && python3 scripts/check_head_labels.py --staged-only"
    [ "$status" -eq 1 ]
    contains "src/ui/x.cpp:1: [display-label]" "$output"
}

@test "a token in a wrapped comment block directly above the statement counts" {
    run_gate wrapped 'void f(int tool) {
    // HEAD_LABEL_OK: a long reason that clang-format wrapped
    // onto a second line
    api->execute_gcode("T" + std::to_string(tool));
}'
    [ "$status" -eq 0 ] || fail "$output"
    run_gate gap 'void f(int tool) {
    // HEAD_LABEL_OK: not adjacent
    int x = 1;
    api->execute_gcode("T" + std::to_string(tool));
}'
    [ "$status" -eq 1 ]
}
