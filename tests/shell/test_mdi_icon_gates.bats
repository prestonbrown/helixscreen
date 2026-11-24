#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/verify_mdi_codepoints.py (a codepoint's label must be
# the name MDI gives it) and scripts/validate_icon_fonts.sh (every required
# glyph is compiled into the font, every XML icon is defined, kIconMap is
# sorted). Each copies the script into a scratch tree beside a stub table
# generator, so the verdict is about the fixture, never the repo.

load helpers

setup() {
    REPO="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    ROOT="$BATS_TEST_TMPDIR/tree"
    mkdir -p "$ROOT/scripts" "$ROOT/assets/fonts" "$ROOT/include" "$ROOT/ui_xml" \
             "$ROOT/firmware/helixscreen-esp32/components/helixcore/fonts"
    cp "$REPO/scripts/verify_mdi_codepoints.py" "$REPO/scripts/validate_icon_fonts.sh" "$ROOT/scripts/"
}

# table <line>...: the icon table the stub mdi_icon_codepoints.py prints.
table() {
    {
        printf 'import sys\n'
        printf 'if sys.argv[1:] != ["--table"]: sys.exit(2)\n'
        for line in "$@"; do printf 'print(%s)\n' "'$line'"; done
    } > "$ROOT/scripts/mdi_icon_codepoints.py"
}

# mdi_cache <codepoint=name>...: the metadata cache the verifier reads.
mdi_cache() {
    python3 - "$ROOT/assets/mdi-icon-metadata.json.gz" "$@" <<'PY'
import gzip, json, sys
entries = [dict(zip(("codepoint", "name"), a.split("=", 1))) for a in sys.argv[2:]]
with gzip.open(sys.argv[1], "wt", encoding="utf-8") as f:
    json.dump(entries, f)
PY
}

verify() { run python3 "$ROOT/scripts/verify_mdi_codepoints.py"; }

@test "verify: a label that is the MDI name, or a prefix of it, passes" {
    table '0xF0001 home' '0xF0002 wifi (signal)'
    mdi_cache F0001=home F0002=wifi-strength-1
    verify
    [ "$status" -eq 0 ]
    contains "2 icons verified" "$output"
}

@test "verify: a label naming a different icon is a mismatch and says where the name lives" {
    table '0xF0001 cog'
    mdi_cache F0001=home F0002=cog
    verify
    [ "$status" -eq 1 ]
    contains "NAME MISMATCH" "$output"
    contains "Actually is:  home" "$output"
    contains "is at 0xF0002" "$output"
}

@test "verify: a codepoint MDI does not define is reported" {
    table '0xF0009 home'
    mdi_cache F0001=home
    verify
    [ "$status" -eq 1 ]
    contains "INVALID CODEPOINT" "$output"
}

@test "verify: the custom block is not MDI's to verify" {
    table '0xFE001 anything-at-all'
    mdi_cache F0001=home
    verify
    [ "$status" -eq 0 ]
}

@test "verify: a missing metadata cache exits 2" {
    table '0xF0001 home'
    verify
    [ "$status" -eq 2 ]
}

# ---------------------------------------------------------------- validate_icon_fonts.sh

# font <path> <codepoint>...: a generated font whose header lists its glyph range.
font() {
    local path="$1"; shift
    { printf '/*\n * range'; for cp in "$@"; do printf ' 0x%s,' "$cp"; done; printf '\n */\n'; } > "$ROOT/$path"
}

both_fonts() {
    font assets/fonts/mdi_icons_32.c "$@"
    font firmware/helixscreen-esp32/components/helixcore/fonts/mdi_icons_32.c "$@"
}

icon_map() {
    {
        printf 'static const Entry kIconMap[] = {\n'
        for name in "$@"; do printf '    {"%s", "\\xF3\\xB0\\x80\\x81"},\n' "$name"; done
        printf '};\n'
    } > "$ROOT/include/ui_icon_codepoints.h"
}

validate() { run bash -c 'cd "$1" && bash scripts/validate_icon_fonts.sh' _ "$ROOT"; }

@test "validate: fonts carrying every required glyph, defined XML icons and a sorted map pass" {
    table '0xF0001 home' '0xF0002 cog'
    both_fonts F0001 F0002
    icon_map cog home
    printf '<lv_obj><icon icon="home"/></lv_obj>\n' > "$ROOT/ui_xml/a.xml"
    validate
    [ "$status" -eq 0 ]
    contains "kIconMap is correctly sorted" "$output"
}

@test "validate: a required glyph missing from a font fails and names the codepoint" {
    table '0xF0001 home' '0xF0002 cog'
    both_fonts F0001
    icon_map cog home
    validate
    [ "$status" -eq 1 ]
    contains "MISSING FROM assets/fonts/mdi_icons_32.c" "$output"
    contains "0xF0002 (cog)" "$output"
}

@test "validate: a glyph missing only from the firmware twin is caught too" {
    table '0xF0001 home' '0xF0002 cog'
    font assets/fonts/mdi_icons_32.c F0001 F0002
    font firmware/helixscreen-esp32/components/helixcore/fonts/mdi_icons_32.c F0001
    icon_map cog home
    validate
    [ "$status" -eq 1 ]
    contains "MISSING FROM firmware/" "$output"
}

@test "validate: an XML icon the codepoint table does not define fails" {
    table '0xF0001 home'
    both_fonts F0001
    icon_map home
    printf '<lv_obj><icon icon="ghost"/><icon icon="home"/><icon icon="$dyn"/></lv_obj>\n' > "$ROOT/ui_xml/a.xml"
    validate
    [ "$status" -eq 1 ]
    contains "UNDEFINED ICONS (1)" "$output"
    contains "ghost (used in: a.xml)" "$output"
}

@test "validate: an undefined icon used by several files lists them comma-separated" {
    table '0xF0001 home'
    both_fonts F0001
    icon_map home
    printf '<icon icon="ghost"/>\n' > "$ROOT/ui_xml/a.xml"
    printf '<icon icon="ghost"/>\n' > "$ROOT/ui_xml/b.xml"
    validate
    contains "ghost (used in: a.xml, b.xml)" "$output"
}

@test "validate: an icon in a commented-out XML line or a *_icon attribute is not a use" {
    table '0xF0001 home'
    both_fonts F0001
    icon_map home
    printf '%s\n' '<!-- <icon icon="ghost"/> -->' '<x hide_action_button_icon="true"/>' > "$ROOT/ui_xml/a.xml"
    validate
    [ "$status" -eq 0 ]
}

@test "validate: an unsorted kIconMap fails and names the first out-of-order entry" {
    table '0xF0001 home'
    both_fonts F0001
    icon_map home cog
    validate
    [ "$status" -eq 1 ]
    contains "not alphabetically sorted" "$output"
    contains "'cog' should come before 'home'" "$output"
}

@test "validate: a missing font file is a script error, exit 2" {
    table '0xF0001 home'
    icon_map home
    validate
    [ "$status" -eq 2 ]
}
