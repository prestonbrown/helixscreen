#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the helix-xml-linter block in scripts/quality-checks.sh — the
# gate that keeps tools/xml-linter/schema/schema.json in step with the inputs
# `make regen-xml-schema` extracts from.
#
# CI's XML Lint job runs `make lint-xml` against the COMMITTED snapshot, so the
# two ways to lose are (a) committing XML whose #const refs the snapshot does
# not know, and (b) never noticing that a non-XML schema input changed. Both
# halves are pinned here.
#
# The loud half: a snapshot that regeneration actually rewrites is stale, blocks
# the commit, and gets staged for the committer when that is safe.
#
# The quiet half, which matters just as much: a snapshot that regeneration
# leaves byte-identical is CORRECT. It may still differ from the index — someone
# regenerated and has not staged it, which is the normal state of a shared tree
# — and reporting that as "stale" is a false failure on a file the hook
# deliberately refuses to stage, so the committer has no way to clear it. That
# is how a gate ends up bypassed with --no-verify, which is why the distinction
# is tested rather than trusted.
#
# The trigger has the same two halves: a staged src/ui/ widget registration must
# reach the check, and an ordinary src/ commit must not, because paying make's
# startup on every commit is exactly what the content-based trigger avoids.

QC="scripts/quality-checks.sh"

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    FIX="${BATS_TEST_TMPDIR:-$(mktemp -d)}/schema_fix"
    BLOCK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/block.sh"
    extract_block
    build_fixture
}

# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------

# Pull the real linter/staleness block out of quality-checks.sh so these tests
# exercise the shipped code rather than a restatement of it. Aborts loudly if
# the markers move: an empty block would make every assertion below vacuous.
extract_block() {
    awk '/^XML_LINTER_SCHEMA_PATH=/{f=1} f && /^# ={20}/{exit} f' "$REPO_ROOT/$QC" \
        > "$BLOCK"
    [ -s "$BLOCK" ] || fail "could not extract the XML linter block from $QC"
    grep -q 'regen-xml-schema' "$BLOCK" \
        || fail "extracted block has no regen step — markers moved?"
    grep -q 'run_xml_linter' "$BLOCK" \
        || fail "extracted block has no run_xml_linter — markers moved?"
}

# A throwaway git repo shaped like the parts of the tree the block touches:
#   ui_xml/t.xml                   references #probe_token
#   tools/xml-linter/{src,schema}   real linter, controlled snapshot
#   Makefile                       regen-xml-schema copies .regen_source
# base.json is the snapshot WITHOUT probe_token (stale); with_probe.json is the
# regenerated one. Nothing here writes to the real repo.
build_fixture() {
    rm -rf "$FIX"
    mkdir -p "$FIX/ui_xml" "$FIX/tools/xml-linter/schema" "$FIX/src/ui" \
             "$FIX/src/system" "$FIX/assets/config/themes/defaults"
    ln -s "$REPO_ROOT/tools/xml-linter/src" "$FIX/tools/xml-linter/src"

    cat > "$FIX/ui_xml/t.xml" <<'EOF'
<component>
  <view extends="lv_obj" style_pad_all="#probe_token"/>
</component>
EOF

    python3 - "$REPO_ROOT/tools/xml-linter/schema/schema.json" "$FIX" <<'PY'
import json, sys
src, out = sys.argv[1], sys.argv[2]
d = json.load(open(src))
base = sorted(c for c in d["runtime_constants"] if c != "probe_token")
d["runtime_constants"] = base
json.dump(d, open(out + "/base.json", "w"), indent=2)
d["runtime_constants"] = sorted(base + ["probe_token"])
json.dump(d, open(out + "/with_probe.json", "w"), indent=2)
PY

    printf 'regen-xml-schema:\n\t@cp "$$(cat .regen_source)" tools/xml-linter/schema/schema.json\n' \
        > "$FIX/Makefile"

    cat > "$FIX/run_block.sh" <<'EOF'
#!/bin/bash
set -e
EXIT_CODE=0
# shellcheck disable=SC1090
. "$BLOCK_PATH"
echo "BLOCK_EXIT_CODE=$EXIT_CODE"
EOF

    git -C "$FIX" init -q
    git -C "$FIX" config user.email t@example.invalid
    git -C "$FIX" config user.name "Test"
    [ -d "$FIX/.git" ] || fail "fixture repo was not created"
}

# Commit $1 as the snapshot, and point `make regen-xml-schema` at $2.
seed_committed_schema() {
    cp "$FIX/$1" "$FIX/tools/xml-linter/schema/schema.json"
    printf '%s\n' "$2" > "$FIX/.regen_source"
    git -C "$FIX" add ui_xml/t.xml tools/xml-linter/schema/schema.json Makefile
    git -C "$FIX" commit -qm seed
}

# Run the extracted block in the fixture. $1 = AUTO_FIX, $2 = STAGED_ONLY
# (both default true, the pre-commit invocation).
run_block() {
    local auto_fix="${1:-true}" staged_only="${2:-true}"
    run bash -c "cd '$FIX' && BLOCK_PATH='$BLOCK' AUTO_FIX='$auto_fix' \
        STAGED_ONLY='$staged_only' bash run_block.sh"
}

block_exit_code() {
    printf '%s\n' "$output" | sed -n 's/^BLOCK_EXIT_CODE=//p'
}

output_has() {
    printf '%s\n' "$output" | grep -q "$1"
}

schema_is_staged() {
    git -C "$FIX" diff --cached --name-only | grep -qx 'tools/xml-linter/schema/schema.json'
}

# An ordinary widget .cpp, plus the line in $2 when one is given.
write_cpp() {
    local path="$1" extra="${2:-}"
    printf 'void helper_%s(void* p) { use(p); }\n' "$$" > "$FIX/$path"
    if [ -n "$extra" ]; then
        printf '%s\n' "$extra" >> "$FIX/$path"
    fi
    git -C "$FIX" add "$path"
}

WIDGET_REG='static void reg(void) { lv_xml_register_widget("my_widget", my_create, lv_xml_obj_apply); }'

# Stage an XML edit that adds another #probe_token reference — the ordinary
# ui_xml path trigger, and the reason the snapshot would be regenerated.
stage_xml_edit() {
    cat > "$FIX/ui_xml/t.xml" <<'EOF'
<component>
  <view extends="lv_obj" style_pad_all="#probe_token">
    <lv_obj style_pad_top="#probe_token"/>
  </view>
</component>
EOF
    git -C "$FIX" add ui_xml/t.xml
}

# ---------------------------------------------------------------------------
# Staleness predicate — the loud half
# ---------------------------------------------------------------------------

@test "a snapshot regeneration rewrites is reported stale and blocks" {
    # Committed snapshot lacks probe_token, so the first lint fails, regen
    # rewrites the bytes and the second lint passes. Nothing may be staged in
    # CI mode, so it must block.
    seed_committed_schema base.json with_probe.json
    run_block false false

    [ "$(block_exit_code)" = "1" ]
    output_has "was stale"
}

@test "a stale snapshot is regenerated and staged for the committer" {
    # The pre-commit invocation on a clean tree: the snapshot is this commit's
    # own collateral, so staging it is right and the commit proceeds.
    seed_committed_schema base.json with_probe.json
    stage_xml_edit
    run_block true true

    [ "$(block_exit_code)" = "0" ]
    output_has "Regenerated and staged"
    schema_is_staged
}

@test "a stale snapshot carrying unstaged edits is not staged for the committer" {
    # Deliberate: those bytes are someone else's work in progress. It still
    # blocks, because regeneration really did change the file.
    seed_committed_schema base.json with_probe.json
    stage_xml_edit
    printf '\n' >> "$FIX/tools/xml-linter/schema/schema.json"
    run_block true true

    [ "$(block_exit_code)" = "1" ]
    output_has "was stale"
    refute schema_is_staged
}

# ---------------------------------------------------------------------------
# Staleness predicate — the quiet half
# ---------------------------------------------------------------------------

@test "a correct-but-unstaged snapshot is NOT reported as stale" {
    # The live state of a shared tree: someone regenerated and did not stage.
    # The bytes on disk are what the extractor produces, so regeneration is a
    # no-op and there is nothing to fix. Asking the index instead of asking the
    # regen output is what called this stale and failed the commit.
    seed_committed_schema base.json with_probe.json
    cp "$FIX/with_probe.json" "$FIX/tools/xml-linter/schema/schema.json"
    write_cpp src/ui/widget.cpp "$WIDGET_REG"

    run_block true true

    [ "$(block_exit_code)" = "0" ]
    refute output_has "was stale"
    output_has "already up to date but unstaged"
}

@test "a correct-but-unstaged snapshot is not stale on the const-deletion path either" {
    # Same predicate, reached through the trigger that predates the content
    # signals: deleting a <px> from XML forces a regen because a passing lint
    # cannot rule out a dangling ref. On a tree whose snapshot is already
    # regenerated and unstaged — main's normal state — this is the combination
    # that failed the commit with "was stale" on a file that was correct.
    cat > "$FIX/ui_xml/t.xml" <<'EOF'
<component>
  <consts>
    <px name="doomed" value="1"/>
  </consts>
  <view extends="lv_obj" style_pad_all="#probe_token"/>
</component>
EOF
    seed_committed_schema base.json with_probe.json
    cp "$FIX/with_probe.json" "$FIX/tools/xml-linter/schema/schema.json"
    cat > "$FIX/ui_xml/t.xml" <<'EOF'
<component>
  <view extends="lv_obj" style_pad_all="#probe_token"/>
</component>
EOF
    git -C "$FIX" add ui_xml/t.xml

    run_block true true

    [ "$(block_exit_code)" = "0" ]
    refute output_has "was stale"
    output_has "already up to date but unstaged"
}

@test "a correct-but-unstaged snapshot is not staged behind the committer's back" {
    # Same content rule as the stale case: an unstaged snapshot belongs to
    # whichever change regenerated it, not to this commit.
    seed_committed_schema base.json with_probe.json
    cp "$FIX/with_probe.json" "$FIX/tools/xml-linter/schema/schema.json"
    write_cpp src/ui/widget.cpp "$WIDGET_REG"

    run_block true true

    refute schema_is_staged
}

@test "a clean, current snapshot passes with no advice attached" {
    seed_committed_schema with_probe.json with_probe.json
    write_cpp src/ui/widget.cpp "$WIDGET_REG"

    run_block true true

    [ "$(block_exit_code)" = "0" ]
    output_has "helix-xml-linter passed"
    refute output_has "was stale"
    refute output_has "unstaged"
}

# ---------------------------------------------------------------------------
# Trigger — the schema inputs outside ui_xml/
# ---------------------------------------------------------------------------

@test "a staged src/ui/ widget registration reaches the check" {
    # lv_xml_register_widget() in src/ui/ is a schema input
    # (extract_schema.py _auto_discover_cpp_widgets). Before the content
    # trigger such a commit had no local signal at all and CI caught it.
    seed_committed_schema with_probe.json with_probe.json
    write_cpp src/ui/widget.cpp "$WIDGET_REG"

    run_block true true

    refute output_has "No XML linter inputs staged"
    output_has "helix-xml-linter passed"
}

@test "a REMOVED src/ui/ widget registration reaches the check too" {
    # The deletion direction is the one a passing lint cannot rule out: the
    # snapshot still lists a widget nothing registers any more.
    seed_committed_schema with_probe.json with_probe.json
    write_cpp src/ui/widget.cpp "$WIDGET_REG"
    git -C "$FIX" commit -qm "add widget"
    write_cpp src/ui/widget.cpp

    run_block true true

    refute output_has "No XML linter inputs staged"
}

@test "a staged lv_xml_register_const() anywhere under src/ reaches the check" {
    # --cpp-const-dirs is src, not src/ui.
    seed_committed_schema with_probe.json with_probe.json
    write_cpp src/system/theme_bits.cpp \
        'void f(void) { lv_xml_register_const(scope, "probe_token", "4"); }'

    run_block true true

    refute output_has "No XML linter inputs staged"
}

@test "a staged mk/tools.mk reaches the check" {
    # mk/tools.mk owns the extractor's argument list, so editing it can change
    # the schema without any other file moving.
    seed_committed_schema with_probe.json with_probe.json
    mkdir -p "$FIX/mk"
    printf '# extractor args\n' > "$FIX/mk/tools.mk"
    git -C "$FIX" add mk/tools.mk

    run_block true true

    refute output_has "No XML linter inputs staged"
}

@test "a staged theme default reaches the check" {
    seed_committed_schema with_probe.json with_probe.json
    printf '{"dark": {"card_bg": "#101010"}}\n' \
        > "$FIX/assets/config/themes/defaults/probe.json"
    git -C "$FIX" add assets/config/themes/defaults/probe.json

    run_block true true

    refute output_has "No XML linter inputs staged"
}

@test "a staged lib/helix-xml pointer bump reaches the check" {
    # The engine is a gitlink: its files never appear in the superproject index,
    # only the pointer does, so the trigger has to match the gitlink path.
    seed_committed_schema with_probe.json with_probe.json
    mkdir -p "$FIX/lib/helix-xml"
    git -C "$FIX/lib/helix-xml" init -q
    git -C "$FIX/lib/helix-xml" config user.email t@example.invalid
    git -C "$FIX/lib/helix-xml" config user.name "Test"
    printf 'a\n' > "$FIX/lib/helix-xml/f.c"
    git -C "$FIX/lib/helix-xml" add f.c
    git -C "$FIX/lib/helix-xml" commit -qm one
    git -C "$FIX" add lib/helix-xml 2>/dev/null
    git -C "$FIX" commit -qm "add engine pointer"
    printf 'b\n' >> "$FIX/lib/helix-xml/f.c"
    git -C "$FIX/lib/helix-xml" commit -qam two
    git -C "$FIX" add lib/helix-xml 2>/dev/null

    # Guard: without a real gitlink in the index this test proves nothing.
    git -C "$FIX" diff --cached --name-only | grep -qx 'lib/helix-xml'
    run_block true true

    refute output_has "No XML linter inputs staged"
}

# ---------------------------------------------------------------------------
# Trigger — the quiet half, which is what keeps the check affordable
# ---------------------------------------------------------------------------

@test "an ordinary src/ commit does NOT reach the check" {
    # Nearly every commit touches src/. If this fired, every commit would pay
    # make's startup plus two lint passes, and the check would get switched off.
    seed_committed_schema with_probe.json with_probe.json
    write_cpp src/ui/panel.cpp \
        'void PanelFoo::update(int v) { label_set(v); if (v > 2) { redraw(); } }'

    run_block true true

    output_has "No XML linter inputs staged"
}

@test "a src/ file merely USING a registered widget does not reach the check" {
    # The extractor reads registration call sites only, so a use site is not a
    # schema input. A trigger keyed on 'lv_xml' alone would fire on these.
    seed_committed_schema with_probe.json with_probe.json
    write_cpp src/ui/panel.cpp \
        'void build(void) { lv_obj_t* w = lv_xml_create(parent, "my_widget", NULL); use(w); }'

    run_block true true

    output_has "No XML linter inputs staged"
}

@test "a docs-only commit does not reach the check" {
    seed_committed_schema with_probe.json with_probe.json
    printf 'notes\n' > "$FIX/NOTES.md"
    git -C "$FIX" add NOTES.md

    run_block true true

    output_has "No XML linter inputs staged"
}

# ---------------------------------------------------------------------------
# Wiring against the real tree
# ---------------------------------------------------------------------------

# The regex the trigger greps staged diff content with, read out of the hook so
# the tests below cannot drift from it. Empty extraction aborts: an empty -E
# pattern matches everything and would turn the negatives into false passes.
trigger_re() {
    local call="$1" re
    re=$(grep -F "lv_xml_register_$call" "$QC" | grep -F 'grep -qE' \
        | sed "s/.*grep -qE '//; s/'.*//" | head -1)
    [ -n "$re" ] || { echo "no $call trigger regex in $QC" >&2; return 1; }
    printf '%s\n' "$re"
}

@test "the trigger regexes are extractable from the hook" {
    # Guards every assertion below it.
    run trigger_re widget
    [ "$status" -eq 0 ]
    [[ "$output" == *"lv_xml_register_widget"* ]]
    run trigger_re const
    [ "$status" -eq 0 ]
    [[ "$output" == *"lv_xml_register_const"* ]]
}

@test "the trigger regexes match the real registration lines in this tree" {
    # Fed with actual source lines rather than invented ones: if the house style
    # for either call changes, the trigger goes deaf and this says so.
    local wline cline
    wline=$(grep -rhm1 'lv_xml_register_widget(' src/ui --include='*.cpp' | head -1)
    cline=$(grep -rhm1 'lv_xml_register_const(' src --include='*.cpp' | head -1)
    [ -n "$wline" ] && [ -n "$cline" ]

    printf '+%s\n' "$wline" | grep -qE "$(trigger_re widget)"
    printf '+%s\n' "$cline" | grep -qE "$(trigger_re const)"
    # Deletions matter as much as additions.
    printf -- '-%s\n' "$wline" | grep -qE "$(trigger_re widget)"
}

@test "the trigger regexes ignore diff file headers and use sites" {
    local wre cre
    wre=$(trigger_re widget)
    cre=$(trigger_re const)
    refute_sh "printf '+++ b/src/ui/ui_switch.cpp\n' | grep -qE '$wre'"
    refute_sh "printf -- '--- a/src/ui/ui_switch.cpp\n' | grep -qE '$wre'"
    refute_sh "printf '+  lv_obj_t* w = lv_xml_create(p, \"my_widget\", NULL);\n' | grep -qE '$wre'"
    refute_sh "printf '+  auto v = theme_manager_get_color(\"card_bg\");\n' | grep -qE '$cre'"
}

@test "every extractor input in mk/tools.mk has a trigger in the hook" {
    # The trigger set is only correct while it mirrors the extractor's argument
    # list. Adding an input there without a trigger here is exactly the silence
    # this check exists to remove.
    local recipe
    recipe=$(sed -n '/^regen-xml-schema:/,/^$/p' mk/tools.mk)
    [ -n "$recipe" ] || fail "regen-xml-schema recipe not found in mk/tools.mk"
    for input in "lib/helix-xml/src/xml" "--cpp-src src/ui" "--cpp-const-dirs src" \
                 "--xml-roots ui_xml" "--theme-dirs assets/config/themes/defaults"; do
        [[ "$recipe" == *"$input"* ]] \
            || fail "extractor input '$input' changed in mk/tools.mk — update the trigger"
    done

    grep -q 'lib/helix-xml\$' "$QC" || fail "$QC lost the helix-xml gitlink trigger"
    grep -q 'mk/tools\\\.mk\$' "$QC" || fail "$QC lost the mk/tools.mk trigger"
    grep -q 'assets/config/themes/defaults/' "$QC" || fail "$QC lost the theme-defaults trigger"
    grep -q "ui_xml/.*\\\\.xml\\$" "$QC" || fail "$QC lost the ui_xml path trigger"
}

@test "extract_schema.py still parses the calls the trigger keys on" {
    # Derived from the extractor, not guessed: these are the two regexes whose
    # call shapes the hook mirrors.
    grep -q 'lv_xml_register_widget' tools/xml-linter/schema/extract_schema.py \
        || fail "extract_schema.py no longer parses lv_xml_register_widget"
    grep -q 'lv_xml_register_const' tools/xml-linter/schema/extract_schema.py \
        || fail "extract_schema.py no longer parses lv_xml_register_const"
    # .cpp-only globbing is why the trigger's pathspecs end in .cpp.
    grep -q 'rglob("\*\.cpp")' tools/xml-linter/schema/extract_schema.py \
        || fail "extract_schema.py no longer globs *.cpp — widen the trigger pathspecs"
}
