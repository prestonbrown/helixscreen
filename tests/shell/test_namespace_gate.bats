#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_namespace_compliance.py - the ratchet that keeps
# HelixScreen declarations under helix::.
#
# The failure mode it exists for: docs/devel/DEVELOPMENT.md has said "all
# HelixScreen code lives under helix::" since before 1.0, nothing ever checked,
# and a third of the tree drifted out from under it (#1370). The drift runs along
# subsystem lines rather than by age - ams_backend_afc.h is six weeks NEWER than
# printer_state.h and still global - so the global areas keep taking new
# global-scope declarations by local precedent. A documented rule with no gate is
# how that happens quietly.
#
# Both halves are pinned here. The catch half proves a new global declaration
# fails. The quiet half matters just as much: a checker that flagged extern "C",
# file-local statics or LVGL's own types would be noise nobody could ratchet down,
# and the baseline would stall above zero forever.

GATE="scripts/check_namespace_compliance.py"

setup() {
    load helpers
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIX="$BATS_TEST_TMPDIR/fix"
    mkdir -p "$FIX"
}

# Write $2 to a fixture named $1, echo its path.
fixture() {
    printf '%s\n' "$2" > "$FIX/$1"
    echo "$FIX/$1"
}

# --- catch ---

@test "a class at global scope is flagged" {
    f=$(fixture probe.h 'class Widget {
    int x;
};')
    run python3 "$GATE" --list "$f"
    [ "$status" -eq 1 ]
    contains "[type]" "$output"
    [[ "$output" == *"Widget"* ]]
}

@test "a free function at global scope is flagged" {
    f=$(fixture probe.h 'int compute_thing(int a, int b);')
    run python3 "$GATE" --list "$f"
    [[ "$output" == *"[function]"* ]]
}

@test "a declaration under a non-helix root namespace is flagged" {
    f=$(fixture probe.h 'namespace gcode {
class Parser {
    int x;
};
}')
    run python3 "$GATE" --list "$f"
    [[ "$output" == *"[foreign-ns]"* ]]
}

@test "an enum at global scope is flagged" {
    f=$(fixture probe.h 'enum class Mode { A, B };')
    run python3 "$GATE" --list "$f"
    [[ "$output" == *"[type]"* ]]
}

# --- quiet ---

@test "the same class under helix:: is not flagged" {
    f=$(fixture probe.h 'namespace helix {
class Widget {
    int x;
};
}')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "a helix:: sub-namespace is not flagged" {
    f=$(fixture probe.h 'namespace helix::ui {
class Widget {
    int x;
};
}')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "old-style nested helix namespaces are not flagged" {
    # `namespace helix {` + `namespace ui {` un-indented is 798 sites in the tree
    # (#1372). It resolves to helix::ui and must read as compliant, or the gate
    # would flag more than half the compliant code.
    f=$(fixture probe.h 'namespace helix {
namespace ui {
class Widget {
    int x;
};
}
}')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "extern \"C\" blocks are structural, not flagged" {
    f=$(fixture probe.h 'extern "C" {
void helix_c_entry(void);
}')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "a file-local static in a .cpp is internal linkage, not flagged" {
    f=$(fixture probe.cpp 'static int helper(int x) {
    return x;
}')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "the same static in a HEADER is flagged - the name reaches every includer" {
    f=$(fixture probe.h 'static int helper(int x) {
    return x;
}')
    run python3 "$GATE" --list "$f"
    [ "$status" -eq 1 ]
}

@test "an anonymous namespace is internal linkage, not flagged" {
    f=$(fixture probe.cpp 'namespace {
class Helper {
    int x;
};
}')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "a forward declaration of a third-party type keeps its foreign spelling" {
    f=$(fixture probe.h 'struct lv_obj_t;
struct hv_loop_t;')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "a line carrying NAMESPACE_OK is not flagged" {
    f=$(fixture probe.h '// NAMESPACE_OK: C ABI callback signature
class Widget {
    int x;
};')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

@test "a continuation line of a multi-line parameter list is not a declaration" {
    # `const Foo& p = {}) {` closing a wrapped signature parsed as a global
    # variable before the continuation guard existed.
    f=$(fixture probe.h 'namespace helix {
int compute(int a,
            const char* name = "x",
            const int& p = 0);
}')
    run python3 "$GATE" "$f"
    [ "$status" -eq 0 ]
}

# --- the scanner ---

@test "a // inside a string literal does not swallow the rest of the line" {
    # Regression: stripping line comments BEFORE string literals truncated
    #   if (detail.rfind("// ", 0) == 0) {
    # at the quoted slashes, losing its opening brace. Brace depth then drifted
    # for the remainder of the file and function-LOCAL declarations were reported
    # as global ones. Found in src/printer/ams_backend_cfs.cpp, which reported a
    # local `const bool` 3300 lines in as a global variable.
    #
    # The fixture is deliberately OUTSIDE a namespace: inside helix:: the drift is
    # invisible, because everything it touches is exempt anyway. Only at global
    # scope does a lost brace turn a local into a reported violation.
    f=$(fixture probe.cpp 'void parse(const char* detail) {
    if (strncmp(detail, "// ", 3) == 0) {
        int inner_local = 1;
    }
    const bool another_local = true;
}')
    run python3 "$GATE" --list "$f"
    # `parse` itself is a genuine global function and stays flagged.
    contains "[function]" "$output"
    contains "parse" "$output"
    # The two locals must not be, and are the first casualties of brace drift.
    lacks "another_local" "$output"
    [[ "$output" != *"inner_local"* ]]
}

@test "braces inside block comments and raw strings do not drift depth" {
    f=$(fixture probe.cpp 'namespace helix {
/* { { { */
const char* SQL = R"sql(SELECT { FROM t)sql";
void f() {
    int local_one = 1;
}
}')
    run python3 "$GATE" --list "$f"
    [ "$status" -eq 0 ]
    [[ "$output" != *"local_one"* ]]
}

# --- ratchet semantics ---

@test "--max-allowed passes at the baseline and fails above it" {
    f=$(fixture probe.h 'class Widget {
    int x;
};')
    run python3 "$GATE" --max-allowed 1 "$f"
    [ "$status" -eq 0 ]
    run python3 "$GATE" --max-allowed 0 "$f"
    [ "$status" -eq 1 ]
}

@test "coming in under the baseline passes and says to ratchet down" {
    f=$(fixture probe.h 'namespace helix {
class Widget {
    int x;
};
}')
    run python3 "$GATE" --max-allowed 5 "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ratchet the baseline down"* ]]
}

# --- wiring ---
#
# The gate has to run from quality-checks.sh, not only from here: that is what the
# pre-commit hook, the pre-push hook and the Code Quality workflow all call.
# `make test-shell` runs late enough that a gate wired only into this file would
# sit green through every commit that broke it.

@test "gate is wired into quality-checks.sh" {
    run grep -q "check_namespace_compliance.py" scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate section is registered in the quality-checks section list" {
    run grep -q 'QC_ALL=.*qc_namespace' scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "gate wakes on C++ sources" {
    run bash -c "sed -n '/|qc_namespace|/,/;;/p' scripts/quality-checks.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"cpp"* ]]
}

@test "the quality-checks baseline matches the tree" {
    # A baseline that drifted above the real count silently stops ratcheting:
    # the gate would pass while new global declarations accumulate underneath it.
    baseline=$(grep -oE 'check_namespace_compliance.py --max-allowed [0-9]+' scripts/quality-checks.sh | grep -oE '[0-9]+')
    actual=$(python3 "$GATE" --summary | awk '/TOTAL/{print $2}')
    [ -n "$baseline" ]
    [ "$actual" -le "$baseline" ]
}
