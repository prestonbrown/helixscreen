#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regression tests for the uninstall-reinstall race hardening:
#   - moonraker [update_manager helixscreen] section must be removed BEFORE
#     INSTALL_DIR (so an auto-refresh or mid-uninstall Mainsail click can't
#     race a re-extract against us)
#   - .uninstalling sentinel must be dropped at the start and swept at the end
#   - helixscreen-update.service template must check for the sentinel
#   - uninstall.sh must refuse to run from inside INSTALL_DIR (self-delete)
#   - install.sh --uninstall must refuse the same when $0 is inside INSTALL_DIR

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source the bundled uninstall.sh (the case guard skips main)
    . "$WORKTREE_ROOT/scripts/uninstall.sh"

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export HELIX_INSTALL_DIRS="$INSTALL_DIR"
    export HELIX_INIT_SCRIPTS=""
    export HELIX_PROCESSES=""
    export INIT_SYSTEM="sysv"
    export AD5M_FIRMWARE=""
    export SUDO=""
    export HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/var/lib/helixscreen"
    export HELIX_STATE_ROOT_HOME="$BATS_TEST_TMPDIR/root/.helixscreen"
}

# ============================================================================
# Sentinel helpers
# ============================================================================

@test "_uninstalling_sentinel_paths is defined" {
    type _uninstalling_sentinel_paths >/dev/null 2>&1
}

@test "_drop_uninstalling_sentinel is defined" {
    type _drop_uninstalling_sentinel >/dev/null 2>&1
}

@test "_uninstalling_sentinel_paths defaults to /var/lib/helixscreen/.uninstalling" {
    # Production default (no env override).  The systemd update.service unit
    # template hardcodes this path so it MUST match.
    unset HELIX_STATE_VAR_LIB
    run _uninstalling_sentinel_paths
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "^/var/lib/helixscreen/.uninstalling$"
}

@test "_uninstalling_sentinel_paths emits INSTALL_PARENT/.helixscreen/.uninstalling" {
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    run _uninstalling_sentinel_paths
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "^$BATS_TEST_TMPDIR/opt/.helixscreen/.uninstalling$"
}

@test "_uninstalling_sentinel_paths skips parent when INSTALL_DIR is /helixscreen" {
    # dirname /helixscreen == /, which the case-guard must skip
    INSTALL_DIR="/helixscreen"
    run _uninstalling_sentinel_paths
    [ "$status" -eq 0 ]
    # Should emit only the /var/lib path, not "/.helixscreen/.uninstalling"
    ! echo "$output" | grep -q "^/\.helixscreen"
}

@test "_drop_uninstalling_sentinel creates parent-dir sentinel" {
    local parent="$BATS_TEST_TMPDIR/opt"
    INSTALL_DIR="$parent/helixscreen"
    # Override the var_lib path so we don't need root for that branch
    _uninstalling_sentinel_paths() {
        echo "$parent/.helixscreen/.uninstalling"
    }

    _drop_uninstalling_sentinel
    [ -f "$parent/.helixscreen/.uninstalling" ]
}

@test "_drop_uninstalling_sentinel is idempotent (re-running is a no-op)" {
    local parent="$BATS_TEST_TMPDIR/opt"
    INSTALL_DIR="$parent/helixscreen"
    _uninstalling_sentinel_paths() {
        echo "$parent/.helixscreen/.uninstalling"
    }

    _drop_uninstalling_sentinel
    _drop_uninstalling_sentinel
    [ -f "$parent/.helixscreen/.uninstalling" ]
}

# ============================================================================
# Order-of-operations: moonraker section gone BEFORE install dir
# ============================================================================

@test "bundle main(): _drop_uninstalling_sentinel runs before remove_installation" {
    # Source-level assertion: the sentinel drop must precede the install-dir
    # removal in main().  Using line numbers from the bundle is fragile but
    # the simplest cross-check that the order didn't get accidentally swapped.
    local sentinel_line install_line
    sentinel_line=$(awk '/^main\(\)/{found=1} found && /_drop_uninstalling_sentinel/{print NR; exit}' \
        "$WORKTREE_ROOT/scripts/uninstall.sh")
    install_line=$(awk '/^main\(\)/{found=1} found && /^    remove_installation$/{print NR; exit}' \
        "$WORKTREE_ROOT/scripts/uninstall.sh")
    [ -n "$sentinel_line" ]
    [ -n "$install_line" ]
    [ "$sentinel_line" -lt "$install_line" ]
}

@test "bundle main(): remove_update_manager_section runs before remove_installation" {
    local mr_line install_line
    mr_line=$(awk '/^main\(\)/{found=1} found && /^    remove_update_manager_section/{print NR; exit}' \
        "$WORKTREE_ROOT/scripts/uninstall.sh")
    install_line=$(awk '/^main\(\)/{found=1} found && /^    remove_installation$/{print NR; exit}' \
        "$WORKTREE_ROOT/scripts/uninstall.sh")
    [ -n "$mr_line" ]
    [ -n "$install_line" ]
    [ "$mr_line" -lt "$install_line" ]
}

@test "install.sh's uninstall() runs remove_update_manager_section before rm -rf" {
    # install.sh bundles lib/installer/uninstall.sh::uninstall().  The same
    # ordering invariant applies there.
    local mr_line rm_line
    # Find the uninstall() function in install.sh and check both lines lie
    # within it.  AWK state machine: enter on uninstall(), exit on next
    # top-level function definition.
    mr_line=$(awk '
        /^uninstall\(\)/{found=1}
        found && /^[a-z_]+\(\)/ && !/^uninstall\(\)/ {found=0}
        found && /remove_update_manager_section/{print NR; exit}
    ' "$WORKTREE_ROOT/scripts/install.sh")
    rm_line=$(awk '
        /^uninstall\(\)/{found=1}
        found && /^[a-z_]+\(\)/ && !/^uninstall\(\)/ {found=0}
        found && /rm -rf "\$install_dir"/{print NR; exit}
    ' "$WORKTREE_ROOT/scripts/install.sh")
    [ -n "$mr_line" ]
    [ -n "$rm_line" ]
    [ "$mr_line" -lt "$rm_line" ]
}

@test "uninstall.sh's uninstall() runs _drop_uninstalling_sentinel first" {
    # Sentinel must be dropped before anything destructive.
    local sentinel_line rm_line
    sentinel_line=$(awk '
        /^uninstall\(\)/{found=1}
        found && /^[a-z_]+\(\)/ && !/^uninstall\(\)/ {found=0}
        found && /_drop_uninstalling_sentinel/{print NR; exit}
    ' "$WORKTREE_ROOT/scripts/uninstall.sh")
    rm_line=$(awk '
        /^uninstall\(\)/{found=1}
        found && /^[a-z_]+\(\)/ && !/^uninstall\(\)/ {found=0}
        found && /rm -rf "\$install_dir"/{print NR; exit}
    ' "$WORKTREE_ROOT/scripts/uninstall.sh")
    [ -n "$sentinel_line" ]
    [ -n "$rm_line" ]
    [ "$sentinel_line" -lt "$rm_line" ]
}

# ============================================================================
# Sentinel-aware update.service template
# ============================================================================

@test "helixscreen-update.service template has .uninstalling guard" {
    grep -q "/var/lib/helixscreen/.uninstalling" \
        "$WORKTREE_ROOT/config/helixscreen-update.service"
    grep -q "@@INSTALL_PARENT@@/.helixscreen/.uninstalling" \
        "$WORKTREE_ROOT/config/helixscreen-update.service"
}

# ============================================================================
# Self-delete guards
# ============================================================================

@test "uninstall.sh defines guard_self_delete" {
    type guard_self_delete >/dev/null 2>&1
}

@test "guard_self_delete is a no-op when \$0 is outside INSTALL_DIR" {
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    # Simulate $0 by running guard in a subshell via 'run' — guard reads $0
    # from the bash context, which in 'run' will be the test runner itself.
    # That's outside INSTALL_DIR, so guard must succeed silently.
    run guard_self_delete
    [ "$status" -eq 0 ]
}

@test "guard_self_delete refuses when \$0 is inside INSTALL_DIR" {
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR"
    # Stage a copy of uninstall.sh inside INSTALL_DIR and invoke main()
    # through that path so $0 resolves inside INSTALL_DIR.
    cp "$WORKTREE_ROOT/scripts/uninstall.sh" "$INSTALL_DIR/uninstall.sh"

    # Run the in-INSTALL_DIR copy.  guard_self_delete runs right after
    # set_install_paths and before check_permissions / detect_init_system,
    # so it fires before any real-hardware detection matters.  Force the
    # prompt skip with --force in case the platform detection succeeds.
    run sh "$INSTALL_DIR/uninstall.sh" --force
    # Expect non-zero exit (1) with self-delete error message.
    [ "$status" -ne 0 ]
    echo "$output" | grep -qi "Refusing to run"
    # INSTALL_DIR must NOT have been removed
    [ -d "$INSTALL_DIR" ]
    [ -f "$INSTALL_DIR/uninstall.sh" ]
}

@test "install.sh --uninstall refuses when \$0 is inside INSTALL_DIR" {
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR"
    cp "$WORKTREE_ROOT/scripts/install.sh" "$INSTALL_DIR/install.sh"

    run sh "$INSTALL_DIR/install.sh" --uninstall
    [ "$status" -ne 0 ]
    echo "$output" | grep -qi "Refusing to run --uninstall"
    [ -d "$INSTALL_DIR" ]
    [ -f "$INSTALL_DIR/install.sh" ]
}

@test "guard_self_delete handles \$INSTALL_DIR with trailing slash" {
    # Regression for the trailing-slash bug: case patterns "$INSTALL_DIR/*"
    # don't match if INSTALL_DIR ends in / (becomes //*).  Normalize with %/.
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen/"
    mkdir -p "$BATS_TEST_TMPDIR/opt/helixscreen"
    cp "$WORKTREE_ROOT/scripts/uninstall.sh" "$BATS_TEST_TMPDIR/opt/helixscreen/uninstall.sh"

    run sh "$BATS_TEST_TMPDIR/opt/helixscreen/uninstall.sh" --force
    [ "$status" -ne 0 ]
    echo "$output" | grep -qi "Refusing to run"
}

# ============================================================================
# Sentinel sweep — file is actually removed, not just the parent dir
# ============================================================================

@test "_sweep_uninstalling_sentinel removes the sentinel file" {
    local parent="$BATS_TEST_TMPDIR/opt"
    INSTALL_DIR="$parent/helixscreen"
    HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/var/lib/helixscreen"

    _drop_uninstalling_sentinel
    [ -f "$parent/.helixscreen/.uninstalling" ]
    [ -f "$HELIX_STATE_VAR_LIB/.uninstalling" ]

    _sweep_uninstalling_sentinel
    [ ! -f "$parent/.helixscreen/.uninstalling" ]
    [ ! -f "$HELIX_STATE_VAR_LIB/.uninstalling" ]
}

@test "_uninstalling_sentinel_paths honors \$HELIX_STATE_VAR_LIB override" {
    # Source-of-truth check: the var_lib path used for the sentinel must
    # match what clean_helix_state_dirs sweeps, or BATS isolation breaks.
    HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/var/lib/helixscreen"
    INSTALL_DIR=""
    run _uninstalling_sentinel_paths
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "^$BATS_TEST_TMPDIR/var/lib/helixscreen/.uninstalling$"
}

# ============================================================================
# Regen check — the lib module and bundle must stay in sync
# ============================================================================

@test "bundle-uninstaller.sh regenerates identical output (idempotent)" {
    local regen="$BATS_TEST_TMPDIR/uninstall-regen.sh"
    "$WORKTREE_ROOT/scripts/bundle-uninstaller.sh" -o "$regen"
    diff "$WORKTREE_ROOT/scripts/uninstall.sh" "$regen"
}

@test "bundle-installer.sh regenerates identical output (idempotent)" {
    local regen="$BATS_TEST_TMPDIR/install-regen.sh"
    "$WORKTREE_ROOT/scripts/bundle-installer.sh" -o "$regen"
    diff "$WORKTREE_ROOT/scripts/install.sh" "$regen"
}
