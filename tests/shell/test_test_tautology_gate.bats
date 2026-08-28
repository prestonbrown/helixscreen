#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the two source-level test-quality gates:
#   scripts/check_test_tautology.py   assertions that cannot fail
#   scripts/check_test_mirrors.py     signal 3, a test redefining shipped code
#
# Both are narrow on purpose. The broad versions of these rules fire on good
# tests: name-collision alone yields 109 mirror findings on this tree (mostly a
# helper sharing a name with an unrelated class method), and a set/assert pair
# without the trivial-accessor filter yields 37 (34 of them against setters that
# validate or clamp, where the round-trip IS the behaviour under test).
#
# So the quiet half below is the more important half. Each "is NOT flagged"
# case is a shape that a looser gate reported and that a reviewer confirmed was
# a real test.

TAUT="scripts/check_test_tautology.py"
MIRROR="scripts/check_test_mirrors.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    rm -rf "$WORK"; mkdir -p "$WORK/scripts" "$WORK/include" "$WORK/src" "$WORK/tests/unit"
    cp "$REPO_ROOT/$TAUT" "$REPO_ROOT/$MIRROR" "$WORK/scripts/"

    cat > "$WORK/include/led.h" <<'EOF'
#pragma once
class LedController {
  public:
    void set_last_brightness(int v) { last_brightness_ = v; }
    int last_brightness() const { return last_brightness_; }
    void set_target(int v) { target_ = clamp(v); notify_changed(); }
    int target() const { return target_; }
  private:
    int last_brightness_ = 0;
    int target_ = 0;
};
EOF
    cat > "$WORK/src/version.cpp" <<'EOF'
#include "version.h"
std::string strip_version_prefix(const std::string& v) { return v; }
EOF
    cat > "$WORK/include/version.h" <<'EOF'
#pragma once
#include <string>
std::string strip_version_prefix(const std::string& v);
EOF
}

taut() { (cd "$WORK" && python3 scripts/check_test_tautology.py --list); }
mirror() { (cd "$WORK" && python3 scripts/check_test_mirrors.py --list); }

# ------------------------------------------------- tautology: catch half

@test "set-then-assert through a trivial accessor pair is flagged" {
    cat > "$WORK/tests/unit/test_a.cpp" <<'EOF'
#include "led.h"
TEST_CASE("brightness") {
    LedController ctrl;
    ctrl.set_last_brightness(75);
    REQUIRE(ctrl.last_brightness() == 75);
}
EOF
    run taut
    [[ "$output" == *"set-then-assert"* ]]
}

@test "an expectation produced by the call under test, with nothing between, is flagged" {
    cat > "$WORK/tests/unit/test_b.cpp" <<'EOF'
#include "version.h"
TEST_CASE("v") {
    const auto expected = strip_version_prefix(input);
    REQUIRE(strip_version_prefix(input) == expected);
}
EOF
    run taut
    [[ "$output" == *"self-fulfilling"* ]]
}

# ------------------------------------------------- tautology: quiet half

@test "set-then-assert is NOT flagged when the setter does something" {
    # set_target() clamps and notifies, so the round-trip is the behaviour.
    cat > "$WORK/tests/unit/test_c.cpp" <<'EOF'
#include "led.h"
TEST_CASE("target") {
    LedController ctrl;
    ctrl.set_target(75);
    REQUIRE(ctrl.target() == 75);
}
EOF
    run taut
    [[ "$output" != *"set-then-assert"* ]]
}

@test "an invariance test with the operation in between is NOT flagged" {
    # `auto before = ...; op(); REQUIRE(... == before)` is the common good shape.
    cat > "$WORK/tests/unit/test_d.cpp" <<'EOF'
#include "version.h"
TEST_CASE("stable") {
    const auto before = counter.value();
    reconnect();
    REQUIRE(counter.value() == before);
}
EOF
    run taut
    [[ "$output" != *"self-fulfilling"* ]]
}

@test "f() == f() is NOT flagged" {
    # A deliberate determinism/cache-stability assertion in this tree.
    cat > "$WORK/tests/unit/test_e.cpp" <<'EOF'
#include "version.h"
TEST_CASE("cached") {
    CHECK(updates_externally_managed() == updates_externally_managed());
}
EOF
    run taut
    [[ "$output" != *"self-fulfilling"* ]]
}

@test "the opt-out annotation silences a finding" {
    cat > "$WORK/tests/unit/test_f.cpp" <<'EOF'
#include "led.h"
TEST_CASE("brightness") {
    LedController ctrl;
    ctrl.set_last_brightness(75);
    // TEST_TAUTOLOGY_OK: pinning that the field survives a row rebuild
    REQUIRE(ctrl.last_brightness() == 75);
}
EOF
    run taut
    [[ "$output" != *"set-then-assert"* ]]
}

# ------------------------------------------------- mirror signal 3: catch

@test "a test redefining a shipped free function it asserts on is flagged" {
    cat > "$WORK/tests/unit/test_g.cpp" <<'EOF'
#include "version.h"
std::string strip_version_prefix(const std::string& v) { return v.substr(1); }
TEST_CASE("strip") {
    REQUIRE(strip_version_prefix("v1.2") == "1.2");
}
EOF
    run mirror
    [[ "$output" == *"redefined-symbol"* ]]
    [[ "$output" == *"strip_version_prefix"* ]]
}

# ------------------------------------------------- mirror signal 3: quiet

@test "an indented helper inside a fixture is NOT flagged" {
    cat > "$WORK/tests/unit/test_h.cpp" <<'EOF'
#include "version.h"
struct Fixture {
    std::string strip_version_prefix(const std::string& v) { return v; }
};
TEST_CASE("x") { REQUIRE(strip_version_prefix("v1") == "1"); }
EOF
    run mirror
    [[ "$output" != *"redefined-symbol"* ]]
}

@test "a stub that no assertion uses is NOT flagged" {
    # Scaffolding to satisfy the linker is not a mirror under test.
    cat > "$WORK/tests/unit/test_i.cpp" <<'EOF'
#include "version.h"
std::string strip_version_prefix(const std::string& v) { return v; }
TEST_CASE("x") { REQUIRE(other_thing() == 1); }
EOF
    run mirror
    [[ "$output" != *"redefined-symbol"* ]]
}

@test "a name matching only a class method is NOT flagged" {
    cat > "$WORK/tests/unit/test_j.cpp" <<'EOF'
#include "led.h"
int last_brightness() { return 3; }
TEST_CASE("x") { REQUIRE(last_brightness() == 3); }
EOF
    run mirror
    [[ "$output" != *"redefined-symbol"* ]]
}

@test "the TEST_MIRROR_OK annotation silences signal 3" {
    cat > "$WORK/tests/unit/test_k.cpp" <<'EOF'
#include "version.h"
// TEST_MIRROR_OK: exercises a patched submodule with no header of ours
std::string strip_version_prefix(const std::string& v) { return v.substr(1); }
TEST_CASE("strip") { REQUIRE(strip_version_prefix("v1.2") == "1.2"); }
EOF
    run mirror
    [[ "$output" != *"redefined-symbol"* ]]
}
