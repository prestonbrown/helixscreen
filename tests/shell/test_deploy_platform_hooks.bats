#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The init script sources $INSTALL_DIR/platform/hooks.sh. When that file is
# absent every platform_* function stays the no-op stub the init script
# declares: platform_pre_start() never exports HELIX_CACHE_DIR, and
# platform_stop_competing_uis() leaves the stock UI running. Nothing reports
# either, so a device deploy that forgets the hooks file looks completely
# successful and the difference only shows up as a runtime symptom - on the
# CC1, a cache that lands on tmpfs.
#
# These tests pin that every device deploy target places the hooks file, and
# that the installer's own copy verifies it landed rather than announcing
# success unconditionally.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
CROSS_MK="$WORKTREE_ROOT/mk/cross.mk"
HOOKS_DIR="$WORKTREE_ROOT/assets/config/platform"
SERVICE_SH="$WORKTREE_ROOT/scripts/lib/installer/service.sh"

setup() {
    load helpers
}

# The recipe body of a make target, up to the next target declaration.
target_body() {
    awk -v t="^$1:" '
        $0 ~ t { inside = 1; next }
        inside && /^[a-zA-Z0-9_.-]+:/ { exit }
        inside { print }
    ' "$CROSS_MK"
}

# The body of a `define NAME ... endef` block.
define_body() {
    awk -v d="^define $1[ \t]*$" '
        $0 ~ d { inside = 1; next }
        inside && /^endef/ { exit }
        inside { print }
    ' "$CROSS_MK"
}

# Does this target place platform hooks?
#
# Targets reach the hooks call three ways: directly, through a per-platform
# `define <plat>-deploy-common`, or through the shared `deploy-common`, which
# forwards its optional 4th argument as the hook key. Only the shared one is
# conditional, so a 3-argument call to it deploys nothing and does not count.
target_deploys_hooks() {
    local body expanded name
    body="$(target_body "$1")"
    expanded="$body"

    # Expand one level of $(call NAME,...) for per-platform defines.
    for name in $(echo "$body" | sed -n 's/.*\$(call \([a-zA-Z0-9_-]*\).*/\1/p'); do
        [ "$name" = "deploy-common" ] && continue
        expanded="$expanded
$(define_body "$name")"
    done

    echo "$expanded" | grep -q 'call deploy-platform-hooks' && return 0
    echo "$body" | grep -qE 'call deploy-common,[^)]+,[^),]+,[^),]+,[^),]+\)' && return 0
    return 1
}

@test "every sysv device deploy target places platform hooks" {
    # Platforms whose hooks file carries real behaviour. The -fg/-bin variants
    # route through the same recipes and are covered by their base target.
    for t in deploy-cc1 deploy-k1 deploy-k1-dynamic deploy-k2 deploy-snapmaker-u1; do
        run target_deploys_hooks "$t"
        [ "$status" -eq 0 ] || {
            echo "$t does not deploy platform/hooks.sh"
            false
        }
    done
}

@test "a keyed deploy-common call names a hook file that exists" {
    # A key with no matching file deploys nothing and says nothing.
    grep -oE 'call deploy-common,[^)]*\)' "$CROSS_MK" | while read -r call; do
        key="$(echo "$call" | awk -F',' 'NF>=5 {gsub(/\)$/,"",$5); print $5}')"
        [ -n "$key" ] || continue
        [ -f "$HOOKS_DIR/hooks-$key.sh" ] || {
            echo "deploy-common names hook key '$key' but $HOOKS_DIR/hooks-$key.sh does not exist"
            false
        }
    done
}

@test "every direct deploy-platform-hooks call names a hook file that exists" {
    grep -oE 'call deploy-platform-hooks,[^)]*\)' "$CROSS_MK" | while read -r call; do
        key="$(echo "$call" | awk -F',' '{gsub(/\)$/,"",$4); print $4}')"
        # The deploy-common forwarder passes $(4) through; skip the definition.
        case "$key" in ''|'$(4)') continue ;; esac
        [ -f "$HOOKS_DIR/hooks-$key.sh" ] || {
            echo "deploy-platform-hooks names hook key '$key' but $HOOKS_DIR/hooks-$key.sh does not exist"
            false
        }
    done
}

@test "the deploy define verifies the hooks file landed on the device" {
    run grep -A24 'define deploy-platform-hooks' "$CROSS_MK"
    [ "$status" -eq 0 ]
    # A copy that cannot fail the build is the failure mode being pinned.
    echo "$output" | grep -q 'test -s'
    echo "$output" | grep -q 'did not land'
}

@test "the installer confirms the hooks file exists before reporting success" {
    run awk '/^deploy_platform_hooks\(\)/,/^}/' "$SERVICE_SH"
    [ "$status" -eq 0 ]
    # Each cp is non-fatal, so the success log must be conditional on the result.
    echo "$output" | grep -q 'if \[ -s "\${install_dir}/platform/hooks.sh" \]'
    echo "$output" | grep -q 'Platform hooks NOT deployed'
}
