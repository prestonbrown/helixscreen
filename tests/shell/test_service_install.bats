#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for service management (service.sh)
# Covers install/stop/hooks/ownership for services.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source modules (reset source guards so each test gets a fresh load)
    unset _HELIX_COMMON_SOURCED _HELIX_SERVICE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"

    # Set required globals
    export INIT_SYSTEM="systemd"
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
    export SERVICE_NAME="helixscreen"
    export SUDO=""
    export KLIPPER_USER=""
    export KLIPPER_HOME=""
    export HELIX_INIT_SCRIPTS=""
    export HELIX_PROCESSES="helix-screen helix-splash helix-watchdog"
    export PLATFORM=""

    # Create fake /etc/systemd/system in tmpdir for systemd tests
    export FAKE_SYSTEMD_DIR="$BATS_TEST_TMPDIR/etc/systemd/system"
    mkdir -p "$FAKE_SYSTEMD_DIR"

    mkdir -p "$INSTALL_DIR/config" "$INSTALL_DIR/bin"
    mkdir -p "$(dirname "$INIT_SCRIPT_DEST")"
}

# Helper: create a SUDO wrapper that rewrites /etc/systemd/system/ paths to tmpdir
# This avoids mocking cp/sed (which causes infinite recursion)
setup_sudo_redirect() {
    local wrapper="$BATS_TEST_TMPDIR/bin/sudo_redirect"
    mkdir -p "$(dirname "$wrapper")"
    cat > "$wrapper" << 'SUDOEOF'
#!/bin/sh
# Rewrite /etc/systemd/system/ paths to test tmpdir
new_args=""
for arg in "$@"; do
    case "$arg" in
        /etc/systemd/system/*)
            basename="${arg##*/}"
            arg="${FAKE_SYSTEMD_DIR}/${basename}"
            ;;
    esac
    new_args="$new_args \"$arg\""
done
eval $new_args
SUDOEOF
    chmod +x "$wrapper"
    SUDO="$wrapper"
}

# Helper: create a systemd service template in the install config dir
create_service_template() {
    cat > "$INSTALL_DIR/config/helixscreen.service" << 'EOF'
[Unit]
Description=HelixScreen UI
After=network.target

[Service]
User=@@HELIX_USER@@
Group=@@HELIX_GROUP@@
WorkingDirectory=@@INSTALL_DIR@@
ExecStart=@@INSTALL_DIR@@/bin/helix-screen

[Install]
WantedBy=multi-user.target
EOF
}

# Helper: create a SysV init template in the install config dir
create_init_template() {
    cat > "$INSTALL_DIR/config/helixscreen.init" << 'INITEOF'
#!/bin/sh
DAEMON_DIR="/opt/helixscreen"
case "$1" in
    start) "$DAEMON_DIR/bin/helix-screen" & ;;
    stop)  killall helix-screen 2>/dev/null ;;
    status) pidof helix-screen >/dev/null ;;
    *) echo "Usage: $0 {start|stop|status}" ;;
esac
INITEOF
    chmod +x "$INSTALL_DIR/config/helixscreen.init"
}

# =============================================================================
# install_service_systemd
# =============================================================================

@test "install_service_systemd: copies and templates service file" {
    create_service_template
    mock_command_script "systemctl" 'exit 0'
    setup_sudo_redirect
    KLIPPER_USER="biqu"

    install_service_systemd

    [ -f "$FAKE_SYSTEMD_DIR/helixscreen.service" ]
}

@test "install_service_systemd: missing template exits with error" {
    rm -f "$INSTALL_DIR/config/helixscreen.service"

    run install_service_systemd
    [ "$status" -ne 0 ]
}

@test "install_service_systemd: daemon-reload failure exits with error" {
    create_service_template
    setup_sudo_redirect
    mock_command_fail "systemctl"

    run install_service_systemd
    [ "$status" -ne 0 ]
}

@test "install_service_systemd: replaces @@HELIX_USER@@ and @@INSTALL_DIR@@" {
    create_service_template
    mock_command_script "systemctl" 'exit 0'
    setup_sudo_redirect
    KLIPPER_USER="biqu"

    install_service_systemd

    local dest="$FAKE_SYSTEMD_DIR/helixscreen.service"
    [ -f "$dest" ]
    grep -q "User=biqu" "$dest"
    grep -q "Group=biqu" "$dest"
    grep -q "ExecStart=$INSTALL_DIR/bin/helix-screen" "$dest"
}

@test "install_service_systemd: KLIPPER_USER sets the service user" {
    create_service_template
    mock_command_script "systemctl" 'exit 0'
    setup_sudo_redirect
    KLIPPER_USER="mks"

    install_service_systemd

    local dest="$FAKE_SYSTEMD_DIR/helixscreen.service"
    grep -q "User=mks" "$dest"
    grep -q "Group=mks" "$dest"
}

# =============================================================================
# install_service_sysv
# =============================================================================

@test "install_service_sysv: copies init script and sets executable" {
    create_init_template

    install_service_sysv

    [ -f "$INIT_SCRIPT_DEST" ]
    [ -x "$INIT_SCRIPT_DEST" ]
}

@test "install_service_sysv: missing init template exits with error" {
    rm -f "$INSTALL_DIR/config/helixscreen.init"

    run install_service_sysv
    [ "$status" -ne 0 ]
}

@test "install_service_sysv: DAEMON_DIR is updated to INSTALL_DIR" {
    create_init_template

    install_service_sysv

    grep -q "DAEMON_DIR=\"$INSTALL_DIR\"" "$INIT_SCRIPT_DEST"
}

@test "install_service_sysv: uses correct INIT_SCRIPT_DEST" {
    create_init_template
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S80helixscreen"
    mkdir -p "$(dirname "$INIT_SCRIPT_DEST")"

    install_service_sysv

    [ -f "$BATS_TEST_TMPDIR/etc/init.d/S80helixscreen" ]
}

# =============================================================================
# Self-update: stale PLATFORM_HOOKS path migration
# =============================================================================

# Helper: seed an already-installed init script at INIT_SCRIPT_DEST with
# the pre-2026-04-20 buggy PLATFORM_HOOKS path (wrong 'assets/config/' prefix).
seed_installed_buggy_init() {
    cat > "$INIT_SCRIPT_DEST" << 'BUGEOF'
#!/bin/sh
DAEMON_DIR="/opt/helixscreen"
PLATFORM_HOOKS="${DAEMON_DIR}/assets/config/platform/hooks.sh"
if [ -f "$PLATFORM_HOOKS" ]; then
    . "$PLATFORM_HOOKS"
fi
# platform-specific customization that must survive migration
platform_pre_start() { echo "zmod-custom" > /tmp/marker; }
BUGEOF
    chmod +x "$INIT_SCRIPT_DEST"
}

@test "install_service_sysv: self-update migrates stale PLATFORM_HOOKS path" {
    create_init_template   # fresh tarball (unused on self-update path)
    seed_installed_buggy_init
    HELIX_SELF_UPDATE=1

    install_service_sysv

    # Bad substring replaced
    refute grep -q 'assets/config/platform/hooks\.sh' "$INIT_SCRIPT_DEST"
    # Correct path written
    grep -q 'PLATFORM_HOOKS="\${DAEMON_DIR}/platform/hooks.sh"' "$INIT_SCRIPT_DEST"
}

@test "install_service_sysv: self-update preserves platform customizations" {
    create_init_template
    seed_installed_buggy_init
    HELIX_SELF_UPDATE=1

    install_service_sysv

    # Unrelated customization (e.g., ZMOD's platform_pre_start override) survives
    grep -q 'zmod-custom' "$INIT_SCRIPT_DEST"
    # Init script still executable after migration
    [ -x "$INIT_SCRIPT_DEST" ]
}

@test "install_service_sysv: self-update is a no-op when already migrated" {
    # Seed an init script that already has the correct path — migration
    # should not touch it, and certainly must not duplicate or corrupt it.
    cat > "$INIT_SCRIPT_DEST" << 'OKEOF'
#!/bin/sh
DAEMON_DIR="/opt/helixscreen"
PLATFORM_HOOKS="${DAEMON_DIR}/platform/hooks.sh"
OKEOF
    chmod +x "$INIT_SCRIPT_DEST"
    local before
    before=$(md5sum "$INIT_SCRIPT_DEST" | awk '{print $1}')
    HELIX_SELF_UPDATE=1

    install_service_sysv

    local after
    after=$(md5sum "$INIT_SCRIPT_DEST" | awk '{print $1}')
    [ "$before" = "$after" ]
}

@test "install_service_sysv: self-update does not overwrite init script from tarball" {
    # Core #314 guarantee: self-update must NOT clobber the installed init
    # script with the fresh tarball copy, even while doing the surgical sed.
    create_init_template
    seed_installed_buggy_init
    HELIX_SELF_UPDATE=1

    install_service_sysv

    # The tarball template has "Usage:" text; the installed (seeded) file does not.
    ! grep -q 'Usage: \$0' "$INIT_SCRIPT_DEST"
}

@test "install_service_sysv: fresh install does not need migration path" {
    # On a fresh install (no HELIX_SELF_UPDATE), the full tarball copy runs
    # and the fixed path is already present — no buggy substring should exist.
    create_init_template
    unset HELIX_SELF_UPDATE

    install_service_sysv

    ! grep -q 'assets/config/platform/hooks\.sh' "$INIT_SCRIPT_DEST"
}

# =============================================================================
# install_service (dispatcher)
# =============================================================================

@test "install_service: INIT_SYSTEM=systemd calls systemd path" {
    INIT_SYSTEM="systemd"
    create_service_template
    mock_command_script "systemctl" 'exit 0'
    setup_sudo_redirect

    install_service "pi"

    [ -f "$FAKE_SYSTEMD_DIR/helixscreen.service" ]
}

@test "install_service: INIT_SYSTEM=sysv calls sysv path" {
    INIT_SYSTEM="sysv"
    create_init_template

    install_service "ad5m"

    [ -f "$INIT_SCRIPT_DEST" ]
    [ -x "$INIT_SCRIPT_DEST" ]
}

# =============================================================================
# stop_service
# =============================================================================

@test "stop_service: systemd calls systemctl stop" {
    INIT_SYSTEM="systemd"
    local stop_called="$BATS_TEST_TMPDIR/stop_called"
    mock_command_script "systemctl" '
        case "$*" in
            *is-active*) exit 0 ;;
            *stop*) touch "'"$stop_called"'"; exit 0 ;;
            *) exit 0 ;;
        esac
    '

    stop_service

    [ -f "$stop_called" ]
}

@test "stop_service: sysv calls init script stop" {
    INIT_SYSTEM="sysv"
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
    # Create executable init script that records stop call
    local stop_called="$BATS_TEST_TMPDIR/sysv_stop_called"
    cat > "$INIT_SCRIPT_DEST" << STOPEOF
#!/bin/sh
case "\$1" in
    stop) touch "$stop_called" ;;
esac
STOPEOF
    chmod +x "$INIT_SCRIPT_DEST"

    # Mock killall/kill to prevent actual process kills
    mock_command "killall" ""
    mock_command_script "pidof" 'exit 1'

    stop_service

    [ -f "$stop_called" ]
}

@test "stop_service: sysv checks all HELIX_INIT_SCRIPTS" {
    INIT_SYSTEM="sysv"
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/nonexistent"
    local alt_script="$BATS_TEST_TMPDIR/etc/init.d/S80helixscreen"
    local alt_called="$BATS_TEST_TMPDIR/alt_stop_called"
    mkdir -p "$(dirname "$alt_script")"
    cat > "$alt_script" << ALTEOF
#!/bin/sh
case "\$1" in
    stop) touch "$alt_called" ;;
esac
ALTEOF
    chmod +x "$alt_script"
    HELIX_INIT_SCRIPTS="$alt_script"

    # Mock killall/kill
    mock_command "killall" ""
    mock_command_script "pidof" 'exit 1'

    stop_service

    [ -f "$alt_called" ]
}

@test "stop_service: failures are suppressed (|| true pattern)" {
    INIT_SYSTEM="systemd"
    # systemctl is-active says running, but stop fails
    mock_command_script "systemctl" '
        case "$*" in
            *is-active*) exit 0 ;;
            *stop*) exit 1 ;;
            *) exit 0 ;;
        esac
    '

    # Should not exit with error despite stop failure
    run stop_service
    [ "$status" -eq 0 ]
}

# =============================================================================
# deploy_platform_hooks
# =============================================================================

@test "deploy_platform_hooks: copies hooks file for known platform" {
    local hooks_src="$INSTALL_DIR/assets/config/platform/hooks-pi.sh"
    mkdir -p "$(dirname "$hooks_src")"
    echo '#!/bin/sh' > "$hooks_src"
    echo 'platform_pre_start() { :; }' >> "$hooks_src"

    deploy_platform_hooks "$INSTALL_DIR" "pi"

    [ -f "$INSTALL_DIR/platform/hooks.sh" ]
    [ -x "$INSTALL_DIR/platform/hooks.sh" ]
}

@test "deploy_platform_hooks: no hooks file returns 0" {
    # No hooks file created for this platform
    run deploy_platform_hooks "$INSTALL_DIR" "unknown-platform"
    [ "$status" -eq 0 ]
}

# Regression: tarball ships hooks under assets/config/platform/, not config/platform/.
# Before the fix, deploy_platform_hooks looked in config/platform/ and silently
# emitted "No platform hooks for: <platform>" on every sysv install because the
# tarball never put anything there.
@test "deploy_platform_hooks: looks under assets/config/platform (not legacy config/platform)" {
    # Only the NEW tarball location is populated; legacy location stays empty.
    local tarball_src="$INSTALL_DIR/assets/config/platform/hooks-k2.sh"
    mkdir -p "$(dirname "$tarball_src")"
    cat > "$tarball_src" << 'EOF'
#!/bin/sh
platform_stop_competing_uis() { /etc/init.d/app stop; }
EOF

    deploy_platform_hooks "$INSTALL_DIR" "k2"

    [ -f "$INSTALL_DIR/platform/hooks.sh" ]
    grep -q '/etc/init.d/app stop' "$INSTALL_DIR/platform/hooks.sh"
}

@test "deploy_platform_hooks: legacy config/platform/ is NOT consulted" {
    # Stash a decoy at the OLD path; deploy must NOT pick it up.
    local decoy="$INSTALL_DIR/config/platform/hooks-k2.sh"
    mkdir -p "$(dirname "$decoy")"
    echo 'exit 42' > "$decoy"

    run deploy_platform_hooks "$INSTALL_DIR" "k2"

    # Tarball location empty → warn and return 0 (no hooks deployed).
    [ "$status" -eq 0 ]
    [ ! -f "$INSTALL_DIR/platform/hooks.sh" ]
}

@test "deploy_platform_hooks: creates platform directory if needed" {
    local hooks_src="$INSTALL_DIR/assets/config/platform/hooks-k1.sh"
    mkdir -p "$(dirname "$hooks_src")"
    echo '#!/bin/sh' > "$hooks_src"

    # Ensure platform dir does not exist
    rm -rf "$INSTALL_DIR/platform"

    deploy_platform_hooks "$INSTALL_DIR" "k1"

    [ -d "$INSTALL_DIR/platform" ]
    [ -f "$INSTALL_DIR/platform/hooks.sh" ]
}

# =============================================================================
# fix_install_ownership
# =============================================================================

@test "fix_install_ownership: non-root user chowns config directory" {
    KLIPPER_USER="biqu"
    mkdir -p "$INSTALL_DIR/config"
    local chown_log="$BATS_TEST_TMPDIR/chown_log"
    mock_command_script "chown" 'echo "$@" >> "'"$chown_log"'"'

    fix_install_ownership

    [ -f "$chown_log" ]
    grep -q "biqu:biqu" "$chown_log"
    grep -q "$INSTALL_DIR" "$chown_log"
}

# Root-run platforms (ad5m/ad5x/k1/k2/cc1/u1) used to be skipped entirely, so
# nothing ever normalised the numeric uid/gid that root's --same-owner extract
# restored out of the release tarball.  A K2 install measured 890 of 915 files
# owned by uid 1001, which has no /etc/passwd entry on that box.  The root case
# must now chown to root:root so existing bad installs heal on the next update.
@test "fix_install_ownership: root user normalises the tree to root:root" {
    KLIPPER_USER="root"
    mkdir -p "$INSTALL_DIR/config"
    local chown_log="$BATS_TEST_TMPDIR/chown_log"
    mock_command_script "chown" 'echo "$@" >> "'"$chown_log"'"'

    fix_install_ownership

    [ -f "$chown_log" ]
    grep -q "root:root" "$chown_log"
    grep -q "$INSTALL_DIR" "$chown_log"
}

@test "fix_install_ownership: root repair follows no symlinks out of INSTALL_DIR" {
    KLIPPER_USER="root"
    mkdir -p "$INSTALL_DIR/config"
    local chown_log="$BATS_TEST_TMPDIR/chown_log"
    mock_command_script "chown" 'echo "$@" >> "'"$chown_log"'"'

    fix_install_ownership

    # -h keeps chown on the symlink itself rather than its target, which may
    # live outside INSTALL_DIR (printer_data config symlinks do exactly that).
    grep -q -- "-Rh" "$chown_log"
}

@test "fix_install_ownership: empty KLIPPER_USER is a no-op" {
    KLIPPER_USER=""
    local chown_log="$BATS_TEST_TMPDIR/chown_log"
    mock_command_script "chown" 'echo "$@" >> "'"$chown_log"'"'

    fix_install_ownership

    [ ! -f "$chown_log" ]
}

@test "fix_install_ownership: missing INSTALL_DIR is a no-op even for root" {
    KLIPPER_USER="root"
    INSTALL_DIR="$BATS_TEST_TMPDIR/definitely/not/here"
    local chown_log="$BATS_TEST_TMPDIR/chown_log"
    mock_command_script "chown" 'echo "$@" >> "'"$chown_log"'"'

    fix_install_ownership

    [ ! -f "$chown_log" ]
}

@test "fix_install_ownership: missing config dir does not crash" {
    KLIPPER_USER="biqu"
    rm -rf "$INSTALL_DIR/config"
    mock_command "chown" ""

    run fix_install_ownership
    [ "$status" -eq 0 ]
}

@test "fix_install_ownership: succeeds without sudo when files are user-owned" {
    # Self-update scenario: config is already owned by the user, so bare chown
    # succeeds and sudo is never invoked.
    KLIPPER_USER="biqu"
    SUDO="sudo_should_not_be_called"
    mkdir -p "$INSTALL_DIR/config"

    local chown_log="$BATS_TEST_TMPDIR/chown_log"
    mock_command_script "chown" 'echo "$@" >> "'"$chown_log"'"; exit 0'

    run fix_install_ownership
    [ "$status" -eq 0 ]
    [ -f "$chown_log" ]
    grep -q "biqu:biqu" "$chown_log"
}

@test "fix_install_ownership: falls back to sudo when bare chown fails" {
    # Fresh install scenario: config is root-owned, bare chown fails,
    # $SUDO chown succeeds.
    KLIPPER_USER="biqu"
    mkdir -p "$INSTALL_DIR/config"

    local sudo_log="$BATS_TEST_TMPDIR/sudo_log"
    # chown without sudo fails; sudo wrapper records the call
    mock_command_script "chown" 'exit 1'
    mock_command_script "sudo" 'echo "$@" >> "'"$sudo_log"'"; exit 0'
    SUDO="$BATS_TEST_TMPDIR/bin/sudo"

    run fix_install_ownership
    [ "$status" -eq 0 ]
    [ -f "$sudo_log" ]
    grep -q "chown" "$sudo_log"
}

@test "fix_install_ownership: survives when both chown and sudo fail (NoNewPrivileges)" {
    # NoNewPrivileges scenario: bare chown fails AND sudo is blocked.
    # The || true must prevent set -eu from killing the script.
    KLIPPER_USER="biqu"
    mkdir -p "$INSTALL_DIR/config"

    mock_command_fail "chown"
    mock_command_fail "sudo"
    SUDO="$BATS_TEST_TMPDIR/bin/sudo"

    run fix_install_ownership
    [ "$status" -eq 0 ]
}

# =============================================================================
# _is_self_update / HELIX_SELF_UPDATE skip logic
# =============================================================================

@test "stop_service: sysv self-update skips stop" {
    INIT_SYSTEM="sysv"
    export HELIX_SELF_UPDATE=1
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
    local stop_called="$BATS_TEST_TMPDIR/sysv_stop_called"
    cat > "$INIT_SCRIPT_DEST" << STOPEOF
#!/bin/sh
case "\$1" in
    stop) touch "$stop_called" ;;
esac
STOPEOF
    chmod +x "$INIT_SCRIPT_DEST"

    mock_command "killall" ""
    mock_command_script "pidof" 'exit 1'

    stop_service

    # Init stop must NOT have been called
    [ ! -f "$stop_called" ]
}

@test "stop_service: sysv without self-update still stops" {
    INIT_SYSTEM="sysv"
    unset HELIX_SELF_UPDATE
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
    local stop_called="$BATS_TEST_TMPDIR/sysv_stop_called"
    cat > "$INIT_SCRIPT_DEST" << STOPEOF
#!/bin/sh
case "\$1" in
    stop) touch "$stop_called" ;;
esac
STOPEOF
    chmod +x "$INIT_SCRIPT_DEST"

    mock_command "killall" ""
    mock_command_script "pidof" 'exit 1'

    stop_service

    [ -f "$stop_called" ]
}

@test "start_service_sysv: self-update skips start" {
    export HELIX_SELF_UPDATE=1
    local start_called="$BATS_TEST_TMPDIR/sysv_start_called"
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
    cat > "$INIT_SCRIPT_DEST" << STARTEOF
#!/bin/sh
case "\$1" in
    start) touch "$start_called" ;;
    status) exit 0 ;;
esac
STARTEOF
    chmod +x "$INIT_SCRIPT_DEST"

    start_service_sysv

    [ ! -f "$start_called" ]
}

@test "start_service_sysv: without self-update starts normally" {
    unset HELIX_SELF_UPDATE
    local start_called="$BATS_TEST_TMPDIR/sysv_start_called"
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
    cat > "$INIT_SCRIPT_DEST" << STARTEOF
#!/bin/sh
case "\$1" in
    start) touch "$start_called" ;;
    status) exit 0 ;;
esac
STARTEOF
    chmod +x "$INIT_SCRIPT_DEST"
    # No instance running -> plain start, not restart
    mock_command_script "pidof" 'exit 1'

    start_service_sysv

    [ -f "$start_called" ]
}

@test "start_service_sysv: restarts when an instance is already running (#1106)" {
    unset HELIX_SELF_UPDATE
    local start_called="$BATS_TEST_TMPDIR/sysv_start_called"
    local restart_called="$BATS_TEST_TMPDIR/sysv_restart_called"
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
    cat > "$INIT_SCRIPT_DEST" << STARTEOF
#!/bin/sh
case "\$1" in
    start) touch "$start_called" ;;
    restart) touch "$restart_called" ;;
    status) exit 0 ;;
esac
STARTEOF
    chmod +x "$INIT_SCRIPT_DEST"
    # Old instance still running: "start" would be a no-op and leave the old
    # binary in place — the installer must restart instead
    mock_command_script "pidof" 'exit 0'

    start_service_sysv

    [ -f "$restart_called" ]
    [ ! -f "$start_called" ]
}

@test "start_service_snapmaker_u1: restarts when an instance is already running (#1106)" {
    unset HELIX_SELF_UPDATE
    local start_called="$BATS_TEST_TMPDIR/u1_start_called"
    local restart_called="$BATS_TEST_TMPDIR/u1_restart_called"
    cat > "$INSTALL_DIR/config/helixscreen.init" << STARTEOF
#!/bin/sh
case "\$1" in
    start) touch "$start_called" ;;
    restart) touch "$restart_called" ;;
    status) exit 0 ;;
esac
STARTEOF
    chmod +x "$INSTALL_DIR/config/helixscreen.init"
    mock_command_script "pidof" 'exit 0'

    start_service_snapmaker_u1

    [ -f "$restart_called" ]
    [ ! -f "$start_called" ]
}

@test "start_service_systemd: restarts when the service is already active (#1106)" {
    unset HELIX_SELF_UPDATE
    local start_called="$BATS_TEST_TMPDIR/systemd_start_called"
    local restart_called="$BATS_TEST_TMPDIR/systemd_restart_called"
    mock_command_script "systemctl" '
        case "$*" in
            *enable*) exit 0 ;;
            *is-active*) exit 0 ;;
            *restart*) touch "'"$restart_called"'"; exit 0 ;;
            *start*) touch "'"$start_called"'"; exit 0 ;;
            *) exit 0 ;;
        esac
    '

    start_service_systemd

    [ -f "$restart_called" ]
    [ ! -f "$start_called" ]
}

@test "stop_service: systemd skips stop during self-update (NoNewPrivs fallback)" {
    INIT_SYSTEM="systemd"
    export HELIX_SELF_UPDATE=1
    local stop_called="$BATS_TEST_TMPDIR/stop_called"
    mock_command_script "systemctl" '
        case "$*" in
            *is-active*) exit 0 ;;
            *stop*) touch "'"$stop_called"'"; exit 0 ;;
            *) exit 0 ;;
        esac
    '

    stop_service

    # _has_no_new_privs() falls back to _is_self_update() when /proc doesn't show
    # NoNewPrivs — so self-update skips stop (watchdog handles restart)
    [ ! -f "$stop_called" ]
}
