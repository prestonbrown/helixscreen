#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_orphan_callbacks.py: an XML event callback and
# its C++ registration must agree. Each finding kind is proven to be reported
# (a gate that misses one reads green forever), and the forms that must stay
# silent are pinned too.

load helpers

GATE="scripts/check_orphan_callbacks.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src" "$ROOT/include" "$ROOT/ui_xml"
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT" --list
}

@test "an XML callback nothing registers is reported" {
    echo '<component><view><lv_button><event_cb trigger="clicked" callback="on_ghost"/></lv_button></view></component>' \
        > "$ROOT/ui_xml/demo.xml"
    run_gate
    contains "unregistered:on_ghost" "$output"
}

@test "a registered name no XML uses is reported" {
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void reg() { lv_xml_register_event_cb(nullptr, "on_dead", on_dead); }
CPP
    run_gate
    contains "unreferenced:on_dead" "$output"
}

@test "a name registered from two files is reported" {
    echo '<component><view><x callback="on_twice"/></view></component>' > "$ROOT/ui_xml/demo.xml"
    cat > "$ROOT/src/a.cpp" <<'CPP'
void a() { register_xml_callbacks({{"on_twice", cb_a}}); }
CPP
    cat > "$ROOT/src/b.cpp" <<'CPP'
void b() {
    register_xml_callbacks({
        {"on_twice", [](lv_event_t* e) { handle(e, 1); }},
    });
}
CPP
    run_gate
    contains "duplicate:on_twice" "$output"
}

@test "a matched pair, a forwarded \$param and a lambda table entry stay silent" {
    cat > "$ROOT/ui_xml/demo.xml" <<'XML'
<component><view>
  <x callback="on_live"/>
  <y primary_callback="on_lambda"/>
  <z callback="$callback"/>
</view></component>
XML
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void reg() {
    register_xml_callbacks({
        {"on_live", on_live},
        {"on_lambda", [](lv_event_t* e) { (void)e; }},
    });
}
CPP
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" != *on_* ]]
}

@test "the baseline fails a new finding and passes accepted debt" {
    echo '<component><view><x callback="on_ghost"/></view></component>' > "$ROOT/ui_xml/demo.xml"
    echo "unregistered:on_ghost" > "$ROOT/baseline.txt"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 0 ]
    echo '<component><view><x callback="on_other"/></view></component>' > "$ROOT/ui_xml/more.xml"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 1 ]
    contains "unregistered:on_other" "$output"
}

@test "a new unreferenced name only warns; a new unregistered one beside it still fails" {
    : > "$ROOT/baseline.txt"
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void reg() { lv_xml_register_event_cb(nullptr, "on_dead", on_dead); }
CPP
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 3 ] || fail "a dead registration did not warn: $status $output"
    contains "⚠️  Orphan callbacks: 1 registered name(s) no XML uses" "$output"
    contains "unreferenced:on_dead" "$output"
    echo '<component><view><x callback="on_ghost"/></view></component>' > "$ROOT/ui_xml/demo.xml"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 1 ]
    contains "❌ Orphan callbacks: 1 not in" "$output"
    contains "unregistered:on_ghost" "$output"
    contains "unreferenced:on_dead" "$output"
}

qc_orphans() { # <staged-only>
    run bash -c '
        STAGED_ONLY=$3; QC_TMP="$(mktemp -d)"; QC_COUNTS="$QC_TMP/counts"
        . "$1/scripts/qc/_lib.sh"
        . "$1/scripts/qc/orphan_callbacks.sh"
        cd "$2" && mkdir -p scripts && ln -sf "$1/scripts/check_orphan_callbacks.py" \
            "$1/scripts/check_required_names.py" "$1/scripts/source_strip.py" "$1/scripts/ratchet.py" scripts/
        qc_orphan_callbacks' _ "$BATS_TEST_DIRNAME/../.." "$ROOT" "$1"
}

@test "the qc wrapper at commit reads the index, so unstaged XML cannot fail it" {
    git -C "$ROOT" init -q
    mkdir -p "$ROOT/scripts"
    : > "$ROOT/scripts/orphan_callback_baseline.txt"
    echo '<component><view><x callback="on_live"/></view></component>' > "$ROOT/ui_xml/demo.xml"
    echo 'void reg() { register_xml_callbacks({{"on_live", on_live}}); }' > "$ROOT/src/demo.cpp"
    git -C "$ROOT" add ui_xml src scripts/orphan_callback_baseline.txt
    echo '<component><view><x callback="on_ghost"/></view></component>' > "$ROOT/ui_xml/wip.xml"
    qc_orphans true
    [ "$status" -eq 0 ] || fail "unstaged WIP failed the commit: $output"
    qc_orphans false
    [ "$status" -eq 1 ]
    contains "unregistered:on_ghost" "$output"
}

@test "an accepted finding that went away asks to drop it and passes, even at a push: a bookkeeping ratchet" {
    echo '<component><view><x callback="on_ghost"/></view></component>' > "$ROOT/ui_xml/demo.xml"
    printf 'unregistered:on_ghost\nunregistered:on_fixed\n' > "$ROOT/baseline.txt"
    run env HELIX_RATCHET_STRICT=0 python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 0 ] || fail "a fixed finding failed by default: $output"
    contains "hold the gain" "$output"
    run env HELIX_RATCHET_STRICT=1 python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 0 ] || fail "a fixed finding failed at a push: $output"
    contains "on_fixed" "$output"
}

@test "a name handed to a component's callback prop from C++ counts as referenced" {
    echo '<component><view><x callback="$click_callback"/></view></component>' > "$ROOT/ui_xml/demo.xml"
    cat > "$ROOT/src/demo.cpp" <<'CPP'
void reg() { lv_xml_register_event_cb(nullptr, "on_prop_fed", on_prop_fed); }
void make() {
    const char* attrs[] = {"click_callback", "on_prop_fed", nullptr};
    lv_xml_create(parent, "demo", attrs);
}
CPP
    run_gate
    lacks "on_prop_fed" "$output"
    sed -i 's/"click_callback", "on_prop_fed"/"label", "x"/' "$ROOT/src/demo.cpp"
    run_gate
    contains "unreferenced:on_prop_fed" "$output"
}
