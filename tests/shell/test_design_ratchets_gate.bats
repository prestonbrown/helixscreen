#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/design_ratchets.py and its qc wrapper, scripts/qc/design_ratchets.sh.
#
# The architecture ratchets warn instead of failing (prestonbrown/helixscreen#2004), and the
# warning is only worth having if it names the rule, why it exists, where it is written down
# and the exact new site, and never fires on a site the change did not add. Each rule is
# driven here through a scratch repository that carries this tree's scripts, so the checker
# each rule calls is the real one.

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE GIT_PREFIX
    SRC="$PWD"
    REPO="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$REPO/scripts/qc" "$REPO/src/ui" "$REPO/src/printer" "$REPO/include" "$REPO/ui_xml"
    cp scripts/*.py "$REPO/scripts/"
    cp scripts/qc/_lib.sh scripts/qc/design_ratchets.sh "$REPO/scripts/qc/"
    # lvgl96_rename.py reads the deprecated names from LVGL's own headers.
    mkdir -p "$REPO/lib" && ln -s "$SRC/lib/lvgl" "$REPO/lib/lvgl"
    git -C "$REPO" init -q -b main
    git -C "$REPO" config user.email t@example.com
    git -C "$REPO" config user.name t
    printf 'namespace helix {\nvoid nav();\n}\n' > "$REPO/include/ui_nav.h"
    printf 'namespace helix {\nvoid nav() {}\n}\n' > "$REPO/src/ui/ui_nav.cpp"
    printf 'namespace helix {\nint ready = 1;\n}\n' > "$REPO/src/printer/state.cpp"
    printf 'class OldGlobal { int x; };\n' > "$REPO/src/printer/old.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
    BASE_SHA=$(git -C "$REPO" rev-parse --short=9 HEAD)
    git -C "$REPO" checkout -qb feature
}

ratchets() { # <mode...>
    run bash -c 'cd "$1" && shift && python3 scripts/design_ratchets.py "$@"' _ "$REPO" "$@"
}

stage() { # <path> <text>
    printf '%s\n' "$2" >> "$REPO/$1"
    git -C "$REPO" add "$1"
}

@test "a staged global declaration warns with the rule, why, its doc and the new file:line" {
    stage src/printer/state.cpp 'class NewGlobal { int y; };'
    ratchets --staged
    [ "$status" -eq 3 ] || fail "$output"
    contains "Design ratchet: namespace grew by 1" "$output"
    contains "Why the rule exists:" "$output"
    contains "docs/devel/DEVELOPMENT.md" "$output"
    contains "src/printer/state.cpp:4:" "$output"
    contains "Ratchet: <rule>: <reason>" "$output"
}

@test "a site the change did not add stays quiet, even in a file it touched" {
    stage src/printer/old.cpp 'namespace helix { int touched = 0; }'
    ratchets --staged
    [ "$status" -eq 0 ] || fail "an old site warned: $output"
    contains "no new sites" "$output"
    lacks "OldGlobal" "$output"
}

@test "an upward include names the header, its layer and the includer's" {
    stage src/printer/state.cpp '#include "ui_nav.h"'
    ratchets --staged
    [ "$status" -eq 3 ] || fail "$output"
    contains "Design ratchet: layering grew by 1" "$output"
    contains "src/printer/state.cpp:4: include/ui_nav.h (src/ui, ui) from state" "$output"
}

@test "a vendor branch, a work marker, a deprecated LVGL call and a raw widget member each warn" {
    stage src/ui/panel.cpp 'namespace helix {
bool is_cfs(AmsType t) { return t == AmsType::CFS; }
// TODO make this generic
void hide(lv_obj_t* o) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
class Panel {
    lv_obj_t* label_ = nullptr;
};
}'
    ratchets --staged
    [ "$status" -eq 3 ] || fail "$output"
    contains "vendor-branches grew by 1" "$output"
    contains "src/ui/panel.cpp:2: amstype-branch" "$output"
    contains "todo-markers grew by 1" "$output"
    contains "src/ui/panel.cpp:3: // TODO" "$output"
    contains "lvgl-deprecated grew by 1" "$output"
    contains "src/ui/panel.cpp:4: lv_obj_add_flag" "$output"
    contains "raw-widget-pointers grew by 1" "$output"
    contains "src/ui/panel.cpp:6: lv_obj_t* label_" "$output"
}

@test "a commit SHA cited in a new comment warns as comment-sha" {
    stage src/printer/state.cpp "// see $BASE_SHA for the reason"
    ratchets --staged
    [ "$status" -eq 3 ] || fail "$output"
    contains "comment-sha grew by 1" "$output"
    contains "src/printer/state.cpp:4: $BASE_SHA" "$output"
}

@test "a new subject nobody reads warns as orphan-subjects, at its registration" {
    stage src/printer/state.cpp 'void init() { lv_xml_register_subject(nullptr, "lonely_subject", &lonely_); }'
    ratchets --staged
    [ "$status" -eq 3 ] || fail "$output"
    contains "orphan-subjects grew by 1" "$output"
    contains 'src/printer/state.cpp:4: subject "lonely_subject"' "$output"
}

@test "a hand-rolled overlay guard and a hand-drawn XML swatch each warn" {
    stage src/ui/overlay.cpp 'void Overlay::create() { spdlog::warn("overlay already exists"); }'
    stage ui_xml/card.xml '<component><view extends="lv_obj"><lv_obj name="color_swatch" style_radius="9999"/></view></component>'
    ratchets --staged
    [ "$status" -eq 3 ] || fail "$output"
    contains "overlay-boilerplate grew by 1" "$output"
    contains "src/ui/overlay.cpp:1: already_exists" "$output"
    contains "hand-rolled-swatch grew by 1" "$output"
    contains "ui_xml/card.xml:1:" "$output"
}

@test "at a push the branch's added lines are judged, and a stated reason is shown beside them" {
    printf 'class BranchGlobal { int z; };\n' >> "$REPO/src/printer/state.cpp"
    git -C "$REPO" commit -qam "add a global" -m "Ratchet: namespace: the C API needs a global symbol"
    ratchets --base main
    [ "$status" -eq 3 ] || fail "$output"
    contains "namespace grew by 1" "$output"
    contains "src/printer/state.cpp:4: [type] class BranchGlobal" "$output"
    contains "Reason stated on this branch: the C API needs a global symbol" "$output"
}

@test "a branch that adds no site passes quietly" {
    printf 'namespace helix { int more = 2; }\n' >> "$REPO/src/printer/state.cpp"
    git -C "$REPO" commit -qam "fine"
    ratchets --base main
    [ "$status" -eq 0 ] || fail "$output"
    contains "no new sites" "$output"
}

# --- the qc wrapper and the driver -----------------------------------------------

qc_design() { # <staged-only>
    run bash -c '
        STAGED_ONLY="$2"; QC_TMP="$(mktemp -d)"; export QC_TMP
        . scripts/qc/_lib.sh; . scripts/qc/design_ratchets.sh
        cd "$1" && qc_design_ratchets; rc=$?
        echo "--- note:"; cat "$QC_TMP/design_ratchets.note" 2>/dev/null; exit $rc' _ "$REPO" "$1"
}

@test "the wrapper passes a warning and hands the blocks to the driver as a note" {
    stage src/printer/state.cpp 'class NewGlobal { int y; };'
    qc_design true
    [ "$status" -eq 0 ] || fail "a design warning failed the commit: $output"
    contains "repeated below the gate list" "$output"
    contains $'--- note:\n⚠️' "$output"
    contains "src/printer/state.cpp:4:" "$output"
}

@test "the wrapper fails when the change cannot be measured" {
    local bare="$BATS_TEST_TMPDIR/bare"
    mkdir -p "$bare/scripts/qc"
    cp scripts/*.py "$bare/scripts/"
    run bash -c '
        STAGED_ONLY=false; QC_TMP="$(mktemp -d)"
        . scripts/qc/_lib.sh; . scripts/qc/design_ratchets.sh
        cd "$1" && GIT_CEILING_DIRECTORIES="$(dirname "$1")" qc_design_ratchets' _ "$bare"
    [ "$status" -eq 1 ] || fail "an unmeasurable change passed: $output"
    contains "cannot measure the change" "$output"
}

@test "the driver runs the gate at commit and prints its note beside the verdict" {
    eval "$(grep '^QC_ALL=' scripts/quality-checks.sh)"
    eval "$(grep '^QC_PUSH_ONLY=' scripts/quality-checks.sh)"
    [[ " $QC_ALL " == *" qc_design_ratchets "* ]] || fail "not in QC_ALL"
    [[ " $QC_PUSH_ONLY " != *" qc_design_ratchets "* ]] || fail "deferred to push"
    run grep -c 'cat "$QC_TMP/design_ratchets.note"' scripts/quality-checks.sh
    [ "$output" = 1 ] || fail "the driver does not repeat the note"
}

@test "the gate wakes on sources, XML and the checkers it calls, not on docs" {
    local re
    re="$(qc_trigger design_ratchets)"
    for p in src/ui/x.cpp include/x.h ui_xml/x.xml scripts/install.sh scripts/check_layering.py \
        scripts/design_ratchets.py scripts/lvgl96_rename.py; do
        [[ "$p" =~ $re ]] || fail "$p does not wake the gate"
    done
    [[ ! "docs/devel/ARCHITECTURE.md" =~ $re ]] || fail "a doc wakes the gate"
}

@test "every rule names a doc that exists" {
    run python3 - <<'PY'
import re, sys
from pathlib import Path
sys.path.insert(0, "scripts")
import design_ratchets as d
for r in d.RULES:
    for path in re.findall(r"[\w./-]+\.(?:md|h)", r.doc):
        assert Path(path).is_file(), f"{r.name}: {path}"
PY
    [ "$status" -eq 0 ] || fail "$output"
}
