#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_theme_artifacts.py: the token table and the
# [theme_pins] golden must move with the ui_xml/*.xml tokens they derive from.
#
# The catch half: a token change committed without `make regen-tokens`, and a
# golden the change provably invalidates (a name added, a name dropped, an
# unsuffixed value the golden records verbatim), each fail with the command
# that fixes them. The quiet half: a regenerated commit, a change the golden
# cannot be shown to depend on (a suffixed variant's value), a golden that is
# part of the commit, names only C++ registers, and unstaged working-tree edits
# all pass.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"
GATE="$REPO_ROOT/scripts/check_theme_artifacts.py"
GEN="$REPO_ROOT/scripts/gen_theme_tokens.py"
GOLDEN="tests/fixtures/theme_const_golden.txt"

setup() {
    FIX="$BATS_TEST_TMPDIR/theme"
    mkdir -p "$FIX/ui_xml/components" "$FIX/tests/fixtures"
    cd "$FIX" || return 1
    git init -q .
    git config user.email t@example.com
    git config user.name tester
    cat > ui_xml/globals.xml <<'XML'
<component><consts>
  <px name="space_md" value="6"/>
  <px name="gap_small" value="4"/>
  <px name="gap_large" value="8"/>
  <color name="accent" value="#112233"/>
</consts></component>
XML
    # Components are not scanned: only top-level files feed either artefact.
    printf '<component><consts><px name="row_h" value="40"/></consts></component>\n' \
        > ui_xml/components/row.xml
    python3 "$GEN" >/dev/null
    # The golden lists XML names, suffix bases, and names only C++ registers.
    cat > "$GOLDEN" <<'TXT'
## 800x480 dark
accent=#112233
gap=8
gap_large=<unset>
gap_small=<unset>
primary=#ff0000
space_md=6
palette:primary=ff0000
TXT
    git add -A
    git commit -qm base --no-verify
}

run_staged() {
    run python3 "$GATE" --staged-only
}

stage_globals() {
    printf '%s\n' "$1" > ui_xml/globals.xml
    git add ui_xml/globals.xml
}

BASE_TOKENS='<px name="space_md" value="6"/>
  <px name="gap_small" value="4"/>
  <px name="gap_large" value="8"/>
  <color name="accent" value="#112233"/>'

@test "an unchanged tree passes" {
    run_staged
    [ "$status" -eq 0 ]
}

@test "a token edit without make regen-tokens fails and names the command" {
    stage_globals "<component><consts>$BASE_TOKENS
  <px name=\"space_md_small\" value=\"4\"/></consts></component>"
    run_staged
    [ "$status" -eq 1 ]
    contains "src/generated/theme_token_table.cpp does not match" "$output"
    contains "make regen-tokens" "$output"
}

@test "a regenerated but unstaged token table still fails: the commit is what CI sees" {
    stage_globals "<component><consts>$BASE_TOKENS
  <px name=\"gap_xlarge\" value=\"12\"/></consts></component>"
    python3 "$GEN" >/dev/null
    run_staged
    [ "$status" -eq 1 ]
    contains "theme_token_table.cpp does not match" "$output"
}

@test "a new token name the golden lacks fails with the golden command" {
    stage_globals "<component><consts>$BASE_TOKENS
  <px name=\"space_xl\" value=\"16\"/></consts></component>"
    python3 "$GEN" >/dev/null
    git add src/generated/theme_token_table.cpp
    run_staged
    [ "$status" -eq 1 ]
    contains "theme_const_golden.txt is stale for: space_xl" "$output"
    contains 'HELIX_UPDATE_GOLDEN=1 ./build/bin/helix-tests "[theme_pins]"' "$output"
    lacks "theme_token_table.cpp does not match" "$output"
}

@test "a dropped token the golden still lists fails" {
    stage_globals '<component><consts><px name="gap_small" value="4"/>
  <px name="gap_large" value="8"/><color name="accent" value="#112233"/></consts></component>'
    python3 "$GEN" >/dev/null
    git add src/generated/theme_token_table.cpp
    run_staged
    [ "$status" -eq 1 ]
    contains "stale for: space_md" "$output"
}

@test "dropping every variant of a suffixed token drops its base name too" {
    stage_globals '<component><consts><px name="space_md" value="6"/>
  <color name="accent" value="#112233"/></consts></component>'
    python3 "$GEN" >/dev/null
    git add src/generated/theme_token_table.cpp
    run_staged
    [ "$status" -eq 1 ]
    contains "gap" "$output"
    contains "gap_large" "$output"
}

@test "an unsuffixed value the golden records verbatim fails when it changes" {
    stage_globals '<component><consts><px name="space_md" value="10"/>
  <px name="gap_small" value="4"/>
  <px name="gap_large" value="8"/>
  <color name="accent" value="#112233"/></consts></component>'
    python3 "$GEN" >/dev/null
    git add src/generated/theme_token_table.cpp
    run_staged
    [ "$status" -eq 1 ]
    contains "stale for: space_md" "$output"
}

@test "a suffixed variant's value change passes: only the test can resolve it" {
    stage_globals '<component><consts><px name="space_md" value="6"/>
  <px name="gap_small" value="5"/>
  <px name="gap_large" value="8"/>
  <color name="accent" value="#112233"/></consts></component>'
    python3 "$GEN" >/dev/null
    git add src/generated/theme_token_table.cpp
    run_staged
    [ "$status" -eq 0 ]
}

@test "a golden that is part of the commit is trusted" {
    stage_globals '<component><consts><px name="space_md" value="10"/>
  <px name="gap_small" value="4"/>
  <px name="gap_large" value="8"/>
  <color name="accent" value="#112233"/></consts></component>'
    python3 "$GEN" >/dev/null
    sed -i 's/^space_md=6$/space_md=10/' "$GOLDEN"
    git add src/generated/theme_token_table.cpp "$GOLDEN"
    run_staged
    [ "$status" -eq 0 ]
}

@test "a value the golden records for one token only is not confused with another's" {
    # gap_large's HEAD value 8 also appears as gap=8, the resolved base. A
    # change to gap_large is still a suffixed variant the golden lists as unset.
    stage_globals '<component><consts><px name="space_md" value="6"/>
  <px name="gap_small" value="4"/>
  <px name="gap_large" value="9"/>
  <color name="accent" value="#112233"/></consts></component>'
    python3 "$GEN" >/dev/null
    git add src/generated/theme_token_table.cpp
    run_staged
    [ "$status" -eq 0 ]
}

@test "a golden regeneration leaves unchanged has a way out" {
    # primary is a C++ name too: dropping the XML copy changes nothing the
    # golden records, so the golden never joins the commit.
    stage_globals "<component><consts>$BASE_TOKENS
  <color name=\"primary\" value=\"#ff0000\"/></consts></component>"
    python3 "$GEN" >/dev/null
    git add -A
    git commit -qm "xml primary" --no-verify
    stage_globals "<component><consts>$BASE_TOKENS</consts></component>"
    python3 "$GEN" >/dev/null
    git add src/generated/theme_token_table.cpp
    run_staged
    [ "$status" -eq 1 ]
    contains "stale for: primary" "$output"
    contains "HELIX_GOLDEN_CURRENT=1" "$output"
    HELIX_GOLDEN_CURRENT=1 run_staged
    [ "$status" -eq 0 ]
}

@test "a component's consts and unstaged edits never decide the verdict" {
    printf '<component><consts><px name="row_h" value="48"/></consts></component>\n' \
        > ui_xml/components/row.xml
    git add ui_xml/components/row.xml
    printf '<component><consts><px name="wip" value="1"/></consts></component>\n' \
        > ui_xml/peer_wip.xml
    run_staged
    [ "$status" -eq 0 ]
}

@test "the gate triggers on top-level XML and the artefacts, not on components or src" {
    cd "$REPO_ROOT" || return 1
    re="$(qc_trigger theme_artifacts)"
    [ -n "$re" ]
    for p in ui_xml/globals.xml src/generated/theme_token_table.cpp "$GOLDEN" \
             scripts/gen_theme_tokens.py; do
        echo "$p" | grep -qE "$re" || fail "$p should trigger"
    done
    for p in ui_xml/components/row.xml src/ui/theme_manager.cpp docs/devel/README.md; do
        if echo "$p" | grep -qE "$re"; then fail "$p should not trigger"; fi
    done
}
