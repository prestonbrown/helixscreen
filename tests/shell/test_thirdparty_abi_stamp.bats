#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pins the third-party header ABI stamp (mk/patches.mk).
#
# Our objects reach the patched LVGL and libhv headers through -isystem, and
# DEPFLAGS is -MMD, which leaves system headers out of the .d files. Two libhv
# patches add members to hv::TcpClientEventLoopTmpl and hv::WebSocketClient, so
# a change to those headers moves every member after the insertion point while
# every .o in the tree still looks up to date. lib/ is shared between worktrees
# and build/ is not, so the change can also land while a build is running.
#
# Nothing downstream reports the resulting disagreement: the link succeeds, and
# a std::mutex read at the wrong offset locks bytes that were never a mutex -
# which glibc accepts and macOS libc++ rejects with EINVAL. The stamp and the
# link guard are the only things standing between that and a green suite, and
# either is one word in a prerequisite list away from being dropped.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# Makefiles that compile translation units into helix-screen or helix-tests.
ABI_MAKEFILES="mk/rules.mk mk/tests.mk mk/bluetooth.mk"

# Self-contained third-party translation units: they include neither our
# headers nor the patched LVGL and libhv types, so no member offset they see
# can disagree with ours.
ABI_EXEMPT='dns_resolv|quirc|minilzo|catch_amalgamated|demos'

# Target lines of every rule in those files whose recipe compiles a TU.
compile_rule_targets() {
    awk '
        /^[^\t #]/ && /:/ { target = $0 }
        /-c \$</ && target != "" { print target; target = "" }
    ' $ABI_MAKEFILES
}

# The make database, without asking make whether the default goal is buildable.
# What these cases pin is wiring, and they run wherever bats runs, a host with
# no SDL2, no libnl and no compiled dependencies included. `help` has no
# prerequisites, so make prints the database and stops; the bare default goal
# walks the whole graph first and exits 2 on the first prerequisite the host
# cannot supply, taking the case down with it under bats' `set -e`.
make_database() {
    make -pn help "$@" 2>/dev/null
}

@test "every rule compiling into helix-screen or helix-tests depends on ABI_STAMP" {
    local missing=""
    while IFS= read -r rule; do
        [[ "$rule" =~ $ABI_EXEMPT ]] && continue
        [[ "$rule" == *'$(ABI_STAMP)'* ]] && continue
        missing="${missing}${rule}"$'\n'
    done < <(compile_rule_targets)
    [ -z "$missing" ] || {
        echo "Compile rules missing \$(ABI_STAMP):"
        echo "$missing"
        false
    }
}

# A stamp prerequisite decides that make reruns the compile; it does not decide
# what the compiler produces. ccache stands between the two, and its depend mode
# keys an entry on the source plus the paths -MMD lists - the set that omits
# these -isystem headers. Without the hash on the command line the rerun resolves
# to the entry built against the previous layout, and the stamp buys nothing.
# Every rule pinned above draws its flags from one of these four variables.
@test "the ABI hash is on the compiler command line, not only in the dependency graph" {
    local db missing=""
    db="$(make_database)"
    for var in CFLAGS CXXFLAGS SUBMODULE_CFLAGS SUBMODULE_CXXFLAGS; do
        printf '%s\n' "$db" | grep -m1 "^${var} :\{0,1\}= " | grep -q -- '-DHELIX_TP_ABI=' \
            || missing="${missing}${var} "
    done
    [ -z "$missing" ] || { echo "flag sets carrying no -DHELIX_TP_ABI: $missing"; false; }
}

# cksum prints a checksum AND a byte count, so the value has to be folded into a
# single token or the second field arrives as a stray source-file argument.
@test "the define is one token and tracks the tracked headers' content" {
    local work="${BATS_TEST_TMPDIR:-$(mktemp -d)}/abi-define"
    rm -rf "$work" && mkdir -p "$work"

    define_for() {
        printf '%s' "$1" > "$work/tracked.h"
        make_database BUILD_DIR="$work" ABI_HEADERS="$work/tracked.h" \
            | grep -m1 '^ABI_DEFINE :\{0,1\}= ' | cut -d= -f2-
    }

    local before after problems=""
    before="$(define_for 'struct S { int a; };')"
    after="$(define_for 'struct S { int b; int a; };')"

    [ -n "$before" ] || problems="ABI_DEFINE is empty; "
    [ "$before" != "$after" ] || problems="${problems}changing a tracked header left the define at '$before'; "
    [ "$(printf '%s' "$before" | wc -w)" -eq 1 ] \
        || problems="${problems}define is not a single token: '$before'"

    [ -z "$problems" ] || { echo "$problems"; false; }
}

@test "both link recipes call check_abi_unchanged" {
    grep -q 'check_abi_unchanged' mk/rules.mk
    grep -q 'check_abi_unchanged' mk/tests.mk
}

@test "ABI_HEADERS covers the layout-bearing libhv headers in both locations" {
    local line missing=""
    line="$(make_database | grep -m1 '^ABI_HEADERS :=')"
    # The patch rewrites evpp/TcpClient.h; -isystem lib/libhv/include resolves
    # to the copy libhv installs beside it. Both have to be watched.
    for header in lib/libhv/evpp/TcpClient.h \
                  lib/libhv/include/hv/TcpClient.h \
                  lib/libhv/http/client/WebSocketClient.h; do
        case "$line" in
            *"$header"*) ;;
            *) missing="${missing}${header} " ;;
        esac
    done
    [ -z "$missing" ] || { echo "ABI_HEADERS does not cover: $missing"; false; }
}

# A stamp left over from an earlier header revision is what lets stale objects
# look current, so the parse-time hook has to overwrite one that disagrees.
#
# Driven against a private BUILD_DIR. Corrupting the real build/.thirdparty-abi
# would leave it repaired but with a fresh mtime, and every object in the tree
# depends on it - the gate would hand whoever ran it a full rebuild.
@test "a stamp that disagrees with the headers is rewritten at parse time" {
    local sandbox="${BATS_TEST_TMPDIR:-$(mktemp -d)}/abi-stamp"
    rm -rf "$sandbox" && mkdir -p "$sandbox"
    printf 'not-a-hash' > "$sandbox/.thirdparty-abi"

    local hash problems=""
    hash="$(make_database BUILD_DIR="$sandbox" | grep -m1 '^ABI_HASH :=' | cut -d' ' -f3-)"
    [ -n "$hash" ] || problems="ABI_HASH is empty; "
    [ "$(cat "$sandbox/.thirdparty-abi")" = "$hash" ] \
        || problems="${problems}stamp still reads '$(cat "$sandbox/.thirdparty-abi")', want '$hash'"
    [ -z "$problems" ] || { echo "$problems"; false; }
}

# A fixture tree carrying the real definitions of the guard and the recorder,
# lifted out of mk/patches.mk. Copies would let the two drift: gutting either
# recipe there has to fail here.
#
#   steady    the headers have not moved since the stamp was written
#   flipped   a header changes under the running build and nothing re-records
#   recorded  a header changes and the build records the new value, the shape a
#             clean checkout takes when it applies its own patches
write_guard_fixture() {
    local work="$1"
    rm -rf "$work" && mkdir -p "$work"

    sed -n '/^define check_abi_unchanged$/,/^endef$/p' mk/patches.mk > "$work/guard.mk"
    sed -n '/^define record_abi_stamp$/,/^endef$/p' mk/patches.mk >> "$work/guard.mk"
    [ -s "$work/guard.mk" ] || fail "check_abi_unchanged / record_abi_stamp not found in mk/patches.mk"

    printf 'struct S { int a; };' > "$work/tracked.h"
    cat > "$work/Makefile" <<'MK'
Q :=
RED :=
BOLD :=
RESET :=
YELLOW :=
BUILD_DIR := .
ABI_HEADERS := tracked.h
ABI_STAMP := thirdparty-abi
ABI_HASH_CMD = cat /dev/null $(ABI_HEADERS) 2>/dev/null | cksum
$(shell $(ABI_HASH_CMD) > $(ABI_STAMP))
include guard.mk
steady:
	$(call check_abi_unchanged)
	@echo LINKED
flipped:
	@printf 'struct S { int b; int a; };' > tracked.h
	$(call check_abi_unchanged)
	@echo LINKED
recorded:
	@printf 'struct S { int b; int a; };' > tracked.h
	$(call record_abi_stamp)
	$(call check_abi_unchanged)
	@echo LINKED
MK
}

@test "check_abi_unchanged fails when a tracked header changes mid-build" {
    local work="${BATS_TEST_TMPDIR:-$(mktemp -d)}/abi-guard"
    write_guard_fixture "$work"

    local problems=""

    run make -C "$work" steady
    [ "$status" -eq 0 ] || problems="untouched headers were rejected; "
    [[ "$output" == *LINKED* ]] || problems="${problems}steady link did not run; "

    run make -C "$work" flipped
    [ "$status" -ne 0 ] || problems="${problems}changed headers were accepted; "
    [[ "$output" != *LINKED* ]] || problems="${problems}link ran anyway; "
    [[ "$output" == *"changed while this build was running"* ]] \
        || problems="${problems}no explanation for the failure"

    [ -z "$problems" ] || { echo "$problems"; false; }
}

# A checkout that has not been patched yet holds upstream's headers while make
# is parsing, and ours by the time the first object is compiled. Comparing the
# link against the parse-time value rejects every clean checkout there is, so
# the reference has to be the stamp the build re-records.
@test "check_abi_unchanged follows the recorded stamp, not the parse-time value" {
    local work="${BATS_TEST_TMPDIR:-$(mktemp -d)}/abi-recorded"
    write_guard_fixture "$work"

    local problems=""
    run make -C "$work" recorded
    [ "$status" -eq 0 ] || problems="a re-recorded header set was rejected; "
    [[ "$output" == *LINKED* ]] || problems="${problems}the link did not run"

    [ -z "$problems" ] || { echo "$problems"; echo "$output"; false; }
}

# Which is only true while the two recipes that move these headers inside the
# build still record it. The patch step rewrites them; libhv's build installs
# the include/hv/ copies. Drop either call and every clean checkout fails at the
# link again.
@test "the recipes that change the tracked headers re-record the stamp" {
    local problems=""

    # Each range runs from the target line to the recipe's own last line, so a
    # call that moved out of the recipe and into a comment or a neighbouring
    # rule reads as missing, which is what it would be.
    awk '/^\$\(PATCHES_STAMP\):/{r=1} r{print} r && index($0,"\t@touch $@")==1{exit}' mk/patches.mk \
        | grep -q 'call record_abi_stamp' \
        || problems="the patch-stamp recipe does not re-record the ABI stamp; "
    awk '/^libhv-build:/{r=1} r{print} r && /libhv built/{exit}' mk/deps.mk \
        | grep -q 'call record_abi_stamp' \
        || problems="${problems}libhv-build does not re-record the ABI stamp"

    [ -z "$problems" ] || { echo "$problems"; false; }
}
