#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_growth_ratchets.py (prestonbrown/helixscreen#1945):
# from its fork point a branch may not grow a C++ file past the size cap, add a
# global accessor call inside a class that already receives that dependency, or
# add a static active-instance pointer. Each ratchet is proven to fail on the
# shape it guards and to stay quiet on the idioms beside it.

load helpers

GATE="$BATS_TEST_DIRNAME/../../scripts/check_growth_ratchets.py"
CAP=1500

setup() {
    REPO="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    mkdir -p "$REPO/src/ui" "$REPO/include" "$REPO/tests/unit"
    git -C "$REPO" init -q -b main
    git -C "$REPO" config user.email t@example.com
    git -C "$REPO" config user.name t
    lines $((CAP + 10)) > "$REPO/src/big.cpp"
    lines 100 > "$REPO/src/small.cpp"
    cat > "$REPO/include/panel.h" <<'EOF'
class Panel {
  public:
    explicit Panel(helix::PrinterState& ps, IMoonrakerAPI* api);
    void refresh();
    static void on_clicked(lv_event_t* e);
    int inline_api() { return api_ != nullptr; }
  private:
    helix::PrinterState& ps_;
    IMoonrakerAPI* api_;
};
class Plain {
  public:
    void refresh();
};
EOF
    cat > "$REPO/src/ui/panel.cpp" <<'EOF'
Panel::Panel(helix::PrinterState& ps, IMoonrakerAPI* api) : ps_(ps), api_(api) {}
void Panel::refresh() {
    ps_.touch();
}
void Panel::on_clicked(lv_event_t* e) {
    (void)e;
}
void Plain::refresh() {
}
static void helper() {
}
EOF
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
    BASE=$(git -C "$REPO" rev-parse HEAD)
    git -C "$REPO" checkout -qb feature
}

lines() {
    local i
    for ((i = 1; i <= $1; i++)); do echo "int v$i;"; done
}

commit_all() {
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "$1" ${2:+-m "$2"}
}

ratchets() {
    run python3 "$GATE" --root "$REPO" --base "$BASE"
}

# Inserts $2 as the first line of the body of the function whose header starts with $1.
inject() {
    python3 - "$REPO/src/ui/panel.cpp" "$1" "$2" <<'EOF'
import sys
path, head, line = sys.argv[1:]
s = open(path).read()
i = s.index("{", s.index(head))
open(path, "w").write(s[:i + 1] + "\n    " + line + s[i + 1:])
EOF
}

# --- file size --------------------------------------------------------------

@test "a new file over the cap fails, one at the cap passes" {
    lines $((CAP + 1)) > "$REPO/src/new_big.cpp"
    lines $CAP > "$REPO/src/new_ok.cpp"
    commit_all "two new files"
    ratchets
    [ "$status" -eq 1 ]
    contains "src/new_big.cpp is over the $CAP-line cap and grew (new file, $((CAP + 1)) lines)" "$output"
    lacks "new_ok.cpp" "$output"
}

@test "an existing file over the cap may shrink but not grow" {
    echo "int more;" >> "$REPO/src/big.cpp"
    commit_all "grow"
    ratchets
    [ "$status" -eq 1 ]
    contains "src/big.cpp is over the $CAP-line cap and grew ($((CAP + 10)) -> $((CAP + 11)) lines)" "$output"
    lines $((CAP + 5)) > "$REPO/src/big.cpp"
    commit_all "shrink"
    ratchets
    [ "$status" -eq 0 ]
}

@test "a file under the cap may not cross it, and a test file counts too" {
    lines $((CAP + 1)) > "$REPO/src/small.cpp"
    lines $((CAP + 1)) > "$REPO/tests/unit/test_big.cpp"
    commit_all "cross"
    ratchets
    [ "$status" -eq 1 ]
    contains "src/small.cpp is over the $CAP-line cap and grew (100 -> $((CAP + 1)) lines)" "$output"
    contains "tests/unit/test_big.cpp" "$output"
}

@test "a renamed oversized file is compared against its old path" {
    git -C "$REPO" mv src/big.cpp src/moved.cpp
    commit_all "rename"
    ratchets
    [ "$status" -eq 0 ]
    echo "int more;" >> "$REPO/src/moved.cpp"
    commit_all "grow it"
    ratchets
    [ "$status" -eq 1 ]
    contains "renamed from src/big.cpp" "$output"
}

@test "a File-size line exempts only the file it names, and needs a reason" {
    echo "int more;" >> "$REPO/src/big.cpp"
    lines $((CAP + 1)) > "$REPO/src/other.cpp"
    commit_all "grow" "File-size: src/big.cpp the fix cannot wait for the split"
    ratchets
    [ "$status" -eq 1 ]
    lacks "src/big.cpp is over" "$output"
    contains "src/other.cpp is over" "$output"
    git -C "$REPO" commit -q --amend -m "grow" -m "File-size: src/big.cpp"
    rm "$REPO/src/other.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "drop other"
    ratchets
    [ "$status" -eq 1 ]
    contains "src/big.cpp is over" "$output"
}

@test "non-C++ files are not measured" {
    lines $((CAP + 1)) > "$REPO/src/table.txt"
    lines $((CAP + 1)) > "$REPO/include/generated.inc"
    commit_all "data"
    ratchets
    [ "$status" -eq 0 ]
}

# --- global access beside an injected dependency ------------------------------

@test "get_printer_state() in a member of a class that receives PrinterState fails" {
    inject "void Panel::refresh()" "get_printer_state().touch();"
    commit_all "reach for the global"
    ratchets
    [ "$status" -eq 1 ]
    contains "src/ui/panel.cpp: get_printer_state() inside a class that receives PrinterState grew 0 -> 1" "$output"
}

@test "get_moonraker_api() in an inline member in the class body fails" {
    sed -i.bak 's/return api_ != nullptr;/return get_moonraker_api() != nullptr;/' "$REPO/include/panel.h"
    rm "$REPO/include/panel.h.bak"
    commit_all "inline global"
    ratchets
    [ "$status" -eq 1 ]
    contains "include/panel.h: get_moonraker_api() inside a class that receives Moonraker API grew 0 -> 1" "$output"
}

@test "a class receives a dependency through an out-of-line method's parameter too" {
    printf 'void Plain::bind(const PrinterState& ps) {\n}\n' >> "$REPO/src/ui/panel.cpp"
    inject "void Plain::refresh()" "get_printer_state().touch();"
    commit_all "plain takes it now"
    ratchets
    [ "$status" -eq 1 ]
    contains "get_printer_state() inside a class that receives PrinterState grew 0 -> 1" "$output"
}

@test "global access in a static member (inline or out-of-line), a free function, a lambda outside the class, a class without the parameter, or a comment is not counted" {
    inject "void Panel::on_clicked(" "get_printer_state().touch();"
    sed -i.bak 's/^    static void on_clicked(lv_event_t\* e);/    static void on_clicked(lv_event_t* e);\n    static void inline_cb() { get_printer_state().touch(); }/' "$REPO/include/panel.h"
    rm "$REPO/include/panel.h.bak"
    grep -q inline_cb "$REPO/include/panel.h"
    inject "static void helper()" "get_moonraker_api(); auto cb = [] { get_printer_state(); };"
    inject "void Plain::refresh()" "get_printer_state().touch(); // not get_moonraker_api()"
    inject "void Panel::refresh()" "// get_printer_state() is the global; spdlog::info(\"get_moonraker_api()\");"
    commit_all "quiet shapes"
    ratchets
    [ "$status" -eq 0 ]
}

@test "moving a global call within the class nets zero per file" {
    inject "void Panel::refresh()" "get_printer_state().touch();"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "base call"
    BASE=$(git -C "$REPO" rev-parse HEAD)
    python3 - "$REPO/src/ui/panel.cpp" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read().replace("\n    get_printer_state().touch();", "", 1)
s = s.replace("Panel::Panel(helix::PrinterState& ps, IMoonrakerAPI* api) : ps_(ps), api_(api) {}",
              "Panel::Panel(helix::PrinterState& ps, IMoonrakerAPI* api) : ps_(ps), api_(api) {\n    get_printer_state();\n}")
open(p, "w").write(s)
EOF
    commit_all "move it"
    ratchets
    [ "$status" -eq 0 ]
}

# --- static active-instance pointers ------------------------------------------

@test "a static active-instance member in a class fails" {
    sed -i.bak 's/^    IMoonrakerAPI\* api_;/    IMoonrakerAPI* api_;\n    static Panel* active_instance_;/' "$REPO/include/panel.h"
    rm "$REPO/include/panel.h.bak"
    commit_all "static pointer"
    ratchets
    [ "$status" -eq 1 ]
    contains "static active-instance pointers grew by 1 (include/panel.h: active_instance_)" "$output"
}

@test "every spelling counts: s_active_, s_active_instance, an anonymous-namespace s_active_ctx, a named s_*_instance" {
    cat > "$REPO/src/ui/statics.cpp" <<'EOF'
namespace {
Ctx* s_active_ctx = nullptr;
}
static helix::Widget* s_active_instance = nullptr;
class Menu {
    static inline Menu* s_active_ = nullptr;
};
static DetailView* s_detail_view_instance = nullptr;
EOF
    commit_all "four statics"
    ratchets
    [ "$status" -eq 1 ]
    contains "grew by 4" "$output"
}

@test "an out-of-line definition, an accessor, a local, a singleton slot and a test file are not counted" {
    cat > "$REPO/src/ui/quiet.cpp" <<'EOF'
Panel* Panel::active_instance_ = nullptr;
static Panel* get_active_instance();
static DisplayManager* s_instance = nullptr;
void f() {
    Panel* active = nullptr;
    return *s_active_;
}
// static Panel* s_active_;
EOF
    printf 'static Panel* s_active_ = nullptr;\n' > "$REPO/tests/unit/test_panel.cpp"
    commit_all "quiet shapes"
    ratchets
    [ "$status" -eq 0 ]
    contains "active-instance pointers +0" "$output"
}

@test "moving an active-instance pointer between files nets zero" {
    printf 'static Panel* s_active_ = nullptr;\n' > "$REPO/src/ui/a.cpp"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm "one static"
    BASE=$(git -C "$REPO" rev-parse HEAD)
    rm "$REPO/src/ui/a.cpp"
    printf 'static Panel* s_active_ = nullptr;\n' > "$REPO/src/ui/b.cpp"
    commit_all "move"
    ratchets
    [ "$status" -eq 0 ]
}

@test "a branch with no commits is not measured" {
    run python3 "$GATE" --root "$REPO" --base HEAD
    [ "$status" -eq 0 ]
    contains "no committed work" "$output"
}

# --- the qc wrapper -------------------------------------------------------------

qc_ratchets() {
    run bash -c '
        STAGED_ONLY=false; QC_TMP="$(mktemp -d)"; QC_COUNTS="$QC_TMP/counts"
        . "$1/scripts/qc/_lib.sh"
        . "$1/scripts/qc/growth_ratchets.sh"
        cd "$2" && mkdir -p scripts && ln -sf "$1/scripts/check_growth_ratchets.py" scripts/ \
            && ln -sf "$1/scripts/diff_base.py" scripts/
        qc_growth_ratchets' _ "$BATS_TEST_DIRNAME/../.." "$REPO"
}

@test "the qc wrapper measures the branch from its fork point and fails on growth" {
    echo "int more;" >> "$REPO/src/big.cpp"
    commit_all "grow"
    qc_ratchets
    [ "$status" -eq 1 ]
    contains "src/big.cpp is over the $CAP-line cap" "$output"
}

@test "the qc wrapper passes a branch that adds none of the shapes" {
    echo "int more;" >> "$REPO/src/small.cpp"
    commit_all "small"
    qc_ratchets
    [ "$status" -eq 0 ]
    contains "Growth ratchets" "$output"
}

@test "the qc wrapper triggers on C++ under tests, src and include and on its script" {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    re="$(qc_trigger growth_ratchets)"
    [ -n "$re" ]
    for p in tests/unit/test_x.cpp src/ui/ui_x.cpp include/x.h scripts/check_growth_ratchets.py; do
        echo "$p" | grep -qE "$re" || fail "$p should trigger"
    done
    for p in docs/devel/README.md tests/shell/test_x.bats ui_xml/globals.xml; do
        if echo "$p" | grep -qE "$re"; then fail "$p should not trigger"; fi
    done
}
