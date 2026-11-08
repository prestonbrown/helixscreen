#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_x11_macro_collisions.py: a C++ name that an
# <X11/X.h> object-like macro would rewrite (`Foo::None`, `enum { None }`) in a
# file preprocessed after an SDL/X11 include. The include walk is order-aware,
# so the same `::None` is fatal after <SDL.h> and fine before it; that ordering
# and the platform-guard exception are what keep the gate from crying wolf.

GATE="scripts/check_x11_macro_collisions.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    load helpers
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"
    mkdir -p "$WORK/src" "$WORK/include"
    cat > "$WORK/include/scope.h" <<'EOF'
#pragma once
enum class Scope { Nothing, Solid };
EOF
}

gate() {
    run python3 "$REPO_ROOT/$GATE" --repo-root "$WORK" --list "$@"
}

# ------------------------------------------------ shapes that must be CAUGHT

@test "flags a qualified None after an SDL include in the same unit" {
    cat > "$WORK/src/render.cpp" <<'EOF'
#include "scope.h"
#include <SDL.h>
void f() { auto s = Scope::None; }
EOF
    gate
    [ "$status" -eq 1 ]
    contains "src/render.cpp:3: [qualified] \`None\`" "$output"
}

@test "flags an enumerator declared in a header pulled in after the root" {
    cat > "$WORK/include/late.h" <<'EOF'
#pragma once
enum class Late {
    None,
    Some,
};
EOF
    cat > "$WORK/src/app.cpp" <<'EOF'
#include <X11/Xlib.h>
#include "late.h"
EOF
    gate
    [ "$status" -eq 1 ]
    contains "include/late.h:3: [enumerator] \`None\`" "$output"
}

@test "a header exposed through one unit is exposed for all of them" {
    printf '#pragma once\ninline int g() { return Kind::Success; }\n' > "$WORK/include/kind.h"
    printf '#include "kind.h"\n' > "$WORK/src/plain.cpp"
    printf '#include <SDL2/SDL.h>\n#include "kind.h"\n' > "$WORK/src/sdl.cpp"
    gate
    [ "$status" -eq 1 ]
    contains "include/kind.h:2: [qualified] \`Success\`" "$output"
}

# ------------------------------------------------ idioms that must stay QUIET

@test "the same name before the SDL include is correct code" {
    cat > "$WORK/src/render.cpp" <<'EOF'
void early() { auto s = Scope::None; }
#include <SDL.h>
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "a unit that never reaches SDL or X11 is not scanned" {
    printf '#include "scope.h"\nvoid f() { auto s = Scope::None; }\n' > "$WORK/src/plain.cpp"
    gate
    [ "$status" -eq 0 ]
}

@test "comments, strings, other casings and bare uses stay quiet" {
    cat > "$WORK/src/render.cpp" <<'EOF'
#include <SDL.h>
// Scope::None used to be the name here
const char* s = "Scope::None";
int none = 0;
auto t = Scope::none;
int Success = 1;
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "an SDL include under an Android-only guard does not expose the unit" {
    cat > "$WORK/src/globals.cpp" <<'EOF'
#ifdef __ANDROID__
#include <SDL.h>
#endif
void f() { auto s = Scope::None; }
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "the opt-out on the line or up to three above passes" {
    cat > "$WORK/src/render.cpp" <<'EOF'
#include <SDL.h>
// X11_MACRO_OK: this platform's SDL has no X11 backend
void f() {
    auto s = Scope::None;
}
EOF
    gate
    [ "$status" -eq 0 ]
}

@test "quality-checks runs the gate from qc_decl_ui" {
    grep -q 'python3 scripts/check_x11_macro_collisions.py --max-allowed 0' scripts/qc/decl_ui.sh
}
