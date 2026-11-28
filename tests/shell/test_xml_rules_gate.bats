#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The table of scripts/check_xml_rules.py and its gate, qc_xml_rules
# (scripts/qc/xml_rules.sh): which rules a run selects, that a run opens each
# ui_xml file once, what a commit's gate reads, and what the gate
# prints and exits with. What each rule finds is pinned in its own file,
# tests/shell/test_<rule>_gate.bats.

load helpers

RULES=(header-actions overlay-width icon-names responsive-token-scope modal-chrome-budget shipped-spacing-tokens)
PASS_LINES=(
    "✅ Every header button is a header_action"
    "✅ No hand-picked overlay widths"
    "✅ Every icon name resolves to a registered codepoint"
    "✅ Responsive tokens are all top-level"
    "✅ Modal chrome budget: every pinned block is accounted for"
    "none declared only in dev-panel XML"
)

# A clean tree: ui_xml/panel.xml and every other input the six rules read.
clean_tree() {
    ROOT="$BATS_TEST_TMPDIR/tree"
    rm -rf "$ROOT"
    mkdir -p "$ROOT/ui_xml" "$ROOT/include" "$ROOT/mk" "$ROOT/src" "$ROOT/scripts"
    cat > "$ROOT/include/ui_icon_codepoints.h" <<'HDR'
static const IconEntry kIconMap[] = {
    {"alert",        "\xF3\xB0\x80\xA6"},
};
HDR
    printf 'DEV_PANEL_XML := dev_panel.xml\n' > "$ROOT/mk/cross.mk"
    printf '%s\n' '<component><view><icon src="alert"/></view></component>' > "$ROOT/ui_xml/panel.xml"
    printf '%s\n' '<component><consts><px name="gizmo_small" value="4"/></consts></component>' > "$ROOT/ui_xml/globals.xml"
    printf '%s\n' 'int32_t g = theme_manager_get_spacing("gizmo");' > "$ROOT/src/gizmo.cpp"
    # The gate runs `python3 scripts/check_xml_rules.py` from the tree it checks.
    cp "$REPO/scripts/check_xml_rules.py" "$ROOT/scripts/"
}

# Puts one violation of the rule named into the clean tree.
offender() {
    case "$1" in
        header-actions)
            printf '%s\n' '<component><view><header_bar-actions><ui_button name="btn"/></header_bar-actions></view></component>' > "$ROOT/ui_xml/screen.xml" ;;
        overlay-width)
            printf '%s\n' '<component><view name="x" extends="overlay_panel" width="#overlay_width_destination"/></component>' > "$ROOT/ui_xml/overlay.xml" ;;
        icon-names)
            printf '%s\n' '<component><view><icon src="no_such_icon"/></view></component>' > "$ROOT/ui_xml/panel.xml" ;;
        responsive-token-scope)
            mkdir -p "$ROOT/ui_xml/components"
            printf '%s\n' '<component><consts><px name="card_gutter_small" value="8"/></consts></component>' > "$ROOT/ui_xml/components/card.xml" ;;
        modal-chrome-budget)
            printf '%s\n' '<component><view name="bad_modal" height="content" style_max_height="85%">' \
                '<lv_obj name="content" height="content" style_max_height="#dialog_content_max"/>' \
                '<lv_obj name="pinned_graphic" height="content"/><divider_horizontal/>' \
                '<lv_obj height="#button_height" flex_flow="row"/></view></component>' > "$ROOT/ui_xml/bad_modal.xml" ;;
        shipped-spacing-tokens)
            printf '%s\n' '<component><consts><px name="ghost" value="1"/></consts></component>' > "$ROOT/ui_xml/dev_panel.xml"
            printf '%s\n' 'int32_t o = theme_manager_get_spacing("ghost");' > "$ROOT/src/ghost.cpp" ;;
    esac
}

# A line the failure of the rule named prints.
finding_line() {
    case "$1" in
        header-actions) echo "<ui_button> in <header_bar-actions>" ;;
        overlay-width) echo "overlay_width_destination" ;;
        icon-names) echo "1 icon name(s) are not in ui_icon_codepoints.h" ;;
        responsive-token-scope) echo "card_gutter_small" ;;
        modal-chrome-budget) echo "❌ Modal chrome budget: 1 violation(s)" ;;
        shipped-spacing-tokens) echo '<px name="ghost"> is declared only in dev_panel.xml' ;;
    esac
}

setup() {
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    QC_DIR="$BATS_TEST_TMPDIR/qc"
    mkdir -p "$QC_DIR"
    clean_tree
    CHECKER="$REPO/scripts/check_xml_rules.py"
}

# Runs the real qc_xml_rules with the tree given as the one it checks.
# $2 is the commit's STAGED_ONLY.
run_gate() {
    run bash -c 'cd "$1" && export PYTHONPATH="$3/scripts" && STAGED_ONLY="$2" QC_TMP="$4" &&
                 . "$3/scripts/qc/_lib.sh" && . "$3/scripts/qc/xml_rules.sh" && qc_xml_rules' \
        _ "$1" "$2" "$REPO" "$QC_DIR"
}

@test "--rule runs the rule named and no other" {
    offender header-actions
    offender overlay-width
    run python3 "$CHECKER" --root "$ROOT" --rule overlay-width
    [ "$status" -eq 1 ] || fail "$output"
    contains "$(finding_line overlay-width)" "$output"
    lacks "$(finding_line header-actions)" "$output"
    run python3 "$CHECKER" --root "$ROOT" --rule header-actions
    [ "$status" -eq 1 ] || fail "$output"
    contains "$(finding_line header-actions)" "$output"
    lacks "$(finding_line overlay-width)" "$output"
    lacks "Checking " "$output"
}

@test "with no --rule every rule runs and prints its pass line" {
    local line
    run python3 "$CHECKER" --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
    for line in "${PASS_LINES[@]}"; do
        contains "$line" "$output" || fail "a rule did not print its pass line"
    done
    lacks "Run:" "$output"
}

@test "a failure in any one rule fails the run and names that rule alone" {
    local rule other
    for rule in "${RULES[@]}"; do
        clean_tree
        offender "$rule"
        run python3 "$CHECKER" --root "$ROOT"
        [ "$status" -eq 1 ] || fail "$rule: expected exit 1, got $status: $output"
        contains "$(finding_line "$rule")" "$output" || fail "$rule did not report its finding"
        contains "Run: python3 scripts/check_xml_rules.py --rule $rule --list" "$output" || fail "$rule is not named"
        for other in "${RULES[@]}"; do
            [ "$other" != "$rule" ] || continue
            lacks "Run: python3 scripts/check_xml_rules.py --rule $other --list" "$output" ||
                fail "$rule's run also named $other"
        done
    done
}

@test "an icon scan that matches nothing is exit 2, alone and in the table" {
    printf '%s\n' '<component><view/></component>' > "$ROOT/ui_xml/panel.xml"
    run python3 "$CHECKER" --root "$ROOT" --rule icon-names
    [ "$status" -eq 2 ] || fail "alone: expected exit 2, got $status: $output"
    contains "No <icon> tags found at all" "$output"
    run python3 "$CHECKER" --root "$ROOT"
    [ "$status" -eq 2 ] || fail "table: expected exit 2, got $status: $output"
    contains "Run: python3 scripts/check_xml_rules.py --rule icon-names --list" "$output"
}

@test "--list marks every scanned icon reference and keeps the verdict" {
    printf '%s\n' '<component><view><icon src="alert"/><icon src="no_such_icon"/></view></component>' > "$ROOT/ui_xml/panel.xml"
    run python3 "$CHECKER" --root "$ROOT" --rule icon-names --list
    [ "$status" -eq 1 ] || fail "$output"
    contains "BAD no_such_icon" "$output"
    contains "ok  alert" "$output"
    contains "$(finding_line icon-names)" "$output"
}

@test "FILE arguments are refused for a rule that does not take them" {
    run python3 "$CHECKER" --root "$ROOT" --rule icon-names "$ROOT/ui_xml/panel.xml"
    [ "$status" -eq 2 ] || fail "expected exit 2, got $status: $output"
    contains "FILE arguments do not apply to rule(s): icon-names" "$output"
}

@test "an element a malformed file leaves open is not judged at its end" {
    # A slot holding four actions and an action with both a callback and an
    # event_cb, none of them closed: the parse error is the only finding.
    printf '%s\n' '<component><view><header_bar-actions>' \
        '<header_action icon="a" callback="a" role="primary"/>' \
        '<header_action icon="b" callback="b" role="primary"/>' \
        '<header_action icon="c" callback="c">' \
        '<event_cb trigger="clicked" callback="c"/>' > "$ROOT/ui_xml/screen.xml"
    run python3 "$CHECKER" --root "$ROOT" --rule header-actions
    [ "$status" -eq 1 ] || fail "$output"
    contains "not parseable XML (no element found: line 6, column 0)" "$output"
    [ "$(grep -c '^error:' <<<"$output")" -eq 1 ] || fail "$output"
    lacks "warning:" "$output"
}

@test "--src given twice reports each call under its own directory" {
    local dir
    for dir in a b; do
        mkdir -p "$ROOT/$dir"
        printf '%s\n' 'int32_t o = theme_manager_get_spacing("ghost");' > "$ROOT/$dir/caller.cpp"
    done
    offender shipped-spacing-tokens
    run python3 "$CHECKER" --root "$ROOT" --rule shipped-spacing-tokens --src a --src b
    [ "$status" -eq 1 ] || fail "$output"
    contains "$(finding_line shipped-spacing-tokens)" "$output"
    contains 'read as theme_manager_get_spacing("ghost") at a/caller.cpp:1' "$output"
    contains 'read as theme_manager_get_spacing("ghost") at b/caller.cpp:1' "$output"
}

@test "a run opens each ui_xml file once" {
    local dir opens total
    mkdir -p "$ROOT/ui_xml/components/nested" "$ROOT/ui_xml/translations" "$ROOT/ui_xml/.worktrees/peer"
    for dir in components components/nested translations .worktrees/peer; do
        printf '%s\n' '<component><view><icon src="alert"/></view></component>' > "$ROOT/ui_xml/$dir/card.xml"
    done
    printf '%s\n' '<component><view><icon src="alert"/></view></component>' > "$ROOT/ui_xml/header_action.xml"
    opens="$BATS_TEST_TMPDIR/opens"
    export HELIX_XML_RULES_TRACE_READS="$opens"
    run python3 "$CHECKER" --root "$ROOT"
    [ "$status" -eq 0 ] || fail "$output"
    total=$(find "$ROOT/ui_xml" -name '*.xml' | wc -l)
    [ "$(wc -l < "$opens")" -eq "$total" ] || fail "opened $(wc -l < "$opens") file(s) for $total under ui_xml/"
    [ "$(sort -u "$opens" | wc -l)" -eq "$total" ] || fail "opened twice: $(sort "$opens" | uniq -d)"
}

@test "two rules reading one named file open it once" {
    local opens
    mkdir -p "$ROOT/ui_xml/components"
    printf '%s\n' '<component><view name="x" extends="overlay_panel" width="#overlay_width_destination"/></component>' \
        > "$ROOT/ui_xml/components/card.xml"
    opens="$BATS_TEST_TMPDIR/opens"
    run bash -c 'cd "$1" && HELIX_XML_RULES_TRACE_READS="$2" python3 "$3" --rule overlay-width --rule responsive-token-scope ui_xml/components/card.xml' \
        _ "$ROOT" "$opens" "$CHECKER"
    [ "$status" -eq 1 ] || fail "$output"
    contains "$(finding_line overlay-width)" "$output"
    [ "$(wc -l < "$opens")" -eq 1 ] || fail "expected one open, got: $(cat "$opens")"
}

# A commit stages what it will contain; the working tree can hold anything.
# The gate's commit mode answers from the index, for the rules it runs.
staged_repo() {
    local repo="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$repo/ui_xml" "$repo/include" "$repo/mk" "$repo/scripts"
    cp "$ROOT/include/ui_icon_codepoints.h" "$repo/include/"
    cp "$ROOT/mk/cross.mk" "$repo/mk/"
    cp "$CHECKER" "$repo/scripts/"
    git -C "$repo" init -q
    git -C "$repo" config user.email t@t
    git -C "$repo" config user.name t
    printf '%s\n' "$1" > "$repo/ui_xml/overlay.xml"
    # Everything the gate writes from the index at a commit has to be in it.
    git -C "$repo" add -A
    printf '%s\n' "$2" > "$repo/ui_xml/overlay.xml"
}

CLEAN_OVERLAY='<component><view name="x" extends="overlay_panel" title="X"><icon src="alert"/></view></component>'
BAD_OVERLAY='<component><view name="x" extends="overlay_panel" width="#overlay_width_destination"><icon src="alert"/></view></component>'
HEADER_OFFENDER='<component><view><header_bar-actions><ui_button name="btn"/></header_bar-actions></view></component>'

@test "at a commit the gate reads the XML as the commit will hold it, and leaves shipped-spacing-tokens to the full sweep" {
    local REPO_TREE="$BATS_TEST_TMPDIR/repo"
    run staged_repo "$BAD_OVERLAY" "$CLEAN_OVERLAY"
    [ "$status" -eq 0 ] || fail "$output"
    run_gate "$REPO_TREE" true
    [ "$status" -eq 1 ] || fail "a staged violation passed: $output"
    contains "Run: python3 scripts/check_xml_rules.py --rule overlay-width --list" "$output"
    lacks "Run: python3 scripts/check_xml_rules.py --rule header-actions --list" "$output"

    run staged_repo "$CLEAN_OVERLAY" "$BAD_OVERLAY"
    [ "$status" -eq 0 ] || fail "$output"
    run_gate "$REPO_TREE" true
    [ "$status" -eq 0 ] || fail "a violation that is only on disk reached overlay-width: $output"
    lacks "Run:" "$output"

    # header-actions answers from the index at a commit, whatever the working copy holds.
    printf '%s\n' "$CLEAN_OVERLAY" > "$REPO_TREE/ui_xml/overlay.xml"
    printf '%s\n' "$HEADER_OFFENDER" > "$REPO_TREE/ui_xml/panel.xml"
    git -C "$REPO_TREE" add ui_xml/panel.xml
    printf '%s\n' '<component><view><icon src="alert"/></view></component>' > "$REPO_TREE/ui_xml/panel.xml"
    run_gate "$REPO_TREE" true
    [ "$status" -eq 1 ] || fail "header-actions did not read the staged content: $output"
    contains "Run: python3 scripts/check_xml_rules.py --rule header-actions --list" "$output"
    lacks "Run: python3 scripts/check_xml_rules.py --rule overlay-width --list" "$output"

    # The rule whose callers are every C++ file is not a commit's.
    lacks "Checking spacing tokens ship in a release" "$output"
}

@test "qc_xml_rules prints each rule's pass line and passes" {
    local line
    run_gate "$ROOT" false
    [ "$status" -eq 0 ] || fail "$output"
    for line in "${PASS_LINES[@]}"; do
        contains "$line" "$output" || fail "the gate did not print a rule's pass line"
    done
    lacks "Run:" "$output"
}

@test "qc_xml_rules fails when one rule fails, with its finding and Run hint" {
    offender modal-chrome-budget
    run_gate "$ROOT" false
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    contains "$(finding_line modal-chrome-budget)" "$output"
    contains "Run: python3 scripts/check_xml_rules.py --rule modal-chrome-budget --list" "$output"
    contains "✅ No hand-picked overlay widths" "$output"
    # The hint's command runs, and shows the finding.
    run python3 "$CHECKER" --root "$ROOT" --rule modal-chrome-budget --list
    [ "$status" -eq 1 ] || fail "$output"
    contains "$(finding_line modal-chrome-budget)" "$output"
}

@test "the driver runs qc_xml_rules on every commit, and none of the gates it replaces" {
    eval "$(grep '^QC_ALL=' scripts/quality-checks.sh)"
    contains "qc_xml_rules" "$QC_ALL"
    lacks "qc_header_actions" "$QC_ALL"
    lacks "qc_overlay_width" "$QC_ALL"
    lacks "qc_icon_names" "$QC_ALL"
    [ -z "$(qc_trigger xml_rules)" ] || fail "qc_xml_rules has a trigger: a commit that stages no XML must run it"
    lacks "qc_xml_rules" "$(grep '^QC_PUSH_ONLY=' scripts/quality-checks.sh)"
    for fn in $QC_ALL; do
        [ -f "scripts/qc/${fn#qc_}.sh" ] || fail "QC_ALL names $fn, which has no wrapper"
    done
}
