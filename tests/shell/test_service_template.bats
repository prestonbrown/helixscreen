#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for systemd service file templating
# Verifies placeholders exist in template and substitution works correctly

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
SERVICE_TEMPLATE="$WORKTREE_ROOT/config/helixscreen.service"

setup() {
    load helpers
    # helix-launcher.sh runs `killall helix-watchdog helix-screen ...`, which is
    # not scoped to this test. bats runs FILES in parallel, so an unmocked run
    # reaches across and kills the long-lived instance test_headless_display.bats
    # is driving - it dies cleanly mid-startup and that test fails for no reason
    # of its own.
    mock_command_script "killall" 'exit 0'
    # These tests substitute template placeholders with GNU-style `sed -i EXPR`.
    install_gnu_sed_shim
}

# --- Template placeholder tests ---

@test "service template has @@HELIX_USER@@ placeholder" {
    grep -q '@@HELIX_USER@@' "$SERVICE_TEMPLATE"
}

@test "service template has @@HELIX_GROUP@@ placeholder" {
    grep -q '@@HELIX_GROUP@@' "$SERVICE_TEMPLATE"
}

@test "service template has @@INSTALL_DIR@@ in WorkingDirectory" {
    grep -q 'WorkingDirectory=@@INSTALL_DIR@@' "$SERVICE_TEMPLATE"
}

@test "service template has @@INSTALL_DIR@@ in ExecStart" {
    grep -q 'ExecStart=@@INSTALL_DIR@@' "$SERVICE_TEMPLATE"
}

@test "service template does NOT contain hardcoded User=root" {
    ! grep -q '^User=root' "$SERVICE_TEMPLATE"
}

@test "service template does NOT contain ProtectHome=read-only" {
    ! grep -q 'ProtectHome=read-only' "$SERVICE_TEMPLATE"
}

@test "service template HAS ReadWritePaths directive" {
    grep -q 'ReadWritePaths=' "$SERVICE_TEMPLATE"
}

@test "service template has @@INSTALL_PARENT@@ in ReadWritePaths" {
    grep -q 'ReadWritePaths=.*@@INSTALL_PARENT@@' "$SERVICE_TEMPLATE"
}

@test "service template HAS ProtectSystem=strict" {
    grep -q 'ProtectSystem=strict' "$SERVICE_TEMPLATE"
}

@test "service template does NOT enable PrivateTmp" {
    # PrivateTmp=true gives the service an isolated /tmp, hiding files Klipper
    # writes to the shared /tmp — notably input-shaper resonance CSVs
    # (/tmp/calibration_data_*.csv), which HelixScreen reads directly. Enabling
    # it silently breaks the frequency-response chart on every systemd install.
    ! grep -qiE '^PrivateTmp=(true|yes|1|on)$' "$SERVICE_TEMPLATE"
}

@test "service template HAS RuntimeDirectory=helixscreen for wpa_ctrl sockets" {
    grep -q 'RuntimeDirectory=helixscreen' "$SERVICE_TEMPLATE"
}

@test "service template HAS SupplementaryGroups=video input render tty" {
    grep -q 'SupplementaryGroups=video input render tty' "$SERVICE_TEMPLATE"
}

@test "service template HAS AmbientCapabilities with CAP_SYS_TTY_CONFIG" {
    grep -q 'AmbientCapabilities=.*CAP_SYS_TTY_CONFIG' "$SERVICE_TEMPLATE"
}

@test "service template HAS AmbientCapabilities with CAP_SYS_BOOT" {
    grep -q 'AmbientCapabilities=.*CAP_SYS_BOOT' "$SERVICE_TEMPLATE"
}

# --- Substitution tests (copy to tmpdir, run sed, verify) ---

@test "substitution replaces all @@HELIX_USER@@ with biqu" {
    cp "$SERVICE_TEMPLATE" "$BATS_TEST_TMPDIR/test.service"
    sed -i "s|@@HELIX_USER@@|biqu|g" "$BATS_TEST_TMPDIR/test.service"
    grep -q 'User=biqu' "$BATS_TEST_TMPDIR/test.service"
}

@test "substitution replaces all @@INSTALL_DIR@@ with /opt/helixscreen" {
    cp "$SERVICE_TEMPLATE" "$BATS_TEST_TMPDIR/test.service"
    sed -i "s|@@INSTALL_DIR@@|/opt/helixscreen|g" "$BATS_TEST_TMPDIR/test.service"
    grep -q 'WorkingDirectory=/opt/helixscreen' "$BATS_TEST_TMPDIR/test.service"
    grep -q 'ExecStart=/opt/helixscreen/bin/helix-launcher.sh' "$BATS_TEST_TMPDIR/test.service"
}

# --- SupplementaryGroups filtering tests ---
# These simulate the installer's group filtering logic

@test "SupplementaryGroups filtered to only existing groups" {
    # This test requires Linux groups (video/input/render) — skip on macOS
    getent group video >/dev/null 2>&1 || getent group input >/dev/null 2>&1 || skip "no video/input groups (not Linux)"
    cp "$SERVICE_TEMPLATE" "$BATS_TEST_TMPDIR/test.service"
    # Simulate filtering: keep only groups that exist on this system
    desired_groups=$(grep '^SupplementaryGroups=' "$BATS_TEST_TMPDIR/test.service" | sed 's/^SupplementaryGroups=//')
    existing_groups=""
    for grp in $desired_groups; do
        if getent group "$grp" >/dev/null 2>&1; then
            existing_groups="${existing_groups:+$existing_groups }$grp"
        fi
    done
    [ -n "$existing_groups" ]
    sed -i "s|^SupplementaryGroups=.*|SupplementaryGroups=$existing_groups|" "$BATS_TEST_TMPDIR/test.service"
    grep -q "^SupplementaryGroups=$existing_groups" "$BATS_TEST_TMPDIR/test.service"
}

@test "SupplementaryGroups line removed when no groups exist" {
    cp "$SERVICE_TEMPLATE" "$BATS_TEST_TMPDIR/test.service"
    # Replace with groups that definitely don't exist
    sed -i 's|^SupplementaryGroups=.*|SupplementaryGroups=fakegrp1 fakegrp2|' "$BATS_TEST_TMPDIR/test.service"
    # Now simulate the filtering logic
    desired_groups=$(grep '^SupplementaryGroups=' "$BATS_TEST_TMPDIR/test.service" | sed 's/^SupplementaryGroups=//')
    existing_groups=""
    for grp in $desired_groups; do
        if getent group "$grp" >/dev/null 2>&1; then
            existing_groups="${existing_groups:+$existing_groups }$grp"
        fi
    done
    # No groups matched — remove the directive
    [ -z "$existing_groups" ]
    sed -i '/^SupplementaryGroups=/d' "$BATS_TEST_TMPDIR/test.service"
    ! grep -q '^SupplementaryGroups=' "$BATS_TEST_TMPDIR/test.service"
}

@test "no @@ markers remain after full substitution" {
    cp "$SERVICE_TEMPLATE" "$BATS_TEST_TMPDIR/test.service"
    sed -i "s|@@HELIX_USER@@|biqu|g;s|@@HELIX_GROUP@@|biqu|g;s|@@INSTALL_DIR@@|/opt/helixscreen|g;s|@@INSTALL_PARENT@@|/opt|g" "$BATS_TEST_TMPDIR/test.service"
    # Line 47 intentionally preserves @@HELIX_USER@@ in a runtime self-heal command
    # using quote-splitting (@@""HELIX_USER""@@) to survive the installer's sed pass.
    # Exclude that line when checking for un-substituted markers.
    ! grep -v '@@""HELIX_USER""@@' "$BATS_TEST_TMPDIR/test.service" | grep -q '@@'
}

# --- Update watcher pause/resume (#470) ---

@test "service template stops update watcher in ExecStartPre" {
    grep -q 'ExecStartPre=.*systemctl stop helixscreen-update.path' "$SERVICE_TEMPLATE"
}

@test "service template re-arms update watcher in ExecStartPost" {
    grep -q 'ExecStartPost=.*systemctl start helixscreen-update.path' "$SERVICE_TEMPLATE"
}

@test "service template re-arms update watcher in ExecStopPost" {
    grep -q 'ExecStopPost=.*systemctl start helixscreen-update.path' "$SERVICE_TEMPLATE"
}

@test "update watcher stop comes before chown ExecStartPre" {
    # The stop must precede the chown to prevent ctime changes from
    # triggering PathChanged on release_info.json.
    local stop_line chown_line
    stop_line=$(grep -n 'systemctl stop helixscreen-update.path' "$SERVICE_TEMPLATE" | head -1 | cut -d: -f1)
    chown_line=$(grep -n 'chown -Rh' "$SERVICE_TEMPLATE" | head -1 | cut -d: -f1)
    [ -n "$stop_line" ] && [ -n "$chown_line" ]
    [ "$stop_line" -lt "$chown_line" ]
}

# --- Shell syntax safety tests (#495) ---

@test "Exec* lines with shell operators must use /bin/sh -c wrapper" {
    # systemd does NOT invoke a shell for Exec* commands. Shell operators like
    # 2>/dev/null, ||, &&, >, |, ; are silently passed as literal arguments,
    # causing systemctl to fail with "Invalid unit name" errors.
    # Every Exec* line that uses shell syntax MUST be wrapped in /bin/sh -c '...'.
    local failures=""
    while IFS= read -r line; do
        # Skip comments and empty lines
        [[ "$line" =~ ^[[:space:]]*# ]] && continue
        [[ -z "$line" ]] && continue
        # Only check Exec* directives
        [[ "$line" =~ ^Exec ]] || continue
        # Strip the Exec* prefix and optional + (root) prefix to get the command
        local cmd="${line#Exec*=}"
        cmd="${cmd#+}"
        # If the command uses shell operators but doesn't start with /bin/sh
        if echo "$cmd" | grep -qE '2>/dev/null|\|\||&&|[^>]>[^>]|\|[^|]|;' && \
           ! echo "$cmd" | grep -qE '^/bin/sh\b'; then
            failures="${failures}\n  ${line}"
        fi
    done < "$SERVICE_TEMPLATE"
    if [ -n "$failures" ]; then
        echo "Exec* lines use shell syntax without /bin/sh -c wrapper:${failures}"
        false
    fi
}

# --- Boot ordering tests (Plymouth, not multi-user.target) ---

@test "service template waits for Plymouth quit in After=" {
    grep -q '^After=plymouth-quit-wait.service' "$SERVICE_TEMPLATE"
}

@test "service template does NOT have After=multi-user.target" {
    ! grep -q '^After=multi-user.target' "$SERVICE_TEMPLATE"
}

@test "service template still has WantedBy=multi-user.target in Install section" {
    grep -q '^WantedBy=multi-user.target' "$SERVICE_TEMPLATE"
}

# --- Substitution edge cases ---

@test "substitution replaces @@INSTALL_PARENT@@ with parent of install dir" {
    cp "$SERVICE_TEMPLATE" "$BATS_TEST_TMPDIR/test.service"
    sed -i "s|@@INSTALL_DIR@@|/opt/helixscreen|g;s|@@INSTALL_PARENT@@|/opt|g" "$BATS_TEST_TMPDIR/test.service"
    grep -q 'ReadWritePaths=/opt/helixscreen /opt /var/log' "$BATS_TEST_TMPDIR/test.service"
}
