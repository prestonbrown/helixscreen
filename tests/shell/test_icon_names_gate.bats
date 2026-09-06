#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_icon_names.py — the icon-name resolution gate.
#
# `<icon src="NAME">` resolves NAME through ui_icon::lookup_codepoint() at
# runtime. A name absent from include/ui_icon_codepoints.h does not fail the
# build and does not abort the panel: the widget substitutes
# image_broken_variant and logs one warning, so the user sees a broken-image
# glyph beside real text with nothing to distinguish it from a deliberate one.
#
# The sibling text="#icon_name" path cannot fail this way — gen_icon_consts.py
# generates those consts from the same header, so an unknown name is a missing
# const and the XML fails loudly. src= has no generator behind it.
#
# These tests pin both halves. The silent half matters as much as the loud one:
# a name arriving as `$param` or `#const` is not resolvable from the XML alone,
# and a gate that guessed at those would fire on every component that takes an
# icon as a prop — which is most of them — and would get switched off.

GATE="scripts/check_icon_names.py"

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE="${BATS_TEST_TMPDIR:-$(mktemp -d)}/icon_names"
    mkdir -p "$FIXTURE/ui_xml" "$FIXTURE/include"
    cat > "$FIXTURE/include/ui_icon_codepoints.h" <<'HDR'
static const IconEntry ICON_MAP[] = {
    {"alert",        "\xF3\xB0\x80\xA6"},
    {"check_circle", "\xF3\xB0\x97\xA1"},
    {"info",         "\xF3\xB0\x8B\xBC"},
};
HDR
}

# Write $2 to ui_xml/$1.xml in the fixture tree, then run the gate over it.
write_xml() {
    printf '%s\n' "$2" > "$FIXTURE/ui_xml/$1.xml"
}

run_gate() {
    run python3 "$GATE" --root "$FIXTURE" --summary
}

# ------------------------------------------------ the shapes that ship broken

@test "flags an unregistered name on a direct <icon src>" {
    write_xml panel '<component><view><icon src="alert_triangle" size="lg"/></view></component>'
    run_gate
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    echo "$output" | grep -q "alert_triangle" || fail "did not name the bad icon: $output"
    echo "$output" | grep -q "panel.xml" || fail "did not name the file: $output"
}

@test "flags an unregistered name in a <prop> default feeding an icon" {
    write_xml widget '<component>
  <api><prop name="src" type="string" default="not_an_icon"/></api>
  <view><icon src="$src"/></view>
</component>'
    run_gate
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    echo "$output" | grep -q "not_an_icon" || fail "missed the prop default: $output"
}

@test "flags an unregistered literal handed to an icon prop at an instance site" {
    write_xml widget '<component>
  <api><prop name="src" type="string" default="alert"/></api>
  <view><icon src="$src"/></view>
</component>'
    write_xml page '<component><view><widget src="bogus_name"/></view></component>'
    run_gate
    [ "$status" -eq 1 ] || fail "expected exit 1, got $status: $output"
    echo "$output" | grep -q "bogus_name" || fail "missed the instance site: $output"
}

# ------------------------------------------------------------ must stay quiet

@test "passes when every name is registered" {
    write_xml panel '<component><view>
  <icon src="alert"/><icon src="check_circle"/><icon src="info"/>
</view></component>'
    run_gate
    [ "$status" -eq 0 ] || fail "expected exit 0, got $status: $output"
}

@test "ignores names that arrive as \$param or #const" {
    write_xml panel '<component><view>
  <icon src="$icon"/><icon src="${row_icon}"/><icon src="#icon_default"/>
  <icon src="alert"/>
</view></component>'
    run_gate
    [ "$status" -eq 0 ] || fail "unresolvable forms must not be reported: $output"
}

# -------------------------------------------- the corpus assertion (drift)
#
# Once the tree is clean this gate finds nothing forever, at which point a
# drifted matcher and a healthy tree produce identical output. The corpus count
# is the only thing that separates them, so an empty scan is a hard error and
# NOT a pass.

@test "reports the corpus size so a silent scan is visible" {
    write_xml panel '<component><view><icon src="alert"/></view></component>'
    run_gate
    [ "$status" -eq 0 ] || fail "expected exit 0, got $status: $output"
    echo "$output" | grep -Eq "Scanned [1-9][0-9]* icon references" \
        || fail "no non-zero corpus count in output: $output"
}

@test "fails with exit 2 when the scan matches nothing at all" {
    write_xml panel '<component><view><lv_label text="no icons here"/></view></component>'
    run_gate
    [ "$status" -eq 2 ] || fail "empty corpus must exit 2, not $status: $output"
    echo "$output" | grep -q "no longer matches" || fail "did not explain the drift: $output"
}
