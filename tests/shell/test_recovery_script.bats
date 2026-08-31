#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/lib/installer/recovery.sh —
# per-platform helix-recover.sh generation, installation, and
# legacy [shell_command helix_recover] migration sweep.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_MOONRAKER_SOURCED _HELIX_RECOVERY_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/recovery.sh"

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    mkdir -p "$INSTALL_DIR/bin"
}

# =============================================================================
# helix_recover_script_for_platform: per-platform body content
# =============================================================================

@test "k2 snippet bounces klipper_mcu then klipper" {
    run helix_recover_script_for_platform k2
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "^#!/bin/sh"
    echo "$output" | grep -q "/etc/init.d/klipper_mcu restart"
    echo "$output" | grep -q "/etc/init.d/klipper restart"
}

@test "snapmaker-u1 snippet uses S60klipper" {
    run helix_recover_script_for_platform snapmaker-u1
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "^#!/bin/sh"
    echo "$output" | grep -q "/etc/init.d/S60klipper restart"
}

@test "k1 snippet uses S57klipper_mcu + S55klipper_service" {
    run helix_recover_script_for_platform k1
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "S57klipper_mcu restart"
    echo "$output" | grep -q "S55klipper_service restart"
}

@test "cc1 snippet uses /etc/init.d/klipper" {
    run helix_recover_script_for_platform cc1
    [ "$status" -eq 0 ]
    # plain /etc/init.d/klipper restart (no S## prefix on COSMOS)
    echo "$output" | grep -qE "exec /etc/init\\.d/klipper restart"
}

@test "ad5m snippet probes for firmware-specific paths" {
    run helix_recover_script_for_platform ad5m
    [ "$status" -eq 0 ]
    # Three known klipper-start paths on AD5M:
    echo "$output" | grep -q "/opt/config/mod/.shell/restart_klipper.sh"
    echo "$output" | grep -q "/etc/init.d/S55klipper_service"
    echo "$output" | grep -q "/root/printer_software/klipper/scripts/klipper-restart.sh"
    # Exits non-zero if none matched, so helix-screen surfaces an error toast
    echo "$output" | grep -qE "exit 1"
}

@test "ad5x routes to ad5m snippet (same firmware family)" {
    local ad5m ad5x
    ad5m=$(helix_recover_script_for_platform ad5m)
    ad5x=$(helix_recover_script_for_platform ad5x)
    [ "$ad5m" = "$ad5x" ]
}

@test "stock systemd platforms emit no snippet (pi/pi32/x86)" {
    for p in pi pi32 x86 unknown ""; do
        run helix_recover_script_for_platform "$p"
        [ "$status" -eq 0 ]
        [ -z "$output" ]
    done
}

@test "every snippet has the safety header (#!/bin/sh + set -u)" {
    for p in k2 k1 cc1 ad5m ad5x snapmaker-u1; do
        run helix_recover_script_for_platform "$p"
        [ "$status" -eq 0 ]
        [ -n "$output" ]
        # First line must be the shebang
        first_line=$(printf '%s\n' "$output" | head -1)
        [ "$first_line" = "#!/bin/sh" ]
        echo "$output" | grep -q "^set -u"
    done
}

# =============================================================================
# install_recovery_script: file write semantics
# =============================================================================

@test "install_recovery_script writes executable file at INSTALL_DIR/bin/helix-recover.sh" {
    install_recovery_script "$INSTALL_DIR" snapmaker-u1
    [ -x "$INSTALL_DIR/bin/helix-recover.sh" ]
    head -1 "$INSTALL_DIR/bin/helix-recover.sh" | grep -q "^#!/bin/sh"
    grep -q "S60klipper" "$INSTALL_DIR/bin/helix-recover.sh"
}

@test "install_recovery_script is idempotent (re-run produces same content)" {
    install_recovery_script "$INSTALL_DIR" k2
    local first
    first=$(sha256sum "$INSTALL_DIR/bin/helix-recover.sh" | awk '{print $1}')

    install_recovery_script "$INSTALL_DIR" k2
    local second
    second=$(sha256sum "$INSTALL_DIR/bin/helix-recover.sh" | awk '{print $1}')

    [ "$first" = "$second" ]
}

@test "install_recovery_script switching platforms overwrites the script" {
    install_recovery_script "$INSTALL_DIR" k2
    grep -q "klipper_mcu" "$INSTALL_DIR/bin/helix-recover.sh"

    install_recovery_script "$INSTALL_DIR" snapmaker-u1
    grep -q "S60klipper" "$INSTALL_DIR/bin/helix-recover.sh"
    # K2-specific content must be gone after the switch:
    ! grep -q "klipper_mcu" "$INSTALL_DIR/bin/helix-recover.sh"
}

@test "install_recovery_script no-op on platforms without a snippet (pi)" {
    install_recovery_script "$INSTALL_DIR" pi
    [ ! -f "$INSTALL_DIR/bin/helix-recover.sh" ]
}

@test "install_recovery_script rejects empty platform" {
    run install_recovery_script "$INSTALL_DIR" ""
    [ "$status" -ne 0 ]
    [ ! -f "$INSTALL_DIR/bin/helix-recover.sh" ]
}

@test "install_recovery_script rejects empty install_dir" {
    run install_recovery_script "" snapmaker-u1
    [ "$status" -ne 0 ]
}

# =============================================================================
# remove_recovery_script
# =============================================================================

@test "remove_recovery_script deletes installed script" {
    install_recovery_script "$INSTALL_DIR" cc1
    [ -f "$INSTALL_DIR/bin/helix-recover.sh" ]

    remove_recovery_script "$INSTALL_DIR"
    [ ! -f "$INSTALL_DIR/bin/helix-recover.sh" ]
}

@test "remove_recovery_script is a no-op when script absent" {
    run remove_recovery_script "$INSTALL_DIR"
    [ "$status" -eq 0 ]
}

# =============================================================================
# remove_legacy_moonraker_block: migration sweep
# =============================================================================

@test "remove_legacy_moonraker_block strips dead [shell_command helix_recover] block" {
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    cat > "$conf" <<'EOF'
[server]
host: 0.0.0.0

[shell_command helix_recover]
command: /etc/init.d/klipper restart
timeout: 60
verbose: True

[update_manager helixscreen]
type: web
EOF

    remove_legacy_moonraker_block "$conf"

    # Block gone:
    refute grep -q "\[shell_command helix_recover\]" "$conf"
    refute grep -q "/etc/init.d/klipper restart" "$conf"
    # Surrounding sections preserved:
    grep -q "\[server\]" "$conf"
    grep -q "\[update_manager helixscreen\]" "$conf"
}

@test "remove_legacy_moonraker_block is a no-op when block absent" {
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    cat > "$conf" <<'EOF'
[server]
host: 0.0.0.0
EOF
    local before
    before=$(sha256sum "$conf" | awk '{print $1}')

    remove_legacy_moonraker_block "$conf"

    local after
    after=$(sha256sum "$conf" | awk '{print $1}')
    [ "$before" = "$after" ]
}

@test "remove_legacy_moonraker_block tolerates missing conf file" {
    run remove_legacy_moonraker_block "$BATS_TEST_TMPDIR/nonexistent.conf"
    [ "$status" -eq 0 ]
}
