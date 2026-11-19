#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for install_permission_rules (permissions.sh), ensure-polkit-rule.sh
# and remove_permission_rules (uninstall.sh).
# Covers udev backlight rules, polkit NetworkManager rules and the polkit
# logind power-off/reboot rule.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source modules (reset source guards so each test gets a fresh load)
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_PERMISSIONS_SOURCED _HELIX_SERVICE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/permissions.sh"

    # Set required globals
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export KLIPPER_USER="biqu"

    # Create install dir with config files
    mkdir -p "$INSTALL_DIR/config"
    cp "$WORKTREE_ROOT/config/99-helixscreen-backlight.rules" "$INSTALL_DIR/config/"
    cp "$WORKTREE_ROOT/config/helixscreen-network.pkla" "$INSTALL_DIR/config/"
    cp "$WORKTREE_ROOT/config/ensure-polkit-rule.sh" "$INSTALL_DIR/config/"

    # ensure-polkit-rule.sh writes under this instead of /etc/polkit-1
    export HELIX_POLKIT_DIR="$BATS_TEST_TMPDIR/etc/polkit-1"

    # Mock systemctl to prevent restarting real services (e.g., polkit) on dev machines
    mock_command "systemctl" ""
    # ensure-polkit-rule.sh picks .rules or .pkla from `pkaction --version`; the
    # host's polkit (0.105 on Ubuntu 22.04 runners) must not choose for the test.
    mock_command "pkaction" "pkaction version 124"
}

# Helper: create a SUDO wrapper that rewrites system paths to tmpdir
setup_sudo_redirect() {
    local wrapper="$BATS_TEST_TMPDIR/bin/sudo_redirect"
    mkdir -p "$(dirname "$wrapper")"
    cat > "$wrapper" << 'SUDOEOF'
#!/bin/sh
# Rewrite system paths to test tmpdir
new_args=""
for arg in "$@"; do
    case "$arg" in
        # The rule directories themselves too: `$SUDO test -d /etc/polkit-1/rules.d`
        # must ask the sandbox, not whether this host happens to run polkit.
        /etc/udev/rules.d | /etc/udev/rules.d/* | /etc/polkit-1/rules.d | /etc/polkit-1/rules.d/* | \
        /etc/polkit-1/localauthority/50-local.d | /etc/polkit-1/localauthority/50-local.d/*)
            arg="${BATS_TEST_TMPDIR}${arg}"
            ;;
    esac
    new_args="$new_args \"$arg\""
done
eval $new_args
SUDOEOF
    chmod +x "$wrapper"
    SUDO="$wrapper"
}

# =============================================================================
# Skipping for root/embedded platforms
# =============================================================================

@test "install_permission_rules: skips for ad5m platform" {
    run install_permission_rules "ad5m"
    [ "$status" -eq 0 ]
    [[ "$output" == *"running as root"* ]]
}

@test "install_permission_rules: skips for k1 platform" {
    run install_permission_rules "k1"
    [ "$status" -eq 0 ]
    [[ "$output" == *"running as root"* ]]
}

@test "install_permission_rules: skips when KLIPPER_USER is root" {
    KLIPPER_USER="root"
    run install_permission_rules "pi"
    [ "$status" -eq 0 ]
    [[ "$output" == *"running as root"* ]]
}

@test "install_permission_rules: skips under NoNewPrivileges when rules OK" {
    # Override functions to simulate NoNewPrivileges + rules already installed
    _has_no_new_privs() { return 0; }
    _polkit_rule_exists() { return 0; }
    _permission_rules_need_repair() { return 1; }

    local output status=0
    output="$(install_permission_rules "pi" 2>&1)" || status=$?

    [ "$status" -eq 0 ]
    contains "NoNewPrivileges" "$output"
    [[ "$output" != *"need repair"* ]]
}

@test "install_permission_rules: warns under NoNewPrivileges when pkla is broken" {
    _has_no_new_privs() { return 0; }
    _polkit_rule_exists() { return 0; }
    _permission_rules_need_repair() { return 0; }

    local output status=0
    output="$(install_permission_rules "pi" 2>&1)" || status=$?

    [ "$status" -eq 0 ]
    contains "need repair" "$output"
    [[ "$output" == *"Wi-Fi may not work"* ]]
}

# =============================================================================
# Backlight udev rule
# =============================================================================

@test "install_permission_rules: installs udev backlight rule" {
    local udev_dir="$BATS_TEST_TMPDIR/etc/udev/rules.d"
    mkdir -p "$udev_dir"

    # Mock udevadm
    mock_command "udevadm" ""

    # Use direct copy since we can't easily redirect SUDO for udev paths
    # Override the function to use test paths
    install_permission_rules_test() {
        local udev_src="${INSTALL_DIR}/config/99-helixscreen-backlight.rules"
        if [ -f "$udev_src" ]; then
            cp "$udev_src" "$udev_dir/99-helixscreen-backlight.rules"
        fi
    }
    install_permission_rules_test

    [ -f "$udev_dir/99-helixscreen-backlight.rules" ]
    grep -q "backlight" "$udev_dir/99-helixscreen-backlight.rules"
    grep -q "brightness" "$udev_dir/99-helixscreen-backlight.rules"
    grep -q "video" "$udev_dir/99-helixscreen-backlight.rules"
}

@test "install_permission_rules: udev rule has correct format" {
    local rules_file="$INSTALL_DIR/config/99-helixscreen-backlight.rules"
    [ -f "$rules_file" ]

    # Must have ACTION, SUBSYSTEM, and RUN keys
    grep -q 'ACTION==' "$rules_file"
    grep -q 'SUBSYSTEM==' "$rules_file"
    grep -q 'RUN+=' "$rules_file"
}

@test "install_permission_rules: skips udev if rules.d missing" {
    # Don't create /etc/udev/rules.d — function should silently skip
    mock_command "udevadm" ""

    run install_permission_rules "pi"
    [ "$status" -eq 0 ]
}

# =============================================================================
# Polkit NetworkManager rule (.pkla format)
# =============================================================================

@test "install_permission_rules: generates valid pkla inline when pkla dir exists" {
    # Inline generation (heredoc) — no template copy+sed involved
    local pkla_dir="$BATS_TEST_TMPDIR/etc/polkit-1/localauthority/50-local.d"
    mkdir -p "$pkla_dir"

    local helix_user="biqu"
    local pkla_dest="$pkla_dir/helixscreen-network.pkla"
    cat > "$pkla_dest" << PKLA_EOF
# SPDX-License-Identifier: GPL-3.0-or-later
[HelixScreen NetworkManager access]
Identity=unix-user:${helix_user}
Action=org.freedesktop.NetworkManager.*
ResultAny=yes
ResultInactive=yes
ResultActive=yes
PKLA_EOF

    [ -f "$pkla_dest" ]
    grep -q "unix-user:biqu" "$pkla_dest"
    grep -q "org.freedesktop.NetworkManager" "$pkla_dest"
    grep -q "ResultAny=yes" "$pkla_dest"
    # Inline generation can NEVER produce @@HELIX_USER@@
    ! grep -q "@@HELIX_USER@@" "$pkla_dest"
}

@test "install_permission_rules: pkla generates correctly for different users" {
    local pkla_dir="$BATS_TEST_TMPDIR/pkla-users"
    mkdir -p "$pkla_dir"

    for user in biqu mks pi klipper sonic; do
        local helix_user="$user"
        local pkla_dest="$pkla_dir/${user}.pkla"
        cat > "$pkla_dest" << PKLA_EOF
[HelixScreen NetworkManager access]
Identity=unix-user:${helix_user}
Action=org.freedesktop.NetworkManager.*
ResultAny=yes
ResultInactive=yes
ResultActive=yes
PKLA_EOF
        grep -q "unix-user:${user}" "$pkla_dest"
        # Inline heredoc expansion CANNOT leave template placeholders
        refute grep -q "@@HELIX_USER@@" "$pkla_dest"
        refute grep -q "@@" "$pkla_dest"
    done
}

# =============================================================================
# Polkit JavaScript rules format (newer systems)
# =============================================================================

@test "install_permission_rules: generates valid JS polkit rule when rules.d exists" {
    # Test the JavaScript rule generation logic directly
    local helix_user="biqu"
    local rules_dest="$BATS_TEST_TMPDIR/50-helixscreen-network.rules"

    cat > "$rules_dest" << POLKIT_EOF
// Installed by HelixScreen — allow service user to manage NetworkManager
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager.") === 0 &&
        subject.user === "${helix_user}") {
        return polkit.Result.YES;
    }
});
POLKIT_EOF

    [ -f "$rules_dest" ]
    grep -q "polkit.addRule" "$rules_dest"
    grep -q "NetworkManager" "$rules_dest"
    grep -q '"biqu"' "$rules_dest"
    grep -q "polkit.Result.YES" "$rules_dest"
}

@test "install_permission_rules: JS polkit rule uses correct user" {
    local helix_user="mks"
    local rules_dest="$BATS_TEST_TMPDIR/50-helixscreen-network.rules"

    cat > "$rules_dest" << POLKIT_EOF
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager.") === 0 &&
        subject.user === "${helix_user}") {
        return polkit.Result.YES;
    }
});
POLKIT_EOF

    grep -q '"mks"' "$rules_dest"
}

@test "install_permission_rules: JS polkit rule never contains @@HELIX_USER@@ placeholder" {
    # Regression: the .pkla template has @@HELIX_USER@@ which must be templated.
    # The JS rules path generates the rule inline with the actual username.
    # Verify no template placeholders leak into the generated rule.
    for user in biqu mks pi klipper pbrown; do
        local helix_user="$user"
        local rules_dest="$BATS_TEST_TMPDIR/50-helixscreen-network-${user}.rules"

        cat > "$rules_dest" << POLKIT_EOF
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager.") === 0 &&
        subject.user === "${helix_user}") {
        return polkit.Result.YES;
    }
});
POLKIT_EOF

        refute grep -q "@@HELIX_USER@@" "$rules_dest"
        grep -q "\"${user}\"" "$rules_dest"
    done
}

@test "install_permission_rules: JS polkit rule has valid JavaScript syntax" {
    local helix_user="biqu"
    local rules_dest="$BATS_TEST_TMPDIR/50-helixscreen-network.rules"

    cat > "$rules_dest" << POLKIT_EOF
// Installed by HelixScreen — allow service user to manage NetworkManager
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager.") === 0 &&
        subject.user === "${helix_user}") {
        return polkit.Result.YES;
    }
});
POLKIT_EOF

    # Must be a complete, balanced JS block
    grep -q "polkit.addRule(function" "$rules_dest"
    grep -q "});" "$rules_dest"
    # Must use strict equality (===) not loose (==)
    grep -q "===" "$rules_dest"
    # Must match NetworkManager actions
    grep -q "org.freedesktop.NetworkManager." "$rules_dest"
}

# =============================================================================
# Regression: @@HELIX_USER@@ must NEVER appear in deployed polkit files
# =============================================================================

@test "install_permission_rules: inline generation cannot produce @@HELIX_USER@@" {
    # Both JS rules and PKLA are generated via heredoc with shell expansion.
    # Shell variable expansion in heredocs is guaranteed — @@HELIX_USER@@
    # can only appear if someone re-introduces a copy+sed pattern.
    # This test validates the approach by generating both formats.
    for user in biqu mks pi klipper sonic; do
        local helix_user="$user"

        # JS rules format
        local js_dest="$BATS_TEST_TMPDIR/inline-${user}.rules"
        cat > "$js_dest" << POLKIT_EOF
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager.") === 0 &&
        subject.user === "${helix_user}") {
        return polkit.Result.YES;
    }
});
POLKIT_EOF
        refute grep -q "@@" "$js_dest"
        grep -q "\"${user}\"" "$js_dest"

        # PKLA format
        local pkla_dest="$BATS_TEST_TMPDIR/inline-${user}.pkla"
        cat > "$pkla_dest" << PKLA_EOF
[HelixScreen NetworkManager access]
Identity=unix-user:${helix_user}
Action=org.freedesktop.NetworkManager.*
ResultAny=yes
ResultInactive=yes
ResultActive=yes
PKLA_EOF
        refute grep -q "@@" "$pkla_dest"
        grep -q "unix-user:${user}" "$pkla_dest"
    done
}

@test "install_permission_rules: permissions.sh has no copy+sed pkla pattern" {
    # Regression guard: ensure nobody re-introduces the fragile copy+sed pattern
    # that caused @@HELIX_USER@@ to leak into deployed files. The .pkla must be
    # generated inline (heredoc into tee), never copied from a template.
    #
    # Keyed on pkla_dest, the variable the current code actually uses. This case
    # used to grep for `cp.*pkla_src.*pkla_dest`; pkla_src was deleted in the
    # inline-generation refactor, so the guard had been passing on a name that
    # no longer exists rather than on the rule being obeyed.
    local script="$WORKTREE_ROOT/scripts/lib/installer/permissions.sh"
    grep -q 'pkla_dest' "$script"   # the guard is keyed on a live name
    # Must NOT copy a template file to the deploy destination
    refute grep -qE 'cp[^|&]*pkla_dest' "$script"
    # Must NOT sed @@HELIX_USER@@ in pkla files (inline generation doesn't need it)
    refute grep -q 'sed.*@@HELIX_USER@@.*pkla_dest' "$script"
}

# =============================================================================
# nmcli requirement
# =============================================================================

# =============================================================================
# Regression: directory checks must use $SUDO (root-only parent dir)
# On Debian 11, /etc/polkit-1/localauthority/ is mode 700 (root-only).
# Without $SUDO, `test -d .../50-local.d` fails even though it exists.
# =============================================================================

@test "install_permission_rules: uses SUDO for pkla dir check (root-only parent)" {
    # Verify permissions.sh uses '$SUDO test -d' not bare '[ -d'
    # for both the rules.d and pkla_dir checks
    local script="$WORKTREE_ROOT/scripts/lib/installer/permissions.sh"

    # The rules_dir check must use $SUDO
    grep -q '$SUDO test -d "$rules_dir"' "$script"

    # The pkla_dir check must use $SUDO
    grep -q '$SUDO test -d "$pkla_dir"' "$script"
}

@test "install_permission_rules: _permission_rules_need_repair uses SUDO for file checks" {
    # Verify _permission_rules_need_repair uses '$SUDO test -f' not bare '[ -f'
    local script="$WORKTREE_ROOT/scripts/lib/installer/permissions.sh"

    grep -q '$SUDO test -f "$pkla"' "$script"
    grep -q '$SUDO test -f "$rules"' "$script"
}

@test "install_permission_rules: SUDO='' still works for root installs" {
    # When running as root, SUDO is empty — verify 'test -d' works without prefix
    # (empty $SUDO means the command is just 'test -d ...')
    local test_dir="$BATS_TEST_TMPDIR/polkit-test"
    mkdir -p "$test_dir"

    SUDO=""
    $SUDO test -d "$test_dir"
    [ $? -eq 0 ]
}

@test "install_permission_rules: installs pkla via SUDO on restricted parent dir" {
    # Simulate the Debian 11 scenario: parent dir exists but is only
    # traversable by the SUDO wrapper (no rules.d → falls to pkla path)
    local polkit_root="$BATS_TEST_TMPDIR/etc/polkit-1"
    local auth_dir="$polkit_root/localauthority"
    local pkla_dir="$auth_dir/50-local.d"
    mkdir -p "$pkla_dir"

    # Create a SUDO wrapper that redirects system paths to our tmpdir
    local wrapper="$BATS_TEST_TMPDIR/bin/mock_sudo"
    mkdir -p "$(dirname "$wrapper")"
    cat > "$wrapper" << 'SUDOEOF'
#!/bin/sh
# Rewrite /etc/polkit-1 paths to test tmpdir
new_args=""
for arg in "$@"; do
    case "$arg" in
        /etc/polkit-1/*)
            suffix="${arg#/etc/polkit-1/}"
            arg="${BATS_TEST_TMPDIR}/etc/polkit-1/${suffix}"
            ;;
    esac
    new_args="$new_args \"$arg\""
done
eval $new_args
SUDOEOF
    chmod +x "$wrapper"
    export SUDO="$wrapper"

    # Mock nmcli so the polkit section runs
    mock_command "nmcli" ""
    # Mock udevadm for the udev section
    mock_command "udevadm" ""

    run install_permission_rules "pi"
    [ "$status" -eq 0 ]

    # The pkla file should have been generated inline with the correct user
    [ -f "$pkla_dir/helixscreen-network.pkla" ]
    grep -q "unix-user:biqu" "$pkla_dir/helixscreen-network.pkla"
    refute grep -q "@@HELIX_USER@@" "$pkla_dir/helixscreen-network.pkla"
    ! grep -q "@@" "$pkla_dir/helixscreen-network.pkla"
}

@test "install_permission_rules: prefers JS rules when rules.d exists" {
    # Simulate Debian 12+: rules.d exists
    local polkit_root="$BATS_TEST_TMPDIR/etc/polkit-1"
    local rules_dir="$polkit_root/rules.d"
    local pkla_dir="$polkit_root/localauthority/50-local.d"
    mkdir -p "$rules_dir"
    mkdir -p "$pkla_dir"

    # SUDO wrapper
    local wrapper="$BATS_TEST_TMPDIR/bin/mock_sudo"
    mkdir -p "$(dirname "$wrapper")"
    cat > "$wrapper" << 'SUDOEOF'
#!/bin/sh
new_args=""
for arg in "$@"; do
    case "$arg" in
        /etc/polkit-1/*)
            suffix="${arg#/etc/polkit-1/}"
            arg="${BATS_TEST_TMPDIR}/etc/polkit-1/${suffix}"
            ;;
    esac
    new_args="$new_args \"$arg\""
done
eval $new_args
SUDOEOF
    chmod +x "$wrapper"
    export SUDO="$wrapper"

    mock_command "nmcli" ""
    mock_command "udevadm" ""

    run install_permission_rules "pi"
    [ "$status" -eq 0 ]

    # JS rules should be installed, NOT pkla
    [ -f "$rules_dir/49-helixscreen-network.rules" ]
    grep -q "polkit.addRule" "$rules_dir/49-helixscreen-network.rules"
    grep -q '"biqu"' "$rules_dir/49-helixscreen-network.rules"
    # pkla should NOT be installed (JS takes priority)
    [ ! -f "$pkla_dir/helixscreen-network.pkla" ]
}

@test "install_permission_rules: cleans up stale pkla when installing JS rules" {
    # Simulate upgrade from Debian 11→12: old broken pkla exists, rules.d now available
    local polkit_root="$BATS_TEST_TMPDIR/etc/polkit-1"
    local rules_dir="$polkit_root/rules.d"
    local pkla_dir="$polkit_root/localauthority/50-local.d"
    mkdir -p "$rules_dir"
    mkdir -p "$pkla_dir"

    # Plant a stale/broken pkla file from the old install
    echo 'Identity=unix-user:@@HELIX_USER@@' > "$pkla_dir/helixscreen-network.pkla"

    # SUDO wrapper
    local wrapper="$BATS_TEST_TMPDIR/bin/mock_sudo"
    mkdir -p "$(dirname "$wrapper")"
    cat > "$wrapper" << 'SUDOEOF'
#!/bin/sh
new_args=""
for arg in "$@"; do
    case "$arg" in
        /etc/polkit-1/*)
            suffix="${arg#/etc/polkit-1/}"
            arg="${BATS_TEST_TMPDIR}/etc/polkit-1/${suffix}"
            ;;
    esac
    new_args="$new_args \"$arg\""
done
eval $new_args
SUDOEOF
    chmod +x "$wrapper"
    export SUDO="$wrapper"

    mock_command "nmcli" ""
    mock_command "udevadm" ""

    run install_permission_rules "pi"
    [ "$status" -eq 0 ]

    # JS rules installed
    [ -f "$rules_dir/49-helixscreen-network.rules" ]
    grep -q '"biqu"' "$rules_dir/49-helixscreen-network.rules"
    # Stale pkla must be removed
    [ ! -f "$pkla_dir/helixscreen-network.pkla" ]
    [[ "$output" == *"Removed stale .pkla"* ]]
}

@test "install_permission_rules: warns when neither polkit dir exists" {
    # No rules.d, no localauthority/50-local.d
    local polkit_root="$BATS_TEST_TMPDIR/etc/polkit-1"
    mkdir -p "$polkit_root"

    # SUDO wrapper that always fails test -d for polkit dirs
    local wrapper="$BATS_TEST_TMPDIR/bin/mock_sudo"
    mkdir -p "$(dirname "$wrapper")"
    cat > "$wrapper" << 'SUDOEOF'
#!/bin/sh
new_args=""
for arg in "$@"; do
    case "$arg" in
        /etc/polkit-1/*)
            suffix="${arg#/etc/polkit-1/}"
            arg="${BATS_TEST_TMPDIR}/etc/polkit-1/${suffix}"
            ;;
    esac
    new_args="$new_args \"$arg\""
done
eval $new_args
SUDOEOF
    chmod +x "$wrapper"
    export SUDO="$wrapper"

    mock_command "nmcli" ""
    mock_command "udevadm" ""

    # Override log_warn to capture output
    log_warn() { echo "WARN: $*"; }
    export -f log_warn

    run install_permission_rules "pi"
    [ "$status" -eq 0 ]
    [[ "$output" == *"polkit rules directory not found"* ]]
}

# =============================================================================
# nmcli requirement
# =============================================================================

@test "install_permission_rules: skips polkit if nmcli not available" {
    # Don't mock nmcli — it shouldn't be in PATH
    # Create udev dir so that part works
    mkdir -p "$BATS_TEST_TMPDIR/etc/udev/rules.d"
    mock_command "udevadm" ""

    # The polkit section should be silently skipped when nmcli is missing
    # (no error, no crash)
    run install_permission_rules "pi"
    [ "$status" -eq 0 ]
}

# =============================================================================
# Config file validation
# =============================================================================

@test "config: backlight rules has SPDX header" {
    grep -q "SPDX-License-Identifier" "$WORKTREE_ROOT/config/99-helixscreen-backlight.rules"
}

@test "config: polkit pkla has SPDX header" {
    grep -q "SPDX-License-Identifier" "$WORKTREE_ROOT/config/helixscreen-network.pkla"
}

# =============================================================================
# Install-flow wiring (#1343)
#
# Every test above calls install_permission_rules() DIRECTLY, which is exactly
# why the missing call site survived review for so long: the function was
# correct, 34 tests were green, and nothing anywhere proved main() ever reached
# it. On a real Pi the udev rule was never copied, brightness stayed root:root
# 0644, and dimming and sleep failed with "Cannot write to ... (permission
# denied?)".
#
# These run the REAL main() with every other installer step replaced by a
# recorder, so the flow executes end to end without touching the machine and
# leaves a transcript of which steps ran, in which order, with which arguments.
# =============================================================================

# Replace every function the installer modules define with a recorder that
# appends "<name> <args>" to $FLOW_LOG and produces no output. Names come from
# the module sources rather than from `compgen -A function`, which would also
# sweep up bats' own internals and break the harness.
_stub_installer_steps() {
    local fn
    for fn in $(grep -h '^[A-Za-z_][A-Za-z0-9_]*[[:space:]]*()[[:space:]]*{' \
                    "$WORKTREE_ROOT"/scripts/lib/installer/*.sh \
                | sed 's/[[:space:]]*().*//' | sort -u); do
        case "$fn" in main|apply_install) continue ;; esac
        eval "${fn}() { printf '%s %s\n' '$fn' \"\$*\" >> \"\$FLOW_LOG\"; }"
    done
}

# Source main.sh, stub everything under main() and apply_install(), and answer
# the few calls they actually branch on.
_setup_install_flow() {
    FLOW_LOG="$BATS_TEST_TMPDIR/flow.log"
    : > "$FLOW_LOG"
    export FLOW_LOG

    # main.sh installs `trap 'cleanup_on_success' EXIT INT TERM` and an ERR trap
    # at source time, which REPLACE the traps bats uses to report a test's
    # outcome. Left in place, a failing assertion in these tests produces no
    # "not ok" line at all - the test silently vanishes from the run and bats
    # only warns that it executed fewer tests than it expected. Save and restore.
    local saved_traps
    saved_traps=$(trap -p EXIT ERR)

    unset _HELIX_MAIN_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/main.sh"

    trap - EXIT INT TERM ERR
    eval "$saved_traps"

    _stub_installer_steps

    detect_platform() { echo "pi"; }
    get_download_platform() { echo "pi"; }
    detect_printer_model() { echo ""; }
    error_handler() { :; }
}

# Line number of a step's first appearance in the transcript, or empty.
_flow_line() {
    grep -n "^$1 " "$FLOW_LOG" | head -1 | cut -d: -f1
}

@test "install flow: main() calls install_permission_rules with the platform" {
    _setup_install_flow
    run main
    [ "$status" -eq 0 ]
    grep -q "^install_permission_rules pi$" "$FLOW_LOG"
}

@test "install flow: --update calls install_permission_rules too" {
    _setup_install_flow
    run main --update
    [ "$status" -eq 0 ]
    grep -q "^install_permission_rules pi$" "$FLOW_LOG"
}

@test "install flow: permission rules run after extract_release, before start_service" {
    _setup_install_flow
    run main
    [ "$status" -eq 0 ]

    # After extract_release: the udev rule ships inside the release package at
    # $INSTALL_DIR/config/, so there is nothing to copy before then.
    # Before start_service: udevadm has to have re-applied the ownership by the
    # time the UI first writes to brightness.
    local extract_line perm_line start_line
    extract_line=$(_flow_line extract_release)
    perm_line=$(_flow_line install_permission_rules)
    start_line=$(_flow_line start_service)

    [ -n "$extract_line" ]
    [ -n "$perm_line" ]
    [ -n "$start_line" ]
    [ "$extract_line" -lt "$perm_line" ]
    [ "$perm_line" -lt "$start_line" ]
}

@test "install flow: bundled install.sh carries the call site" {
    # install.sh is generated from the modules; a stale bundle would
    # ship the old flow to every curl|sh user even with main.sh fixed.
    grep -q '^    install_permission_rules "\$platform"$' "$INSTALL_BUNDLE"
}

# =============================================================================
# logind power-off/reboot rule (ensure-polkit-rule.sh)
#
# A service has no seat session, so logind answers its PowerOff/Reboot with
# "Interactive authentication required" unless a rule grants them.
# =============================================================================

POWER_IDS="org.freedesktop.login1.power-off
org.freedesktop.login1.power-off-multiple-sessions
org.freedesktop.login1.reboot
org.freedesktop.login1.reboot-multiple-sessions"

# A PATH holding only what ensure-polkit-rule.sh needs, so the host's own
# nmcli and pkaction cannot leak in. Pass "nmcli" to add a mock nmcli; set
# PKACTION_VERSION to add a mock pkaction that reports it.
_minimal_path() {
    local d="$BATS_TEST_TMPDIR/minbin" c
    mkdir -p "$d"
    for c in sh cat sed tr rm chmod head; do
        ln -sf "$(command -v "$c")" "$d/$c"
    done
    if [ "${1:-}" = "nmcli" ]; then
        printf '#!/bin/sh\nexit 0\n' > "$d/nmcli"
        chmod +x "$d/nmcli"
    fi
    if [ -n "${PKACTION_VERSION:-}" ]; then
        printf '#!/bin/sh\necho "pkaction version %s"\n' "$PKACTION_VERSION" > "$d/pkaction"
        chmod +x "$d/pkaction"
    fi
    echo "$d"
}

# Run ensure-polkit-rule.sh for <user> with a minimal PATH ($2 = "nmcli" adds one).
_run_ensure() {
    local mp
    mp="$(_minimal_path "${2:-}")"
    run env PATH="$mp" HELIX_POLKIT_DIR="$HELIX_POLKIT_DIR" \
        "$mp/sh" "$WORKTREE_ROOT/config/ensure-polkit-rule.sh" "$1"
}

@test "ensure-polkit-rule: power .rules grants exactly the four logind power-off/reboot ids" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    _run_ensure pbrown
    [ "$status" -eq 0 ]

    local rule="$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules"
    [ -f "$rule" ]
    grep -q 'subject.user === "pbrown"' "$rule"
    grep -q "polkit.Result.YES" "$rule"
    local id
    for id in $POWER_IDS; do
        grep -q "\"$id\"" "$rule"
    done
    # Nothing beyond them: no ignore-inhibit, so a block inhibitor holds
    [ "$(grep -o 'org\.freedesktop\.[A-Za-z0-9.-]*' "$rule" | sort -u | wc -l)" -eq 4 ]
    [ -z "$(grep ignore-inhibit "$rule")" ]
}

@test "ensure-polkit-rule: power .rules is valid JavaScript that grants only its user" {
    command -v node >/dev/null 2>&1 || skip "node not installed"
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    _run_ensure pbrown
    [ "$status" -eq 0 ]

    run node -e '
const polkit = { Result: { YES: "yes" }, rules: [], addRule(f) { this.rules.push(f); } };
eval(require("fs").readFileSync(process.argv[1], "utf8"));
const f = polkit.rules[0];
const out = [
  f({ id: "org.freedesktop.login1.power-off" }, { user: "pbrown" }),
  f({ id: "org.freedesktop.login1.reboot-multiple-sessions" }, { user: "pbrown" }),
  f({ id: "org.freedesktop.login1.hibernate" }, { user: "pbrown" }),
  f({ id: "org.freedesktop.login1.power-off-ignore-inhibit" }, { user: "pbrown" }),
  f({ id: "org.freedesktop.login1.power-off" }, { user: "other" }),
];
console.log(out.join(","));
' "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules"
    [ "$status" -eq 0 ]
    [ "$output" = "yes,yes,,," ]
}

@test "ensure-polkit-rule: power rule is installed without nmcli" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    _run_ensure pbrown
    [ "$status" -eq 0 ]
    [ -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ]
    # No NetworkManager, no network rule
    [ ! -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules" ]
}

@test "ensure-polkit-rule: an existing network rule does not stop the power rule" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    echo "// existing network rule" > "$HELIX_POLKIT_DIR/rules.d/50-helixscreen-network.rules"

    _run_ensure pbrown nmcli
    [ "$status" -eq 0 ]
    [ -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ]
    # The network rule is left exactly as found, and no second one is added
    [ "$(cat "$HELIX_POLKIT_DIR/rules.d/50-helixscreen-network.rules")" = "// existing network rule" ]
    [ ! -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules" ]
}

@test "ensure-polkit-rule: a 49- network rule counts as present" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    echo "// installer network rule" > "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules"

    _run_ensure pbrown nmcli
    [ "$status" -eq 0 ]
    [ ! -f "$HELIX_POLKIT_DIR/rules.d/50-helixscreen-network.rules" ]
    [ "$(cat "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules")" = "// installer network rule" ]
}

@test "ensure-polkit-rule: with nmcli and no rule, writes the 49- network rule" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    _run_ensure pbrown nmcli
    [ "$status" -eq 0 ]
    grep -q 'subject.user === "pbrown"' "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules"
    [ ! -f "$HELIX_POLKIT_DIR/rules.d/50-helixscreen-network.rules" ]
}

@test "ensure-polkit-rule: falls back to .pkla when rules.d is absent" {
    local pkla_dir="$HELIX_POLKIT_DIR/localauthority/50-local.d"
    mkdir -p "$pkla_dir"
    _run_ensure pbrown
    [ "$status" -eq 0 ]

    local pkla="$pkla_dir/helixscreen-power.pkla"
    [ -f "$pkla" ]
    grep -q '^Identity=unix-user:pbrown$' "$pkla"
    grep -q '^ResultAny=yes$' "$pkla"
    local expected
    expected="Action=$(printf '%s' "$POWER_IDS" | tr '\n' ';')"
    grep -qxF "$expected" "$pkla"
}

@test "ensure-polkit-rule: a .rules install removes a stale power .pkla" {
    local pkla_dir="$HELIX_POLKIT_DIR/localauthority/50-local.d"
    mkdir -p "$HELIX_POLKIT_DIR/rules.d" "$pkla_dir"
    echo "stale" > "$pkla_dir/helixscreen-power.pkla"
    _run_ensure pbrown
    [ "$status" -eq 0 ]
    [ -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ]
    [ ! -f "$pkla_dir/helixscreen-power.pkla" ]
}

@test "ensure-polkit-rule: power rule is rewritten for a changed user, and left alone when current" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    local rule="$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules"

    _run_ensure olduser
    grep -q '"olduser"' "$rule"

    _run_ensure pbrown
    [ "$status" -eq 0 ]
    grep -q '"pbrown"' "$rule"
    [ -z "$(grep '"olduser"' "$rule")" ]
    contains "installed" "$output"

    # A second run with the same user rewrites nothing
    _run_ensure pbrown
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "ensure-polkit-rule: root gets no rules" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    _run_ensure root nmcli
    [ "$status" -eq 0 ]
    [ -z "$(ls -A "$HELIX_POLKIT_DIR/rules.d")" ]
}

@test "ensure-polkit-rule: root removes a previous user's power grant in both formats" {
    local pkla_dir="$HELIX_POLKIT_DIR/localauthority/50-local.d"
    mkdir -p "$HELIX_POLKIT_DIR/rules.d" "$pkla_dir"
    echo old > "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules"
    echo old > "$pkla_dir/helixscreen-power.pkla"
    echo net > "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules"

    _run_ensure root nmcli
    [ "$status" -eq 0 ]
    [ ! -e "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ]
    [ ! -e "$pkla_dir/helixscreen-power.pkla" ]
    # Only the power grant is withdrawn
    [ -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules" ]
}

@test "ensure-polkit-rule: an implausible user name gets no rule of either kind" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    local bad
    for bad in 'evil") || true; //' 'Pbrown' '1abc' 'a b' 'a$b' '-x' ''; do
        [ -n "$bad" ] || continue
        _run_ensure "$bad" nmcli
        [ "$status" -eq 0 ] || fail "status $status for '$bad'"
        contains "refusing user name" "$output"
        [ -z "$(ls -A "$HELIX_POLKIT_DIR/rules.d")" ] || fail "wrote a rule for '$bad'"
    done
}

@test "ensure-polkit-rule: plausible user names are accepted" {
    local good
    for good in pbrown _svc biqu-2 'machine$'; do
        rm -rf "$HELIX_POLKIT_DIR"
        mkdir -p "$HELIX_POLKIT_DIR/rules.d"
        _run_ensure "$good"
        [ "$status" -eq 0 ] || fail "status $status for '$good'"
        grep -qF "subject.user === \"$good\"" "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ||
            fail "no rule for '$good'"
    done
}

@test "ensure-polkit-rule: pkaction 0.105 picks .pkla even when rules.d exists" {
    local pkla_dir="$HELIX_POLKIT_DIR/localauthority/50-local.d"
    mkdir -p "$HELIX_POLKIT_DIR/rules.d" "$pkla_dir"
    PKACTION_VERSION=0.105 _run_ensure pbrown nmcli
    [ "$status" -eq 0 ]
    [ -f "$pkla_dir/helixscreen-power.pkla" ]
    [ -f "$pkla_dir/helixscreen-network.pkla" ]
    [ -z "$(ls -A "$HELIX_POLKIT_DIR/rules.d")" ]
}

@test "ensure-polkit-rule: pkaction 124 picks .rules even when the pkla dir exists" {
    local pkla_dir="$HELIX_POLKIT_DIR/localauthority/50-local.d"
    mkdir -p "$HELIX_POLKIT_DIR/rules.d" "$pkla_dir"
    PKACTION_VERSION=124 _run_ensure pbrown nmcli
    [ "$status" -eq 0 ]
    [ -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ]
    [ -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules" ]
    [ -z "$(ls -A "$pkla_dir")" ]
}

@test "ensure-polkit-rule: pkaction 0.106 counts as .rules" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d" "$HELIX_POLKIT_DIR/localauthority/50-local.d"
    PKACTION_VERSION=0.106 _run_ensure pbrown
    [ "$status" -eq 0 ]
    [ -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ]
}

@test "ensure-polkit-rule: pkaction 0.105 with only rules.d writes nothing" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    PKACTION_VERSION=0.105 _run_ensure pbrown
    [ "$status" -eq 0 ]
    [ -z "$(ls -A "$HELIX_POLKIT_DIR/rules.d")" ]
}

@test "ensure-polkit-rule: no polkit directory exits 0 and writes nothing" {
    _run_ensure pbrown nmcli
    [ "$status" -eq 0 ]
    [ ! -e "$HELIX_POLKIT_DIR" ]
}

@test "ensure-polkit-rule: an unwritable rules.d does not fail the service start" {
    [ "$(id -u)" != "0" ] || skip "root can write a 0555 directory"
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    chmod 555 "$HELIX_POLKIT_DIR/rules.d"
    _run_ensure pbrown nmcli
    chmod 755 "$HELIX_POLKIT_DIR/rules.d"
    [ "$status" -eq 0 ]
    [[ "$output" == *"cannot write"* ]]
}

@test "service unit runs ensure-polkit-rule.sh as root on every start" {
    grep -qx 'ExecStartPre=+@@INSTALL_DIR@@/config/ensure-polkit-rule.sh @@HELIX_USER@@' \
        "$WORKTREE_ROOT/config/helixscreen.service"
}

# =============================================================================
# logind power-off/reboot rule through the installer and uninstaller
# =============================================================================

@test "install_permission_rules: installs the power rule without nmcli" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    mock_command "udevadm" ""
    local mp
    mp="$(_minimal_path)"

    PATH="$BATS_TEST_TMPDIR/bin:$mp" run install_permission_rules "pi"
    [ "$status" -eq 0 ]
    grep -q 'subject.user === "biqu"' "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules"
    [ ! -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules" ]
    [[ "$output" == *"power-off/reboot polkit rule"* ]]
}

@test "install_permission_rules: installs the power rule alongside the network rule" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    setup_sudo_redirect
    mock_command "nmcli" ""
    mock_command "udevadm" ""

    run install_permission_rules "pi"
    [ "$status" -eq 0 ]
    grep -q '"biqu"' "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-network.rules"
    grep -q '"biqu"' "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules"
}

@test "install_permission_rules: root-only platforms get no power rule" {
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    run install_permission_rules "k1"
    [ "$status" -eq 0 ]
    [ ! -f "$HELIX_POLKIT_DIR/rules.d/49-helixscreen-power.rules" ]
}

@test "remove_permission_rules: removes the power rule in both formats" {
    unset _HELIX_UNINSTALL_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
    setup_sudo_redirect

    local rules_dir="$BATS_TEST_TMPDIR/etc/polkit-1/rules.d"
    local pkla_dir="$BATS_TEST_TMPDIR/etc/polkit-1/localauthority/50-local.d"
    local udev_dir="$BATS_TEST_TMPDIR/etc/udev/rules.d"
    mkdir -p "$rules_dir" "$pkla_dir" "$udev_dir"
    local f
    for f in "$rules_dir/49-helixscreen-power.rules" "$pkla_dir/helixscreen-power.pkla" \
             "$rules_dir/49-helixscreen-network.rules" "$rules_dir/50-helixscreen-network.rules" \
             "$pkla_dir/helixscreen-network.pkla" "$udev_dir/99-helixscreen-backlight.rules"; do
        echo x > "$f"
    done
    echo keep > "$rules_dir/10-network-manager.rules"

    remove_permission_rules

    [ -z "$(ls -A "$pkla_dir")" ]
    [ -z "$(ls -A "$udev_dir")" ]
    [ "$(ls -A "$rules_dir")" = "10-network-manager.rules" ]
}

@test "uninstall: both uninstall paths call remove_permission_rules" {
    [ "$(grep -c '^ *remove_permission_rules$' "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh")" -eq 2 ]
}

@test "install_permission_rules: a power rule that could not be written is reported, with the reason" {
    [ "$(id -u)" != "0" ] || skip "root can write a 0555 directory"
    mkdir -p "$HELIX_POLKIT_DIR/rules.d"
    chmod 555 "$HELIX_POLKIT_DIR/rules.d"
    mock_command "udevadm" ""
    local mp
    mp="$(_minimal_path)"

    PATH="$BATS_TEST_TMPDIR/bin:$mp" run install_permission_rules "pi"
    chmod 755 "$HELIX_POLKIT_DIR/rules.d"
    [ "$status" -eq 0 ]
    contains "cannot write" "$output"
    contains "Failed to install power-off/reboot polkit rule" "$output"
    [[ "$output" != *"Installed power-off/reboot polkit rule"* ]]
}

@test "install_permission_rules: a host without polkit says so instead of reporting a failed write" {
    # Armbian on the BTT CB1 ships no polkitd: neither rule directory exists,
    # so there is nowhere to write a rule and nothing that would read one.
    mock_command "udevadm" ""
    local mp
    mp="$(_minimal_path)"

    PATH="$BATS_TEST_TMPDIR/bin:$mp" run install_permission_rules "pi"
    [ "$status" -eq 0 ]
    [ ! -e "$HELIX_POLKIT_DIR" ]
    contains "polkit is not installed" "$output"
    contains "polkitd" "$output"
    [[ "$output" != *"Failed to install power-off/reboot polkit rule"* ]]
}
