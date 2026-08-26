#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regression tests for `make reset-patches` covering BOTH submodules.
#
# Why this exists: reset-patches looped over LVGL_PATCHED_FILES only.
# LIBHV_PATCHED_FILES was defined and never used, so `make reapply-patches` —
# the remedy mk/patches.mk prints when a libhv patch will not apply — could not
# fix a libhv patch. A tree carrying an OLDER revision of a libhv patch fails
# `git apply --check` on the newer one; you were told to run reapply-patches; it
# reset only LVGL, left the stale libhv hunks in place, and failed identically
# the next time. That is how the #1212 null-hloop guard stayed out of a working
# tree with no supported way to get it back, while `make test` (which skips
# apply-patches) kept building a binary that SIGSEGV'd on the regression test
# written to catch that exact fault.
#
# The untracked-file half matters just as much, and has already caused one
# outage of its own: a patch that CREATES a file leaves it untracked, where
# `git checkout` cannot restore it. Leaving those behind is what orphaned
# base/dns_resolv.c and convinced the old DNS guard the patch was applied when
# the wiring was not — see test_libhv_dns_resolver_patch.bats.

setup() {
    REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    PATCHES_MK="$REPO_ROOT/mk/patches.mk"

    # The reset loop for each submodule is emitted as one shell line, so the
    # libhv loop can be isolated by the submodule path inside it.
    DRYRUN="$(cd "$REPO_ROOT" && make -n reset-patches 2>/dev/null)"
    LIBHV_LOOP="$(printf '%s\n' "$DRYRUN" | grep 'for file in' | grep 'lib/libhv' || true)"
}

# The list as mk/patches.mk declares it, so the tests follow the variable rather
# than a copy of it that can drift.
declared_libhv_files() {
    sed -n '/^LIBHV_PATCHED_FILES[[:space:]]*:=/,/^$/p' "$PATCHES_MK" |
        sed 's/^LIBHV_PATCHED_FILES[[:space:]]*:=//' |
        tr -d '\\' | tr ' \t' '\n' | grep -v '^$'
}

@test "reset-patches covers the libhv submodule, not only LVGL" {
    [ -n "$LIBHV_LOOP" ]
}

@test "reset-patches still covers the LVGL submodule" {
    printf '%s\n' "$DRYRUN" | grep 'for file in' | grep -q 'lib/lvgl'
}

@test "LIBHV_PATCHED_FILES is not empty" {
    run declared_libhv_files
    [ "${#lines[@]}" -gt 0 ]
}

@test "every declared libhv patched file is in the reset loop" {
    while read -r f; do
        [ -n "$f" ] || continue
        if ! printf '%s' "$LIBHV_LOOP" | grep -q -- "$f"; then
            echo "LIBHV_PATCHED_FILES entry not reset: $f"
            return 1
        fi
    done < <(declared_libhv_files)
}

@test "the #1212 TcpClient guard file is reset" {
    # The specific file whose stale patch revision started all of this.
    printf '%s' "$LIBHV_LOOP" | grep -q 'evpp/TcpClient.h'
}

@test "patch-created files are removed, not checked out" {
    # `git checkout` fails on an untracked file ("did not match any file(s)
    # known to git"), and leaving it behind makes the re-apply fail the other
    # way ("already exists"). Trackedness has to be tested, with an rm branch.
    printf '%s' "$LIBHV_LOOP" | grep -q 'ls-files --error-unmatch'
    printf '%s' "$LIBHV_LOOP" | grep -q 'rm -f'
}

@test "tracked files are restored with checkout, not deleted" {
    printf '%s' "$LIBHV_LOOP" | grep -q 'checkout'
}

@test "configure-generated libhv files are NOT reset" {
    # config.mk and hconfig.h are dirty in every built tree but are written by
    # libhv's own ./configure, not by a patch. Resetting them would fight the
    # build rather than restore a patch.
    ! printf '%s' "$LIBHV_LOOP" | grep -qE '(^|[^a-z])config\.mk'
    ! printf '%s' "$LIBHV_LOOP" | grep -q 'hconfig\.h'
}

@test "reapply-patches resets before re-applying" {
    # Without the reset half running first, reapply-patches degrades to
    # apply-patches and cannot clear a stale hunk — the failure mode this whole
    # file is about.
    #
    # Asserted against `make -n` output rather than the makefile text. The target
    # used to name reset-patches as a prerequisite and now invokes it as an
    # ordered sub-make, because `make -jN reapply-patches` is free to run two
    # prerequisites concurrently and the apply half rewrites the very files the
    # reset half is restoring. Both spellings satisfy the invariant, so only the
    # resulting order is worth pinning.
    local dryrun reset_at apply_at
    dryrun="$(cd "$REPO_ROOT" && make -n reapply-patches 2>/dev/null)"

    # Reset half: the libhv restore loop, the same anchor setup() keys on.
    reset_at="$(printf '%s\n' "$dryrun" | grep -n 'for file in' | grep 'lib/libhv' |
        head -1 | cut -d: -f1)"
    # Apply half: dropping the stamp is precisely what makes this a FORCED
    # apply rather than the no-op that a current stamp would produce.
    apply_at="$(printf '%s\n' "$dryrun" | grep -n 'rm -f build/\.patches-applied' |
        head -1 | cut -d: -f1)"

    [ -n "$reset_at" ]
    [ -n "$apply_at" ]
    [ "$reset_at" -lt "$apply_at" ]
}
