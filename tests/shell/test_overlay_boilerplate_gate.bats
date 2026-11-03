#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_overlay_boilerplate.py: the hand-rolled overlay
# guard string and global-accessor slot only shrink. Each counted shape is
# proven to be counted, comments are proven not to be, and the baseline is
# proven to fail growth and pass a hold.

load helpers

GATE="scripts/check_overlay_boilerplate.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src/ui"
}

@test "an 'already exists' string and a static unique_ptr slot are counted" {
    cat > "$ROOT/src/ui/demo.cpp" <<'CPP'
static std::unique_ptr<Demo> g_demo;
void f() { spdlog::warn("[Demo] create() called but overlay already exists"); }
CPP
    run python3 "$GATE" --repo-root "$ROOT"
    contains "'already_exists': 1" "$output"
    contains "'static_unique': 1" "$output"
}

@test "the same words in a comment are not counted" {
    cat > "$ROOT/src/ui/demo.cpp" <<'CPP'
// static std::unique_ptr<Demo> g_demo; the root already exists
/* static std::unique_ptr<Demo> g_other; */
CPP
    run python3 "$GATE" --repo-root "$ROOT"
    contains "'already_exists': 0" "$output"
    contains "'static_unique': 0" "$output"
}

@test "a user-facing string and a function returning unique_ptr are not counted" {
    cat > "$ROOT/src/ui/demo.cpp" <<'CPP'
static std::unique_ptr<Geometry>
build_geometry(const File& file) { return nullptr; }
void f() { toast(lv_tr("Vendor already exists")); }
CPP
    run python3 "$GATE" --repo-root "$ROOT"
    contains "'already_exists': 0" "$output"
    contains "'static_unique': 0" "$output"
}

@test "the baseline fails growth and passes a hold" {
    printf 'already_exists=1\nstatic_unique=1\n' > "$ROOT/baseline.txt"
    cat > "$ROOT/src/ui/demo.cpp" <<'CPP'
static std::unique_ptr<Demo> g_demo;
void f() { spdlog::warn("overlay already exists"); }
CPP
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 0 ]
    echo 'static std::unique_ptr<Other> g_other;' > "$ROOT/src/ui/more.cpp"
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/baseline.txt"
    [ "$status" -eq 1 ]
    contains "static_unique 1 -> 2" "$output"
}
