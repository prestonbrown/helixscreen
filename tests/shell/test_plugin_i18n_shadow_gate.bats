#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

# The plugin-i18n-shadow gate's two halves: it fires on a pack entry whose
# tag matches a system string (that entry is dead weight: the app's own
# translation always wins), and it stays silent on plugin-only strings the
# catalog does not carry.

setup() {
    load helpers
    GATE="scripts/check_plugin_i18n_shadow.py"
}

@test "plugin i18n shadow: fires on a tag the app catalog already has" {
    tmp=$(mktemp -d)
    mkdir -p "$tmp/i18n"
    cat > "$tmp/i18n/de.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<translations languages="de">
  <translation tag="Start" de="Starten"/>
  <translation tag="a string only this plugin says" de="Nur dieses Plugin"/>
</translations>
EOF
    run python3 "$GATE" --root "$tmp"
    [ "$status" -ne 0 ]
    grep -q "shadows the app catalog" <<<"$output"
    grep -q "'Start'" <<<"$output"
    rm -rf "$tmp"
}

@test "plugin i18n shadow: silent on a plugin-only string" {
    tmp=$(mktemp -d)
    mkdir -p "$tmp/i18n"
    cat > "$tmp/i18n/de.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<translations languages="de">
  <translation tag="a string only this plugin says" de="Nur dieses Plugin"/>
</translations>
EOF
    run python3 "$GATE" --root "$tmp"
    [ "$status" -eq 0 ]
    rm -rf "$tmp"
}

@test "plugin i18n shadow: fires on a catalog key written with a YAML '' escape" {
    tmp=$(mktemp -d)
    mkdir -p "$tmp/i18n"
    cat > "$tmp/i18n/de.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<translations languages="de">
  <translation tag="Failed to connect to '{}': {}" de="x"/>
</translations>
EOF
    run python3 "$GATE" --root "$tmp"
    [ "$status" -ne 0 ]
    grep -q "shadows the app catalog" <<<"$output"
    rm -rf "$tmp"
}

@test "plugin i18n shadow: fires on a catalog key folded across lines" {
    tmp=$(mktemp -d)
    mkdir -p "$tmp/i18n"
    cat > "$tmp/i18n/de.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<translations languages="de">
  <translation tag="Could not check webcam status.&#10;Check printer connection." de="x"/>
</translations>
EOF
    run python3 "$GATE" --root "$tmp"
    [ "$status" -ne 0 ]
    grep -q "shadows the app catalog" <<<"$output"
    rm -rf "$tmp"
}

@test "plugin i18n shadow: fails when it finds no packs to check" {
    tmp=$(mktemp -d)
    run python3 "$GATE" --root "$tmp"
    [ "$status" -ne 0 ]
    grep -q "No plugin translation packs found" <<<"$output"
    rm -rf "$tmp"
}

@test "plugin i18n shadow: reports how many packs it checked" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
    grep -Eq "Checked [1-9][0-9]* plugin translation pack" <<<"$output"
}
