#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for resolve_platform_hook_key() (platform.sh) and the device-profile
# probe (scripts/device-profile.sh).
#
# These two are what keep the firmware-detection rules to a single
# implementation: the installer resolves the hook key, and mk/cross.mk's deploy
# targets ask for the same answer over ssh instead of deriving their own. A
# second copy drifts silently, so the cases below pin every flavor the resolver
# must answer for, not just the common one.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    KLIPPER_USER=""
    KLIPPER_HOME=""
    INIT_SCRIPT_DEST=""
    PREVIOUS_UI_SCRIPT=""
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    INSTALL_DIR="/opt/helixscreen"
    TMP_DIR=""
    HOST_PLATFORM_HOOK_KEY=""

    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    load helpers
}

# ---------------------------------------------------------------------------
# resolve_platform_hook_key: the flavor dispatch
# ---------------------------------------------------------------------------

@test "resolve_platform_hook_key: ad5m + forge_x selects the forgex hook" {
    AD5M_FIRMWARE="forge_x"
    run resolve_platform_hook_key "ad5m"
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m-forgex" ]
}

@test "resolve_platform_hook_key: ad5m + klipper_mod selects the kmod hook" {
    AD5M_FIRMWARE="klipper_mod"
    run resolve_platform_hook_key "ad5m"
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m-kmod" ]
}

@test "resolve_platform_hook_key: ad5m + zmod selects the zmod hook" {
    # hooks-ad5m-zmod.sh is only ever reachable through this branch.
    AD5M_FIRMWARE="zmod"
    run resolve_platform_hook_key "ad5m"
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m-zmod" ]
}

@test "resolve_platform_hook_key: every ad5m flavor names a hook file that exists" {
    for flavor_hook in "forge_x:ad5m-forgex" "klipper_mod:ad5m-kmod" "zmod:ad5m-zmod"; do
        AD5M_FIRMWARE="${flavor_hook%%:*}"
        expected="${flavor_hook##*:}"
        run resolve_platform_hook_key "ad5m"
        [ "$output" = "$expected" ]
        [ -f "$WORKTREE_ROOT/assets/config/platform/hooks-${output}.sh" ]
    done
}

@test "resolve_platform_hook_key: stock ad5m names no hook" {
    AD5M_FIRMWARE="stock"
    run resolve_platform_hook_key "ad5m"
    [ "$status" -eq 0 ]
    [ "$output" = "" ]
}

# ---------------------------------------------------------------------------
# resolve_platform_hook_key: the platform dispatch and the probe override
# ---------------------------------------------------------------------------

@test "resolve_platform_hook_key: platform dispatch outranks the flavor dispatch" {
    # An AD5X reports flavor=forge_x too; the platform case must win so it does
    # not receive the AD5M hook, whose cache paths assume a /data it lacks.
    AD5M_FIRMWARE="forge_x"
    run resolve_platform_hook_key "ad5x"
    [ "$output" = "ad5x" ]
}

@test "resolve_platform_hook_key: a probed mod host outranks both dispatches" {
    AD5M_FIRMWARE="forge_x"
    HOST_PLATFORM_HOOK_KEY="ad5x-forgex"
    run resolve_platform_hook_key "ad5x"
    [ "$output" = "ad5x-forgex" ]
}

@test "resolve_platform_hook_key: pi32 shares the pi hook" {
    run resolve_platform_hook_key "pi32"
    [ "$output" = "pi" ]
}

@test "resolve_platform_hook_key: an unknown platform names no hook" {
    run resolve_platform_hook_key "definitely-not-a-platform"
    [ "$status" -eq 0 ]
    [ "$output" = "" ]
}

# ---------------------------------------------------------------------------
# install_platform_hooks still consumes the resolver
# ---------------------------------------------------------------------------

@test "install_platform_hooks delegates to resolve_platform_hook_key" {
    # install_platform_hooks must not re-derive the key. Stub the resolver and
    # assert the deploy call follows ITS answer, not a recomputed one.
    unset _HELIX_MAIN_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/main.sh"

    resolve_platform_hook_key() { echo "sentinel-hook"; }
    deploy_platform_hooks() { echo "DEPLOYED:$2"; }
    platform="ad5m"
    AD5M_FIRMWARE="forge_x"

    run install_platform_hooks
    [ "$status" -eq 0 ]
    [ "$output" = "DEPLOYED:sentinel-hook" ]
}

@test "install_platform_hooks deploys nothing when the resolver names no hook" {
    unset _HELIX_MAIN_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/main.sh"

    resolve_platform_hook_key() { echo ""; }
    deploy_platform_hooks() { echo "DEPLOYED:$2"; }
    platform="x86"

    run install_platform_hooks
    [ "$status" -eq 0 ]
    [ "$output" = "" ]
}

# ---------------------------------------------------------------------------
# scripts/device-profile.sh
# ---------------------------------------------------------------------------

@test "device-profile --emit produces a syntactically valid POSIX program" {
    run "$WORKTREE_ROOT/scripts/device-profile.sh" --emit
    [ "$status" -eq 0 ]
    echo "$output" > "$BATS_TEST_TMPDIR/probe.sh"
    run sh -n "$BATS_TEST_TMPDIR/probe.sh"
    [ "$status" -eq 0 ]
}

@test "device-profile probe prints every documented key" {
    "$WORKTREE_ROOT/scripts/device-profile.sh" --emit > "$BATS_TEST_TMPDIR/probe.sh"
    run sh "$BATS_TEST_TMPDIR/probe.sh"
    [ "$status" -eq 0 ]
    for key in PLATFORM MOD_FLAVOR INSTALL_DIR INIT_SCRIPT_DEST PLATFORM_HOOK_KEY SERVICE_MECHANISM; do
        echo "$output" | grep -q "^${key}=" || {
            echo "missing key $key in: $output" >&2
            return 1
        }
    done
}

@test "device-profile probe emits only KEY=VALUE lines on stdout" {
    # The log_* stubs are what keep this true; without them set_install_paths'
    # log_info calls land on stdout and the makefile's $(filter) picks up noise.
    "$WORKTREE_ROOT/scripts/device-profile.sh" --emit > "$BATS_TEST_TMPDIR/probe.sh"
    run sh "$BATS_TEST_TMPDIR/probe.sh"
    [ "$status" -eq 0 ]
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        echo "$line" | grep -qE '^[A-Z_]+=' || {
            echo "non KEY=VALUE line on stdout: $line" >&2
            return 1
        }
    done <<< "$output"
}

@test "device-profile probe survives the real guards being present" {
    # The stubs must come AFTER the modules: defining them first lets the real
    # validate_install_dir load over them, and its `|| exit 1` inside
    # set_install_paths kills the probe with no output at all.
    "$WORKTREE_ROOT/scripts/device-profile.sh" --emit > "$BATS_TEST_TMPDIR/probe.sh"
    run sh "$BATS_TEST_TMPDIR/probe.sh"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
}

@test "device-profile requires an ssh target" {
    run "$WORKTREE_ROOT/scripts/device-profile.sh"
    [ "$status" -eq 2 ]
    echo "$output" | grep -q "usage:"
}

@test "device-profile ships the modules the probe actually needs" {
    run "$WORKTREE_ROOT/scripts/device-profile.sh" --emit
    [ "$status" -eq 0 ]
    # The three functions the probe calls must be defined in what it emits.
    echo "$output" | grep -q "^host_profile_probe()"
    echo "$output" | grep -q "^detect_mod_flavor()"
    echo "$output" | grep -q "^resolve_platform_hook_key()"
}

@test "device-profile.sh passes shellcheck" {
    if ! command -v shellcheck >/dev/null 2>&1; then
        skip "shellcheck not installed"
    fi
    run shellcheck -s sh "$WORKTREE_ROOT/scripts/device-profile.sh"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# mk/cross.mk no longer keeps its own copy
# ---------------------------------------------------------------------------

@test "cross.mk asks device-profile.sh instead of hand-rolling detection" {
    run grep -c 'device-profile.sh' "$WORKTREE_ROOT/mk/cross.mk"
    [ "$status" -eq 0 ]
    [ "$output" -ge 1 ]
}

@test "cross.mk has no hand-written ad5m firmware dispatch left" {
    # The exact shapes that drifted. A recipe line naming a specific hook file
    # or testing a firmware marker means the second copy is back.
    #
    # Comments are stripped first: the rule is documented in prose here while
    # living in exactly one place in code.
    strip_comments() {
        sed -e "s/^[[:space:]]*@\\{0,1\\}#.*$//" "/home/pbrown/Code/Printing/helixscreen/.worktrees/devel-1.1/mk/cross.mk"
    }

    run bash -c "$(declare -f strip_comments); strip_comments | grep -nE 'hooks-ad5m-(forgex|kmod|zmod)\.sh'"
    [ "$status" -ne 0 ]

    run bash -c "$(declare -f strip_comments); strip_comments | grep -nE '\-d /mnt/data/\.klipper_mod|\-d /opt/config/mod/\.root'"
    [ "$status" -ne 0 ]
}

@test "cross.mk evaluates the device profile lazily" {
    # A simply-expanded (:=) AD5M_PROFILE would ssh the printer on EVERY make
    # invocation, including a plain `make -j` that never touches a device.
    run grep -E '^AD5M_PROFILE[[:space:]]*=' "$WORKTREE_ROOT/mk/cross.mk"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'eval'
}
