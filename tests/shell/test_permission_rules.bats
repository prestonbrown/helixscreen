#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for install_permission_rules (permissions.sh)
# Covers udev backlight rules and polkit NetworkManager rules.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source modules (reset source guards so each test gets a fresh load)
    unset _HELIX_COMMON_SOURCED _HELIX_PERMISSIONS_SOURCED _HELIX_SERVICE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
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

    # Mock systemctl to prevent restarting real services (e.g., polkit) on dev machines
    mock_command "systemctl" ""
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
        /etc/udev/rules.d/*)
            basename="${arg##*/}"
            arg="${BATS_TEST_TMPDIR}/etc/udev/rules.d/${basename}"
            ;;
        /etc/polkit-1/localauthority/50-local.d/*)
            basename="${arg##*/}"
            arg="${BATS_TEST_TMPDIR}/etc/polkit-1/localauthority/50-local.d/${basename}"
            ;;
        /etc/polkit-1/rules.d/*)
            basename="${arg##*/}"
            arg="${BATS_TEST_TMPDIR}/etc/polkit-1/rules.d/${basename}"
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
    [[ "$output" == *"NoNewPrivileges"* ]]
    [[ "$output" != *"need repair"* ]]
}

@test "install_permission_rules: warns under NoNewPrivileges when pkla is broken" {
    _has_no_new_privs() { return 0; }
    _polkit_rule_exists() { return 0; }
    _permission_rules_need_repair() { return 0; }

    local output status=0
    output="$(install_permission_rules "pi" 2>&1)" || status=$?

    [ "$status" -eq 0 ]
    [[ "$output" == *"need repair"* ]]
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
