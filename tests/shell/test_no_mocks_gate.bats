#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check_no_mocks.py in a scratch repo whose Makefile mirrors what the real
# one does under ENABLE_MOCKS=no: it drops the *_mock source and the define. The
# compiler and nm are real, so a finding comes from an object, as it does in a gate.

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
CHECK="$REPO_ROOT/scripts/check_no_mocks.py"

setup() {
    export JOBPOOL=0
    T="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$T/src" "$T/include"
    cat >"$T/Makefile" <<'MK'
APP_SRCS := $(wildcard src/*.cpp)
ENABLE_MOCKS ?= yes
ifeq ($(ENABLE_MOCKS),yes)
MOCK_DEFINES := -DHELIX_ENABLE_MOCKS
else
APP_SRCS := $(filter-out src/%_mock.cpp,$(APP_SRCS))
endif
print-var-%:
	@echo "$($*)"
print-compile-command:
	@for f in $(F); do echo "c++ -std=c++17 -O2 -g $(MOCK_DEFINES) -Iinclude -c $$f"; done
compile-prereqs:
	@$(PREREQS_RECIPE)
MK
    echo 'int clean() { return 1; }' >"$T/src/clean.cpp"
    git -C "$T" init -q
    git -C "$T" add -A
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m base
    cd "$T" || return 1
}

no_mocks() {
    run python3 "$CHECK" --jobs 2 "$@"
}

@test "an unguarded mock definition fails, naming the file and the symbol" {
    echo 'struct MockWidget { int poke(); }; int MockWidget::poke() { return 1; }' >src/widget.cpp
    no_mocks src/widget.cpp
    [ "$status" -eq 1 ] || fail "$output"
    contains "❌ src/widget.cpp: defines MockWidget::poke()" "$output"
    contains "1 of 1 packaged units carry mock code" "$output"
}

@test "the same definition behind HELIX_ENABLE_MOCKS passes: the packaged command drops the define" {
    cat >src/widget.cpp <<'CPP'
#ifdef HELIX_ENABLE_MOCKS
struct MockWidget { int poke(); }; int MockWidget::poke() { return 1; }
#endif
int widget() { return 2; }
CPP
    no_mocks src/widget.cpp
    [ "$status" -eq 0 ] || fail "$output"
    contains "✓ no-mocks: 1 packaged units carry no mock symbols" "$output"
}

@test "an undefined reference to a mock symbol fails" {
    echo 'struct MockWidget { static int poke(); }; int use() { return MockWidget::poke(); }' >src/user.cpp
    no_mocks src/user.cpp
    [ "$status" -eq 1 ] || fail "$output"
    contains "❌ src/user.cpp: references MockWidget::poke()" "$output"
}

@test "a unit that does not compile with mocks off fails and shows the compiler's error" {
    cat >src/needs.cpp <<'CPP'
#ifdef HELIX_ENABLE_MOCKS
int only_with_mocks();
#endif
int use() { return only_with_mocks(); }
CPP
    no_mocks src/needs.cpp
    [ "$status" -eq 1 ] || fail "$output"
    contains "❌ src/needs.cpp: does not compile with mocks off" "$output"
    contains "only_with_mocks" "$output"
}

@test "a source the packaged build filters out is never compiled" {
    echo 'struct MockWidget { int poke(); }; int MockWidget::poke() { return 1; }' >src/widget_mock.cpp
    no_mocks src/widget_mock.cpp
    [ "$status" -eq 0 ] || fail "$output"
    contains "reaches no packaged unit" "$output"
}

@test "--base selects what changed, and a unit reaching a changed header through another" {
    echo '#include "inner.h"' >include/outer.h
    echo 'int inner();' >include/inner.h
    echo 'int quiet() { return 1; }' >src/quiet.cpp
    printf '#include "outer.h"\nint consumer() { return inner(); }\n' >src/consumer.cpp
    git add -A
    git -c user.email=t@t -c user.name=t commit -q -m more
    cat >include/inner.h <<'H'
struct MockInner { static int value() { return 3; } };
inline int inner() { return MockInner::value(); }
H
    no_mocks --base HEAD
    [ "$status" -eq 1 ] || fail "$output"
    contains "→ no-mocks: 1 of 3 packaged units" "$output"
    contains "❌ src/consumer.cpp: defines MockInner::value()" "$output"
}

@test "a moved source counts as changed" {
    echo 'struct MockWidget { int poke(); }; int MockWidget::poke() { return 1; }' >src/old.cpp
    git add -A
    git -c user.email=t@t -c user.name=t commit -q -m old
    git mv src/old.cpp src/moved.cpp
    no_mocks --base HEAD
    [ "$status" -eq 1 ] || fail "$output"
    contains "❌ src/moved.cpp: defines MockWidget::poke()" "$output"
}

@test "--max-units keeps every changed source, samples the widest header, and says what it left out" {
    echo 'int shared();' >include/shared.h
    for n in a b c d e f; do printf '#include "shared.h"\nint %s() { return 1; }\n' "$n" >"src/$n.cpp"; done
    git add -A
    git -c user.email=t@t -c user.name=t commit -q -m more
    echo 'int shared2();' >>include/shared.h
    echo 'int clean() { return 2; }' >src/clean.cpp
    no_mocks --base HEAD --max-units 3
    [ "$status" -eq 0 ] || fail "$output"
    contains "→ no-mocks: 3 of 7 packaged units" "$output"
    contains "4 units reaching a changed header are past the budget of 3" "$output"
}

@test "--all scans every packaged unit and nothing a filter drops" {
    echo 'struct MockWidget { int poke(); }; int MockWidget::poke() { return 1; }' >src/leak.cpp
    echo 'struct MockQuiet { int poke(); }; int MockQuiet::poke() { return 1; }' >src/quiet_mock.cpp
    no_mocks --all
    [ "$status" -eq 1 ] || fail "$output"
    contains "❌ src/leak.cpp: defines MockWidget::poke()" "$output"
    lacks "MockQuiet" "$output"
    contains "1 of 2 packaged units carry mock code" "$output"
}

@test "no packaged sources is no answer, never a pass" {
    sed -i.orig 's/^APP_SRCS := \$(wildcard.*/APP_SRCS :=/' Makefile
    no_mocks --all
    [ "$status" -eq 3 ] || fail "$output"
    contains "make named no packaged sources" "$output"
}

@test "a named file that does not exist is no answer, never a pass" {
    no_mocks "src/clean.cpp src/typo.cpp"
    [ "$status" -eq 3 ] || fail "$output"
    contains "no such file" "$output"
}

@test "failed prerequisites are no answer, never a pass" {
    sed -i.orig 's/@\$(PREREQS_RECIPE)/@exit 7/' Makefile
    no_mocks src/clean.cpp
    [ "$status" -eq 3 ] || fail "$output"
    contains "compile-prereqs failed" "$output"
}

@test "the release gate reads the same pattern" {
    grep -q 'mock_symbol_pattern.txt' "$REPO_ROOT/scripts/release-gate.sh"
    refute_grep 'Mock\[A-Z' "$REPO_ROOT/scripts/release-gate.sh"
    grep -q 'mock_symbol_pattern.txt' "$CHECK"
}

@test "the real Makefile: packaged flags and sources, and the flags stamp left alone" {
    local scratch="$BATS_TEST_TMPDIR/obj" stamp dev packaged srcs before
    cd "$REPO_ROOT" || return 1
    dev=$(scratch_make -s print-compile-command F=src/system/config.cpp 2>/dev/null)
    contains "-DHELIX_ENABLE_MOCKS" "$dev"
    stamp="$(scratch_tree)/build/obj/.build-flags"
    before=$(cat "$stamp")
    packaged=$(scratch_make -s print-compile-command F=src/system/config.cpp \
        HELIX_PACKAGING=1 ENABLE_MOCKS=no "OBJ_DIR=$scratch" 2>/dev/null)
    lacks "-DHELIX_ENABLE_MOCKS" "$packaged"
    [ "$(cat "$stamp")" = "$before" ] || fail "the packaged parse rewrote the flags stamp"
    srcs=$(scratch_make -s print-var-APP_SRCS HELIX_PACKAGING=1 ENABLE_MOCKS=no "OBJ_DIR=$scratch" 2>/dev/null)
    contains "src/system/config.cpp" "$srcs"
    lacks "src/printer/ams_backend_mock.cpp" "$srcs"
}
